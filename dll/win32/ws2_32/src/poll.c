/*
 * PROJECT:     ReactOS WinSock 2 API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     WSAPoll
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *              Based on WSAPoll in Wine's ws2_32 (dlls/ws2_32/socket.c),
 *              Copyright 1993,1994,1996 John Brezak, Erik Bos, Alex Korobka,
 *              Copyright 2001 Stefan Leichter, Copyright 2020 Zebediah Figura
 *              for CodeWeavers, and other Wine contributors.
 */

/* INCLUDES ******************************************************************/

/* WSAPOLLFD needs the Vista Winsock headers, the rest of the DLL builds as 5.2 */
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x600
#include <ws2_32.h>
#include <ndk/iofuncs.h>
#include <tdi.h>
#include <afd/shared.h>

#define NDEBUG
#include <debug.h>

#define POLL_REQUEST_EVENTS (POLLRDNORM | POLLRDBAND | POLLWRNORM)

/* FUNCTIONS *****************************************************************/

static
BOOL
WsPollIsSocket(IN SOCKET Handle)
{
    PWSSOCKET Socket;

    if ((INT_PTR)Handle < 0)
        return FALSE;

    Socket = WsSockGetSocket(Handle);
    if (!Socket)
        return FALSE;

    WsSockDereference(Socket);
    return TRUE;
}

static
INT
WsPollStatusToError(IN NTSTATUS Status)
{
    switch (Status)
    {
        case STATUS_SUCCESS:
        case STATUS_TIMEOUT:
            return NO_ERROR;
        case STATUS_INVALID_HANDLE:
        case STATUS_OBJECT_TYPE_MISMATCH:
            return WSAENOTSOCK;
        case STATUS_NO_MEMORY:
        case STATUS_INSUFFICIENT_RESOURCES:
            return WSAENOBUFS;
        case STATUS_CANCELLED:
            return WSAEINTR;
        case STATUS_ACCESS_VIOLATION:
            return WSAEFAULT;
        default:
            return WSAEINVAL;
    }
}

/*
 * @implemented
 */
INT
WSAAPI
WSAPoll(IN OUT LPWSAPOLLFD fdArray,
        IN ULONG fds,
        IN INT timeout)
{
    PAFD_POLL_INFO PollInfo;
    PULONG Index;
    SOCKET PollSocket = INVALID_SOCKET;
    IO_STATUS_BLOCK IoStatus;
    HANDLE Event;
    NTSTATUS Status;
    ULONG PollSize, Valid = 0, i, k;
    INT ErrorCode, Ready = 0;

    ErrorCode = WsQuickProlog();
    if (ErrorCode != ERROR_SUCCESS)
    {
        SetLastError(ErrorCode);
        return SOCKET_ERROR;
    }

    if (!fds)
    {
        SetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }
    if (!fdArray)
    {
        SetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    if (fds > (MAXLONG - sizeof(AFD_POLL_INFO)) / (sizeof(AFD_HANDLE) + sizeof(ULONG)))
    {
        SetLastError(WSAENOBUFS);
        return SOCKET_ERROR;
    }

    for (i = 0; i < fds; i++)
    {
        fdArray[i].revents = 0;
        if (WsPollIsSocket(fdArray[i].fd))
        {
            PollSocket = fdArray[i].fd;
            Valid++;
        }
    }

    if (!Valid)
    {
        for (i = 0; i < fds; i++)
            fdArray[i].revents = POLLNVAL;
        SetLastError(WSAENOTSOCK);
        return SOCKET_ERROR;
    }

    /* The requested events are checked for every entry that is not a negative handle */
    for (i = 0; i < fds; i++)
    {
        if ((INT_PTR)fdArray[i].fd >= 0 && (fdArray[i].events & ~POLL_REQUEST_EVENTS))
        {
            SetLastError(WSAEINVAL);
            return SOCKET_ERROR;
        }
    }

    PollSize = FIELD_OFFSET(AFD_POLL_INFO, Handles) + Valid * sizeof(AFD_HANDLE);
    PollInfo = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PollSize + Valid * sizeof(ULONG));
    if (!PollInfo)
    {
        SetLastError(WSAENOBUFS);
        return SOCKET_ERROR;
    }
    Index = (PULONG)((PUCHAR)PollInfo + PollSize);

    Event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!Event)
    {
        HeapFree(GetProcessHeap(), 0, PollInfo);
        SetLastError(WSAENOBUFS);
        return SOCKET_ERROR;
    }

    if (timeout >= 0)
        PollInfo->Timeout.QuadPart = (LONGLONG)timeout * -10000;
    else
        PollInfo->Timeout.QuadPart = MAXLONGLONG;
    PollInfo->HandleCount = Valid;

    for (i = 0, k = 0; i < fds; i++)
    {
        ULONG Events = AFD_EVENT_DISCONNECT | AFD_EVENT_ABORT | AFD_EVENT_CONNECT_FAIL;

        if (!WsPollIsSocket(fdArray[i].fd))
        {
            fdArray[i].revents = POLLNVAL;
            continue;
        }

        if (fdArray[i].events & POLLRDNORM)
            Events |= AFD_EVENT_ACCEPT | AFD_EVENT_RECEIVE;
        if (fdArray[i].events & POLLRDBAND)
            Events |= AFD_EVENT_OOB_RECEIVE;
        if (fdArray[i].events & POLLWRNORM)
            Events |= AFD_EVENT_SEND;

        /* A socket closed in between is dropped from the request */
        if (k >= Valid)
            break;
        PollInfo->Handles[k].Handle = fdArray[i].fd;
        PollInfo->Handles[k].Events = Events;
        Index[k++] = i;
    }
    PollInfo->HandleCount = k;
    PollSize = FIELD_OFFSET(AFD_POLL_INFO, Handles) + k * sizeof(AFD_HANDLE);

    if (!k)
    {
        CloseHandle(Event);
        HeapFree(GetProcessHeap(), 0, PollInfo);
        SetLastError(WSAENOTSOCK);
        return SOCKET_ERROR;
    }

    Status = NtDeviceIoControlFile((HANDLE)PollSocket,
                                   Event,
                                   NULL,
                                   NULL,
                                   &IoStatus,
                                   IOCTL_AFD_SELECT,
                                   PollInfo,
                                   PollSize,
                                   PollInfo,
                                   PollSize);
    if (Status == STATUS_PENDING)
    {
        /* Queued APCs run, the poll goes on */
        while (WaitForSingleObjectEx(Event, INFINITE, TRUE) == WAIT_IO_COMPLETION)
            ;
        Status = IoStatus.Status;
    }

    if (Status == STATUS_CANCELLED)
    {
        /* AFD cancels the poll, without events, when a socket of the set is closed */
        for (k = 0; k < PollInfo->HandleCount; k++)
        {
            ULONG Flags;

            if (!GetHandleInformation((HANDLE)PollInfo->Handles[k].Handle, &Flags))
            {
                fdArray[Index[k]].revents = POLLNVAL;
                Ready++;
            }
        }
        if (Ready)
            Status = STATUS_SUCCESS;
    }
    else if (NT_SUCCESS(Status))
    {
        for (k = 0; k < PollInfo->HandleCount; k++)
        {
            ULONG Flags = PollInfo->Handles[k].Events;
            ULONG Revents = 0;
            PWSAPOLLFD Entry = &fdArray[Index[k]];

            if (Flags & (AFD_EVENT_ACCEPT | AFD_EVENT_RECEIVE))
                Revents |= POLLRDNORM;
            if (Flags & AFD_EVENT_OOB_RECEIVE)
                Revents |= POLLRDBAND;
            if (Flags & AFD_EVENT_SEND)
                Revents |= POLLWRNORM;
            if (Flags & (AFD_EVENT_ABORT | AFD_EVENT_DISCONNECT))
                Revents |= POLLHUP;
            if (Flags & (AFD_EVENT_ABORT | AFD_EVENT_CONNECT_FAIL))
                Revents |= POLLERR;
            if (Flags & AFD_EVENT_CLOSE)
                Revents |= POLLNVAL;

            Entry->revents = (SHORT)(Revents & (Entry->events | POLLHUP | POLLERR | POLLNVAL));
            if (Entry->revents)
                Ready++;
        }
    }

    CloseHandle(Event);
    HeapFree(GetProcessHeap(), 0, PollInfo);

    ErrorCode = WsPollStatusToError(Status);
    SetLastError(ErrorCode);
    return ErrorCode != NO_ERROR ? SOCKET_ERROR : Ready;
}
