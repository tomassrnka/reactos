/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtSetInformationFile
 * COPYRIGHT:   Copyright 2019 Thomas Faber (thomas.faber@reactos.org)
 *              Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#ifndef FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
#define FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 0x1
#define FILE_SKIP_SET_EVENT_ON_HANDLE        0x2
#endif
#ifndef FILE_SKIP_SET_USER_EVENT_ON_FAST_IO
#define FILE_SKIP_SET_USER_EVENT_ON_FAST_IO  0x4
#endif

#define KEY 0x77
#define NOTIFICATION_CLASS FileIoCompletionNotificationInformation
#define ALL_SKIP_FLAGS (FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE | \
                        FILE_SKIP_SET_USER_EVENT_ON_FAST_IO)

typedef BOOL (WINAPI *PFN_SetFileCompletionNotificationModes)(HANDLE, UCHAR);

static WCHAR TempFile[MAX_PATH];
static ULONG PipeCounter;

/* Each request gets its own OVERLAPPED: a completion that comes late never lands in a reused one */
static OVERLAPPED OverlappedPool[128];
static ULONG OverlappedNext;

static
LPOVERLAPPED
NewOverlapped(
    BOOL WithEvent)
{
    LPOVERLAPPED Ov;

    if (OverlappedNext >= RTL_NUMBER_OF(OverlappedPool))
    {
        ok(FALSE, "out of OVERLAPPED structures\n");
        OverlappedNext = 0;
    }
    Ov = &OverlappedPool[OverlappedNext++];
    RtlZeroMemory(Ov, sizeof(*Ov));
    if (WithEvent) Ov->hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    return Ov;
}

static
NTSTATUS
SetFlags(
    HANDLE Handle,
    ULONG Flags,
    PIO_STATUS_BLOCK Iosb)
{
    FILE_IO_COMPLETION_NOTIFICATION_INFORMATION Info;
    IO_STATUS_BLOCK LocalIosb;

    Info.Flags = Flags;
    if (!Iosb) Iosb = &LocalIosb;
    return NtSetInformationFile(Handle, Iosb, &Info, sizeof(Info), NOTIFICATION_CLASS);
}

static
NTSTATUS
QueryFlags(
    HANDLE Handle,
    PULONG Flags,
    PULONG_PTR Information)
{
    FILE_IO_COMPLETION_NOTIFICATION_INFORMATION Info;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    Info.Flags = 0xdeadbeef;
    Iosb.Status = 0x12345678;
    Iosb.Information = 0xdead;
    Status = NtQueryInformationFile(Handle, &Iosb, &Info, sizeof(Info), NOTIFICATION_CLASS);
    *Flags = Info.Flags;
    if (Information) *Information = Iosb.Information;
    return Status;
}

static
BOOL
IsSignaled(
    HANDLE Handle)
{
    return WaitForSingleObject(Handle, 0) == WAIT_OBJECT_0;
}

/* Returns TRUE and the packet if one arrives within Timeout */
static
BOOL
GetPacket(
    HANDLE Port,
    DWORD Timeout,
    ULONG_PTR *Key,
    LPOVERLAPPED *Overlapped,
    DWORD *Bytes)
{
    *Key = 0xdeadbeef;
    *Overlapped = (LPOVERLAPPED)(ULONG_PTR)0xdeadbeef;
    *Bytes = 0xdeadbeef;
    return GetQueuedCompletionStatus(Port, Bytes, Key, Overlapped, Timeout) || *Overlapped != NULL;
}

static
BOOL
WaitIoDone(
    LPOVERLAPPED Overlapped)
{
    DWORD i;
    for (i = 0; i < 500 && Overlapped->Internal == STATUS_PENDING; i++) Sleep(10);
    return Overlapped->Internal != STATUS_PENDING;
}

static
HANDLE
CreatePipePair(
    HANDLE *Client,
    DWORD Type)
{
    WCHAR Name[64];
    HANDLE Server;
    LPOVERLAPPED Ov;
    BOOL Ret;
    DWORD ReadMode, ConnectError;

    StringCbPrintfW(Name, sizeof(Name), L"\\\\.\\pipe\\kio_ionotify_%lu_%lu",
                    GetCurrentProcessId(), ++PipeCounter);
    ReadMode = (Type == PIPE_TYPE_MESSAGE) ? PIPE_READMODE_MESSAGE : PIPE_READMODE_BYTE;
    Server = CreateNamedPipeW(Name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                              Type | ReadMode | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    ok(Server != INVALID_HANDLE_VALUE, "CreateNamedPipeW failed, error %lu\n", GetLastError());
    if (Server == INVALID_HANDLE_VALUE) return NULL;
    Ov = NewOverlapped(TRUE);
    Ret = ConnectNamedPipe(Server, Ov);
    ConnectError = GetLastError();
    ok(!Ret && (ConnectError == ERROR_IO_PENDING || ConnectError == ERROR_PIPE_CONNECTED),
       "ConnectNamedPipe returned %d, error %lu\n", Ret, ConnectError);
    *Client = CreateFileW(Name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                          FILE_FLAG_OVERLAPPED, NULL);
    ok(*Client != INVALID_HANDLE_VALUE, "CreateFileW failed, error %lu\n", GetLastError());
    if (ConnectError == ERROR_IO_PENDING)
        ok(WaitForSingleObject(Ov->hEvent, 5000) == WAIT_OBJECT_0, "connect did not finish\n");
    CloseHandle(Ov->hEvent);
    if (*Client == INVALID_HANDLE_VALUE)
    {
        CloseHandle(Server);
        return NULL;
    }
    return Server;
}

static
void
ClientWrite(
    HANDLE Client,
    const void *Data,
    DWORD Length)
{
    LPOVERLAPPED Ov;
    DWORD Bytes = 0;
    BOOL Ret;

    Ov = NewOverlapped(TRUE);
    Ret = WriteFile(Client, Data, Length, &Bytes, Ov);
    if (!Ret && GetLastError() == ERROR_IO_PENDING)
    {
        Ret = (WaitForSingleObject(Ov->hEvent, 5000) == WAIT_OBJECT_0) &&
              GetOverlappedResult(Client, Ov, &Bytes, FALSE);
    }
    ok(Ret, "client WriteFile failed, error %lu\n", GetLastError());
    ok(Bytes == Length, "client wrote %lu bytes\n", Bytes);
    CloseHandle(Ov->hEvent);
}

static
void
Test_InfoClass(void)
{
    HANDLE File;
    NTSTATUS Status;
    IO_STATUS_BLOCK Iosb;
    FILE_IO_COMPLETION_NOTIFICATION_INFORMATION Info;
    ULONG Flags;
    ULONG_PTR Information;
    UNICODE_STRING NtName;
    OBJECT_ATTRIBUTES Attributes;

    /* A synchronous handle */
    File = CreateFileW(TempFile, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_FLAG_DELETE_ON_CLOSE, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed, error %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE) return;

    Info.Flags = FILE_SKIP_COMPLETION_PORT_ON_SUCCESS;
    Status = NtSetInformationFile(File, &Iosb, &Info, sizeof(Info) - 1, NOTIFICATION_CLASS);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    Status = NtQueryInformationFile(File, &Iosb, &Info, sizeof(Info) - 1, NOTIFICATION_CLASS);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    Status = SetFlags(File, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS, NULL);
    ok_hex(Status, STATUS_INVALID_PARAMETER);
    Status = SetFlags(File, 0, NULL);
    ok_hex(Status, STATUS_INVALID_PARAMETER);
    Status = QueryFlags(File, &Flags, &Information);
    ok_hex(Status, STATUS_SUCCESS);
    ok_hex(Flags, 0);
    ok(Information == sizeof(Info), "Information = %Iu\n", Information);
    CloseHandle(File);

    /* An asynchronous handle: the flags only accumulate */
    File = CreateFileW(TempFile, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed, error %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE) return;

    Status = QueryFlags(File, &Flags, &Information);
    ok_hex(Status, STATUS_SUCCESS);
    ok_hex(Flags, 0);
    ok(Information == sizeof(Info), "Information = %Iu\n", Information);

    Iosb.Status = 0x12345678;
    Iosb.Information = 0xdead;
    Status = SetFlags(File, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS, &Iosb);
    ok_hex(Status, STATUS_SUCCESS);
    ok_hex(Iosb.Status, STATUS_SUCCESS);
    ok(Iosb.Information == 0, "Information = %Iu\n", Iosb.Information);
    Status = QueryFlags(File, &Flags, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    ok_hex(Flags, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);

    Status = SetFlags(File, FILE_SKIP_SET_EVENT_ON_HANDLE, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    Status = QueryFlags(File, &Flags, NULL);
    ok_hex(Flags, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE);

    Status = SetFlags(File, 0, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    Status = QueryFlags(File, &Flags, NULL);
    ok_hex(Flags, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE);

    Status = SetFlags(File, FILE_SKIP_SET_USER_EVENT_ON_FAST_IO, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    Status = QueryFlags(File, &Flags, NULL);
    ok_hex(Flags, ALL_SKIP_FLAGS);

    Status = SetFlags(File, 0xfffffff8, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    Status = QueryFlags(File, &Flags, NULL);
    ok_hex(Flags, ALL_SKIP_FLAGS);

    Status = NtQueryInformationFile(File, &Iosb, &Info, sizeof(Info) - 1, NOTIFICATION_CLASS);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    Status = NtQueryInformationFile(File, &Iosb, &Info, sizeof(Info) + 4, NOTIFICATION_CLASS);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Iosb.Information == sizeof(Info), "Information = %Iu\n", Iosb.Information);
    CloseHandle(File);

    /* Access needed: a handle with nothing but FILE_READ_ATTRIBUTES */
    File = CreateFileW(TempFile, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       CREATE_ALWAYS, 0, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed, error %lu\n", GetLastError());
    if (File != INVALID_HANDLE_VALUE)
    {
        HANDLE Weak;
        if (RtlDosPathNameToNtPathName_U(TempFile, &NtName, NULL, NULL))
        {
            InitializeObjectAttributes(&Attributes, &NtName, OBJ_CASE_INSENSITIVE, NULL, NULL);
            Status = NtCreateFile(&Weak, FILE_READ_ATTRIBUTES, &Attributes, &Iosb, NULL, 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                  0, NULL, 0);
            ok_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
            {
                Status = QueryFlags(Weak, &Flags, NULL);
                ok_hex(Status, STATUS_SUCCESS);
                ok_hex(Flags, 0);
                Status = SetFlags(Weak, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS, NULL);
                ok_hex(Status, STATUS_SUCCESS);
                NtClose(Weak);
            }
            Status = NtCreateFile(&Weak, SYNCHRONIZE, &Attributes, &Iosb, NULL, 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                  0, NULL, 0);
            ok_hex(Status, STATUS_SUCCESS);
            if (NT_SUCCESS(Status))
            {
                Status = QueryFlags(Weak, &Flags, NULL);
                ok_hex(Status, STATUS_ACCESS_DENIED);
                Status = SetFlags(Weak, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS, NULL);
                ok_hex(Status, STATUS_SUCCESS);
                NtClose(Weak);
            }
            RtlFreeUnicodeString(&NtName);
        }
        CloseHandle(File);
        DeleteFileW(TempFile);
    }
}

static
void
Test_SetSignalsFile(void)
{
    HANDLE Server, Client;
    LPOVERLAPPED Ov;
    static BYTE Buffer[16];
    DWORD Bytes;
    BOOL Ret;
    NTSTATUS Status;

    /* A pending read clears the file object event; does the set request signal it? */
    Server = CreatePipePair(&Client, PIPE_TYPE_BYTE);
    if (!Server) return;
    Ov = NewOverlapped(FALSE);
    Ret = ReadFile(Server, Buffer, sizeof(Buffer), &Bytes, Ov);
    ok(!Ret && GetLastError() == ERROR_IO_PENDING, "ReadFile returned %d, error %lu\n",
       Ret, GetLastError());
    ok(!IsSignaled(Server), "server signaled after pending read\n");
    Status = SetFlags(Server, 0, NULL);
    ok_hex(Status, STATUS_SUCCESS);
    ok(IsSignaled(Server), "server not signaled after the set request\n");
    CancelIo(Server);
    WaitIoDone(Ov);
    CloseHandle(Client);
    CloseHandle(Server);
}

static
void
Test_SkipPort(
    BOOL Skip)
{
    HANDLE Server, Client, Port;
    LPOVERLAPPED Ov, Pov;
    static BYTE Buffer[16];
    DWORD Bytes;
    ULONG_PTR Key;
    BOOL Ret;
    NTSTATUS Status;

    Server = CreatePipePair(&Client, PIPE_TYPE_BYTE);
    if (!Server) return;
    Port = CreateIoCompletionPort(Server, NULL, KEY, 0);
    ok(Port != NULL, "CreateIoCompletionPort failed, error %lu\n", GetLastError());
    if (Skip)
    {
        Status = SetFlags(Server, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS, NULL);
        ok_hex(Status, STATUS_SUCCESS);
    }

    /* Data is there: the read completes at once */
    ClientWrite(Client, "abcd", 4);
    Ov = NewOverlapped(TRUE);
    Ret = ReadFile(Server, Buffer, sizeof(Buffer), &Bytes, Ov);
    ok(Ret, "ReadFile failed, error %lu\n", GetLastError());
    ok(Bytes == 4, "Bytes = %lu\n", Bytes);
    ok(IsSignaled(Ov->hEvent), "user event not signaled\n");
    Ret = GetPacket(Port, 200, &Key, &Pov, &Bytes);
    if (Skip)
    {
        ok(!Ret, "got a packet for a synchronous success, key %Ix\n", Key);
        ok(Pov == NULL, "Pov = %p\n", Pov);
    }
    else
    {
        ok(Ret, "no packet for a synchronous success\n");
        ok(Key == KEY, "Key = %Ix\n", Key);
        ok(Pov == Ov, "Pov = %p, expected %p\n", Pov, Ov);
        ok(Bytes == 4, "Bytes = %lu\n", Bytes);
    }

    /* Nothing there: the read pends and its completion is always queued */
    CloseHandle(Ov->hEvent);
    Ov = NewOverlapped(TRUE);
    Ret = ReadFile(Server, Buffer, sizeof(Buffer), &Bytes, Ov);
    ok(!Ret && GetLastError() == ERROR_IO_PENDING, "ReadFile returned %d, error %lu\n",
       Ret, GetLastError());
    ClientWrite(Client, "xyz", 3);
    Ret = GetPacket(Port, 2000, &Key, &Pov, &Bytes);
    ok(Ret, "no packet for a pending read\n");
    ok(Key == KEY, "Key = %Ix\n", Key);
    ok(Pov == Ov, "Pov = %p, expected %p\n", Pov, Ov);
    ok(Bytes == 3, "Bytes = %lu\n", Bytes);

    /* A request that fails at once queues nothing */
    CloseHandle(Client);
    CloseHandle(Ov->hEvent);
    Ov = NewOverlapped(TRUE);
    Ret = ReadFile(Server, Buffer, sizeof(Buffer), &Bytes, Ov);
    ok(!Ret && GetLastError() == ERROR_BROKEN_PIPE, "ReadFile returned %d, error %lu\n",
       Ret, GetLastError());
    Ret = GetPacket(Port, 200, &Key, &Pov, &Bytes);
    ok(!Ret, "got a packet for an immediate failure\n");

    CloseHandle(Ov->hEvent);
    CloseHandle(Server);
    CloseHandle(Port);
}

static
void
Test_SkipPortWarning(
    BOOL Skip)
{
    HANDLE Server, Client, Port;
    LPOVERLAPPED Ov, Pov;
    static BYTE Buffer[16];
    DWORD Bytes;
    ULONG_PTR Key;
    BOOL Ret;
    DWORD Error;

    /* A message longer than the buffer completes at once with STATUS_BUFFER_OVERFLOW */
    Server = CreatePipePair(&Client, PIPE_TYPE_MESSAGE);
    if (!Server) return;
    Port = CreateIoCompletionPort(Server, NULL, KEY, 0);
    if (Skip) ok_hex(SetFlags(Server, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS, NULL), STATUS_SUCCESS);
    ClientWrite(Client, "0123456789", 10);
    Ov = NewOverlapped(TRUE);
    Ret = ReadFile(Server, Buffer, 4, &Bytes, Ov);
    Error = GetLastError();
    ok(!Ret && (Error == ERROR_MORE_DATA || broken(Error == ERROR_IO_PENDING) /* Windows 10 */),
       "ReadFile returned %d, error %lu\n", Ret, Error);
    Ret = GetPacket(Port, 200, &Key, &Pov, &Bytes);
    if (Error != ERROR_MORE_DATA)
        ok(Ret, "no packet for a pending read\n");
    else if (Skip)
        ok(!Ret, "got a packet for a synchronous warning\n");
    else
        ok(Ret && Key == KEY && Pov == Ov && Bytes == 4, "packet %d key %Ix ov %p bytes %lu\n",
           Ret, Key, Pov, Bytes);
    CloseHandle(Ov->hEvent);
    CloseHandle(Client);
    CloseHandle(Server);
    CloseHandle(Port);
}

/* Byte-range locks complete at once, through the file system's fast I/O path where it has one */
static
NTSTATUS
LockRange(
    HANDLE File,
    LPOVERLAPPED Ov,
    ULONG Offset)
{
    LARGE_INTEGER ByteOffset, Length;

    ByteOffset.QuadPart = Offset;
    Length.QuadPart = 10;
    /* kernel32's LockFileEx passes no APC context, so call the system service directly */
    return NtLockFile(File, NULL, NULL, Ov, (PIO_STATUS_BLOCK)Ov, &ByteOffset, &Length, 0,
                      TRUE, TRUE);
}

static
void
Test_SkipPortLock(
    BOOL Skip)
{
    HANDLE File, Other, Port;
    LPOVERLAPPED Ov, Pov;
    DWORD Bytes;
    ULONG_PTR Key;
    NTSTATUS Status;
    BOOL Ret;

    File = CreateFileW(TempFile, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed, error %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE) return;
    Port = CreateIoCompletionPort(File, NULL, KEY, 0);
    ok(Port != NULL, "CreateIoCompletionPort failed, error %lu\n", GetLastError());
    if (Skip) ok_hex(SetFlags(File, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS, NULL), STATUS_SUCCESS);

    Ov = NewOverlapped(FALSE);
    Status = LockRange(File, Ov, 0);
    ok_hex(Status, STATUS_SUCCESS);
    Ret = GetPacket(Port, 200, &Key, &Pov, &Bytes);
    if (Skip)
        ok(!Ret, "got a packet for an immediate lock\n");
    else
        ok(Ret && Key == KEY && Pov == Ov, "packet %d key %Ix ov %p\n", Ret, Key, Pov);

    /* A range another handle holds: the lock fails at once; nothing is queued with the flag */
    Other = CreateFileW(TempFile, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    ok(Other != INVALID_HANDLE_VALUE, "CreateFileW failed, error %lu\n", GetLastError());
    if (Other != INVALID_HANDLE_VALUE)
    {
        ok(LockFile(Other, 20, 0, 10, 0), "LockFile failed, error %lu\n", GetLastError());
        Ov = NewOverlapped(FALSE);
        Status = LockRange(File, Ov, 20);
        ok(NT_ERROR(Status), "lock of a held range returned 0x%lx\n", Status);
        Ret = GetPacket(Port, 200, &Key, &Pov, &Bytes);
        if (Skip)
            ok(!Ret, "got a packet for an immediate lock failure\n");
        else
            trace("immediate lock failure without the flag: packet %d\n", Ret);
        CloseHandle(Other);
    }

    CloseHandle(Port);
    CloseHandle(File);
    DeleteFileW(TempFile);
}

static
void
Test_SkipSetEvent(
    BOOL Skip)
{
    HANDLE Server, Client;
    LPOVERLAPPED Ov;
    static BYTE Buffer[16];
    DWORD Bytes;
    BOOL Ret;

    Server = CreatePipePair(&Client, PIPE_TYPE_BYTE);
    if (!Server) return;
    if (Skip) ok_hex(SetFlags(Server, FILE_SKIP_SET_EVENT_ON_HANDLE, NULL), STATUS_SUCCESS);

    /* Pending read without an event: the file handle is the only thing to wait on */
    Ov = NewOverlapped(FALSE);
    Ret = ReadFile(Server, Buffer, sizeof(Buffer), &Bytes, Ov);
    ok(!Ret && GetLastError() == ERROR_IO_PENDING, "ReadFile returned %d, error %lu\n",
       Ret, GetLastError());
    ok(!IsSignaled(Server), "server signaled while the read is pending\n");
    ClientWrite(Client, "abcd", 4);
    ok(WaitIoDone(Ov), "read did not complete\n");
    ok(Ov->InternalHigh == 4, "InternalHigh = %Iu\n", Ov->InternalHigh);
    if (Skip)
        ok(!IsSignaled(Server), "server signaled with FILE_SKIP_SET_EVENT_ON_HANDLE\n");
    else
        ok(IsSignaled(Server), "server not signaled\n");

    /* An explicit event is still signaled */
    ClientWrite(Client, "efgh", 4);
    Ov = NewOverlapped(TRUE);
    Ret = ReadFile(Server, Buffer, sizeof(Buffer), &Bytes, Ov);
    ok(Ret, "ReadFile failed, error %lu\n", GetLastError());
    ok(IsSignaled(Ov->hEvent), "user event not signaled\n");
    CloseHandle(Ov->hEvent);

    /* Synchronous success without an event */
    ClientWrite(Client, "ijkl", 4);
    Ov = NewOverlapped(FALSE);
    Ret = ReadFile(Server, Buffer, sizeof(Buffer), &Bytes, Ov);
    ok(Ret, "ReadFile failed, error %lu\n", GetLastError());
    if (Skip)
        ok(!IsSignaled(Server), "server signaled after a synchronous success with the skip flag\n");
    else
        ok(IsSignaled(Server), "server not signaled after a synchronous success\n");

    CloseHandle(Client);
    CloseHandle(Server);
}

static
void
Test_Kernel32(void)
{
    PFN_SetFileCompletionNotificationModes pSetModes;
    HANDLE File;
    BOOL Ret;
    ULONG Flags;

    pSetModes = (PFN_SetFileCompletionNotificationModes)
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetFileCompletionNotificationModes");
    if (!pSetModes)
    {
        skip("SetFileCompletionNotificationModes not exported\n");
        return;
    }
    File = CreateFileW(TempFile, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed, error %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE) return;
    SetLastError(0xdeadbeef);
    Ret = pSetModes(File, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE);
    ok(Ret, "SetFileCompletionNotificationModes failed, error %lu\n", GetLastError());
    ok(QueryFlags(File, &Flags, NULL) == STATUS_SUCCESS, "query failed\n");
    ok_hex(Flags, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE);
    SetLastError(0xdeadbeef);
    Ret = pSetModes(File, FILE_SKIP_SET_USER_EVENT_ON_FAST_IO);
    ok(!Ret, "SetFileCompletionNotificationModes accepted FILE_SKIP_SET_USER_EVENT_ON_FAST_IO\n");
    ok_err(ERROR_INVALID_PARAMETER);
    CloseHandle(File);

    File = CreateFileW(TempFile, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (File == INVALID_HANDLE_VALUE) return;
    SetLastError(0xdeadbeef);
    Ret = pSetModes(File, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
    ok(!Ret, "SetFileCompletionNotificationModes succeeded on a synchronous handle\n");
    ok_err(ERROR_INVALID_PARAMETER);
    CloseHandle(File);
}

/* ---- file information classes whose 32-bit layout differs from the 64-bit one, and the plain ones ---- */

#define PATTERN 0x55

#ifndef FILE_DISPOSITION_DELETE
#define FILE_DISPOSITION_DELETE 0x00000001
#endif

typedef struct
{
    ULONG Flags;
    HANDLE Root;
    ULONG Length;
    WCHAR Name[MAX_PATH + 8];
} RENAME_BUFFER;

static
ULONG_PTR
PatternValue(void)
{
    ULONG_PTR Value;
    RtlFillMemory(&Value, sizeof(Value), PATTERN);
    return Value;
}

static
void
ResetIosb(
    PIO_STATUS_BLOCK Iosb)
{
    RtlFillMemory(Iosb, sizeof(*Iosb), PATTERN);
}

/* a request refused before it started leaves the guest's block alone */
static
void
CheckIosbUntouched(
    PIO_STATUS_BLOCK Iosb,
    const char *What)
{
    ok(Iosb->Status == 0x55555555 && Iosb->Information == PatternValue(),
       "%s: IoStatusBlock changed to (0x%lx, %Iu)\n", What, Iosb->Status, Iosb->Information);
}

static
void
CheckIosbDone(
    PIO_STATUS_BLOCK Iosb,
    const char *What)
{
    ok(Iosb->Status == STATUS_SUCCESS, "%s: IoStatusBlock.Status = 0x%lx\n", What, Iosb->Status);
    ok(Iosb->Information == 0, "%s: IoStatusBlock.Information = %Iu\n", What, Iosb->Information);
}

static
NTSTATUS
SetInfo(
    HANDLE Handle,
    ULONG Class,
    PVOID Buffer,
    ULONG Length,
    PIO_STATUS_BLOCK Iosb)
{
    ResetIosb(Iosb);
    return NtSetInformationFile(Handle, Iosb, Buffer, Length, Class);
}

static
NTSTATUS
QueryStandard(
    HANDLE Handle,
    LONGLONG *Allocation,
    LONGLONG *EndOfFile,
    ULONG *Links,
    UCHAR *DeletePending,
    UCHAR *Directory)
{
    /* the raw layout: two LARGE_INTEGERs, ULONG, two BOOLEANs, in 24 bytes */
    UCHAR Buffer[24];
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    RtlFillMemory(Buffer, sizeof(Buffer), PATTERN);
    Status = NtQueryInformationFile(Handle, &Iosb, Buffer, sizeof(Buffer), FileStandardInformation);
    if (!NT_SUCCESS(Status)) return Status;
    RtlCopyMemory(Allocation, Buffer + 0, sizeof(LONGLONG));
    RtlCopyMemory(EndOfFile, Buffer + 8, sizeof(LONGLONG));
    RtlCopyMemory(Links, Buffer + 16, sizeof(ULONG));
    *DeletePending = Buffer[20];
    *Directory = Buffer[21];
    return Status;
}

static
void
CheckStandard(
    HANDLE Handle,
    LONGLONG EndOfFile,
    ULONG Links,
    UCHAR DeletePending,
    const char *What)
{
    LONGLONG Allocation, Eof;
    ULONG NumberOfLinks;
    UCHAR Pending, Directory;
    NTSTATUS Status;

    Status = QueryStandard(Handle, &Allocation, &Eof, &NumberOfLinks, &Pending, &Directory);
    ok_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    ok(Eof == EndOfFile, "%s: EndOfFile = %I64d, expected %I64d\n", What, Eof, EndOfFile);
    ok(Allocation >= Eof, "%s: AllocationSize = %I64d\n", What, Allocation);
    if (Links != MAXULONG) ok(NumberOfLinks == Links, "%s: NumberOfLinks = %lu, expected %lu\n", What, NumberOfLinks, Links);
    ok(Pending == DeletePending, "%s: DeletePending = %u\n", What, Pending);
    ok(!Directory, "%s: Directory = %u\n", What, Directory);
}

static
HANDLE
OpenTemp(
    PCWSTR Path,
    ULONG Flags)
{
    static char Data[5000];
    HANDLE File;
    DWORD Written;

    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE | DELETE | FILE_WRITE_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       CREATE_ALWAYS, Flags, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed, error %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE) return NULL;
    if (!(Flags & FILE_FLAG_OVERLAPPED))
    {
        RtlFillMemory(Data, sizeof(Data), 'A');
        ok(WriteFile(File, Data, sizeof(Data), &Written, NULL), "WriteFile failed, error %lu\n", GetLastError());
    }
    return File;
}

static
void
Test_FixedSizeClasses(void)
{
    HANDLE File;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;
    UCHAR Buffer[64];
    LARGE_INTEGER Value;
    BOOLEAN Delete;
    ULONG Flags;

    File = OpenTemp(TempFile, 0);
    if (!File) return;

    /* FileBasicInformation: 40 bytes, the times are LARGE_INTEGERs; zero fields change nothing */
    RtlZeroMemory(Buffer, sizeof(Buffer));
    Value.QuadPart = 0x01d0000000000000LL;
    RtlCopyMemory(Buffer + 16, &Value, sizeof(Value)); /* LastWriteTime */
    Status = SetInfo(File, FileBasicInformation, Buffer, 36, &Iosb);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    CheckIosbUntouched(&Iosb, "basic, 36 bytes");
    Status = SetInfo(File, FileBasicInformation, Buffer, 0, &Iosb);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    CheckIosbUntouched(&Iosb, "basic, 0 bytes");
    Status = SetInfo(File, FileBasicInformation, Buffer, 40, &Iosb);
    ok_hex(Status, STATUS_SUCCESS);
    CheckIosbDone(&Iosb, "basic");
    RtlFillMemory(Buffer, sizeof(Buffer), PATTERN);
    Status = NtQueryInformationFile(File, &Iosb, Buffer, 40, FileBasicInformation);
    ok_hex(Status, STATUS_SUCCESS);
    RtlCopyMemory(&Value, Buffer + 16, sizeof(Value));
    ok(Value.QuadPart == 0x01d0000000000000LL, "LastWriteTime = 0x%I64x\n", Value.QuadPart);

    /* FilePositionInformation: one LARGE_INTEGER */
    Value.QuadPart = 100;
    Status = SetInfo(File, FilePositionInformation, &Value, 4, &Iosb);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    CheckIosbUntouched(&Iosb, "position, 4 bytes");
    Status = SetInfo(File, FilePositionInformation, &Value, sizeof(Value), &Iosb);
    ok_hex(Status, STATUS_SUCCESS);
    CheckIosbDone(&Iosb, "position");
    Value.QuadPart = 0;
    Status = NtQueryInformationFile(File, &Iosb, &Value, sizeof(Value), FilePositionInformation);
    ok_hex(Status, STATUS_SUCCESS);
    ok(Value.QuadPart == 100, "CurrentByteOffset = %I64d\n", Value.QuadPart);

    /* end of file and allocation */
    Value.QuadPart = 1234;
    Status = SetInfo(File, FileEndOfFileInformation, &Value, 4, &Iosb);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    CheckIosbUntouched(&Iosb, "end of file, 4 bytes");
    Status = SetInfo(File, FileEndOfFileInformation, &Value, sizeof(Value), &Iosb);
    ok_hex(Status, STATUS_SUCCESS);
    CheckIosbDone(&Iosb, "end of file");
    CheckStandard(File, 1234, 1, 0, "after FileEndOfFileInformation");
    Value.QuadPart = 8192;
    Status = SetInfo(File, FileAllocationInformation, &Value, 4, &Iosb);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    Status = SetInfo(File, FileAllocationInformation, &Value, sizeof(Value), &Iosb);
    ok_hex(Status, STATUS_SUCCESS);
    CheckIosbDone(&Iosb, "allocation");
    CheckStandard(File, 1234, 1, 0, "after FileAllocationInformation");

    /* FileDispositionInformation: a BOOLEAN, so even a ULONG is accepted */
    Delete = TRUE;
    Status = SetInfo(File, FileDispositionInformation, &Delete, 0, &Iosb);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    CheckIosbUntouched(&Iosb, "disposition, 0 bytes");
    Status = SetInfo(File, FileDispositionInformation, &Delete, sizeof(Delete), &Iosb);
    ok_hex(Status, STATUS_SUCCESS);
    CheckIosbDone(&Iosb, "disposition");
    CheckStandard(File, 1234, MAXULONG, 1, "delete pending");
    Delete = FALSE;
    Status = SetInfo(File, FileDispositionInformation, &Delete, sizeof(Delete), &Iosb);
    ok_hex(Status, STATUS_SUCCESS);
    CheckStandard(File, 1234, 1, 0, "delete cancelled");

    /* FileModeInformation can only be set to what the handle has... and the pipe/other classes are not for files */
    Flags = FILE_SYNCHRONOUS_IO_NONALERT;
    Status = SetInfo(File, FileModeInformation, &Flags, 3, &Iosb);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    CheckIosbUntouched(&Iosb, "mode, 3 bytes");
    Status = SetInfo(File, FilePipeInformation, &Flags, sizeof(Flags), &Iosb);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    /* FileShortNameInformation is a FILE_NAME_INFORMATION: 8 bytes are enough to be looked at */
    RtlZeroMemory(Buffer, sizeof(Buffer));
    Status = SetInfo(File, FileShortNameInformation, Buffer, 7, &Iosb);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    CheckIosbUntouched(&Iosb, "short name, 7 bytes");
    Status = SetInfo(File, FileShortNameInformation, Buffer, 8, &Iosb);
    ok(Status != STATUS_INFO_LENGTH_MISMATCH, "short name, 8 bytes: 0x%lx\n", Status);

    CloseHandle(File);
    DeleteFileW(TempFile);
}

static
VOID
FillRename(
    RENAME_BUFFER *Rename,
    ULONG Flags,
    HANDLE Root,
    PCWSTR Name,
    ULONG *Size)
{
    RtlZeroMemory(Rename, sizeof(*Rename));
    Rename->Flags = Flags;
    Rename->Root = Root;
    Rename->Length = (ULONG)wcslen(Name) * sizeof(WCHAR);
    RtlCopyMemory(Rename->Name, Name, Rename->Length);
    *Size = FIELD_OFFSET(RENAME_BUFFER, Name) + Rename->Length;
}

static
BOOL
Exists(
    PCWSTR Path)
{
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

static
BOOL
NtPathOf(
    PCWSTR Path,
    PWSTR NtPath)
{
    UNICODE_STRING Name;
    if (!RtlDosPathNameToNtPathName_U(Path, &Name, NULL, NULL)) return FALSE;
    RtlCopyMemory(NtPath, Name.Buffer, Name.Length);
    NtPath[Name.Length / sizeof(WCHAR)] = 0;
    RtlFreeUnicodeString(&Name);
    return TRUE;
}

static
void
Test_RenameAndLink(void)
{
    static const ULONG Classes[] = { FileRenameInformation, FileLinkInformation };
    WCHAR Dir[MAX_PATH], Source[MAX_PATH], Target[MAX_PATH], Other[MAX_PATH], NtTarget[MAX_PATH], NtOther[MAX_PATH];
    RENAME_BUFFER Rename;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;
    HANDLE File, Root, Second;
    ULONG Size, i;

    wcscpy(Dir, TempFile);
    *wcsrchr(Dir, L'\\') = 0;
    StringCbPrintfW(Source, sizeof(Source), L"%ls\\kio_src.tmp", Dir);
    StringCbPrintfW(Target, sizeof(Target), L"%ls\\kio_dst.tmp", Dir);
    StringCbPrintfW(Other, sizeof(Other), L"%ls\\kio_other.tmp", Dir);
    if (!NtPathOf(Target, NtTarget) || !NtPathOf(Other, NtOther))
    {
        ok(FALSE, "no NT path\n");
        return;
    }
    DeleteFileW(Source); DeleteFileW(Target); DeleteFileW(Other);

    for (i = 0; i < RTL_NUMBER_OF(Classes); i++)
    {
        ULONG Class = Classes[i];
        BOOL Link = Class == FileLinkInformation;

        File = OpenTemp(Source, 0);
        if (!File) return;

        /* size and name checks come before anything is done */
        FillRename(&Rename, 0, NULL, NtTarget, &Size);
        Status = SetInfo(File, Class, &Rename, FIELD_OFFSET(RENAME_BUFFER, Name) - 1, &Iosb);
        ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
        CheckIosbUntouched(&Iosb, "name header too short");
        Status = SetInfo(File, Class, &Rename, FIELD_OFFSET(RENAME_BUFFER, Name), &Iosb);
        ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
        CheckIosbUntouched(&Iosb, "no room for the name");
        Rename.Length += 1;
        Status = SetInfo(File, Class, &Rename, Size, &Iosb);
        ok_hex(Status, STATUS_INVALID_PARAMETER);
        CheckIosbUntouched(&Iosb, "odd name length");
        FillRename(&Rename, 0, NULL, NtTarget, &Size);
        Rename.Length += 2 * sizeof(WCHAR);
        Status = SetInfo(File, Class, &Rename, Size, &Iosb);
        ok_hex(Status, STATUS_INVALID_PARAMETER);
        CheckIosbUntouched(&Iosb, "name longer than the buffer");
        FillRename(&Rename, 0, (HANDLE)(ULONG_PTR)0x7777, L"kio_x.tmp", &Size);
        Status = SetInfo(File, Class, &Rename, Size, &Iosb);
        ok_hex(Status, STATUS_INVALID_HANDLE);
        CheckIosbUntouched(&Iosb, "bad root handle");

        /* an NT path target */
        FillRename(&Rename, 0, NULL, NtTarget, &Size);
        Status = SetInfo(File, Class, &Rename, Size, &Iosb);
        ok_hex(Status, STATUS_SUCCESS);
        CheckIosbDone(&Iosb, Link ? "link" : "rename");
        ok(Exists(Target), "%s: target does not exist\n", Link ? "link" : "rename");
        ok(Exists(Source) == Link, "%s: source exists: %d\n", Link ? "link" : "rename", Exists(Source));
        if (Link) CheckStandard(File, 5000, 2, 0, "after link");

        /* a target that exists (and is not open): refused, unless replacing */
        Second = OpenTemp(Other, 0);
        if (Second) CloseHandle(Second);
        FillRename(&Rename, 0, NULL, NtOther, &Size);
        Status = SetInfo(File, Class, &Rename, Size, &Iosb);
        ok_hex(Status, STATUS_OBJECT_NAME_COLLISION);
        CheckIosbUntouched(&Iosb, "collision");
        FillRename(&Rename, 1, NULL, NtOther, &Size);
        Status = SetInfo(File, Class, &Rename, Size, &Iosb);
        ok_hex(Status, STATUS_SUCCESS);
        CheckIosbDone(&Iosb, "replace");
        if (Link) CheckStandard(File, 5000, 3, 0, "after the second link");

        /* a target relative to a directory handle */
        Root = CreateFileW(Dir, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        ok(Root != INVALID_HANDLE_VALUE, "opening the directory failed, error %lu\n", GetLastError());
        if (Root != INVALID_HANDLE_VALUE)
        {
            FillRename(&Rename, 1, Root, L"kio_rel.tmp", &Size);
            Status = SetInfo(File, Class, &Rename, Size, &Iosb);
            ok_hex(Status, STATUS_SUCCESS);
            CheckIosbDone(&Iosb, "relative to a root handle");
            CloseHandle(Root);
        }
        CloseHandle(File);
        DeleteFileW(Source); DeleteFileW(Target); DeleteFileW(Other);
        StringCbPrintfW(Source, sizeof(Source), L"%ls\\kio_rel.tmp", Dir); DeleteFileW(Source);
        StringCbPrintfW(Source, sizeof(Source), L"%ls\\kio_src.tmp", Dir);
    }
}

static
BOOL
IsWow64(void)
{
    BOOL Wow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &Wow64);
    return Wow64;
}

static
void
Test_HandleAndPointerClasses(void)
{
    static const ULONG Bypass[] = { FileRenameInformationBypassAccessCheck, FileLinkInformationBypassAccessCheck,
                                    FileRenameInformationExBypassAccessCheck, FileLinkInformationExBypassAccessCheck };
    static const ULONG Ex[] = { FileRenameInformationEx, FileLinkInformationEx };
    static const ULONG BypassLengths[] = { 0, 16, 140 };
    WCHAR Dir[MAX_PATH], Source[MAX_PATH], Target[MAX_PATH], NtTarget[MAX_PATH], Name[128];
    RENAME_BUFFER Rename;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;
    HANDLE File, Port, Server;
    ULONG Size, i, j, Flags;
    UCHAR Buffer[64];

    /* the classes meant for kernel callers are refused whatever the length or contents */
    File = OpenTemp(TempFile, 0);
    if (!File) return;
    RtlZeroMemory(&Rename, sizeof(Rename));
    for (i = 0; i < RTL_NUMBER_OF(Bypass); i++)
        for (j = 0; j < RTL_NUMBER_OF(BypassLengths); j++)
        {
            Status = SetInfo(File, Bypass[i], &Rename, BypassLengths[j], &Iosb);
            ok(Status == STATUS_INVALID_INFO_CLASS, "class %lu, %lu bytes: 0x%lx\n", Bypass[i], BypassLengths[j], Status);
            CheckIosbUntouched(&Iosb, "bypass class");
        }

    /* The Ex variants take a ULONG of flags in place of the BOOLEAN */
    wcscpy(Dir, TempFile);
    *wcsrchr(Dir, L'\\') = 0;
    StringCbPrintfW(Source, sizeof(Source), L"%ls\\kio_exsrc.tmp", Dir);
    StringCbPrintfW(Target, sizeof(Target), L"%ls\\kio_exdst.tmp", Dir);
    NtPathOf(Target, NtTarget);
    DeleteFileW(Target);
    for (i = 0; i < RTL_NUMBER_OF(Ex); i++)
    {
        HANDLE Moved = OpenTemp(Source, 0);
        if (!Moved) break;
        FillRename(&Rename, 0, NULL, NtTarget, &Size);
        Status = SetInfo(Moved, Ex[i], &Rename, FIELD_OFFSET(RENAME_BUFFER, Name) - 1, &Iosb);
        /* the guest's record is checked before the kernel is asked, so only the 32-bit caller gets this far */
        todo_if(is_reactos() && !IsWow64()) ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
        FillRename(&Rename, 0, NULL, NtTarget, &Size);
        Status = SetInfo(Moved, Ex[i], &Rename, Size, &Iosb);
        todo_if(is_reactos()) ok_hex(Status, STATUS_SUCCESS);
        todo_if(is_reactos()) ok(Exists(Target), "class %lu: target does not exist\n", Ex[i]);
        CloseHandle(Moved);
        DeleteFileW(Source);
        DeleteFileW(Target);
    }

    /* FileDispositionInformationEx: one ULONG of flags */
    Flags = FILE_DISPOSITION_DELETE;
    Status = SetInfo(File, FileDispositionInformationEx, &Flags, sizeof(Flags), &Iosb);
    todo_if(is_reactos()) ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) CheckStandard(File, 5000, MAXULONG, 1, "FileDispositionInformationEx(DELETE)");
    Flags = 0;
    Status = SetInfo(File, FileDispositionInformationEx, &Flags, sizeof(Flags), &Iosb);
    todo_if(is_reactos()) ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status)) CheckStandard(File, 5000, 1, 0, "FileDispositionInformationEx(0)");
    Status = SetInfo(File, FileDispositionInformationEx, &Flags, 3, &Iosb);
    todo_if(is_reactos()) ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
    CloseHandle(File);
    DeleteFileW(TempFile);

    /* FileCompletionInformation: a port handle and the key, both 32 bits wide in a 32-bit caller */
    File = OpenTemp(TempFile, 0);
    if (File) CloseHandle(File);
    File = CreateFileW(TempFile, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFileW failed, error %lu\n", GetLastError());
    Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    ok(Port != NULL, "CreateIoCompletionPort failed, error %lu\n", GetLastError());
    if (File != INVALID_HANDLE_VALUE && Port)
    {
        FILE_COMPLETION_INFORMATION Completion;
        OVERLAPPED Overlapped = {0};
        ULONG_PTR Key = 0;
        LPOVERLAPPED Returned = NULL;
        DWORD Bytes = 0;
        BYTE Data[16];
        BOOL Ret;

        Completion.Port = Port;
        Completion.Key = (PVOID)(ULONG_PTR)0x80001234;
        Status = SetInfo(File, FileCompletionInformation, &Completion, sizeof(Completion) - 1, &Iosb);
        ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
        CheckIosbUntouched(&Iosb, "completion, short");
        Status = SetInfo(File, FileCompletionInformation, &Completion, sizeof(Completion), &Iosb);
        ok_hex(Status, STATUS_SUCCESS);
        CheckIosbDone(&Iosb, "completion");
        Status = SetInfo(File, FileCompletionInformation, &Completion, sizeof(Completion), &Iosb);
        ok_hex(Status, STATUS_INVALID_PARAMETER);
        Ret = ReadFile(File, Data, sizeof(Data), NULL, &Overlapped);
        ok(Ret || GetLastError() == ERROR_IO_PENDING, "ReadFile failed, error %lu\n", GetLastError());
        Ret = GetQueuedCompletionStatus(Port, &Bytes, &Key, &Returned, 5000);
        ok(Ret, "no completion packet, error %lu\n", GetLastError());
        ok(Key == (ULONG_PTR)0x80001234, "completion key 0x%Ix\n", Key);
        ok(Returned == &Overlapped, "OVERLAPPED %p, expected %p\n", Returned, &Overlapped);
        ok(Bytes == sizeof(Data), "%lu bytes\n", Bytes);
    }
    if (Port) CloseHandle(Port);
    if (File != INVALID_HANDLE_VALUE) CloseHandle(File);
    DeleteFileW(TempFile);

    /* FileMailslotSetInformation: a pointer to the read timeout */
    StringCbPrintfW(Name, sizeof(Name), L"\\\\.\\mailslot\\kio_%lu", GetCurrentProcessId());
    Server = CreateMailslotW(Name, 0, MAILSLOT_WAIT_FOREVER, NULL);
    ok(Server != INVALID_HANDLE_VALUE, "CreateMailslotW failed, error %lu\n", GetLastError());
    if (Server != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER Timeout;
        PLARGE_INTEGER Pointer = &Timeout;
        LARGE_INTEGER Read;

        Timeout.QuadPart = -50000000;
        Status = SetInfo(Server, FileMailslotSetInformation, &Pointer, 0, &Iosb);
        ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
        CheckIosbUntouched(&Iosb, "mailslot, 0 bytes");
        Status = SetInfo(Server, FileMailslotSetInformation, &Pointer, sizeof(Pointer), &Iosb);
        ok_hex(Status, STATUS_SUCCESS);
        CheckIosbDone(&Iosb, "mailslot");
        RtlFillMemory(Buffer, sizeof(Buffer), PATTERN);
        Status = NtQueryInformationFile(Server, &Iosb, Buffer, 24, FileMailslotQueryInformation);
        ok_hex(Status, STATUS_SUCCESS);
        ok(Iosb.Information == 24, "Information = %Iu\n", Iosb.Information);
        RtlCopyMemory(&Read, Buffer + 16, sizeof(Read));
        ok(Read.QuadPart == -50000000, "ReadTimeout = %I64d\n", Read.QuadPart);
        Pointer = (PLARGE_INTEGER)(ULONG_PTR)0x1000;
        Status = SetInfo(Server, FileMailslotSetInformation, &Pointer, sizeof(Pointer), &Iosb);
        ok_hex(Status, STATUS_ACCESS_VIOLATION);
        CloseHandle(Server);
    }
    File = OpenTemp(TempFile, 0);
    if (File)
    {
        PLARGE_INTEGER Pointer = NULL;
        Status = SetInfo(File, FileMailslotSetInformation, &Pointer, sizeof(Pointer), &Iosb);
        ok_hex(Status, STATUS_INVALID_PARAMETER);
        CheckIosbUntouched(&Iosb, "mailslot class on a file");

        /* FileTrackingInformation: a handle, a length, and the bytes */
        RtlZeroMemory(Buffer, sizeof(Buffer));
        Status = SetInfo(File, FileTrackingInformation, Buffer, 8, &Iosb);
        ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
        CheckIosbUntouched(&Iosb, "tracking, 8 bytes");
        Status = SetInfo(File, FileTrackingInformation, Buffer, 16, &Iosb);
        ok_hex(Status, STATUS_INVALID_PARAMETER);
        CloseHandle(File);
        DeleteFileW(TempFile);
    }

    /* FilePipeInformation on a pipe */
    StringCbPrintfW(Name, sizeof(Name), L"\\\\.\\pipe\\kio_info_%lu", GetCurrentProcessId());
    Server = CreateNamedPipeW(Name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    ok(Server != INVALID_HANDLE_VALUE, "CreateNamedPipeW failed, error %lu\n", GetLastError());
    if (Server != INVALID_HANDLE_VALUE)
    {
        ULONG Mode[2] = { FILE_PIPE_MESSAGE_MODE, FILE_PIPE_QUEUE_OPERATION };
        Status = SetInfo(Server, FilePipeInformation, Mode, 4, &Iosb);
        ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);
        CheckIosbUntouched(&Iosb, "pipe, 4 bytes");
        Status = SetInfo(Server, FilePipeInformation, Mode, sizeof(Mode), &Iosb);
        ok_hex(Status, STATUS_SUCCESS);
        CheckIosbDone(&Iosb, "pipe");
        Mode[0] = Mode[1] = 0xffffffff;
        Status = NtQueryInformationFile(Server, &Iosb, Mode, sizeof(Mode), FilePipeInformation);
        ok_hex(Status, STATUS_SUCCESS);
        ok(Mode[0] == FILE_PIPE_MESSAGE_MODE && Mode[1] == FILE_PIPE_QUEUE_OPERATION, "pipe modes %lu, %lu\n", Mode[0], Mode[1]);
        CloseHandle(Server);
    }
}

START_TEST(NtSetInformationFile)
{
    NTSTATUS Status;
    WCHAR TempPath[MAX_PATH];

    Status = NtSetInformationFile(NULL, NULL, NULL, 0, 0);
    ok(Status == STATUS_INVALID_INFO_CLASS, "Status = %lx\n", Status);

    Status = NtSetInformationFile(NULL, NULL, NULL, 0, 0x80000000);
    ok(Status == STATUS_INVALID_INFO_CLASS, "Status = %lx\n", Status);

    /* FileIoCompletionNotificationInformation */
    GetTempPathW(MAX_PATH, TempPath);
    GetTempFileNameW(TempPath, L"kio", 0, TempFile);
    Test_FixedSizeClasses();
    Test_RenameAndLink();
    Test_HandleAndPointerClasses();
    Test_InfoClass();
    Test_SetSignalsFile();
    Test_SkipPort(FALSE);
    Test_SkipPort(TRUE);
    Test_SkipPortWarning(FALSE);
    Test_SkipPortWarning(TRUE);
    Test_SkipPortLock(FALSE);
    Test_SkipPortLock(TRUE);
    Test_SkipSetEvent(FALSE);
    Test_SkipSetEvent(TRUE);
    Test_Kernel32();
    DeleteFileW(TempFile);
}
