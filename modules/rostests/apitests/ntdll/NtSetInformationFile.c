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
