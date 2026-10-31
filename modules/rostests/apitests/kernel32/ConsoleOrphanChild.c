/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that a child sharing its parent's console starts when the parent exits right after creating it
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef SW_HIDE
#define SW_HIDE 0
#endif

/*
 * This is what "start /b program" in a batch file does when the batch file ends
 * on that line: the batch interpreter owns a console, creates a program that
 * shares it, and exits at once. The new process connects to the console server
 * only while it initializes, after its creator has gone. The console must stay
 * alive for it, and the program must run.
 *
 * Test process -> "middle" (new hidden console, nobody else attached to it)
 *   -> "child" (shares the console of "middle"); "middle" exits right away.
 * The child signals a named event once it can use its console.
 */
#define ROUNDS              20
#define CHILD_TIMEOUT_MS    30000

static
VOID
RunChild(
    _In_ PCWSTR EventName)
{
    HANDLE Event;

    Event = OpenEventW(EVENT_MODIFY_STATE, FALSE, EventName);
    if (!Event)
        ExitProcess(2);
    /* GetConsoleCP asks the console server, so it fails without a console */
    if (GetConsoleCP() == 0)
        ExitProcess(3);
    SetEvent(Event);
    CloseHandle(Event);
    ExitProcess(0);
}

static
VOID
RunMiddle(
    _In_ PCWSTR EventName)
{
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 100];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" ConsoleOrphanChild child %ls", FileName, EventName);
    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);

    /* no CREATE_NEW_CONSOLE: the child shares the console of this process */
    if (!CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
        ExitProcess(4);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
    ExitProcess(0);
}

START_TEST(ConsoleOrphanChild)
{
    int argc;
    char **argv;
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 100];
    WCHAR EventName[64];
    WCHAR Arg[64];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    HANDLE Event, Job;
    ULONG i, NotStarted = 0, Failed = 0;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 4 && !strcmp(argv[2], "child"))
    {
        MultiByteToWideChar(CP_ACP, 0, argv[3], -1, Arg, _countof(Arg));
        RunChild(Arg);
    }
    if (argc >= 4 && !strcmp(argv[2], "middle"))
    {
        MultiByteToWideChar(CP_ACP, 0, argv[3], -1, Arg, _countof(Arg));
        RunMiddle(Arg);
    }

    GetModuleFileNameW(NULL, FileName, _countof(FileName));

    /* a job to clean up a child that never got going; without one only that cleanup is lost */
    Job = CreateJobObjectW(NULL, NULL);

    for (i = 0; i < ROUNDS; i++)
    {
        StringCbPrintfW(EventName, sizeof(EventName), L"ConsoleOrphanChild_%lu_%lu", GetCurrentProcessId(), i);
        Event = CreateEventW(NULL, TRUE, FALSE, EventName);
        ok(Event != NULL, "CreateEvent failed with %lu\n", GetLastError());
        if (!Event)
            break;

        StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" ConsoleOrphanChild middle %ls", FileName, EventName);
        RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
        StartupInfo.cb = sizeof(StartupInfo);
        StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
        StartupInfo.wShowWindow = SW_HIDE;
        if (!CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, CREATE_NEW_CONSOLE | CREATE_SUSPENDED,
                            NULL, NULL, &StartupInfo, &ProcessInfo))
        {
            NotStarted++;
            CloseHandle(Event);
            continue;
        }
        if (Job)
            AssignProcessToJobObject(Job, ProcessInfo.hProcess);
        ResumeThread(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hThread);
        CloseHandle(ProcessInfo.hProcess);

        if (WaitForSingleObject(Event, CHILD_TIMEOUT_MS) != WAIT_OBJECT_0)
        {
            Failed++;
            if (Job)
                TerminateJobObject(Job, 1);
        }
        CloseHandle(Event);
        if (Failed)
            break;
    }

    ok(NotStarted == 0, "%lu processes could not be started\n", NotStarted);
    ok(Failed == 0, "a child of an exited console owner did not start within %d ms, after %lu rounds\n",
       CHILD_TIMEOUT_MS, i);
    if (Job)
        CloseHandle(Job);
}
