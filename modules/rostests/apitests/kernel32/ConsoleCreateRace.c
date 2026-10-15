/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that consoles created and destroyed at the same time all get their window
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef SW_SHOWNOACTIVATE
#define SW_SHOWNOACTIVATE 4
#endif

/*
 * Each thread starts short-lived children with consoles of their own, so that
 * the console server creates one console window while it destroys another.
 * A child that does not exit in time waits for a console that is never
 * created.
 */
#define THREADS             4
#define ROUNDS              40
#define CHILD_TIMEOUT_MS    30000

static WCHAR FileName[MAX_PATH];
static LONG Started, Hung, NotStarted;

static
VOID
RunChild(VOID)
{
    WCHAR CommandLine[MAX_PATH + 40];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;

    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" ConsoleCreateRace exit", FileName);
    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    StartupInfo.wShowWindow = SW_SHOWNOACTIVATE;

    if (!CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, CREATE_NEW_CONSOLE,
                        NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        InterlockedIncrement(&NotStarted);
        return;
    }
    InterlockedIncrement(&Started);
    CloseHandle(ProcessInfo.hThread);
    if (WaitForSingleObject(ProcessInfo.hProcess, CHILD_TIMEOUT_MS) != WAIT_OBJECT_0)
    {
        InterlockedIncrement(&Hung);
        TerminateProcess(ProcessInfo.hProcess, 1);
    }
    CloseHandle(ProcessInfo.hProcess);
}

static
DWORD
WINAPI
RaceThread(
    _In_ LPVOID Parameter)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Parameter);

    for (i = 0; i < ROUNDS && !Hung; i++)
        RunChild();
    return 0;
}

START_TEST(ConsoleCreateRace)
{
    int argc;
    char **argv;
    HANDLE Threads[THREADS];
    ULONG i, Count = 0;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "exit"))
        return;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));

    /*
     * One child first, alone: the first CreateProcess of a process initializes
     * kernel32's application certification state, and threads racing there can
     * leave its lock owned, which is not what this test is about.
     */
    RunChild();

    for (i = 0; i < THREADS; i++)
    {
        Threads[Count] = CreateThread(NULL, 0, RaceThread, NULL, 0, NULL);
        if (Threads[Count]) Count++;
    }
    if (!Count)
    {
        skip("CreateThread failed with %lu\n", GetLastError());
        return;
    }
    WaitForMultipleObjects(Count, Threads, TRUE, INFINITE);
    for (i = 0; i < Count; i++)
        CloseHandle(Threads[i]);

    trace("%ld children started, %ld not started\n", Started, NotStarted);
    ok(Started > 0, "No child could be started\n");
    ok(Hung == 0, "%ld children did not exit within %d ms (their console was never created)\n",
       Hung, CHILD_TIMEOUT_MS);
}
