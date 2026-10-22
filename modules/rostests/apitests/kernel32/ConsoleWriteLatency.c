/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that a console write does not wait for the window to scroll
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef SW_SHOWNOACTIVATE
#define SW_SHOWNOACTIVATE 4
#endif

/* Writes timed in each phase, the pause between paced writes, the bound for a median */
#define TIMED_WRITES        40
#define PACE_MS             20
#define MAX_MEDIAN_US       10000
#define CHILD_TIMEOUT_MS    240000

static
int
CompareUlong(
    _In_ const void *A,
    _In_ const void *B)
{
    ULONG X = *(const ULONG *)A, Y = *(const ULONG *)B;
    return (X > Y) - (X < Y);
}

static
ULONG
MedianOf(
    _Inout_ ULONG *Values,
    _In_ ULONG Count)
{
    qsort(Values, Count, sizeof(*Values), CompareUlong);
    return Values[Count / 2];
}

static
ULONG
WriteLine(
    _In_ HANDLE hCon,
    _In_ ULONG Number)
{
    WCHAR Text[64];
    LARGE_INTEGER Freq, T0, T1;
    DWORD Written, Length;

    StringCchPrintfW(Text, _countof(Text), L"console write latency line %lu\r\n", Number);
    Length = (DWORD)wcslen(Text);
    QueryPerformanceFrequency(&Freq);
    QueryPerformanceCounter(&T0);
    if (!WriteConsoleW(hCon, Text, Length, &Written, NULL))
        Written = 0;
    QueryPerformanceCounter(&T1);
    return Written == Length ? (ULONG)((T1.QuadPart - T0.QuadPart) * 1000000 / Freq.QuadPart)
                             : MAXLONG;
}

/*
 * Phase 1 writes paced lines past the bottom of the window, so that the view
 * scrolls with the window thread running between the writes. Phase 2 writes
 * without a pause until the screen buffer is full and scrolls with every line.
 */
static
VOID
RunChild(VOID)
{
    HANDLE hCon, hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO Info;
    CHAR Line[128], Check[64];
    ULONG Time[TIMED_WRITES], Number = 0, i, ViewRows, PacedUs, BurstUs, Total;
    DWORD Written;
    COORD Pos;
    BOOL LastLineOk = FALSE;

    hCon = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
    if (hCon == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(hCon, &Info))
    {
        StringCbPrintfA(Line, sizeof(Line), "noconsole\n");
        WriteFile(hOut, Line, (DWORD)strlen(Line), &Written, NULL);
        return;
    }

    ViewRows = Info.srWindow.Bottom - Info.srWindow.Top + 1;
    if (Info.dwSize.Y < (SHORT)(ViewRows + TIMED_WRITES + 10))
    {
        StringCbPrintfA(Line, sizeof(Line), "noconsole\n");
        WriteFile(hOut, Line, (DWORD)strlen(Line), &Written, NULL);
        return;
    }

    for (i = 0; i < ViewRows + 5; i++)
    {
        WriteLine(hCon, Number++);
        Sleep(PACE_MS);
    }
    for (i = 0; i < TIMED_WRITES; i++)
    {
        Time[i] = WriteLine(hCon, Number++);
        Sleep(PACE_MS);
    }
    PacedUs = MedianOf(Time, TIMED_WRITES);

    Total = Info.dwSize.Y + TIMED_WRITES;
    for (i = 0; i < Total; i++)
    {
        ULONG Us = WriteLine(hCon, Number++);
        if (i >= Total - TIMED_WRITES)
            Time[i - (Total - TIMED_WRITES)] = Us;
    }
    BurstUs = MedianOf(Time, TIMED_WRITES);

    /* The last line must be in the buffer, one row above the cursor */
    if (GetConsoleScreenBufferInfo(hCon, &Info) && Info.dwCursorPosition.Y > 0)
    {
        Pos.X = 0;
        Pos.Y = Info.dwCursorPosition.Y - 1;
        StringCbPrintfA(Line, sizeof(Line), "console write latency line %lu", Number - 1);
        if (ReadConsoleOutputCharacterA(hCon, Check, (DWORD)strlen(Line), Pos, &Written) &&
            Written == strlen(Line))
        {
            LastLineOk = !memcmp(Check, Line, Written);
        }
    }

    StringCbPrintfA(Line, sizeof(Line), "paced_us=%lu burst_us=%lu lastline=%d\n",
                    PacedUs, BurstUs, LastLineOk);
    WriteFile(hOut, Line, (DWORD)strlen(Line), &Written, NULL);
}

START_TEST(ConsoleWriteLatency)
{
    int argc;
    char **argv;
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 40];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    SECURITY_ATTRIBUTES Sa = { sizeof(Sa), NULL, TRUE };
    HANDLE hRead, hWrite;
    CHAR Output[256];
    DWORD Wait, Count, Used = 0;
    ULONG PacedUs = 0, BurstUs = 0;
    int LastLine = 0;
    BOOL Success;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "child"))
    {
        RunChild();
        return;
    }

    if (!CreatePipe(&hRead, &hWrite, &Sa, 0))
    {
        skip("CreatePipe failed with %lu\n", GetLastError());
        return;
    }

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" ConsoleWriteLatency child", FileName);

    /* The child gets a console of its own, shown but not activated */
    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    StartupInfo.wShowWindow = SW_SHOWNOACTIVATE;
    StartupInfo.hStdOutput = hWrite;
    StartupInfo.hStdError = hWrite;
    StartupInfo.hStdInput = INVALID_HANDLE_VALUE;

    Success = CreateProcessW(FileName, CommandLine, NULL, NULL, TRUE, CREATE_NEW_CONSOLE,
                             NULL, NULL, &StartupInfo, &ProcessInfo);
    CloseHandle(hWrite);
    if (!Success)
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
        CloseHandle(hRead);
        return;
    }
    CloseHandle(ProcessInfo.hThread);

    Wait = WaitForSingleObject(ProcessInfo.hProcess, CHILD_TIMEOUT_MS);
    ok(Wait == WAIT_OBJECT_0, "The child process did not exit: %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
        TerminateProcess(ProcessInfo.hProcess, 1);
    CloseHandle(ProcessInfo.hProcess);

    if (Wait == WAIT_OBJECT_0 &&
        ReadFile(hRead, Output, sizeof(Output) - 1, &Count, NULL))
    {
        Used = Count;
    }
    CloseHandle(hRead);
    Output[Used] = 0;

    if (!strncmp(Output, "noconsole", 9))
    {
        skip("The child has no usable console\n");
        return;
    }
    if (sscanf(Output, "paced_us=%lu burst_us=%lu lastline=%d", &PacedUs, &BurstUs, &LastLine) != 3)
    {
        ok(0, "Unexpected child output '%s'\n", Output);
        return;
    }

    trace("median write time: %lu us paced, %lu us in a burst\n", PacedUs, BurstUs);
    ok(PacedUs < MAX_MEDIAN_US,
       "Median write time while the view scrolls is %lu us, expected below %d us\n",
       PacedUs, MAX_MEDIAN_US);
    ok(BurstUs < MAX_MEDIAN_US,
       "Median write time while the buffer scrolls is %lu us, expected below %d us\n",
       BurstUs, MAX_MEDIAN_US);
    ok(LastLine, "The last line written is not in the screen buffer\n");
}
