/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for kernel32!GetNamedPipeServerProcessId
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef BOOL (WINAPI *PFN_GetNamedPipePid)(HANDLE, PULONG);

START_TEST(GetNamedPipeServerProcessId)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    PFN_GetNamedPipePid pServer = (PFN_GetNamedPipePid)GetProcAddress(Kernel32, "GetNamedPipeServerProcessId");
    PFN_GetNamedPipePid pClient = (PFN_GetNamedPipePid)GetProcAddress(Kernel32, "GetNamedPipeClientProcessId");
    WCHAR Name[64];
    HANDLE Server, Client;
    ULONG Pid;
    BOOL Ret;
    DWORD Error;

    if (!pServer)
    {
        skip("GetNamedPipeServerProcessId is not available\n");
        return;
    }

    _snwprintf(Name, RTL_NUMBER_OF(Name), L"\\\\.\\pipe\\GetNamedPipeServerProcessId_%lu", GetCurrentProcessId());
    Server = CreateNamedPipeW(Name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 512, 512, 0, NULL);
    ok(Server != INVALID_HANDLE_VALUE, "CreateNamedPipe failed with %lu\n", GetLastError());
    if (Server == INVALID_HANDLE_VALUE)
        return;

    /* The server end knows its creator before any client connects */
    Pid = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    Ret = pServer(Server, &Pid);
    ok(Ret, "GetNamedPipeServerProcessId on the server end failed with %lu\n", GetLastError());
    ok(Pid == GetCurrentProcessId(), "Pid = %lu, expected %lu\n", Pid, GetCurrentProcessId());

    Client = CreateFileW(Name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ok(Client != INVALID_HANDLE_VALUE, "CreateFile failed with %lu\n", GetLastError());
    if (Client != INVALID_HANDLE_VALUE)
    {
        /* Both ends report the creator of the server end */
        Pid = 0xdeadbeef;
        Ret = pServer(Client, &Pid);
        ok(Ret, "GetNamedPipeServerProcessId on the client end failed with %lu\n", GetLastError());
        ok(Pid == GetCurrentProcessId(), "Pid = %lu, expected %lu\n", Pid, GetCurrentProcessId());

        Pid = 0xdeadbeef;
        Ret = pServer(Server, &Pid);
        ok(Ret, "GetNamedPipeServerProcessId on the connected server end failed with %lu\n", GetLastError());
        ok(Pid == GetCurrentProcessId(), "Pid = %lu, expected %lu\n", Pid, GetCurrentProcessId());

        /* ... and the client attribute still works, for the same process here */
        if (pClient)
        {
            Pid = 0xdeadbeef;
            Ret = pClient(Server, &Pid);
            ok(Ret, "GetNamedPipeClientProcessId failed with %lu\n", GetLastError());
            ok(Pid == GetCurrentProcessId(), "Pid = %lu, expected %lu\n", Pid, GetCurrentProcessId());
        }
        CloseHandle(Client);
    }

    /* Not a pipe */
    SetLastError(0xdeadbeef);
    Ret = pServer(GetCurrentProcess(), &Pid);
    Error = GetLastError();
    ok(!Ret, "GetNamedPipeServerProcessId succeeded on a process handle\n");
    ok(Error != 0xdeadbeef && Error != ERROR_SUCCESS, "error %lu\n", Error);

    CloseHandle(Server);
}
