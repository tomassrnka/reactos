/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the named pipe file system controls FSCTL_PIPE_IMPERSONATE and
 *              FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#define PATTERN 0x55
#define FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE_ CTL_CODE(FILE_DEVICE_NAMED_PIPE, 12, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* An asynchronous handle may answer STATUS_PENDING for a request that is complete at once (Windows does, for
 * the impersonation request): wait for the event, then take the status from the block. */
static
NTSTATUS
Control(HANDLE Pipe, ULONG Code, PVOID In, ULONG InLength, PVOID Out, ULONG OutLength, IO_STATUS_BLOCK *Io)
{
    NTSTATUS Status;
    HANDLE Event = CreateEventW(NULL, TRUE, FALSE, NULL);

    RtlFillMemory(Io, sizeof(*Io), PATTERN);
    Status = NtFsControlFile(Pipe, Event, NULL, NULL, Io, Code, In, InLength, Out, OutLength);
    if (Status == STATUS_PENDING)
    {
        ok_eq_hex(WaitForSingleObject(Event, 5000), WAIT_OBJECT_0);
        Status = Io->Status;
    }
    CloseHandle(Event);
    return Status;
}

static
void
TestImpersonation(DWORD QualityOfService, SECURITY_IMPERSONATION_LEVEL Expected, BOOLEAN Overlapped)
{
    WCHAR Name[MAX_PATH];
    IO_STATUS_BLOCK Io;
    OVERLAPPED Connect = {0};
    NTSTATUS Status;
    HANDLE Server, Client, Token;
    DWORD Count;
    CHAR Buffer[8];
    ULONG Level, Type, Length;
    BOOL Ret;

    StringCbPrintfW(Name, sizeof(Name), L"\\\\.\\pipe\\wow64pf_fsctl_%lu_%lu_%lu", GetCurrentProcessId(), GetTickCount(), QualityOfService);
    Server = CreateNamedPipeW(Name, PIPE_ACCESS_DUPLEX | (Overlapped ? FILE_FLAG_OVERLAPPED : 0),
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
    ok(Server != INVALID_HANDLE_VALUE, "CreateNamedPipe failed, error %lu\n", GetLastError());
    if (Server == INVALID_HANDLE_VALUE)
        return;

    /* Not connected */
    Status = Control(Server, FSCTL_PIPE_IMPERSONATE, NULL, 0, NULL, 0, &Io);
    ok_hex(Status, STATUS_CANNOT_IMPERSONATE);

    Client = CreateFileW(Name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                         SECURITY_SQOS_PRESENT | QualityOfService, NULL);
    ok(Client != INVALID_HANDLE_VALUE, "open failed, error %lu\n", GetLastError());
    if (Client == INVALID_HANDLE_VALUE)
    {
        CloseHandle(Server);
        return;
    }
    Connect.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Ret = ConnectNamedPipe(Server, Overlapped ? &Connect : NULL);
    ok(Ret || GetLastError() == ERROR_PIPE_CONNECTED, "ConnectNamedPipe: error %lu\n", GetLastError());

    Ret = WriteFile(Client, "x", 1, &Count, NULL);
    ok(Ret && Count == 1, "write failed, error %lu\n", GetLastError());
    ResetEvent(Connect.hEvent);
    if (Overlapped)
    {
        Ret = ReadFile(Server, Buffer, sizeof(Buffer), NULL, &Connect);
        ok(Ret || GetLastError() == ERROR_IO_PENDING, "ReadFile: error %lu\n", GetLastError());
        Ret = GetOverlappedResult(Server, &Connect, &Count, TRUE);
    }
    else
        Ret = ReadFile(Server, Buffer, sizeof(Buffer), &Count, NULL);
    ok(Ret, "read failed, error %lu\n", GetLastError());

    /* The client's process id: Windows 10 answers STATUS_NOT_FOUND for both ends when the other end is in the
     * caller's own process, ReactOS gives the id. */
    Level = 0xdeadbeef;
    Status = Control(Server, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE_, (PVOID)"ClientProcessId", sizeof("ClientProcessId"),
                     &Level, sizeof(Level), &Io);
    ok(Status == STATUS_SUCCESS || Status == STATUS_NOT_FOUND, "ClientProcessId: status %lx\n", Status);
    if (Status == STATUS_SUCCESS)
    {
        ok_eq_ulong(Level, GetCurrentProcessId());
        ok_eq_ulongptr(Io.Information, sizeof(ULONG));
    }
    else
        ok_eq_ulong(Level, 0xdeadbeef);
    /* The server's, from the client end */
    Level = 0xdeadbeef;
    Status = Control(Client, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE_, (PVOID)"ServerProcessId", sizeof("ServerProcessId"),
                     &Level, sizeof(Level), &Io);
    ok(Status == STATUS_SUCCESS || Status == STATUS_NOT_FOUND, "ServerProcessId: status %lx\n", Status);
    if (Status == STATUS_SUCCESS)
        ok_eq_ulong(Level, GetCurrentProcessId());
    else
        ok_eq_ulong(Level, 0xdeadbeef);
    Level = 0xdeadbeef;
    Status = Control(Server, FSCTL_PIPE_GET_CONNECTION_ATTRIBUTE_, (PVOID)"NoSuchAttribute", sizeof("NoSuchAttribute"),
                     &Level, sizeof(Level), &Io);
    ok(Status != STATUS_SUCCESS, "unknown attribute accepted\n");
    ok_eq_ulong(Level, 0xdeadbeef);

    Status = Control(Server, FSCTL_PIPE_IMPERSONATE, NULL, 0, NULL, 0, &Io);
    ok_hex(Status, STATUS_SUCCESS);
    ok_hex(Io.Status, STATUS_SUCCESS);
    ok_eq_ulongptr(Io.Information, 0);

    Token = NULL;
    Status = NtOpenThreadToken(NtCurrentThread(), TOKEN_QUERY, TRUE, &Token);
    if (Expected == SecurityAnonymous)
        ok_hex(Status, STATUS_CANT_OPEN_ANONYMOUS);
    else
        ok_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Level = 0xdeadbeef;
        Status = NtQueryInformationToken(Token, TokenImpersonationLevel, &Level, sizeof(Level), &Length);
        ok_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(Level, Expected);
        Type = 0xdeadbeef;
        Status = NtQueryInformationToken(Token, TokenType, &Type, sizeof(Type), &Length);
        ok_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(Type, TokenImpersonation);
        NtClose(Token);
    }

    /* Revert */
    Token = NULL;
    Status = NtSetInformationThread(NtCurrentThread(), ThreadImpersonationToken, &Token, sizeof(Token));
    ok_hex(Status, STATUS_SUCCESS);
    Status = NtOpenThreadToken(NtCurrentThread(), TOKEN_QUERY, TRUE, &Token);
    if (Expected != SecurityAnonymous)
        ok_hex(Status, STATUS_NO_TOKEN);

    CloseHandle(Connect.hEvent);
    CloseHandle(Client);
    CloseHandle(Server);
}

START_TEST(NtFsControlFilePipe)
{
    TestImpersonation(SECURITY_IMPERSONATION, SecurityImpersonation, FALSE);
    TestImpersonation(SECURITY_IMPERSONATION, SecurityImpersonation, TRUE);
    TestImpersonation(SECURITY_IDENTIFICATION, SecurityIdentification, FALSE);
    TestImpersonation(SECURITY_ANONYMOUS, SecurityAnonymous, TRUE);
}
