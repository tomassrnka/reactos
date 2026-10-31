/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtFlushBuffersFile: synchronous and asynchronous handles
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#define PATTERN 0x55

typedef struct
{
    HANDLE Handle;
    NTSTATUS Status;
    IO_STATUS_BLOCK Io;
} FLUSH_PARAMS;

static
NTSTATUS
Flush(HANDLE Handle, IO_STATUS_BLOCK *Io)
{
    RtlFillMemory(Io, sizeof(*Io), PATTERN);
    return NtFlushBuffersFile(Handle, Io);
}

static
HANDLE
CreateTemp(PWSTR Path, DWORD Access, DWORD Flags)
{
    return CreateFileW(Path, Access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL | Flags, NULL);
}

static
void
TestFile(PWSTR Path, DWORD Flags, PCSTR Kind)
{
    IO_STATUS_BLOCK Io;
    NTSTATUS Status;
    HANDLE File;
    DWORD Written;
    ULONG_PTR Pattern;

    RtlFillMemory(&Pattern, sizeof(Pattern), PATTERN);

    File = CreateTemp(Path, GENERIC_READ | GENERIC_WRITE, Flags);
    ok(File != INVALID_HANDLE_VALUE, "%s: create failed, error %lu\n", Kind, GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    if (Flags & FILE_FLAG_OVERLAPPED)
    {
        OVERLAPPED Overlapped = {0};
        BOOL Ret;
        Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        Ret = WriteFile(File, "flush", 5, NULL, &Overlapped);
        ok(Ret || GetLastError() == ERROR_IO_PENDING, "%s: WriteFile failed, error %lu\n", Kind, GetLastError());
        Ret = GetOverlappedResult(File, &Overlapped, &Written, TRUE);
        ok(Ret && Written == 5, "%s: write completed %d, %lu bytes\n", Kind, Ret, Written);
        CloseHandle(Overlapped.hEvent);
    }
    else
    {
        BOOL Ret = WriteFile(File, "flush", 5, &Written, NULL);
        ok(Ret && Written == 5, "%s: write failed, error %lu\n", Kind, GetLastError());
    }

    Status = Flush(File, &Io);
    ok_hex(Status, STATUS_SUCCESS);
    ok_hex(Io.Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Io.Information, 0);
    ok(FlushFileBuffers(File), "%s: FlushFileBuffers failed, error %lu\n", Kind, GetLastError());
    CloseHandle(File);

    /* No write access: refused before the block is touched */
    File = CreateTemp(Path, GENERIC_READ, Flags);
    ok(File != INVALID_HANDLE_VALUE, "%s: open for read failed, error %lu\n", Kind, GetLastError());
    if (File != INVALID_HANDLE_VALUE)
    {
        Status = Flush(File, &Io);
        ok_hex(Status, STATUS_ACCESS_DENIED);
        ok_eq_hex((ULONG)Io.Status, PATTERN * 0x01010101);
        ok_eq_ulongptr(Io.Information, Pattern);
        CloseHandle(File);
    }
}

static
DWORD
WINAPI
FlushThread(PVOID Context)
{
    FLUSH_PARAMS *Params = Context;
    Params->Status = Flush(Params->Handle, &Params->Io);
    return 0;
}

static
void
TestPipe(void)
{
    WCHAR Name[MAX_PATH];
    OVERLAPPED Overlapped = {0};
    FLUSH_PARAMS Params;
    HANDLE Server, Client, Thread;
    DWORD Count, Wait;
    BOOL Ret;
    CHAR Buffer[4];

    StringCbPrintfW(Name, sizeof(Name), L"\\\\.\\pipe\\wow64pf_flush_%lu_%lu", GetCurrentProcessId(), GetTickCount());
    Server = CreateNamedPipeW(Name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    ok(Server != INVALID_HANDLE_VALUE, "CreateNamedPipe failed, error %lu\n", GetLastError());
    if (Server == INVALID_HANDLE_VALUE)
        return;
    Client = CreateFileW(Name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(Client != INVALID_HANDLE_VALUE, "open failed, error %lu\n", GetLastError());
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Ret = ConnectNamedPipe(Server, &Overlapped);
    ok(!Ret && GetLastError() == ERROR_PIPE_CONNECTED, "ConnectNamedPipe: %d, error %lu\n", Ret, GetLastError());

    /* Nothing waits to be read: returns at once on the asynchronous server end */
    Params.Handle = Server;
    Params.Status = 0xdeadbeef;
    Thread = CreateThread(NULL, 0, FlushThread, &Params, 0, NULL);
    Wait = WaitForSingleObject(Thread, 5000);
    ok_eq_hex(Wait, WAIT_OBJECT_0);
    ok_hex(Params.Status, STATUS_SUCCESS);
    ok_hex(Params.Io.Status, STATUS_SUCCESS);
    CloseHandle(Thread);

    /* Unread data: the flush waits for the other end to read it */
    ResetEvent(Overlapped.hEvent);
    Ret = WriteFile(Server, "x", 1, NULL, &Overlapped);
    ok(Ret || GetLastError() == ERROR_IO_PENDING, "WriteFile failed, error %lu\n", GetLastError());
    Ret = GetOverlappedResult(Server, &Overlapped, &Count, TRUE);
    ok(Ret && Count == 1, "write: %d, %lu bytes\n", Ret, Count);

    Params.Status = 0xdeadbeef;
    Thread = CreateThread(NULL, 0, FlushThread, &Params, 0, NULL);
    Wait = WaitForSingleObject(Thread, 300);
    ok_eq_hex(Wait, WAIT_TIMEOUT);
    ok_hex(Params.Status, 0xdeadbeef);

    Ret = ReadFile(Client, Buffer, 1, &Count, NULL);
    ok(Ret && Count == 1, "read: %d, %lu bytes\n", Ret, Count);
    Wait = WaitForSingleObject(Thread, 5000);
    ok_eq_hex(Wait, WAIT_OBJECT_0);
    ok_hex(Params.Status, STATUS_SUCCESS);
    ok_hex(Params.Io.Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Params.Io.Information, 0);
    CloseHandle(Thread);

    ok(FlushFileBuffers(Server), "FlushFileBuffers failed, error %lu\n", GetLastError());
    CloseHandle(Overlapped.hEvent);
    CloseHandle(Client);
    CloseHandle(Server);
}

START_TEST(NtFlushBuffersFile)
{
    IO_STATUS_BLOCK Io;
    NTSTATUS Status;
    WCHAR Path[MAX_PATH];
    ULONG Length;

    Length = GetTempPathW(RTL_NUMBER_OF(Path), Path);
    ok(Length != 0, "GetTempPath failed\n");
    StringCbPrintfW(Path + Length, sizeof(Path) - Length * sizeof(WCHAR), L"wow64pf_flush_%lu.tmp", GetCurrentProcessId());

    TestFile(Path, 0, "synchronous");
    TestFile(Path, FILE_FLAG_OVERLAPPED, "asynchronous");
    DeleteFileW(Path);

    Status = Flush((HANDLE)(ULONG_PTR)0x12344, &Io);
    ok_hex(Status, STATUS_INVALID_HANDLE);
    ok_eq_hex((ULONG)Io.Status, PATTERN * 0x01010101);

    TestPipe();
}
