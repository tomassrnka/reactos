/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test terminating a process while its first thread connects to win32k
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define SPAWNERS 4
#define HAMMERS 2

static WCHAR HelperPath[MAX_PATH];
static volatile LONG Stop;
static LONG Killed, KilledConnecting, Other, Failed;
static LONGLONG ConnectTicks;

typedef struct _CHILD
{
    PROCESS_INFORMATION Info;
    HANDLE Started;
    HANDLE Connected;
} CHILD, *PCHILD;

/* The helper imports no user32: it signals Started, loads user32, signals Connected after a
   successful load and sleeps for at most a minute */
static BOOL
StartChild(PCHILD Child)
{
    SECURITY_ATTRIBUTES Inherit = { sizeof(Inherit), NULL, TRUE };
    STARTUPINFOW Startup = { sizeof(Startup) };
    WCHAR Cmd[MAX_PATH + 64];

    Child->Started = CreateEventW(&Inherit, TRUE, FALSE, NULL);
    Child->Connected = CreateEventW(&Inherit, TRUE, FALSE, NULL);
    if (!Child->Started || !Child->Connected)
        goto Fail;
    _snwprintf(Cmd, ARRAYSIZE(Cmd) - 1, L"\"%s\" %Ix %Ix",
               HelperPath, (ULONG_PTR)Child->Started, (ULONG_PTR)Child->Connected);
    Cmd[ARRAYSIZE(Cmd) - 1] = UNICODE_NULL;
    if (!CreateProcessW(NULL, Cmd, NULL, NULL, TRUE, DETACHED_PROCESS,
                        NULL, NULL, &Startup, &Child->Info))
        goto Fail;
    CloseHandle(Child->Info.hThread);
    if (WaitForSingleObject(Child->Started, 30000) == WAIT_OBJECT_0)
        return TRUE;
    TerminateProcess(Child->Info.hProcess, 0xDEAD);
    WaitForSingleObject(Child->Info.hProcess, 30000);
    CloseHandle(Child->Info.hProcess);
Fail:
    if (Child->Started)
        CloseHandle(Child->Started);
    if (Child->Connected)
        CloseHandle(Child->Connected);
    return FALSE;
}

static VOID
EndChild(PCHILD Child)
{
    DWORD Code;

    TerminateProcess(Child->Info.hProcess, 0xDEAD);
    if (WaitForSingleObject(Child->Info.hProcess, 60000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(Child->Info.hProcess, &Code))
    {
        InterlockedIncrement(&Failed);
    }
    else if (Code == 0xDEAD)
    {
        InterlockedIncrement(&Killed);
    }
    else
    {
        trace("Child exited with 0x%lx\n", Code);
        InterlockedIncrement(&Other);
    }
    CloseHandle(Child->Info.hProcess);
    CloseHandle(Child->Started);
    CloseHandle(Child->Connected);
}

static ULONG
NextRandom(PULONG State)
{
    ULONG x = *State;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *State = x;
}

static LONGLONG
MeasureConnect(void)
{
    LONGLONG Max = 0;
    LARGE_INTEGER Start, End;
    HANDLE Wait[2];
    CHILD Child;
    int i;

    for (i = 0; i < 8; i++)
    {
        if (!StartChild(&Child))
            return 0;
        QueryPerformanceCounter(&Start);
        Wait[0] = Child.Connected;
        Wait[1] = Child.Info.hProcess;
        if (WaitForMultipleObjects(2, Wait, FALSE, 30000) != WAIT_OBJECT_0)
        {
            EndChild(&Child);
            return 0;
        }
        QueryPerformanceCounter(&End);
        if (End.QuadPart - Start.QuadPart > Max)
            Max = End.QuadPart - Start.QuadPart;
        EndChild(&Child);
    }
    return Max;
}

static DWORD WINAPI
SpawnerThread(PVOID Param)
{
    ULONG Seed = GetTickCount() ^ (GetCurrentThreadId() << 16) ^ 0x9E3779B9;
    LARGE_INTEGER Start, Now;
    LONGLONG Delay;
    CHILD Child;

    while (!Stop)
    {
        if (!StartChild(&Child))
        {
            InterlockedIncrement(&Failed);
            Sleep(100);
            continue;
        }

        /* Kill it at a random point of loading user32, spinning for sub-tick delays */
        Delay = ConnectTicks * (NextRandom(&Seed) % 1024) / 1024;
        QueryPerformanceCounter(&Start);
        do
        {
            QueryPerformanceCounter(&Now);
        } while (Now.QuadPart - Start.QuadPart < Delay);
        if (WaitForSingleObject(Child.Connected, 0) != WAIT_OBJECT_0)
            InterlockedIncrement(&KilledConnecting);
        EndChild(&Child);
    }
    return 0;
}

/* Keep the USER lock busy so that connecting children wait for it */
static DWORD WINAPI
HammerThread(PVOID Param)
{
    HWND hWnd;
    MSG Msg;

    while (!Stop)
    {
        hWnd = CreateWindowExW(0, L"STATIC", L"ProcessConnectTerminate", WS_POPUP,
                               0, 0, 10, 10, NULL, NULL, NULL, NULL);
        if (hWnd)
            DestroyWindow(hWnd);
        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&Msg);
    }
    return 0;
}

START_TEST(ProcessConnectTerminate)
{
    HANDLE Threads[SPAWNERS + HAMMERS];
    LARGE_INTEGER Frequency;
    ULONG Seconds = 20, Count = 0, i;
    static const WCHAR Helper[] = L"user32_apitest_connect.exe";
    DWORD Length;
    PWSTR FileName;
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3)
        Seconds = strtoul(argv[2], NULL, 10);

    Length = GetModuleFileNameW(NULL, HelperPath, ARRAYSIZE(HelperPath));
    FileName = (Length && Length < ARRAYSIZE(HelperPath)) ? wcsrchr(HelperPath, L'\\') : NULL;
    if (!FileName || (FileName + 1 - HelperPath) + ARRAYSIZE(Helper) > ARRAYSIZE(HelperPath))
    {
        skip("The path of the test is too long for the helper's path\n");
        return;
    }
    wcscpy(FileName + 1, Helper);

    QueryPerformanceFrequency(&Frequency);
    ConnectTicks = MeasureConnect();
    ok(ConnectTicks > 0, "The helper process did not connect to win32k\n");
    if (ConnectTicks <= 0)
        return;

    for (i = 0; i < HAMMERS; i++)
    {
        Threads[Count] = CreateThread(NULL, 0, HammerThread, NULL, 0, NULL);
        if (Threads[Count])
            Count++;
    }
    for (i = 0; i < SPAWNERS; i++)
    {
        Threads[Count] = CreateThread(NULL, 0, SpawnerThread, NULL, 0, NULL);
        if (Threads[Count])
            Count++;
    }

    ok(Count == SPAWNERS + HAMMERS, "Only %lu of %u worker threads started\n", Count, SPAWNERS + HAMMERS);
    if (Count == SPAWNERS + HAMMERS)
        Sleep(Seconds * 1000);
    Stop = TRUE;
    WaitForMultipleObjects(Count, Threads, TRUE, INFINITE);
    for (i = 0; i < Count; i++)
        CloseHandle(Threads[i]);

    trace("Connect %lu us; %ld killed, %ld of them while connecting, %ld other exit codes\n",
          (ULONG)(ConnectTicks * 1000000 / Frequency.QuadPart), Killed, KilledConnecting, Other);
    ok(Failed == 0, "%ld helpers could not be started or waited for\n", Failed);
    ok(Other == 0, "%ld helpers exited with another code than 0xDEAD\n", Other);
}
