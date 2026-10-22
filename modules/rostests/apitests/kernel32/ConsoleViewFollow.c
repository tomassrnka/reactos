/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that output which only moves the cursor brings it into the view
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define BUFFER_WIDTH    200
#define BUFFER_HEIGHT   30
#define VIEW_WIDTH      40
#define VIEW_HEIGHT     10

static
BOOL
GetInfo(
    _In_ HANDLE hCon,
    _Out_ PCONSOLE_SCREEN_BUFFER_INFO Info)
{
    BOOL Success = GetConsoleScreenBufferInfo(hCon, Info);
    ok(Success, "GetConsoleScreenBufferInfo failed with error %lu\n", GetLastError());
    return Success;
}

/* Move the view so that it starts at column Left, without moving the cursor */
static
BOOL
ScrollViewTo(
    _In_ HANDLE hCon,
    _In_ SHORT Left)
{
    CONSOLE_SCREEN_BUFFER_INFO Info;
    SMALL_RECT View = { Left, 0, Left + VIEW_WIDTH - 1, VIEW_HEIGHT - 1 };
    BOOL Success = SetConsoleWindowInfo(hCon, TRUE, &View);

    ok(Success, "SetConsoleWindowInfo(%d) failed with error %lu\n", Left, GetLastError());
    if (!Success || !GetInfo(hCon, &Info))
        return FALSE;
    ok(Info.srWindow.Left == Left, "View starts at %d, expected %d\n", Info.srWindow.Left, Left);
    return (Info.srWindow.Left == Left);
}

static
VOID
CheckCursorInView(
    _In_ HANDLE hCon,
    _In_ PCSTR What,
    _In_ SHORT ExpectX)
{
    CONSOLE_SCREEN_BUFFER_INFO Info;

    if (!GetInfo(hCon, &Info))
        return;
    ok(Info.dwCursorPosition.X == ExpectX, "%s: cursor at column %d, expected %d\n",
       What, Info.dwCursorPosition.X, ExpectX);
    ok(Info.srWindow.Left <= Info.dwCursorPosition.X && Info.dwCursorPosition.X <= Info.srWindow.Right,
       "%s: cursor column %d outside the view %d-%d\n",
       What, Info.dwCursorPosition.X, Info.srWindow.Left, Info.srWindow.Right);
}

/* A screen buffer of our own, wider than its view, so the view scrolls horizontally */
static
HANDLE
CreateWideBuffer(VOID)
{
    HANDLE hCon;
    SMALL_RECT View = { 0, 0, VIEW_WIDTH - 1, VIEW_HEIGHT - 1 };
    COORD Size = { BUFFER_WIDTH, BUFFER_HEIGHT };
    CONSOLE_SCREEN_BUFFER_INFO Info;

    hCon = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
    ok(hCon != INVALID_HANDLE_VALUE, "CreateConsoleScreenBuffer failed with error %lu\n", GetLastError());
    if (hCon == INVALID_HANDLE_VALUE)
        return NULL;
    ok(SetConsoleWindowInfo(hCon, TRUE, &View), "SetConsoleWindowInfo failed with error %lu\n", GetLastError());
    ok(SetConsoleScreenBufferSize(hCon, Size), "SetConsoleScreenBufferSize failed with error %lu\n", GetLastError());
    ok(SetConsoleWindowInfo(hCon, TRUE, &View), "SetConsoleWindowInfo failed with error %lu\n", GetLastError());
    if (!GetInfo(hCon, &Info) || Info.dwSize.X != BUFFER_WIDTH ||
        Info.srWindow.Left != 0 || Info.srWindow.Right != VIEW_WIDTH - 1)
    {
        skip("Could not set up a buffer wider than its view\n");
        CloseHandle(hCon);
        return NULL;
    }
    return hCon;
}

static
VOID
Write(
    _In_ HANDLE hCon,
    _In_ PCWSTR Text)
{
    DWORD Written = 0;
    BOOL Success = WriteConsoleW(hCon, Text, lstrlenW(Text), &Written, NULL);
    ok(Success && Written == (DWORD)lstrlenW(Text), "WriteConsoleW failed with error %lu\n", GetLastError());
}

START_TEST(ConsoleViewFollow)
{
    HANDLE hOrig, hCon, hOther;
    COORD Pos;
    CHAR_INFO Cells[2];
    COORD CellsSize = { 2, 1 }, CellsAt = { 0, 0 };
    SMALL_RECT CellsRect = { 0, 0, 1, 0 };
    CONSOLE_SCREEN_BUFFER_INFO Info;

    if (!GetConsoleWindow())
    {
        skip("No console window\n");
        return;
    }

    /* The active buffer to restore at the end, even if standard output is redirected */
    hOrig = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    ok(hOrig != INVALID_HANDLE_VALUE, "Opening CONOUT$ failed with error %lu\n", GetLastError());
    if (hOrig == INVALID_HANDLE_VALUE)
        return;

    hCon = CreateWideBuffer();
    if (!hCon)
        goto Done;
    ok(SetConsoleActiveScreenBuffer(hCon), "SetConsoleActiveScreenBuffer failed with error %lu\n", GetLastError());

    /* Output that draws: the view follows the cursor to the right... */
    Pos.X = 150; Pos.Y = 2;
    ok(SetConsoleCursorPosition(hCon, Pos), "SetConsoleCursorPosition failed with error %lu\n", GetLastError());
    Write(hCon, L"x");
    CheckCursorInView(hCon, "x at column 150", 151);

    /* ... and a lone carriage return brings the cursor back into view */
    if (ScrollViewTo(hCon, 120))
    {
        Write(hCon, L"\r");
        CheckCursorInView(hCon, "CR", 0);
        GetInfo(hCon, &Info);
        ok(Info.srWindow.Left == 0, "CR: view starts at %d, expected 0\n", Info.srWindow.Left);
    }

    /*
     * A backspace that only moves the cursor and draws nothing: from the
     * trailing half of a full-width character in the first cell of the
     * buffer, the cursor moves onto its leading half. The cells carry the
     * halves in their attributes, whatever the code page.
     */
    Cells[0].Char.UnicodeChar = 0x3042;     /* HIRAGANA LETTER A, full-width */
    Cells[0].Attributes = FOREGROUND_INTENSITY | COMMON_LVB_LEADING_BYTE;
    Cells[1].Char.UnicodeChar = 0x3042;
    Cells[1].Attributes = FOREGROUND_INTENSITY | COMMON_LVB_TRAILING_BYTE;
    ok(WriteConsoleOutputW(hCon, Cells, CellsSize, CellsAt, &CellsRect),
       "WriteConsoleOutputW failed with error %lu\n", GetLastError());
    Pos.X = 1; Pos.Y = 0;
    ok(SetConsoleCursorPosition(hCon, Pos), "SetConsoleCursorPosition failed with error %lu\n", GetLastError());
    if (ScrollViewTo(hCon, 1))
    {
        Write(hCon, L"\b");
        CheckCursorInView(hCon, "BS onto a leading half", 0);
    }

    /* Moving the cursor of a buffer that is not active; activating it brings the cursor into view */
    hOther = CreateWideBuffer();
    if (hOther)
    {
        Pos.X = 150; Pos.Y = 2;
        ok(SetConsoleCursorPosition(hOther, Pos), "SetConsoleCursorPosition failed with error %lu\n", GetLastError());
        ok(SetConsoleActiveScreenBuffer(hOther), "SetConsoleActiveScreenBuffer failed with error %lu\n", GetLastError());
        CheckCursorInView(hOther, "activated buffer", 150);
        ok(SetConsoleActiveScreenBuffer(hCon), "SetConsoleActiveScreenBuffer failed with error %lu\n", GetLastError());
        CloseHandle(hOther);
    }

    ok(SetConsoleActiveScreenBuffer(hOrig), "Restoring the active buffer failed with error %lu\n", GetLastError());
    CloseHandle(hCon);
Done:
    CloseHandle(hOrig);
}
