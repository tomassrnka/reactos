/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for BindIoCompletionCallback
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define CHILD_NO_CALLBACK   0xC0DE0001
#define CHILD_SETUP_FAILED  0xC0DE0002
#define CHILD_BAD_ARGUMENTS 0xC0DE0003
#define CHILD_BIND_FAILED   0xC0DE0004

static HANDLE CallbackEvent;
static DWORD CallbackError;
static DWORD CallbackBytes;
static LPOVERLAPPED CallbackOverlapped;

static
VOID
CALLBACK
IoCompletionCallback(
    _In_ DWORD dwErrorCode,
    _In_ DWORD dwNumberOfBytesTransfered,
    _In_ LPOVERLAPPED lpOverlapped)
{
    CallbackError = dwErrorCode;
    CallbackBytes = dwNumberOfBytesTransfered;
    CallbackOverlapped = lpOverlapped;
    SetEvent(CallbackEvent);
}

static
DWORD
WINAPI
WorkItem(
    _In_ PVOID Context)
{
    SetEvent(Context);
    return 0;
}

/* Runs in a child process, so the thread pool starts with no thread.
 * The exit code is the error code the callback received. */
static
DWORD
RunChild(
    _In_ BOOL Warm)
{
    WCHAR PipeName[64];
    HANDLE Server, Client;
    OVERLAPPED Overlapped;
    CHAR Buffer[16];
    BOOL Success;

    CallbackEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!CallbackEvent)
        return CHILD_SETUP_FAILED;

    if (Warm)
    {
        /* Start a worker thread first, to test the error code alone */
        if (!QueueUserWorkItem(WorkItem, CallbackEvent, WT_EXECUTEDEFAULT) ||
            WaitForSingleObject(CallbackEvent, 10000) != WAIT_OBJECT_0)
        {
            return CHILD_SETUP_FAILED;
        }
    }

    StringCbPrintfW(PipeName, sizeof(PipeName),
                    L"\\\\.\\pipe\\kernel32_apitest_iocb_%lu",
                    GetCurrentProcessId());
    Server = CreateNamedPipeW(PipeName,
                              PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                              PIPE_TYPE_BYTE | PIPE_WAIT,
                              1, 0, sizeof(Buffer), 0, NULL);
    if (Server == INVALID_HANDLE_VALUE)
        return CHILD_SETUP_FAILED;
    Client = CreateFileW(PipeName, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (Client == INVALID_HANDLE_VALUE)
        return CHILD_SETUP_FAILED;

    if (!BindIoCompletionCallback(Server, IoCompletionCallback, 0))
        return CHILD_BIND_FAILED;

    /* No data yet, so the read stays pending until the client goes away */
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Success = ReadFile(Server, Buffer, sizeof(Buffer), NULL, &Overlapped);
    if (Success || GetLastError() != ERROR_IO_PENDING)
        return CHILD_SETUP_FAILED;
    CloseHandle(Client);

    if (WaitForSingleObject(CallbackEvent, 10000) != WAIT_OBJECT_0)
        return CHILD_NO_CALLBACK;
    if (CallbackOverlapped != &Overlapped || CallbackBytes != 0)
        return CHILD_BAD_ARGUMENTS;

    CloseHandle(Server);
    return CallbackError;
}

static
VOID
TestChild(
    _In_ PCWSTR Mode)
{
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 64];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    DWORD Wait, ExitCode;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine, sizeof(CommandLine),
                    L"\"%ls\" BindIoCompletionCallback %ls", FileName, Mode);
    ZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    if (!CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, 0,
                        NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
        return;
    }
    CloseHandle(ProcessInfo.hThread);

    Wait = WaitForSingleObject(ProcessInfo.hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "%ls: child did not exit, wait returned %lu\n", Mode, Wait);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(ProcessInfo.hProcess, 1);
        CloseHandle(ProcessInfo.hProcess);
        return;
    }
    GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
    CloseHandle(ProcessInfo.hProcess);

    if (ExitCode == CHILD_SETUP_FAILED)
    {
        skip("%ls: child setup failed\n", Mode);
        return;
    }
    ok(ExitCode != CHILD_BIND_FAILED, "%ls: BindIoCompletionCallback failed\n", Mode);
    ok(ExitCode != CHILD_NO_CALLBACK, "%ls: the callback did not run\n", Mode);
    ok(ExitCode != CHILD_BAD_ARGUMENTS, "%ls: wrong overlapped or byte count\n", Mode);
    ok(ExitCode == ERROR_BROKEN_PIPE, "%ls: callback error code is 0x%lx, expected %u\n",
       Mode, ExitCode, ERROR_BROKEN_PIPE);
}

START_TEST(BindIoCompletionCallback)
{
    int argc;
    char **argv;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3)
    {
        if (!strcmp(argv[2], "fresh"))
            ExitProcess(RunChild(FALSE));
        if (!strcmp(argv[2], "warm"))
            ExitProcess(RunChild(TRUE));
    }

    TestChild(L"fresh");
    TestChild(L"warm");
}
