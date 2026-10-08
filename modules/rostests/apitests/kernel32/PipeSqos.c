/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the CreateFile security quality of service flags
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static
VOID
TestImpersonationLevel(
    _In_ DWORD dwFlags,
    _In_ SECURITY_IMPERSONATION_LEVEL ExpectedLevel)
{
    WCHAR PipeName[MAX_PATH];
    HANDLE hServer, hClient, hToken;
    SECURITY_IMPERSONATION_LEVEL Level;
    DWORD cbDone, cbLength;
    CHAR Byte = 'x';
    BOOL Success;

    StringCchPrintfW(PipeName, _countof(PipeName),
                     L"\\\\.\\pipe\\rostest_sqos_%lu", GetCurrentProcessId());

    hServer = CreateNamedPipeW(PipeName,
                               PIPE_ACCESS_DUPLEX,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                               1, 64, 64, 0, NULL);
    ok(hServer != INVALID_HANDLE_VALUE, "CreateNamedPipeW failed, error %lu\n", GetLastError());
    if (hServer == INVALID_HANDLE_VALUE)
        return;

    hClient = CreateFileW(PipeName,
                          GENERIC_READ | GENERIC_WRITE,
                          0,
                          NULL,
                          OPEN_EXISTING,
                          dwFlags,
                          NULL);
    ok(hClient != INVALID_HANDLE_VALUE, "0x%lx: CreateFileW failed, error %lu\n", dwFlags, GetLastError());
    if (hClient == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hServer);
        return;
    }

    Success = ConnectNamedPipe(hServer, NULL);
    ok(Success || GetLastError() == ERROR_PIPE_CONNECTED,
       "0x%lx: ConnectNamedPipe failed, error %lu\n", dwFlags, GetLastError());

    /* With dynamic tracking the server gets the client context with the data */
    Success = WriteFile(hClient, &Byte, sizeof(Byte), &cbDone, NULL);
    ok(Success, "0x%lx: WriteFile failed, error %lu\n", dwFlags, GetLastError());
    if (Success)
    {
        Success = ReadFile(hServer, &Byte, sizeof(Byte), &cbDone, NULL);
        ok(Success, "0x%lx: ReadFile failed, error %lu\n", dwFlags, GetLastError());
    }
    if (!Success)
    {
        CloseHandle(hClient);
        CloseHandle(hServer);
        return;
    }

    Success = ImpersonateNamedPipeClient(hServer);
    ok(Success, "0x%lx: ImpersonateNamedPipeClient failed, error %lu\n", dwFlags, GetLastError());
    if (Success)
    {
        /* An identification-level token cannot be used to open objects */
        Success = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &hToken);
        ok(Success, "0x%lx: OpenThreadToken failed, error %lu\n", dwFlags, GetLastError());
        if (Success)
        {
            Level = (SECURITY_IMPERSONATION_LEVEL)0xdeadbeef;
            Success = GetTokenInformation(hToken, TokenImpersonationLevel,
                                          &Level, sizeof(Level), &cbLength);
            ok(Success, "0x%lx: GetTokenInformation failed, error %lu\n", dwFlags, GetLastError());
            ok(Level == ExpectedLevel, "0x%lx: impersonation level is %d, expected %d\n",
               dwFlags, Level, ExpectedLevel);
            CloseHandle(hToken);
        }

        Success = RevertToSelf();
        ok(Success, "0x%lx: RevertToSelf failed, error %lu\n", dwFlags, GetLastError());
    }

    CloseHandle(hClient);
    CloseHandle(hServer);
}

START_TEST(PipeSqos)
{
    /* Without SECURITY_SQOS_PRESENT the server may impersonate the client */
    TestImpersonationLevel(0, SecurityImpersonation);

    TestImpersonationLevel(SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION,
                           SecurityImpersonation);
    TestImpersonationLevel(SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                           SecurityIdentification);
    TestImpersonationLevel(SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION |
                           SECURITY_CONTEXT_TRACKING,
                           SecurityIdentification);
    TestImpersonationLevel(SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION |
                           SECURITY_EFFECTIVE_ONLY,
                           SecurityIdentification);
}
