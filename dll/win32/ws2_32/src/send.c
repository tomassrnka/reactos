/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS WinSock 2 API
 * FILE:        dll/win32/ws2_32/src/send.c
 * PURPOSE:     Socket Sending Support.
 * PROGRAMMER:  Alex Ionescu (alex@relsoft.net)
 */

/* INCLUDES ******************************************************************/

#include <ws2_32.h>
#include <ws2tcpip.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

/*
 * @implemented
 */
INT
WSAAPI
send(IN SOCKET s,
     IN CONST CHAR FAR* buf,
     IN INT len,
     IN INT flags)
{
    PWSSOCKET Socket;
    INT Status;
    INT ErrorCode;
    LPWSATHREADID ThreadId;
    WSABUF Buffers;
    DWORD BytesSent;
    DPRINT("send: %lx, %lx, %lx, %p\n", s, flags, len, buf);

    /* Check for WSAStartup */
    if ((ErrorCode = WsQuickPrologTid(&ThreadId)) == ERROR_SUCCESS)
    {
        /* Get the Socket Context */
        if ((Socket = WsSockGetSocket(s)))
        {
            /* Setup the buffers */
            Buffers.buf = (PCHAR)buf;
            Buffers.len = len;

            /* Make the call */
            Status = Socket->Provider->Service.lpWSPSend(s,
                                                         &Buffers,
                                                         1,
                                                         &BytesSent,
                                                         (DWORD)flags,
                                                         NULL,
                                                         NULL,
                                                         ThreadId,
                                                         &ErrorCode);
            /* Deference the Socket Context */
            WsSockDereference(Socket);

            /* Return Provider Value */
            if (Status == ERROR_SUCCESS) return BytesSent;

            /* If everything seemed fine, then the WSP call failed itself */
            if (ErrorCode == NO_ERROR) ErrorCode = WSASYSCALLFAILURE;
        }
        else
        {
            /* No Socket Context Found */
            ErrorCode = WSAENOTSOCK;
        }
    }

    /* Return with an Error */
    SetLastError(ErrorCode);
    return SOCKET_ERROR;
}

/*
 * @implemented
 */
INT
WSAAPI
sendto(IN SOCKET s,
       IN CONST CHAR FAR* buf,
       IN INT len,
       IN INT flags,
       IN CONST struct sockaddr *to,
       IN INT tolen)
{
    PWSSOCKET Socket;
    INT Status;
    INT ErrorCode;
    LPWSATHREADID ThreadId;
    WSABUF Buffers;
    DWORD BytesSent;
    DPRINT("sendto: %lx, %lx, %lx, %p\n", s, flags, len, buf);

    /* Check for WSAStartup */
    if ((ErrorCode = WsQuickPrologTid(&ThreadId)) == ERROR_SUCCESS)
    {
        /* Get the Socket Context */
        if ((Socket = WsSockGetSocket(s)))
        {
            /* Setup the buffers */
            Buffers.buf = (PCHAR)buf;
            Buffers.len = len;

            /* Make the call */
            Status = Socket->Provider->Service.lpWSPSendTo(s,
                                                           &Buffers,
                                                           1,
                                                           &BytesSent,
                                                           (DWORD)flags,
                                                           to,
                                                           tolen,
                                                           NULL,
                                                           NULL,
                                                           ThreadId,
                                                           &ErrorCode);
            /* Deference the Socket Context */
            WsSockDereference(Socket);

            /* Return Provider Value */
            if (Status == ERROR_SUCCESS) return BytesSent;

            /* If everything seemed fine, then the WSP call failed itself */
            if (ErrorCode == NO_ERROR) ErrorCode = WSASYSCALLFAILURE;
        }
        else
        {
            /* No Socket Context Found */
            ErrorCode = WSAENOTSOCK;
        }
    }

    /* Return with an Error */
    SetLastError(ErrorCode);
    return SOCKET_ERROR;
}

/*
 * @implemented
 */
INT
WSAAPI
WSASend(IN SOCKET s,
        IN LPWSABUF lpBuffers,
        IN DWORD dwBufferCount,
        OUT LPDWORD lpNumberOfBytesSent,
        IN DWORD dwFlags,
        IN LPWSAOVERLAPPED lpOverlapped,
        IN LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    PWSSOCKET Socket;
    INT Status;
    INT ErrorCode;
    LPWSATHREADID ThreadId;
    DPRINT("WSASend: %lx, %lx, %lx, %p\n", s, dwFlags, dwBufferCount, lpBuffers);

    /* Check for WSAStartup */
    if ((ErrorCode = WsQuickPrologTid(&ThreadId)) == ERROR_SUCCESS)
    {
        /* Get the Socket Context */
        if ((Socket = WsSockGetSocket(s)))
        {
            /* Make the call */
            Status = Socket->Provider->Service.lpWSPSend(s,
                                                         lpBuffers,
                                                         dwBufferCount,
                                                         lpNumberOfBytesSent,
                                                         dwFlags,
                                                         lpOverlapped,
                                                         lpCompletionRoutine,
                                                         ThreadId,
                                                         &ErrorCode);
            /* Deference the Socket Context */
            WsSockDereference(Socket);

            /* Return Provider Value */
            if (Status == ERROR_SUCCESS) return Status;

            /* If everything seemed fine, then the WSP call failed itself */
            if (ErrorCode == NO_ERROR) ErrorCode = WSASYSCALLFAILURE;
        }
        else
        {
            /* No Socket Context Found */
            ErrorCode = WSAENOTSOCK;
        }
    }

    /* Return with an Error */
    SetLastError(ErrorCode);
    return SOCKET_ERROR;
}

/*
 * @implemented
 */
INT
WSAAPI
WSASendDisconnect(IN SOCKET s,
                  IN LPWSABUF lpOutboundDisconnectData)
{
    PWSPROCESS Process;
    PWSTHREAD Thread;
    PWSSOCKET Socket;
    INT ErrorCode;
    INT Status;
    DPRINT("WSASendDisconnect: %lx %p\n", s, lpOutboundDisconnectData);

    /* Enter prolog */
    if ((ErrorCode = WsApiProlog(&Process, &Thread)) == ERROR_SUCCESS)
    {
        /* Get the Socket Context */
        if ((Socket = WsSockGetSocket(s)))
        {
            /* Make the call */
            Status = Socket->Provider->Service.lpWSPSendDisconnect(s,
                                                                   lpOutboundDisconnectData,
                                                                   &ErrorCode);
            /* Deference the Socket Context */
            WsSockDereference(Socket);

            /* Return Provider Value */
            if (Status == ERROR_SUCCESS) return ERROR_SUCCESS;

            /* If everything seemed fine, then the WSP call failed itself */
            if (ErrorCode == NO_ERROR) ErrorCode = WSASYSCALLFAILURE;
        }
        else
        {
            /* No Socket Context Found */
            ErrorCode = WSAENOTSOCK;
        }
    }

    /* Return with an Error */
    SetLastError(ErrorCode);
    return SOCKET_ERROR;
}


/*
 * Checks the ancillary data of WSASendMsg: a buffer and a length go together,
 * and every header fits and has a level that Windows accepts on an IP socket.
 * The data itself is not applied: neither AFD nor the TCP/IP driver have an
 * interface for per-message options such as IP_PKTINFO.
 */
static
INT
WsCheckSendMsgControl(IN LPWSABUF Control)
{
    PUCHAR Cursor = (PUCHAR)Control->buf;
    ULONG Remaining = Control->len;
    LPWSACMSGHDR Header;
    ULONGLONG Step;

    if ((Control->buf == NULL) != (Control->len == 0))
        return WSAEFAULT;

    while (Remaining)
    {
        if (Remaining < sizeof(WSACMSGHDR))
            return WSAEINVAL;

        Header = (LPWSACMSGHDR)Cursor;
        if (Header->cmsg_len < sizeof(WSACMSGHDR) || Header->cmsg_len > Remaining)
            return WSAEINVAL;
        if (Header->cmsg_level != IPPROTO_IP && Header->cmsg_level != IPPROTO_IPV6)
            return WSAEINVAL;

        Step = ((ULONGLONG)Header->cmsg_len + TYPE_ALIGNMENT(WSACMSGHDR) - 1) & ~((ULONGLONG)TYPE_ALIGNMENT(WSACMSGHDR) - 1);
        if (Step >= Remaining)
            break;
        Cursor += Step;
        Remaining -= (ULONG)Step;
    }

    return NO_ERROR;
}

/* The largest datagram of an IPv4 UDP socket, and of an IPv4 raw socket */
#define WS_MAX_UDP_PAYLOAD 65507
#define WS_MAX_RAW_PAYLOAD 65535

/*
 * @implemented
 *
 * Only datagram and raw sockets, as on Windows. The message is sent with the
 * provider's send or send-to call. Ancillary data is checked but not applied.
 *
 * AFD sends only the first buffer of a datagram, so a synchronous send of a
 * message with several buffers gathers them into one here. An overlapped one
 * would need the gathered storage to outlive the call; that is not supported
 * and fails with WSAEOPNOTSUPP instead of losing data.
 */
INT
WSAAPI
WSASendMsg(IN SOCKET s,
           IN LPWSAMSG lpMsg,
           IN DWORD dwFlags,
           OUT LPDWORD lpNumberOfBytesSent,
           IN LPWSAOVERLAPPED lpOverlapped,
           IN LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    PWSSOCKET Socket;
    INT Status;
    INT ErrorCode;
    INT SocketType = 0;
    INT OptionLength;
    LPWSATHREADID ThreadId;
    WSAMSG Msg;
    SOCKADDR_STORAGE Name;
    WSABUF Single;
    LPWSABUF SendBuffers = NULL;
    LPWSABUF volatile Snapshot = NULL;
    DWORD Sent = 0, Index;
    ULONGLONG Total = 0;
    PCHAR volatile Gathered = NULL;
    DPRINT("WSASendMsg: %lx, %p, %lx\n", s, lpMsg, dwFlags);

    /* Check for WSAStartup */
    if ((ErrorCode = WsQuickPrologTid(&ThreadId)) != ERROR_SUCCESS)
        goto Fail;

    if (!lpMsg)
    {
        ErrorCode = WSAEFAULT;
        goto Fail;
    }

    /* Get the Socket Context */
    if (!(Socket = WsSockGetSocket(s)))
    {
        ErrorCode = WSAENOTSOCK;
        goto Fail;
    }

    ErrorCode = NO_ERROR;
    _SEH2_TRY
    {
        /* The caller's memory is read once; the checks below use the copy */
        Msg = *lpMsg;

        if (!Msg.lpBuffers || !Msg.dwBufferCount)
        {
            ErrorCode = WSAEINVAL;
        }
        else if (Msg.dwBufferCount > MAXLONG / sizeof(WSABUF))
        {
            ErrorCode = WSAENOBUFS;
        }
        else
        {
            /* The descriptors are copied, so that the caller cannot change them between the checks and the use */
            Snapshot = HeapAlloc(GetProcessHeap(), 0, Msg.dwBufferCount * sizeof(WSABUF));
            if (!Snapshot)
            {
                ErrorCode = WSAENOBUFS;
            }
            else
            {
                RtlCopyMemory(Snapshot, Msg.lpBuffers, Msg.dwBufferCount * sizeof(WSABUF));
                Msg.lpBuffers = Snapshot;
                for (Index = 0; Index < Msg.dwBufferCount; Index++)
                {
                    if (!Msg.lpBuffers[Index].buf && Msg.lpBuffers[Index].len)
                    {
                        ErrorCode = WSAEFAULT;
                        break;
                    }
                    Total += Msg.lpBuffers[Index].len;
                }
            }
        }

        if (ErrorCode == NO_ERROR && (dwFlags & ~(MSG_OOB | MSG_DONTROUTE | MSG_PARTIAL)))
            ErrorCode = WSAEOPNOTSUPP;

        if (ErrorCode == NO_ERROR)
        {
            OptionLength = sizeof(SocketType);
            if (Socket->Provider->Service.lpWSPGetSockOpt(s, SOL_SOCKET, SO_TYPE, (PCHAR)&SocketType, &OptionLength, &ErrorCode))
            {
                if (ErrorCode == NO_ERROR) ErrorCode = WSASYSCALLFAILURE;
            }
            else if (SocketType != SOCK_DGRAM && SocketType != SOCK_RAW)
            {
                ErrorCode = WSAEINVAL;
            }
            else if (Total > MAXLONG ||
                     (Socket->CatalogEntry->ProtocolInfo.iAddressFamily == AF_INET &&
                      Total > (SocketType == SOCK_DGRAM ? WS_MAX_UDP_PAYLOAD : WS_MAX_RAW_PAYLOAD)))
            {
                ErrorCode = WSAEMSGSIZE;
            }
        }

        /* The destination: required unless the socket is connected, and of the socket's address family */
        if (ErrorCode == NO_ERROR)
        {
            if (Msg.name)
            {
                INT ExpectedLength;

                if (Msg.namelen < (INT)sizeof(Msg.name->sa_family) || Msg.namelen > (INT)sizeof(Name))
                {
                    ErrorCode = WSAEFAULT;
                }
                else
                {
                    /* The provider gets a copy that cannot fault half way */
                    RtlCopyMemory(&Name, Msg.name, Msg.namelen);
                    Msg.name = (LPSOCKADDR)&Name;

                    if (Msg.name->sa_family != Socket->CatalogEntry->ProtocolInfo.iAddressFamily)
                    {
                        ErrorCode = WSAEAFNOSUPPORT;
                    }
                    else
                    {
                        switch (Msg.name->sa_family)
                        {
                            case AF_INET:
                                ExpectedLength = sizeof(SOCKADDR_IN);
                                break;
                            case AF_INET6:
                                ExpectedLength = sizeof(SOCKADDR_IN6);
                                break;
                            default:
                                ExpectedLength = sizeof(SOCKADDR);
                                break;
                        }
                        if (Msg.namelen != ExpectedLength)
                            ErrorCode = WSAEFAULT;
                    }
                }
            }
            else if (Msg.namelen)
            {
                ErrorCode = WSAEFAULT;
            }
            else
            {
                SOCKADDR_STORAGE Peer;
                INT PeerLength = sizeof(Peer);
                INT PeerError = NO_ERROR;

                /* Without a destination the socket must have a peer */
                if (Socket->Provider->Service.lpWSPGetPeerName(s, (LPSOCKADDR)&Peer, &PeerLength, &PeerError))
                {
                    if (PeerError == WSAENOTCONN)
                        ErrorCode = WSAEINVAL;
                    else
                        ErrorCode = (PeerError != NO_ERROR) ? PeerError : WSASYSCALLFAILURE;
                }
            }
        }

        if (ErrorCode == NO_ERROR)
            ErrorCode = WsCheckSendMsgControl(&Msg.Control);

        /* A synchronous send has to report its byte count */
        if (ErrorCode == NO_ERROR && !lpNumberOfBytesSent && !lpOverlapped)
            ErrorCode = WSAEFAULT;

        /* One buffer goes to the provider as it is; several are gathered */
        if (ErrorCode == NO_ERROR)
        {
            if (Msg.dwBufferCount == 1)
            {
                SendBuffers = Msg.lpBuffers;
            }
            else if (lpOverlapped)
            {
                ErrorCode = WSAEOPNOTSUPP;
            }
            else
            {
                if (Total)
                {
                    Gathered = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)Total);
                    if (!Gathered)
                    {
                        ErrorCode = WSAENOBUFS;
                    }
                    else
                    {
                        PCHAR Cursor = Gathered;

                        for (Index = 0; Index < Msg.dwBufferCount; Index++)
                        {
                            RtlCopyMemory(Cursor, Msg.lpBuffers[Index].buf, Msg.lpBuffers[Index].len);
                            Cursor += Msg.lpBuffers[Index].len;
                        }
                    }
                }
                Single.buf = Gathered;
                Single.len = (ULONG)Total;
                SendBuffers = &Single;
            }
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        ErrorCode = WSAEFAULT;
    }
    _SEH2_END;

    if (ErrorCode == NO_ERROR)
    {
        Status = SOCKET_ERROR;
        _SEH2_TRY
        {
            if (Msg.name)
            {
                Status = Socket->Provider->Service.lpWSPSendTo(s,
                                                               SendBuffers,
                                                               1,
                                                               &Sent,
                                                               dwFlags,
                                                               Msg.name,
                                                               Msg.namelen,
                                                               lpOverlapped,
                                                               lpCompletionRoutine,
                                                               ThreadId,
                                                               &ErrorCode);
            }
            else
            {
                Status = Socket->Provider->Service.lpWSPSend(s,
                                                             SendBuffers,
                                                             1,
                                                             &Sent,
                                                             dwFlags,
                                                             lpOverlapped,
                                                             lpCompletionRoutine,
                                                             ThreadId,
                                                             &ErrorCode);
            }
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            /* Bad overlapped storage or buffer memory: the provider touches them directly */
            Status = SOCKET_ERROR;
            ErrorCode = WSAEFAULT;
        }
        _SEH2_END;

        if (Gathered)
            HeapFree(GetProcessHeap(), 0, Gathered);
        if (Snapshot)
            HeapFree(GetProcessHeap(), 0, Snapshot);

        /* Deference the Socket Context */
        WsSockDereference(Socket);

        if (Status == ERROR_SUCCESS)
        {
            /* The byte count of a send that completed; the provider leaves a failed or pending one alone */
            if (lpNumberOfBytesSent)
            {
                _SEH2_TRY
                {
                    *lpNumberOfBytesSent = Sent;
                }
                _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
                {
                    ErrorCode = WSAEFAULT;
                    Status = SOCKET_ERROR;
                }
                _SEH2_END;
            }
            if (Status == ERROR_SUCCESS)
                return ERROR_SUCCESS;
        }
        else if (ErrorCode == NO_ERROR)
        {
            /* If everything seemed fine, then the WSP call failed itself */
            ErrorCode = WSASYSCALLFAILURE;
        }
    }
    else
    {
        if (Gathered)
            HeapFree(GetProcessHeap(), 0, Gathered);
        if (Snapshot)
            HeapFree(GetProcessHeap(), 0, Snapshot);
        WsSockDereference(Socket);
    }

Fail:
    /* Return with an Error */
    SetLastError(ErrorCode);
    return SOCKET_ERROR;
}

/*
 * @implemented
 */
INT
WSAAPI
WSASendTo(IN SOCKET s,
          IN LPWSABUF lpBuffers,
          IN DWORD dwBufferCount,
          OUT LPDWORD lpNumberOfBytesSent,
          IN DWORD dwFlags,
          IN CONST struct sockaddr *lpTo,
          IN INT iToLen,
          IN LPWSAOVERLAPPED lpOverlapped,
          IN LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
    PWSSOCKET Socket;
    INT Status;
    INT ErrorCode;
    LPWSATHREADID ThreadId;
    DPRINT("WSASendTo: %lx, %lx, %lx, %p\n", s, dwFlags, dwBufferCount, lpBuffers);

    /* Check for WSAStartup */
    if ((ErrorCode = WsQuickPrologTid(&ThreadId)) == ERROR_SUCCESS)
    {
        /* Get the Socket Context */
        if ((Socket = WsSockGetSocket(s)))
        {
            /* Make the call */
            Status = Socket->Provider->Service.lpWSPSendTo(s,
                                                           lpBuffers,
                                                           dwBufferCount,
                                                           lpNumberOfBytesSent,
                                                           dwFlags,
                                                           lpTo,
                                                           iToLen,
                                                           lpOverlapped,
                                                           lpCompletionRoutine,
                                                           ThreadId,
                                                           &ErrorCode);
            /* Deference the Socket Context */
            WsSockDereference(Socket);

            /* Return Provider Value */
            if (Status == ERROR_SUCCESS) return Status;

            /* If everything seemed fine, then the WSP call failed itself */
            if (ErrorCode == NO_ERROR) ErrorCode = WSASYSCALLFAILURE;
        }
        else
        {
            /* No Socket Context Found */
            ErrorCode = WSAENOTSOCK;
        }
    }

    /* Return with an Error */
    SetLastError(ErrorCode);
    return SOCKET_ERROR;
}
