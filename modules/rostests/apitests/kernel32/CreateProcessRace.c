/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that threads racing through the first CreateProcess of a process do not hang
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/*
 * Each child process starts many threads that call CreateProcess at the same
 * moment, so that they all run kernel32's one-time initialization of process
 * creation together. A child that does not exit in time has a thread that
 * waits forever inside CreateProcess.
 */
#define CHILDREN            30
#define THREADS             16
#define CHILD_TIMEOUT_MS    30000
#define MAX_HUNG            3

static WCHAR FileName[MAX_PATH];
static HANDLE StartEvent;
static LONG NotStarted;

static
BOOL
StartSelf(
    _In_ PCWSTR Mode,
    _Out_ PPROCESS_INFORMATION ProcessInfo)
{
    WCHAR CommandLine[MAX_PATH + 40];
    STARTUPINFOW StartupInfo;

    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" CreateProcessRace %ls", FileName, Mode);
    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    return CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, 0,
                          NULL, NULL, &StartupInfo, ProcessInfo);
}

static
DWORD
WINAPI
RaceThread(
    _In_ LPVOID Parameter)
{
    PROCESS_INFORMATION ProcessInfo;

    UNREFERENCED_PARAMETER(Parameter);

    WaitForSingleObject(StartEvent, INFINITE);
    if (!StartSelf(L"exit", &ProcessInfo))
    {
        InterlockedIncrement(&NotStarted);
        return 0;
    }
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
    return 0;
}

/* Runs in a fresh process: its first CreateProcess calls all happen here */
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
    if (Count < 3)
        ExitProcess(201);

    /* Let the threads reach the wait before they are released together */
    Sleep(100);
    SetEvent(StartEvent);
    WaitForMultipleObjects(Count, Threads, TRUE, INFINITE);
    ExitProcess(NotStarted);
}

START_TEST(CreateProcessRace)
{
    int argc;
    char **argv;
    PROCESS_INFORMATION ProcessInfo;
    DWORD ExitCode = 0;
    ULONG i, Started = 0, Hung = 0, Failed = 0;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "exit"))
        ExitProcess(0);
    if (argc >= 3 && !strcmp(argv[2], "race"))
        RaceChild();

    for (i = 0; i < CHILDREN && Hung < MAX_HUNG; i++)
    {
        if (!StartSelf(L"race", &ProcessInfo))
        {
            ok(FALSE, "CreateProcessW failed with %lu\n", GetLastError());
            break;
        }
        Started++;
        CloseHandle(ProcessInfo.hThread);
        if (WaitForSingleObject(ProcessInfo.hProcess, CHILD_TIMEOUT_MS) != WAIT_OBJECT_0)
        {
            Hung++;
            TerminateProcess(ProcessInfo.hProcess, 1);
            WaitForSingleObject(ProcessInfo.hProcess, 5000);
        }
        else if (!GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode) || ExitCode != 0)
        {
            Failed++;
            trace("Child %lu exited with %lu\n", i, ExitCode);
        }
        CloseHandle(ProcessInfo.hProcess);
    }

    trace("%lu children started, %lu threads each\n", Started, (ULONG)THREADS);
    ok(Hung == 0, "%lu of %lu children did not exit within %d ms (a thread waits forever in CreateProcess)\n",
       Hung, Started, CHILD_TIMEOUT_MS);
    ok(Failed == 0, "%lu children had CreateProcess failures\n", Failed);
}
