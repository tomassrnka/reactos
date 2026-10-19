/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that threads can initialize and delete critical sections at the same time
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/*
 * A child process starts many threads that initialize, use and delete
 * critical sections at the same moment, as threads calling FindFirstFile
 * or CreateProcess together do. A child that crashes or does not exit in
 * time has a broken process-wide critical section list.
 */
#define THREADS             16
#define LOOPS               20000
#define SECTIONS            4
#define CHILD_TIMEOUT_MS    60000

static HANDLE StartEvent;

static
DWORD
WINAPI
RaceThread(
    _In_ LPVOID Parameter)
{
    RTL_CRITICAL_SECTION Sections[SECTIONS];
    ULONG i, k;

    UNREFERENCED_PARAMETER(Parameter);

    WaitForSingleObject(StartEvent, INFINITE);
    for (i = 0; i < LOOPS; i++)
    {
        for (k = 0; k < SECTIONS; k++)
            RtlInitializeCriticalSection(&Sections[k]);
        for (k = 0; k < SECTIONS; k++)
        {
            RtlEnterCriticalSection(&Sections[k]);
            RtlLeaveCriticalSection(&Sections[k]);
            RtlDeleteCriticalSection(&Sections[k]);
        }
    }
    return 0;
}

static
VOID
RaceChild(VOID)
{
    HANDLE Threads[THREADS];
    ULONG i, Count = 0;

    StartEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!StartEvent)
        ExitProcess(200);
    for (i = 0; i < THREADS; i++)
    {
        Threads[Count] = CreateThread(NULL, 0, RaceThread, NULL, 0, NULL);
        if (Threads[Count]) Count++;
    }
    if (Count < 2)
        ExitProcess(201);
    SetEvent(StartEvent);
    WaitForMultipleObjects(Count, Threads, TRUE, INFINITE);
    ExitProcess(0);
}

START_TEST(RtlCriticalSectionRace)
{
    int argc;
    char **argv;
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 40];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    DWORD Wait, ExitCode = 0;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "race"))
        RaceChild();

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" RtlCriticalSectionRace race", FileName);
    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    if (!CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcessW failed with %lu\n", GetLastError());
        return;
    }
    CloseHandle(ProcessInfo.hThread);
    Wait = WaitForSingleObject(ProcessInfo.hProcess, CHILD_TIMEOUT_MS);
    ok(Wait == WAIT_OBJECT_0, "The child did not exit within %d ms\n", CHILD_TIMEOUT_MS);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(ProcessInfo.hProcess, 1);
    }
    else
    {
        GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
        ok(ExitCode == 0, "The child exited with 0x%lx\n", ExitCode);
    }
    CloseHandle(ProcessInfo.hProcess);
}
