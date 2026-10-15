/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that console control events racing with process creation, attach and exit do not hang
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/*
 * A child on a console of its own sends Ctrl+Break events to its console while
 * other threads start console processes that inherit it, write to it, and
 * while grandchildren attach to it, free it and exit. The console server
 * delivers each event by creating a thread in every process on the console,
 * and process creation and attach look up a process and then lock its console.
 * A child that does not exit in time is stuck in a console server request.
 */
#define RACE_MS             15000
#define SPAWNERS            2
#define ATTACHERS           2
#define CHILD_TIMEOUT_MS    (RACE_MS + 60000)

static WCHAR FileName[MAX_PATH];
static volatile LONG Stop;
static LONG Events, Spawned, Handled;

static
BOOL
StartSelf(
    _In_ PCWSTR Mode,
    _In_ DWORD Flags,
    _Out_ PPROCESS_INFORMATION ProcessInfo)
{
    WCHAR CommandLine[MAX_PATH + 60];
    STARTUPINFOW StartupInfo;

    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" ConsoleCtrlEventRace %ls %lu",
                    FileName, Mode, GetCurrentProcessId());
    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    return CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, Flags,
                          NULL, NULL, &StartupInfo, ProcessInfo);
}

static
BOOL
WINAPI
CtrlHandler(
    _In_ DWORD CtrlType)
{
    if (CtrlType != CTRL_BREAK_EVENT && CtrlType != CTRL_C_EVENT)
        return FALSE;
    InterlockedIncrement(&Handled);
    return TRUE;
}

static
DWORD
WINAPI
EventThread(
    _In_ LPVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);

    /* A target that exits meanwhile can make the call fail; only a hang counts here */
    while (!Stop)
    {
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
        InterlockedIncrement(&Events);
    }
    return 0;
}

static
DWORD
WINAPI
SpawnThread(
    _In_ LPVOID Parameter)
{
    PROCESS_INFORMATION ProcessInfo;

    UNREFERENCED_PARAMETER(Parameter);

    while (!Stop)
    {
        if (!StartSelf(L"exit", 0, &ProcessInfo))
        {
            Sleep(1);
            continue;
        }
        InterlockedIncrement(&Spawned);
        CloseHandle(ProcessInfo.hThread);
        WaitForSingleObject(ProcessInfo.hProcess, INFINITE);
        CloseHandle(ProcessInfo.hProcess);
    }
    return 0;
}

static
DWORD
WINAPI
WriteThread(
    _In_ LPVOID Parameter)
{
    static const WCHAR Line[] = L"ConsoleCtrlEventRace output line, keeping the console busy\r\n";
    HANDLE Output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD Written;

    UNREFERENCED_PARAMETER(Parameter);

    while (!Stop)
        WriteConsoleW(Output, Line, _countof(Line) - 1, &Written, NULL);
    return 0;
}

/* Grandchild: attach to the race child's console and free it, over and over */
static
VOID
AttachChild(
    _In_ DWORD ParentId)
{
    DWORD Start = GetTickCount();

    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    FreeConsole();
    while (GetTickCount() - Start < RACE_MS)
    {
        if (AttachConsole(ParentId))
            FreeConsole();
        else if (GetLastError() != ERROR_ACCESS_DENIED)
            Sleep(1);
    }
    ExitProcess(0);
}

/* Child: runs on a console of its own, so the events never reach the test runner */
static
VOID
RaceChild(VOID)
{
    HANDLE Threads[SPAWNERS + 2];
    PROCESS_INFORMATION Attachers[ATTACHERS];
    ULONG i, Count = 0, AttachCount = 0;

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    for (i = 0; i < ATTACHERS; i++)
    {
        if (StartSelf(L"attach", 0, &Attachers[AttachCount]))
            CloseHandle(Attachers[AttachCount++].hThread);
    }

    Threads[Count] = CreateThread(NULL, 0, EventThread, NULL, 0, NULL);
    if (Threads[Count]) Count++;
    Threads[Count] = CreateThread(NULL, 0, WriteThread, NULL, 0, NULL);
    if (Threads[Count]) Count++;
    for (i = 0; i < SPAWNERS; i++)
    {
        Threads[Count] = CreateThread(NULL, 0, SpawnThread, NULL, 0, NULL);
        if (Threads[Count]) Count++;
    }
    if (Count < SPAWNERS + 2)
        ExitProcess(201);

    Sleep(RACE_MS);
    Stop = TRUE;
    WaitForMultipleObjects(Count, Threads, TRUE, INFINITE);
    for (i = 0; i < AttachCount; i++)
        WaitForSingleObject(Attachers[i].hProcess, INFINITE);

    if (!Events)
        ExitProcess(202);
    if (!Spawned)
        ExitProcess(203);
    if (AttachCount != ATTACHERS)
        ExitProcess(204);
    if (!Handled)
        ExitProcess(205);
    ExitProcess(0);
}

START_TEST(ConsoleCtrlEventRace)
{
    int argc;
    char **argv;
    PROCESS_INFORMATION ProcessInfo;
    DWORD Wait, ExitCode = 0;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "exit"))
        ExitProcess(0);
    if (argc >= 4 && !strcmp(argv[2], "attach"))
        AttachChild(strtoul(argv[3], NULL, 10));
    if (argc >= 3 && !strcmp(argv[2], "race"))
        RaceChild();

    if (!StartSelf(L"race", CREATE_NEW_CONSOLE, &ProcessInfo))
    {
        skip("CreateProcessW failed with %lu\n", GetLastError());
        return;
    }
    CloseHandle(ProcessInfo.hThread);
    Wait = WaitForSingleObject(ProcessInfo.hProcess, CHILD_TIMEOUT_MS);
    ok(Wait == WAIT_OBJECT_0, "The child did not exit within %d ms (stuck in a console server request)\n",
       CHILD_TIMEOUT_MS);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(ProcessInfo.hProcess, 1);
    }
    else
    {
        GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
        ok(ExitCode == 0, "The child exited with %lu\n", ExitCode);
    }
    CloseHandle(ProcessInfo.hProcess);
}
