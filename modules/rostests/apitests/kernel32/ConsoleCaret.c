/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that the console window shows the caret right after output
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <wingdi.h>
#include <winuser.h>

#ifndef SW_SHOWNOACTIVATE
#define SW_SHOWNOACTIVATE 4
#endif

/*
 * The child's console window is shown but not active, so its caret stays off
 * once a blink tick has turned it off, until the next output. The child finds
 * when the blink timer ticks by watching the screen pixels of the caret cell,
 * then writes a character just before, at or just after the next tick: the
 * output must show the caret until the tick that follows it, 500 ms later.
 * A tick that is due when the window thread handles that output must not
 * turn the caret off again.
 */
#define ROUNDS              34
#define BLINK_MS            500
#define WATCH_MS            400
#define CHILD_TIMEOUT_MS    240000

static
BOOL
CaretCellLit(
    _In_ HWND hWnd,
    _In_ HANDLE hCon,
    _Out_ PBOOL Visible)
{
    CONSOLE_SCREEN_BUFFER_INFO Info;
    RECT Client;
    LONG CellWidth, CellHeight, X, Y;
    COLORREF Color;
    HDC hDC;

    *Visible = FALSE;
    if (!GetConsoleScreenBufferInfo(hCon, &Info) || !GetClientRect(hWnd, &Client))
        return FALSE;
    CellWidth = Client.right / (Info.srWindow.Right - Info.srWindow.Left + 1);
    CellHeight = Client.bottom / (Info.srWindow.Bottom - Info.srWindow.Top + 1);
    if (CellWidth <= 0 || CellHeight <= 0)
        return FALSE;

    /* The caret fills the bottom of its cell, in the cell's text color */
    X = (Info.dwCursorPosition.X - Info.srWindow.Left) * CellWidth + CellWidth / 2;
    Y = (Info.dwCursorPosition.Y - Info.srWindow.Top) * CellHeight + CellHeight - 2;
    hDC = GetDC(hWnd);
    if (!hDC)
        return FALSE;
    Color = GetPixel(hDC, X, Y);
    ReleaseDC(hWnd, hDC);
    if (Color == CLR_INVALID)
        return FALSE;
    *Visible = (Color != RGB(0, 0, 0));
    return TRUE;
}

/* Milliseconds from the performance counter: GetTickCount is too coarse here */
static
DWORD
NowMs(VOID)
{
    LARGE_INTEGER Freq, Now;

    QueryPerformanceFrequency(&Freq);
    QueryPerformanceCounter(&Now);
    return (DWORD)(Now.QuadPart * 1000 / Freq.QuadPart);
}

/* Poll the caret cell until it shows the wanted state; returns the time or 0 */
static
DWORD
WaitCaret(
    _In_ HWND hWnd,
    _In_ HANDLE hCon,
    _In_ BOOL Wanted,
    _In_ DWORD TimeoutMs,
    _Out_ PBOOL Readable)
{
    DWORD Start = NowMs(), Now;
    BOOL Visible;

    *Readable = TRUE;
    do
    {
        if (!CaretCellLit(hWnd, hCon, &Visible))
        {
            *Readable = FALSE;
            return 0;
        }
        Now = NowMs();
        if (Visible == Wanted)
            return Now ? Now : 1;
        Sleep(1);
    } while (Now - Start < TimeoutMs);
    return 0;
}

static
VOID
RunChild(VOID)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE), hCon;
    HWND hWnd = GetConsoleWindow();
    CHAR Line[64];
    DWORD Written, Off, Target;
    ULONG i, Missing = 0, Measured = 0;
    LONG Offset;
    BOOL Readable, Seen;

    hCon = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
    if (hCon == INVALID_HANDLE_VALUE || !hWnd || !IsWindowVisible(hWnd))
    {
        StringCbPrintfA(Line, sizeof(Line), "noconsole\n");
        WriteFile(hOut, Line, (DWORD)strlen(Line), &Written, NULL);
        return;
    }
    SetConsoleTextAttribute(hCon, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    for (i = 0; i < ROUNDS; i++)
    {
        /* Output shows the caret; the tick 500 ms later turns it off */
        WriteConsoleA(hCon, "\nx", 2, &Written, NULL);
        if (!WaitCaret(hWnd, hCon, TRUE, 3 * BLINK_MS, &Readable) ||
            !(Off = WaitCaret(hWnd, hCon, FALSE, 3 * BLINK_MS, &Readable)))
        {
            continue;
        }

        /* Write from 16 ms before to 16 ms after the next tick */
        Offset = -16 + (LONG)(i % 17) * 2;
        Target = Off + BLINK_MS + Offset;
        while ((LONG)(Target - NowMs()) > 20) Sleep(10);
        while ((LONG)(Target - NowMs()) > 0) ;
        WriteConsoleA(hCon, "y", 1, &Written, NULL);

        Seen = (WaitCaret(hWnd, hCon, TRUE, WATCH_MS, &Readable) != 0);
        if (!Seen && !Readable)
            continue;
        Measured++;
        if (!Seen)
            Missing++;
    }
    CloseHandle(hCon);

    StringCbPrintfA(Line, sizeof(Line), "measured=%lu missing=%lu\n", Measured, Missing);
    WriteFile(hOut, Line, (DWORD)strlen(Line), &Written, NULL);
}

START_TEST(ConsoleCaret)
{
    int argc;
    char **argv;
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 40];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    SECURITY_ATTRIBUTES Sa = { sizeof(Sa), NULL, TRUE };
    HANDLE hRead, hWrite;
    CHAR Output[128];
    DWORD Wait, Count, Used = 0;
    ULONG Measured = 0, Missing = 0;
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
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" ConsoleCaret child", FileName);

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
    if (Wait != WAIT_OBJECT_0)
        return;

    if (!strncmp(Output, "noconsole", 9))
    {
        skip("The child has no shown console window\n");
        return;
    }
    if (sscanf(Output, "measured=%lu missing=%lu", &Measured, &Missing) != 2)
    {
        ok(0, "Unexpected child output '%s'\n", Output);
        return;
    }
    trace("caret checked after %lu writes\n", Measured);
    if (Measured < ROUNDS / 2)
    {
        skip("The caret cell could be read after only %lu of %d writes\n", Measured, ROUNDS);
        return;
    }
    ok(Missing == 0, "The caret did not appear within %d ms after %lu of %lu writes\n",
       WATCH_MS, Missing, Measured);
}
