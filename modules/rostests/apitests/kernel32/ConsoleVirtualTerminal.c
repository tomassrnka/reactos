/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test the virtual terminal sequences of console output (ENABLE_VIRTUAL_TERMINAL_PROCESSING)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING  0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN         0x0008
#endif
#ifndef ENABLE_LVB_GRID_WORLDWIDE
#define ENABLE_LVB_GRID_WORLDWIDE           0x0010
#endif

#define WIDTH   80
#define HEIGHT  100
#define VIEW    25

#define VT_MODE (ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)

static HANDLE hCon, hIn;

static VOID
Write(PCSTR Text)
{
    DWORD Written = 0;
    BOOL Success = WriteConsoleA(hCon, Text, lstrlenA(Text), &Written, NULL);
    ok(Success && Written == (DWORD)lstrlenA(Text), "WriteConsoleA failed (%lu written, error %lu)\n",
       Written, GetLastError());
}

static WORD
AttrAt(SHORT X, SHORT Y)
{
    WORD Attr = 0xDEAD;
    DWORD Read;
    COORD Pos = { X, Y };
    ReadConsoleOutputAttribute(hCon, &Attr, 1, Pos, &Read);
    return Attr;
}

static WCHAR
CharAt(SHORT X, SHORT Y)
{
    WCHAR Char = 0;
    DWORD Read;
    COORD Pos = { X, Y };
    ReadConsoleOutputCharacterW(hCon, &Char, 1, Pos, &Read);
    return Char;
}

static CONSOLE_SCREEN_BUFFER_INFO
BufferInfo(VOID)
{
    CONSOLE_SCREEN_BUFFER_INFO Info;
    ZeroMemory(&Info, sizeof(Info));
    GetConsoleScreenBufferInfo(hCon, &Info);
    return Info;
}

static VOID
MoveCursor(SHORT X, SHORT Y)
{
    COORD Pos = { X, Y };
    SetConsoleCursorPosition(hCon, Pos);
}

static VOID
SetView(SHORT Top)
{
    SMALL_RECT View = { 0, Top, WIDTH - 1, Top + VIEW - 1 };
    SetConsoleWindowInfo(hCon, TRUE, &View);
}

static VOID
PutText(SHORT Y, PCSTR Text)
{
    COORD Pos = { 0, Y };
    DWORD Written;
    WriteConsoleOutputCharacterA(hCon, Text, lstrlenA(Text), Pos, &Written);
}

/* Each row starts with a letter: a, b, c, ... */
static VOID
PutLetters(VOID)
{
    SHORT Y;
    for (Y = 0; Y < 26; ++Y)
    {
        CHAR Text[2] = { (CHAR)('a' + Y), 0 };
        PutText(Y, Text);
    }
}

/* A clean buffer: blank, default attribute, view at the top, cursor home, given mode */
static VOID
Reset(DWORD Mode)
{
    COORD Origin = { 0, 0 };
    DWORD Written;

    SetConsoleMode(hCon, VT_MODE);
    Write("\x1b[0m\x1b[r");
    SetConsoleMode(hCon, Mode);
    SetConsoleTextAttribute(hCon, 0x07);
    FillConsoleOutputCharacterW(hCon, L' ', WIDTH * HEIGHT, Origin, &Written);
    FillConsoleOutputAttribute(hCon, 0x07, WIDTH * HEIGHT, Origin, &Written);
    SetView(0);
    MoveCursor(0, 0);
}

static VOID
CheckCursor(PCSTR What, SHORT X, SHORT Y)
{
    CONSOLE_SCREEN_BUFFER_INFO i = BufferInfo();
    ok(i.dwCursorPosition.X == X && i.dwCursorPosition.Y == Y, "%s: cursor at %d,%d, expected %d,%d\n",
       What, i.dwCursorPosition.X, i.dwCursorPosition.Y, X, Y);
}

static VOID
CheckView(PCSTR What, SHORT Top)
{
    CONSOLE_SCREEN_BUFFER_INFO i = BufferInfo();
    ok(i.srWindow.Top == Top, "%s: view at row %d, expected %d\n", What, i.srWindow.Top, Top);
}

/* The first character of the rows Top..Top+Count-1, blanks as '.' */
static VOID
CheckColumn(PCSTR What, SHORT Top, PCSTR Expected)
{
    CHAR Column[128];
    SHORT i, Count = (SHORT)lstrlenA(Expected);

    for (i = 0; i < Count; ++i)
    {
        WCHAR c = CharAt(0, Top + i);
        Column[i] = (c == L' ') ? '.' : ((c >= 32 && c < 128) ? (CHAR)c : '?');
    }
    Column[Count] = 0;
    ok(!lstrcmpA(Column, Expected), "%s: rows %d.. are '%s', expected '%s'\n", What, Top, Column, Expected);
}

static VOID
CheckRow(PCSTR What, SHORT Y, PCSTR Expected)
{
    CHAR Row[128];
    SHORT i, Count = (SHORT)lstrlenA(Expected);

    for (i = 0; i < Count; ++i)
    {
        WCHAR c = CharAt(i, Y);
        Row[i] = (c == L' ') ? '.' : ((c >= 32 && c < 128) ? (CHAR)c : '?');
    }
    Row[Count] = 0;
    ok(!lstrcmpA(Row, Expected), "%s: row %d is '%s', expected '%s'\n", What, Y, Row, Expected);
}

static VOID
TestModes(VOID)
{
    DWORD Mode, Bit, Got;
    BOOL Success;
    HANDLE hOther;

    ok(GetConsoleMode(hCon, &Mode), "GetConsoleMode failed with error %lu\n", GetLastError());
    ok(Mode == (ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT), "Default output mode 0x%lx\n", Mode);

    /* Exactly 0x1 | 0x2 | 0x4 | 0x8 | 0x10 are output modes */
    for (Bit = 1; Bit <= 0x8000; Bit <<= 1)
    {
        BOOL Valid = (Bit <= ENABLE_LVB_GRID_WORLDWIDE);

        SetLastError(0xdeadbeef);
        Success = SetConsoleMode(hCon, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | Bit);
        ok(Success == Valid, "SetConsoleMode(0x%lx) returned %d\n", 0x3 | Bit, Success);
        if (!Success)
            ok(GetLastError() == ERROR_INVALID_PARAMETER, "SetConsoleMode(0x%lx): error %lu\n", 0x3 | Bit, GetLastError());
        Got = 0;
        GetConsoleMode(hCon, &Got);
        ok(Got == (Valid ? (0x3 | Bit) : 0x3), "After SetConsoleMode(0x%lx): mode 0x%lx\n", 0x3 | Bit, Got);
        SetConsoleMode(hCon, 0x3);
    }

    ok(SetConsoleMode(hCon, 0x1F), "SetConsoleMode(0x1f) failed with error %lu\n", GetLastError());
    ok(!SetConsoleMode(hCon, 0x3F), "SetConsoleMode(0x3f) succeeded\n");
    ok(SetConsoleMode(hCon, ENABLE_VIRTUAL_TERMINAL_PROCESSING), "SetConsoleMode(0x4) failed\n");
    ok(SetConsoleMode(hCon, 0), "SetConsoleMode(0) failed\n");

    /* The mode belongs to the buffer, not to the handle */
    SetConsoleMode(hCon, 0x3);
    ok(DuplicateHandle(GetCurrentProcess(), hCon, GetCurrentProcess(), &hOther, 0, FALSE, DUPLICATE_SAME_ACCESS),
       "DuplicateHandle failed with error %lu\n", GetLastError());
    ok(SetConsoleMode(hOther, VT_MODE), "SetConsoleMode(dup) failed\n");
    Got = 0;
    GetConsoleMode(hCon, &Got);
    ok(Got == VT_MODE, "Mode through the other handle 0x%lx\n", Got);
    CloseHandle(hOther);

    /* A new buffer takes the virtual terminal mode last set on an output buffer, nothing else */
    SetConsoleMode(hCon, 0x1F);
    hOther = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
    if (hOther != INVALID_HANDLE_VALUE)
    {
        Got = 0;
        GetConsoleMode(hOther, &Got);
        ok(Got == VT_MODE, "Mode of a buffer created after mode 0x1f: 0x%lx\n", Got);
        CloseHandle(hOther);
    }
    SetConsoleMode(hCon, 0x3);
    hOther = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
    if (hOther != INVALID_HANDLE_VALUE)
    {
        Got = 0;
        GetConsoleMode(hOther, &Got);
        ok(Got == 0x3, "Mode of a buffer created after mode 0x3: 0x%lx\n", Got);
        CloseHandle(hOther);
    }
}

static VOID
TestNotInterpreted(VOID)
{
    /* Without processed output or without the VT mode, the sequence is text */
    Reset(ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    Write("\x1b[31mX");
    CheckCursor("Mode 0x4, no processed output", 6, 0);
    ok(CharAt(0, 0) == 0x1B, "Mode 0x4: cell 0 is U+%04x\n", CharAt(0, 0));
    ok(AttrAt(5, 0) == 0x07, "Mode 0x4: attribute 0x%x\n", AttrAt(5, 0));

    Reset(ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT);
    Write("\x1b[31mX");
    CheckCursor("Mode 0x3", 6, 0);
    ok(CharAt(1, 0) == L'[', "Mode 0x3: cell 1 is U+%04x\n", CharAt(1, 0));
    ok(BufferInfo().wAttributes == 0x07, "Mode 0x3: attribute 0x%x\n", BufferInfo().wAttributes);
}

static const struct
{
    WORD Init;
    PCSTR Params;
    WORD Expected;
    BOOL CheckCell;     /* Windows reads back the default and extended colors differently */
} SgrTests[] =
{
    { 0x07, "1",        0x000F, TRUE },
    { 0x07, "4",        0x8007, TRUE },
    { 0x07, "7",        0x4007, TRUE },
    { 0x07, "2",        0x0007, TRUE },
    { 0x07, "5",        0x0007, TRUE },
    { 0x07, "53",       0x0007, TRUE },
    { 0x07, "30",       0x0000, TRUE },
    { 0x07, "31",       0x0004, TRUE },
    { 0x07, "32",       0x0002, TRUE },
    { 0x07, "33",       0x0006, TRUE },
    { 0x07, "34",       0x0001, TRUE },
    { 0x07, "35",       0x0005, TRUE },
    { 0x07, "36",       0x0003, TRUE },
    { 0x07, "37",       0x0007, TRUE },
    { 0x07, "40",       0x0007, TRUE },
    { 0x07, "41",       0x0047, TRUE },
    { 0x07, "44",       0x0017, TRUE },
    { 0x07, "47",       0x0077, TRUE },
    { 0x07, "90",       0x0008, TRUE },
    { 0x07, "91",       0x000C, TRUE },
    { 0x07, "94",       0x0009, TRUE },
    { 0x07, "97",       0x000F, TRUE },
    { 0x07, "100",      0x0087, TRUE },
    { 0x07, "101",      0x00C7, TRUE },
    { 0x07, "107",      0x00F7, TRUE },
    { 0x07, "1;31",     0x000C, TRUE },
    { 0x07, "31;1",     0x000C, TRUE },
    { 0x07, "7;31",     0x4004, TRUE },
    { 0x07, "4;32",     0x8002, TRUE },
    { 0x07, "1;22",     0x0007, TRUE },
    { 0x07, "7;27",     0x0007, TRUE },
    { 0x07, "4;24",     0x0007, TRUE },
    { 0x07, "1;91",     0x000C, TRUE },
    { 0x07, "0",        0x0007, FALSE },
    { 0x07, "",         0x0007, FALSE },
    { 0x07, "39",       0x0007, FALSE },
    { 0x07, "31;39",    0x0007, FALSE },
    { 0x07, "41;49",    0x0007, TRUE },
    { 0x07, "31;;42",   0x0027, FALSE },
    { 0x07, "38;5;1",   0x0004, FALSE },
    { 0x07, "38;5;9",   0x000C, FALSE },
    { 0x07, "38;5;15",  0x000F, FALSE },
    { 0x07, "38;5;16",  0x0000, FALSE },
    { 0x07, "38;5;231", 0x000F, FALSE },
    { 0x07, "38;5;232", 0x0000, FALSE },
    { 0x07, "38;5;244", 0x0008, FALSE },
    { 0x07, "48;5;2",   0x0027, FALSE },
    { 0x07, "48;5;12",  0x0097, FALSE },
    { 0x07, "38;2;0;0;0",       0x0000, FALSE },
    { 0x07, "38;2;255;255;255", 0x000F, FALSE },
    { 0x07, "1;38;5;1", 0x000C, FALSE },
    { 0x07, "38;5",     0x0007, TRUE },
    { 0x07, "38;5;300", 0x0007, TRUE },
    { 0x07, "38:5:9",   0x0007, TRUE },
    /* At most 16 parameters are used: 4 and 7 apply, 24, 27 and the colors do not */
    { 0x07, "2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;26;27;28;29;30;31;32;33;34;35;36;37;38;39;40",
                        0xC007, TRUE },
    /* Bold is a rendition of its own; the default colors are those of the console */
    { 0x1E, "0",        0x0007, FALSE },
    { 0x1E, "1",        0x001E, TRUE },
    { 0x1E, "22",       0x001E, TRUE },
    { 0x1E, "31",       0x0014, TRUE },
    { 0x1E, "39",       0x0017, FALSE },
    { 0x1E, "40",       0x000E, TRUE },
    { 0x1E, "49",       0x000E, TRUE },
    { 0x1E, "97",       0x001F, TRUE },
    { 0x1E, "7",        0x401E, TRUE },
    { 0x0F, "22",       0x000F, TRUE },
    { 0x0F, "31",       0x0004, TRUE },
    { 0x0F, "1;31",     0x000C, TRUE },
    { 0x0F, "39",       0x0007, FALSE },
    { 0x0F, "1;2",      0x000F, TRUE },
    { 0x70, "1",        0x0078, TRUE },
    { 0x70, "30",       0x0070, TRUE },
    { 0x70, "39",       0x0077, FALSE },
    { 0x70, "49",       0x0000, TRUE },
    { 0x70, "1;2",      0x0078, TRUE },
};

static VOID
TestGraphicsRendition(VOID)
{
    UINT i;
    CHAR Sequence[200];

    for (i = 0; i < ARRAYSIZE(SgrTests); ++i)
    {
        CONSOLE_SCREEN_BUFFER_INFO Now;

        Reset(VT_MODE);
        SetConsoleTextAttribute(hCon, SgrTests[i].Init);
        StringCbPrintfA(Sequence, sizeof(Sequence), "\x1b[%smX", SgrTests[i].Params);
        Write(Sequence);
        Now = BufferInfo();
        ok(Now.wAttributes == SgrTests[i].Expected, "SGR [%s] from 0x%02x: attribute 0x%04x, expected 0x%04x\n",
           SgrTests[i].Params, SgrTests[i].Init, Now.wAttributes, SgrTests[i].Expected);
        ok(Now.dwCursorPosition.X == 1, "SGR [%s]: cursor at %d\n", SgrTests[i].Params, Now.dwCursorPosition.X);
        if (SgrTests[i].CheckCell)
        {
            ok(AttrAt(0, 0) == SgrTests[i].Expected, "SGR [%s] from 0x%02x: cell 0x%04x, expected 0x%04x\n",
               SgrTests[i].Params, SgrTests[i].Init, AttrAt(0, 0), SgrTests[i].Expected);
        }
        ok(CharAt(0, 0) == L'X', "SGR [%s]: cell 0 is U+%04x\n", SgrTests[i].Params, CharAt(0, 0));
    }

    /* SetConsoleTextAttribute replaces the bold rendition and the flags */
    Reset(VT_MODE);
    Write("\x1b[1m");
    SetConsoleTextAttribute(hCon, 0x02);
    ok(BufferInfo().wAttributes == 0x02, "Bold, then 0x02: 0x%x\n", BufferInfo().wAttributes);
    Write("\x1b[22m");
    ok(BufferInfo().wAttributes == 0x02, "Then SGR 22: 0x%x\n", BufferInfo().wAttributes);

    Reset(VT_MODE);
    SetConsoleTextAttribute(hCon, 0x02);
    Write("\x1b[1m");
    ok(BufferInfo().wAttributes == 0x0A, "0x02, then bold: 0x%x\n", BufferInfo().wAttributes);
    Write("\x1b[22m");
    ok(BufferInfo().wAttributes == 0x02, "Then SGR 22: 0x%x\n", BufferInfo().wAttributes);
    Write("\x1b[1m\x1b[94m");
    ok(BufferInfo().wAttributes == 0x09, "Then 1, 94: 0x%x\n", BufferInfo().wAttributes);
    Write("\x1b[22m");
    ok(BufferInfo().wAttributes == 0x09, "Then 22: 0x%x\n", BufferInfo().wAttributes);

    Reset(VT_MODE);
    Write("\x1b[1;34m");
    ok(BufferInfo().wAttributes == 0x09, "1;34: 0x%x\n", BufferInfo().wAttributes);
    Write("\x1b[39m");
    ok(BufferInfo().wAttributes == 0x0F, "Then 39: 0x%x\n", BufferInfo().wAttributes);
    Write("\x1b[22m");
    ok(BufferInfo().wAttributes == 0x07, "Then 22: 0x%x\n", BufferInfo().wAttributes);

    Reset(VT_MODE);
    SetConsoleTextAttribute(hCon, 0x4007);
    Write("\x1b[27m");
    ok(BufferInfo().wAttributes == 0x07, "0x4007, then 27: 0x%x\n", BufferInfo().wAttributes);

    /* The 8-bit CSI */
    Reset(VT_MODE);
    {
        DWORD Written;
        WriteConsoleW(hCon, L"\x9b" L"31mX", 5, &Written, NULL);
    }
    ok(CharAt(0, 0) == L'X' && AttrAt(0, 0) == 0x04, "C1 CSI: U+%04x 0x%x\n", CharAt(0, 0), AttrAt(0, 0));
    CheckCursor("C1 CSI", 1, 0);
}

static const struct
{
    PCSTR Params;
    SHORT X, Y;
} MoveTests[] =
{
    { "A", 10, 9 }, { "3A", 10, 7 }, { "0A", 10, 9 }, { "20A", 10, 0 },
    { "B", 10, 11 }, { "3B", 10, 13 }, { "30B", 10, 24 },
    { "C", 11, 10 }, { "5C", 15, 10 }, { "100C", 79, 10 },
    { "D", 9, 10 }, { "5D", 5, 10 }, { "20D", 0, 10 },
    { "5;7H", 6, 4 }, { "H", 0, 0 }, { "5H", 0, 4 }, { ";7H", 6, 0 }, { "100;100H", 79, 24 }, { "0;0H", 0, 0 },
    { "7G", 6, 10 }, { "100G", 79, 10 }, { "G", 0, 10 },
    { "4d", 10, 3 }, { "100d", 10, 24 },
    { "5;7f", 6, 4 },
    { "2E", 0, 12 }, { "2F", 0, 8 }, { "E", 0, 11 }, { "F", 0, 9 },
};

static VOID
TestCursorMoves(VOID)
{
    UINT i;
    CHAR Sequence[64];

    for (i = 0; i < ARRAYSIZE(MoveTests); ++i)
    {
        Reset(VT_MODE);
        MoveCursor(10, 10);
        StringCbPrintfA(Sequence, sizeof(Sequence), "\x1b[%s", MoveTests[i].Params);
        Write(Sequence);
        StringCbPrintfA(Sequence, sizeof(Sequence), "CSI %s from 10,10", MoveTests[i].Params);
        CheckCursor(Sequence, MoveTests[i].X, MoveTests[i].Y);
        CheckView(Sequence, 0);
    }

    /* Simple cursor positioning */
    Reset(VT_MODE);
    MoveCursor(5, 5);
    Write("\x1b" "A");
    CheckCursor("ESC A", 5, 4);
    Write("\x1b" "B\x1b" "B");
    CheckCursor("ESC B twice", 5, 6);
    Write("\x1b" "C");
    CheckCursor("ESC C", 6, 6);
    Write("\x1b" "D\x1b" "D");
    CheckCursor("ESC D twice", 4, 6);

    /* Positions are relative to the view, which the cursor does not leave */
    Reset(VT_MODE);
    SetView(50);
    MoveCursor(10, 60);
    Write("\x1b[1;1H");
    CheckCursor("CUP 1;1, view at 50", 0, 50);
    MoveCursor(10, 60);
    Write("\x1b[100A");
    CheckCursor("CUU 100, view at 50", 10, 50);
    MoveCursor(10, 60);
    Write("\x1b[100B");
    CheckCursor("CUD 100, view at 50", 10, 74);
    MoveCursor(10, 60);
    Write("\x1b[3d");
    CheckCursor("VPA 3, view at 50", 10, 52);
    CheckView("View at 50", 50);
}

static VOID
TestErase(VOID)
{
    static const struct
    {
        PCSTR Params;
        PCSTR Row4, Row5, Row6;
        PCSTR Row0, Row24;
        WORD Attr45, Attr795;
    } Tests[] =
    {
        { "K",    "exxxxx", "0123........", "gxxxxx", "axxx", "yxxx", 0x17, 0x17 },
        { "1K",   "exxxxx", ".....56789xx", "gxxxxx", "axxx", "yxxx", 0x17, 0x07 },
        { "2K",   "exxxxx", "............", "gxxxxx", "axxx", "yxxx", 0x17, 0x17 },
        { "J",    "exxxxx", "0123........", "......", "axxx", "....", 0x17, 0x17 },
        { "1J",   "......", ".....56789xx", "gxxxxx", "....", "yxxx", 0x17, 0x07 },
        { "3X",   "exxxxx", "0123...789xx", "gxxxxx", "axxx", "yxxx", 0x17, 0x07 },
        { "X",    "exxxxx", "0123.56789xx", "gxxxxx", "axxx", "yxxx", 0x17, 0x07 },
        { "100X", "exxxxx", "0123........", "gxxxxx", "axxx", "yxxx", 0x17, 0x17 },
        { "2@",   "exxxxx", "0123..456789", "gxxxxx", "axxx", "yxxx", 0x17, 0x07 },
        { "100@", "exxxxx", "0123........", "gxxxxx", "axxx", "yxxx", 0x17, 0x17 },
        { "2P",   "exxxxx", "01236789xxxx", "gxxxxx", "axxx", "yxxx", 0x07, 0x17 },
        { "2S",   "gxxxxx", "hxxxxx",       "ixxxxx", "cxxx", "....", 0x07, 0x07 },
        { "2T",   "cxxxxx", "dxxxxx",       "exxxxx", "....", "wxxx", 0x07, 0x07 },
    };
    UINT i;
    CHAR Sequence[64];

    for (i = 0; i < ARRAYSIZE(Tests); ++i)
    {
        COORD Origin = { 0, 0 };
        DWORD Written;
        SHORT Y;

        Reset(VT_MODE);
        FillConsoleOutputCharacterW(hCon, L'x', WIDTH * HEIGHT, Origin, &Written);
        for (Y = 0; Y < VIEW; ++Y)
        {
            COORD Pos = { 0, Y };
            FillConsoleOutputCharacterW(hCon, L'a' + Y, 1, Pos, &Written);
        }
        PutText(5, "0123456789");
        MoveCursor(4, 5);
        Write("\x1b[44m");
        StringCbPrintfA(Sequence, sizeof(Sequence), "\x1b[%s", Tests[i].Params);
        Write(Sequence);

        StringCbPrintfA(Sequence, sizeof(Sequence), "CSI %s", Tests[i].Params);
        CheckCursor(Sequence, 4, 5);
        CheckRow(Sequence, 4, Tests[i].Row4);
        CheckRow(Sequence, 5, Tests[i].Row5);
        CheckRow(Sequence, 6, Tests[i].Row6);
        CheckRow(Sequence, 0, Tests[i].Row0);
        CheckRow(Sequence, 24, Tests[i].Row24);
        CheckRow(Sequence, 25, "xxxx");
        ok(AttrAt(4, 5) == Tests[i].Attr45, "%s: attribute at 4,5 0x%x, expected 0x%x\n",
           Sequence, AttrAt(4, 5), Tests[i].Attr45);
        ok(AttrAt(79, 5) == Tests[i].Attr795, "%s: attribute at 79,5 0x%x, expected 0x%x\n",
           Sequence, AttrAt(79, 5), Tests[i].Attr795);
    }
}

static VOID
TestEraseDisplay(VOID)
{
    /* ED 2 keeps the text in the scrollback: the view moves below the last line with text */
    Reset(VT_MODE);
    PutText(0, "A"); PutText(1, "B"); PutText(2, "C");
    MoveCursor(2, 3);
    Write("\x1b[2J");
    CheckView("ED 2, text on rows 0-2", 3);
    CheckCursor("ED 2, text on rows 0-2", 2, 6);
    CheckColumn("ED 2, text on rows 0-2", 0, "ABC....");

    Reset(VT_MODE);
    PutText(10, "T");
    MoveCursor(2, 3);
    Write("\x1b[2J");
    CheckView("ED 2, text on row 10", 11);
    CheckCursor("ED 2, text on row 10", 2, 14);

    Reset(VT_MODE);
    MoveCursor(2, 3);
    Write("\x1b[2J");
    CheckView("ED 2, blank buffer", 1);
    CheckCursor("ED 2, blank buffer", 2, 4);

    Reset(VT_MODE);
    PutText(0, "A"); PutText(30, "Z");
    SetView(20);
    MoveCursor(5, 25);
    Write("\x1b[2J");
    CheckView("ED 2, view at 20", 31);
    CheckCursor("ED 2, view at 20", 5, 36);

    /* Near the end of the buffer, the buffer scrolls */
    Reset(VT_MODE);
    PutText(0, "A"); PutText(90, "Z");
    MoveCursor(5, 3);
    Write("\x1b[2J");
    CheckView("ED 2, text on row 90", 75);
    CheckCursor("ED 2, text on row 90", 5, 78);
    ok(CharAt(0, 74) == L'Z' && CharAt(0, 0) == L' ', "ED 2, text on row 90: rows 74 and 0 are U+%04x U+%04x\n",
       CharAt(0, 74), CharAt(0, 0));

    /* The view is erased with the current attribute */
    Reset(VT_MODE);
    PutText(0, "A"); PutText(3, "B");
    MoveCursor(5, 3);
    Write("\x1b[44m\x1b[2J");
    CheckView("ED 2 blue", 4);
    CheckCursor("ED 2 blue", 5, 7);
    ok(AttrAt(0, 4) == 0x17 && AttrAt(0, 28) == 0x17 && AttrAt(0, 29) == 0x07 && AttrAt(0, 3) == 0x07,
       "ED 2 blue: attributes 0x%x 0x%x 0x%x 0x%x\n", AttrAt(0, 4), AttrAt(0, 28), AttrAt(0, 29), AttrAt(0, 3));

    /* ED 3 erases the scrollback */
    Reset(VT_MODE);
    PutText(0, "A"); PutText(3, "B");
    MoveCursor(5, 3);
    Write("\x1b[2J\x1b[3J");
    CheckView("ED 2 + ED 3", 0);
    CheckCursor("ED 2 + ED 3", 5, 3);
    CheckColumn("ED 2 + ED 3", 0, "......");

    Reset(VT_MODE);
    PutText(0, "A"); PutText(3, "B"); PutText(12, "C");
    SetView(10);
    MoveCursor(5, 15);
    Write("\x1b[3J");
    CheckView("ED 3, view at 10", 0);
    CheckCursor("ED 3, view at 10", 5, 5);
    CheckColumn("ED 3, view at 10", 0, "..C.....");
}

static VOID
TestScrollingRegion(VOID)
{
    Reset(VT_MODE);
    PutLetters();
    Write("\x1b[5;10r");
    CheckCursor("DECSTBM 5;10", 0, 0);
    Write("\x1b[10;1H\n");
    CheckCursor("LF at the bottom margin", 0, 9);
    CheckColumn("LF at the bottom margin", 0, "abcdfghij.klm");
    Write("\x1b[5;1H\x1bM");
    CheckCursor("RI at the top margin", 0, 4);
    CheckColumn("RI at the top margin", 0, "abcd.fghijklm");
    Write("\x1b[7;1H\x1b[10A");
    CheckCursor("CUU 10 in the region", 0, 4);
    Write("\x1b[7;1H\x1b[10B");
    CheckCursor("CUD 10 in the region", 0, 9);
    Write("\x1b[20;1H\x1b[5A");
    CheckCursor("CUU 5 below the region", 0, 14);
    Write("\x1b[2S");
    CheckColumn("SU 2 in the region", 0, "abcdghij..klm");
    Write("\x1b[r");
    CheckCursor("DECSTBM reset", 0, 0);

    /* Without a region, RI at the top of the view scrolls the view down */
    Reset(VT_MODE);
    PutLetters();
    Write("\x1b[1;1H\x1bM");
    CheckCursor("RI at the top", 0, 0);
    CheckColumn("RI at the top", 0, ".abcdefghijklmnopqrstuvwxz");
}

static VOID
TestSaveRestore(VOID)
{
    /* Only the position is restored */
    Reset(VT_MODE);
    MoveCursor(3, 4);
    Write("\x1b[31m\x1b" "7\x1b[32m\x1b[10;10H\x1b" "8X");
    CheckCursor("DECSC / DECRC", 4, 4);
    ok(CharAt(3, 4) == L'X' && AttrAt(3, 4) == 0x02, "DECRC: U+%04x 0x%x\n", CharAt(3, 4), AttrAt(3, 4));

    Reset(VT_MODE);
    MoveCursor(3, 4);
    Write("\x1b[31m\x1b[s\x1b[32m\x1b[10;10H\x1b[uX");
    CheckCursor("CSI s / CSI u", 4, 4);
    ok(CharAt(3, 4) == L'X' && AttrAt(3, 4) == 0x02, "CSI u: U+%04x 0x%x\n", CharAt(3, 4), AttrAt(3, 4));

    /* The saved position is relative to the view */
    Reset(VT_MODE);
    SetView(10);
    MoveCursor(7, 15);
    Write("\x1b" "7");
    SetView(0);
    MoveCursor(1, 1);
    Write("\x1b" "8");
    CheckCursor("DECRC after the view moved", 7, 5);
}

static VOID
TestCursorVisibility(VOID)
{
    CONSOLE_CURSOR_INFO Cursor;

    Reset(VT_MODE);
    Write("\x1b[?25l");
    ok(GetConsoleCursorInfo(hCon, &Cursor) && !Cursor.bVisible, "?25l: cursor visible %d\n", Cursor.bVisible);
    Write("\x1b[?25h");
    ok(GetConsoleCursorInfo(hCon, &Cursor) && Cursor.bVisible, "?25h: cursor visible %d\n", Cursor.bVisible);
}

static VOID
TestTitle(VOID)
{
    CHAR Original[1024], Title[1024];

    Original[0] = 0;
    GetConsoleTitleA(Original, sizeof(Original));

    Reset(VT_MODE);
    Write("\x1b]0;Hello VT\a");
    GetConsoleTitleA(Title, sizeof(Title));
    ok(!lstrcmpA(Title, "Hello VT"), "OSC 0: title '%s'\n", Title);
    CheckCursor("OSC 0", 0, 0);

    Write("\x1b]2;Two\x1b\\");
    GetConsoleTitleA(Title, sizeof(Title));
    ok(!lstrcmpA(Title, "Two"), "OSC 2 with ST: title '%s'\n", Title);

    Write("\x1b]1;One\a");
    GetConsoleTitleA(Title, sizeof(Title));
    ok(!lstrcmpA(Title, "One"), "OSC 1: title '%s'\n", Title);

    Write("\x1b]0;Sp");
    Write("lit\a");
    GetConsoleTitleA(Title, sizeof(Title));
    ok(!lstrcmpA(Title, "Split"), "Split OSC 0: title '%s'\n", Title);

    Write("\x1b]0;\a");
    GetConsoleTitleA(Title, sizeof(Title));
    ok(!lstrcmpA(Title, "Split"), "Empty OSC 0: title '%s'\n", Title);

    /* Other OSC (hyperlink) are swallowed */
    Write("\x1b]8;;http://x\x1b\\L\x1b]8;;\x1b\\");
    CheckCursor("OSC 8", 1, 0);
    ok(CharAt(0, 0) == L'L', "OSC 8: cell 0 is U+%04x\n", CharAt(0, 0));

    SetConsoleTitleA(Original);
}

static VOID
TestSplitSequences(VOID)
{
    Reset(VT_MODE);
    Write("\x1b[3");
    Write("1mX");
    ok(CharAt(0, 0) == L'X' && AttrAt(0, 0) == 0x04, "Split CSI: U+%04x 0x%x\n", CharAt(0, 0), AttrAt(0, 0));
    CheckCursor("Split CSI", 1, 0);

    Reset(VT_MODE);
    Write("\x1b");
    Write("[32mY");
    ok(CharAt(0, 0) == L'Y' && AttrAt(0, 0) == 0x02, "Split after ESC: U+%04x 0x%x\n", CharAt(0, 0), AttrAt(0, 0));

    Reset(VT_MODE);
    Write("\x1b[");
    Write("4");
    Write(";");
    Write("1");
    Write("H");
    CheckCursor("CUP in 5 writes", 0, 3);

    /* A sequence does not continue once the mode is off */
    Reset(VT_MODE);
    Write("\x1b");
    SetConsoleMode(hCon, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT);
    Write("Q");
    SetConsoleMode(hCon, VT_MODE);
    Write("Z");
    ok(CharAt(0, 0) == L'Q' && CharAt(1, 0) == L'Z', "Mode change in a sequence: U+%04x U+%04x\n",
       CharAt(0, 0), CharAt(1, 0));
}

static VOID
TestLineFeed(VOID)
{
    static const struct { DWORD Mode; SHORT X, Y; } Tests[] =
    {
        { 0x3, 0, 3 }, { 0x7, 0, 3 }, { 0xF, 5, 3 }, { 0xB, 5, 3 }, { 0xD, 5, 3 }, { 0x5, 0, 3 },
    };
    UINT i;
    CHAR What[64];

    for (i = 0; i < ARRAYSIZE(Tests); ++i)
    {
        Reset(Tests[i].Mode);
        MoveCursor(5, 2);
        Write("\n");
        StringCbPrintfA(What, sizeof(What), "LF in mode 0x%lx", Tests[i].Mode);
        CheckCursor(What, Tests[i].X, Tests[i].Y);
    }
}

static VOID
Write80(VOID)
{
    CHAR Line[81];
    UINT i;

    for (i = 0; i < 80; ++i) Line[i] = 'a';
    Line[80] = 0;
    MoveCursor(0, 2);
    Write(Line);
}

static VOID
TestWrap(VOID)
{
    static const struct { DWORD Mode; SHORT X, Y; } Tests[] =
    {
        { 0x3, 0, 3 }, { 0xB, 0, 3 }, { 0x7, 79, 2 }, { 0xF, 79, 2 }, { 0x5, 79, 2 },
    };
    UINT i;
    CHAR What[64];

    for (i = 0; i < ARRAYSIZE(Tests); ++i)
    {
        Reset(Tests[i].Mode);
        Write80();
        StringCbPrintfA(What, sizeof(What), "80 characters in mode 0x%lx", Tests[i].Mode);
        CheckCursor(What, Tests[i].X, Tests[i].Y);
        Write("b");
        ok(CharAt(0, 3) == L'b' && CharAt(79, 2) == L'a', "%s, then b: U+%04x U+%04x\n", What,
           CharAt(0, 3), CharAt(79, 2));
        CheckCursor(What, 1, 3);
    }

    /* The pending wrap and the controls */
    Reset(VT_MODE);
    Write80();
    Write("\n");
    CheckCursor("Pending wrap, LF", 0, 3);

    Reset(VT_MODE | DISABLE_NEWLINE_AUTO_RETURN);
    Write80();
    Write("\n");
    CheckCursor("Pending wrap, LF without CR", 79, 3);

    Reset(VT_MODE);
    Write80();
    Write("\r");
    CheckCursor("Pending wrap, CR", 0, 2);

    Reset(VT_MODE);
    Write80();
    Write("\x1b[D");
    CheckCursor("Pending wrap, CUB", 78, 2);

    Reset(VT_MODE);
    Write80();
    Write("\x1b[31m");
    CheckCursor("Pending wrap, SGR", 79, 2);
    Write("b");
    CheckCursor("Pending wrap, SGR, b", 1, 3);

    Reset(VT_MODE);
    Write80();
    MoveCursor(10, 10);
    Write("b");
    ok(CharAt(10, 10) == L'b', "Pending wrap, SetConsoleCursorPosition: U+%04x\n", CharAt(10, 10));

    /* Backspace and tab only move the cursor */
    Reset(VT_MODE);
    Write("AB\b");
    CheckCursor("AB BS", 1, 0);
    ok(CharAt(1, 0) == L'B', "AB BS: U+%04x\n", CharAt(1, 0));
    Reset(VT_MODE);
    MoveCursor(0, 3);
    Write("\b");
    CheckCursor("BS in column 0", 0, 3);
    Reset(VT_MODE);
    MoveCursor(78, 1);
    Write("\t");
    CheckCursor("TAB in column 78", 79, 1);
}

static VOID
TestOtherSequences(VOID)
{
    static const struct
    {
        PCSTR Text;
        SHORT X, Y;
        PCSTR Row3;
        WORD Attr;
    } Tests[] =
    {
        { "\x1b[5;5xZ",          3, 3, "..Z.." },   /* unknown CSI */
        { "\x1b" "ZZ",           3, 3, "..Z.." },   /* unknown ESC */
        { "\x1b[#Z",             2, 3, "....." },   /* intermediate, then final Z */
        { "\x1b#8Z",             3, 3, "..Z.." },
        { "\x1b\x1b[31mZ",       3, 3, "..Z..", 0x04 },
        { "\x1b[3\x1b[32mZ",     3, 3, "..Z..", 0x02 },
        { "\x1b[?12lZ",          3, 3, "..Z.." },
        { "\x1b[1 qZ",           3, 3, "..Z.." },
        { "\x1b[!pZ",            3, 3, "..Z.." },
    };
    UINT i;
    CHAR What[64];

    for (i = 0; i < ARRAYSIZE(Tests); ++i)
    {
        Reset(VT_MODE);
        MoveCursor(2, 3);
        Write(Tests[i].Text);
        StringCbPrintfA(What, sizeof(What), "Sequence %u", i);
        CheckCursor(What, Tests[i].X, Tests[i].Y);
        CheckRow(What, 3, Tests[i].Row3);
        if (Tests[i].Attr)
            ok(AttrAt(2, 3) == Tests[i].Attr, "%s: attribute 0x%x\n", What, AttrAt(2, 3));
    }

    /* A control character in a sequence is executed */
    Reset(VT_MODE);
    MoveCursor(2, 3);
    Write("\x1b[31\nmZ");
    CheckCursor("LF in CSI", 1, 4);
    ok(CharAt(0, 4) == L'Z' && AttrAt(0, 4) == 0x04, "LF in CSI: U+%04x 0x%x\n", CharAt(0, 4), AttrAt(0, 4));

    /* DEC special graphics */
    Reset(VT_MODE);
    Write("\x1b(0qx\x1b(BZ");
    ok(CharAt(0, 0) == 0x2500 && CharAt(1, 0) == 0x2502 && CharAt(2, 0) == L'Z',
       "DEC graphics: U+%04x U+%04x U+%04x\n", CharAt(0, 0), CharAt(1, 0), CharAt(2, 0));

    /* REP */
    Reset(VT_MODE);
    Write("x\x1b[3b");
    CheckCursor("REP 3", 4, 0);
    CheckRow("REP 3", 0, "xxxx.");

    /* Alternate screen buffer, of the size of the view */
    Reset(VT_MODE);
    PutText(0, "main");
    MoveCursor(3, 3);
    Write("\x1b[31m\x1b[?1049h");
    {
        CONSOLE_SCREEN_BUFFER_INFO i = BufferInfo();
        ok(i.dwSize.X == WIDTH && i.dwSize.Y == VIEW, "Alternate buffer: size %dx%d\n", i.dwSize.X, i.dwSize.Y);
        ok(i.wAttributes == 0x04, "Alternate buffer: attribute 0x%x\n", i.wAttributes);
    }
    CheckCursor("?1049h", 0, 0);
    Write("alt");
    CheckRow("Alternate buffer", 0, "alt.");
    Write("\x1b[?1049l");
    {
        CONSOLE_SCREEN_BUFFER_INFO i = BufferInfo();
        ok(i.dwSize.X == WIDTH && i.dwSize.Y == HEIGHT, "Main buffer again: size %dx%d\n", i.dwSize.X, i.dwSize.Y);
    }
    CheckCursor("?1049l", 3, 3);
    CheckRow("Main buffer again", 0, "main");

    /* The written count is that of the whole string */
    Reset(VT_MODE);
    {
        DWORD Written = 0;
        ok(WriteFile(hCon, "\x1b[33mEF", 7, &Written, NULL) && Written == 7, "WriteFile: %lu written\n", Written);
        ok(AttrAt(0, 0) == 0x06, "WriteFile: attribute 0x%x\n", AttrAt(0, 0));
        Written = 0;
        ok(WriteConsoleW(hCon, L"\x1b[32mCD", 7, &Written, NULL) && Written == 7, "WriteConsoleW: %lu written\n", Written);
    }

    /* The new line at the bottom takes the current attribute */
    Reset(VT_MODE);
    MoveCursor(0, HEIGHT - 1);
    Write("\x1b[44mx\n");
    ok(AttrAt(5, HEIGHT - 1) == 0x17, "Scrolled-in line: attribute 0x%x\n", AttrAt(5, HEIGHT - 1));
}

/* A multibyte ANSI string writes the characters it converts to, sequences included */
static VOID
TestUtf8(VOID)
{
    UINT OldCP = GetConsoleOutputCP();

    if (!SetConsoleOutputCP(CP_UTF8))
    {
        skip("SetConsoleOutputCP(CP_UTF8) failed with error %lu\n", GetLastError());
        return;
    }
    Reset(VT_MODE);
    Write("\xe2\x94\x80\x1b[31m\xc3\xa9Z");
    CheckCursor("UTF-8", 3, 0);
    ok(CharAt(0, 0) == 0x2500 && CharAt(1, 0) == 0x00E9 && CharAt(2, 0) == L'Z' && CharAt(3, 0) == L' ',
       "UTF-8: U+%04x U+%04x U+%04x U+%04x\n", CharAt(0, 0), CharAt(1, 0), CharAt(2, 0), CharAt(3, 0));
    ok(AttrAt(1, 0) == 0x04, "UTF-8: attribute 0x%x\n", AttrAt(1, 0));
    SetConsoleOutputCP(OldCP);
}

/* C1 controls, ESC D / ESC E and strings, as measured on Windows 10 */
static VOID
TestControls(VOID)
{
    CHAR Title[256], Original[1024];
    DWORD Written;

    Original[0] = 0;
    GetConsoleTitleA(Original, sizeof(Original));

    /* ESC D moves left (not IND), ESC E does nothing (not NEL), also on the bottom margin */
    Reset(VT_MODE);
    MoveCursor(5, 5);
    Write("\x1b" "D");
    CheckCursor("ESC D", 4, 5);
    Write("\x1b" "E");
    CheckCursor("ESC E", 4, 5);
    Reset(VT_MODE);
    MoveCursor(5, 24);
    Write("x\x1b" "D");
    CheckCursor("ESC D at the bottom", 5, 24);
    CheckView("ESC D at the bottom", 0);
    Write("\x1b[5;10r\x1b[10;3H\x1b" "D\x1b" "E");
    CheckCursor("ESC D and ESC E on the bottom margin", 1, 9);

    /* U+009C ends an OSC string; U+009D and a lone U+009C are text */
    Reset(VT_MODE);
    WriteConsoleW(hCon, L"\x1b]0;T1\x9c" L"Y", 8, &Written, NULL);
    GetConsoleTitleA(Title, sizeof(Title));
    ok(!lstrcmpA(Title, "T1"), "OSC ended by U+009C: title '%s'\n", Title);
    ok(CharAt(0, 0) == L'Y', "OSC ended by U+009C: U+%04x\n", CharAt(0, 0));
    Reset(VT_MODE);
    WriteConsoleW(hCon, L"\x9d" L"0;T2\x07Z", 7, &Written, NULL);
    CheckCursor("U+009D", 6, 0);
    ok(CharAt(0, 0) == 0x9D, "U+009D: U+%04x\n", CharAt(0, 0));
    Reset(VT_MODE);
    WriteConsoleW(hCon, L"a\x9c" L"b", 3, &Written, NULL);
    CheckCursor("Lone U+009C", 3, 0);
    /* A C1 control in a sequence cancels it and is dropped */
    Reset(VT_MODE);
    WriteConsoleW(hCon, L"\x1b[3\x9b" L"32mQ", 8, &Written, NULL);
    CheckRow("U+009B in CSI", 0, "32mQ.");
    ok(AttrAt(0, 0) == 0x07, "U+009B in CSI: attribute 0x%x\n", AttrAt(0, 0));

    /* ESC P does not start a string */
    Reset(VT_MODE);
    Write("\x1bP1;2|abc");
    CheckCursor("ESC P", 7, 0);
    CheckRow("ESC P", 0, "1;2|abc.");

    /* An OSC string goes on over several writes until BEL or ST */
    Reset(VT_MODE);
    Write("\x1b]0;abc");
    Write("def");
    Write("\a");
    GetConsoleTitleA(Title, sizeof(Title));
    ok(!lstrcmpA(Title, "abcdef"), "OSC over three writes: title '%s'\n", Title);
    CheckCursor("OSC over three writes", 0, 0);

    /* Unterminated, it swallows the text; CAN cancels it and is output */
    Reset(VT_MODE);
    Write("\x1b]0;unterminated");
    Write("hello");
    CheckCursor("Unterminated OSC", 0, 0);
    Write("\x18" "after");
    CheckCursor("Unterminated OSC, CAN", 6, 0);
    CheckRow("Unterminated OSC, CAN", 0, "?after.");
    GetConsoleTitleA(Title, sizeof(Title));
    ok(!lstrcmpA(Title, "abcdef"), "Cancelled OSC: title '%s'\n", Title);
    Reset(VT_MODE);
    Write("\x1b]0;abc\x1a" "Z");
    CheckCursor("OSC, SUB", 2, 0);
    ok(CharAt(1, 0) == L'Z', "OSC, SUB: U+%04x\n", CharAt(1, 0));
    Reset(VT_MODE);
    Write("\x1b]0;abc\x1b" "ZQ");
    CheckRow("OSC, ESC Z", 0, "Q.");

    SetConsoleTitleA(Original);
}

/* Hostile parameters do not take the buffer out of shape */
static VOID
TestAdversarial(VOID)
{
    static const PCSTR Counts[] = { "32767", "99999999999" };
    UINT i;
    CHAR Sequence[64];
    CONSOLE_SCREEN_BUFFER_INFO Now;

    /* More parameters than the function takes: ignored */
    Reset(VT_MODE);
    MoveCursor(10, 10);
    Write("\x1b[3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20H");
    CheckCursor("CUP with 18 parameters", 10, 10);
    Write("\x1b[3;4;5H");
    CheckCursor("CUP with 3 parameters", 10, 10);
    Write("\x1b[3;4;;H");
    CheckCursor("CUP with 4 parameters", 10, 10);
    Write("\x1b[5;6A\x1b[5;B\x1b[5;6G\x1b[0;1K");
    CheckCursor("CUU, CUD, CHA with 2 parameters", 10, 10);
    Write("\x1b[2;3f");
    CheckCursor("HVP 2;3", 2, 1);
    Reset(VT_MODE);
    Write("\x1b[99999999999H");
    CheckCursor("CUP 99999999999", 0, 24);
    Write("\x1b[99999999999;99999999999H");
    CheckCursor("CUP 99999999999;99999999999", 79, 24);
    Write("\x1b[99999999999A");
    CheckCursor("CUU 99999999999", 79, 0);
    Write("\x1b[99999999999G");
    CheckCursor("CHA 99999999999", 79, 0);
    CheckView("Huge positions", 0);

    for (i = 0; i < ARRAYSIZE(Counts); ++i)
    {
        static const PCSTR Finals[] = { "@", "P", "X", "S" };
        UINT j;

        for (j = 0; j < ARRAYSIZE(Finals); ++j)
        {
            Reset(VT_MODE);
            PutLetters();
            PutText(5, "f123456789");
            MoveCursor(3, 5);
            StringCbPrintfA(Sequence, sizeof(Sequence), "x\x1b[%s%s", Counts[i], Finals[j]);
            Write(Sequence);
            Now = BufferInfo();
            StringCbPrintfA(Sequence, sizeof(Sequence), "CSI %s%s", Counts[i], Finals[j]);
            ok(Now.dwSize.X == WIDTH && Now.dwSize.Y == HEIGHT, "%s: size %dx%d\n", Sequence, Now.dwSize.X, Now.dwSize.Y);
            CheckCursor(Sequence, 4, 5);
            CheckView(Sequence, 0);
            CheckColumn(Sequence, 25, "z");
            if (Finals[j][0] == 'S')
            {
                CheckColumn(Sequence, 0, ".........................z");
            }
            else
            {
                CheckColumn(Sequence, 3, "defg");
                ok(CharAt(3, 5) == L'x', "%s: U+%04x at 3,5\n", Sequence, CharAt(3, 5));
                if (Finals[j][0] != '@')
                    CheckRow(Sequence, 5, "f12x......");
            }
        }
    }

    /* Windows 10 loses the console with these (IL, DL, SD of 32767 lines): only on ReactOS */
    if (is_reactos())
    {
        static const PCSTR Finals[] = { "L", "M", "T", "b" };
        UINT j;

        for (i = 0; i < ARRAYSIZE(Counts); ++i)
        {
            for (j = 0; j < ARRAYSIZE(Finals); ++j)
            {
                Reset(VT_MODE);
                PutLetters();
                MoveCursor(3, 5);
                StringCbPrintfA(Sequence, sizeof(Sequence), "x\x1b[%s%s", Counts[i], Finals[j]);
                Write(Sequence);
                Now = BufferInfo();
                StringCbPrintfA(Sequence, sizeof(Sequence), "CSI %s%s", Counts[i], Finals[j]);
                ok(Now.dwSize.X == WIDTH && Now.dwSize.Y == HEIGHT, "%s: size %dx%d\n", Sequence, Now.dwSize.X, Now.dwSize.Y);
                ok(Now.dwCursorPosition.X < WIDTH && Now.dwCursorPosition.Y >= Now.srWindow.Top &&
                   Now.dwCursorPosition.Y <= Now.srWindow.Bottom, "%s: cursor at %d,%d\n",
                   Sequence, Now.dwCursorPosition.X, Now.dwCursorPosition.Y);
                if (Finals[j][0] == 'T')
                    CheckColumn(Sequence, 0, ".........................z");
                else if (Finals[j][0] != 'b')
                {
                    CheckColumn(Sequence, 0, "abcde");
                    CheckColumn(Sequence, 25, "z");
                }
            }
        }
    }
    else
    {
        skip("IL, DL, SD with huge counts are not tested on Windows (its console exits)\n");
    }

    /* The alternate buffer resized, then left, then a buffer closed in it */
    Reset(VT_MODE);
    PutText(0, "main");
    Write("\x1b[?1049h");
    {
        COORD Size = { 100, 40 };
        SetConsoleScreenBufferSize(hCon, Size);
    }
    Write("\x1b[?1049l");
    Now = BufferInfo();
    ok(Now.dwSize.X == WIDTH && Now.dwSize.Y == HEIGHT, "Alternate buffer resized, then left: size %dx%d\n",
       Now.dwSize.X, Now.dwSize.Y);
    CheckRow("Alternate buffer resized, then left", 0, "main");
    {
        HANDLE hOther = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                  NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
        DWORD Written;

        ok(hOther != INVALID_HANDLE_VALUE, "CreateConsoleScreenBuffer failed with error %lu\n", GetLastError());
        if (hOther != INVALID_HANDLE_VALUE)
        {
            SetConsoleMode(hOther, VT_MODE);
            WriteConsoleA(hOther, "\x1b[?1049hx", 9, &Written, NULL);
            ok(CloseHandle(hOther), "Closing a buffer in its alternate screen failed\n");
        }
        hOther = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
        ok(hOther != INVALID_HANDLE_VALUE, "The console is gone after closing a buffer in its alternate screen\n");
        if (hOther != INVALID_HANDLE_VALUE)
            CloseHandle(hOther);
    }
}

/* The reports come back as typed characters */
static VOID
CheckReport(PCSTR What, PCSTR Expected)
{
    INPUT_RECORD Events[64];
    DWORD Count = 0, i;
    CHAR Report[64];
    UINT n = 0;

    if (!GetNumberOfConsoleInputEvents(hIn, &Count) || !Count)
    {
        ok(FALSE, "%s: no report\n", What);
        return;
    }
    ReadConsoleInputA(hIn, Events, ARRAYSIZE(Events), &Count);
    for (i = 0; i < Count && n < sizeof(Report) - 1; ++i)
    {
        if (Events[i].EventType == KEY_EVENT && Events[i].Event.KeyEvent.bKeyDown)
        {
            CHAR c = Events[i].Event.KeyEvent.uChar.AsciiChar;
            Report[n++] = (c == 0x1B) ? '^' : c;
            ok(Events[i].Event.KeyEvent.wVirtualKeyCode == 0, "%s: virtual key 0x%x\n", What,
               Events[i].Event.KeyEvent.wVirtualKeyCode);
        }
    }
    Report[n] = 0;
    ok(!lstrcmpA(Report, Expected), "%s: report '%s', expected '%s'\n", What, Report, Expected);
}

static VOID
TestReports(VOID)
{
    DWORD InMode = 0;

    if (hIn == INVALID_HANDLE_VALUE)
    {
        skip("No console input\n");
        return;
    }
    GetConsoleMode(hIn, &InMode);

    Reset(VT_MODE);
    MoveCursor(6, 4);
    FlushConsoleInputBuffer(hIn);
    Write("\x1b[6n");
    CheckReport("DSR 6", "^[5;7R");

    Reset(VT_MODE);
    SetView(10);
    MoveCursor(4, 12);
    FlushConsoleInputBuffer(hIn);
    Write("\x1b[6n");
    CheckReport("DSR 6, view at 10", "^[3;5R");

    Reset(VT_MODE);
    Write80();
    FlushConsoleInputBuffer(hIn);
    Write("\x1b[6n");
    CheckReport("DSR 6, pending wrap", "^[3;80R");

    Reset(VT_MODE);
    FlushConsoleInputBuffer(hIn);
    Write("\x1b[c");
    CheckReport("DA", "^[?1;0c");

    /* A flood of requests that nothing reads does not queue input without bound (Windows queues them all) */
    if (is_reactos())
    {
        DWORD Count = 0;
        UINT i;

        Reset(VT_MODE);
        FlushConsoleInputBuffer(hIn);
        for (i = 0; i < 5000; ++i)
            Write("\x1b[6n");
        GetNumberOfConsoleInputEvents(hIn, &Count);
        ok(Count >= 1000 && Count <= 1100, "5000 DSR requests: %lu input events\n", Count);
        FlushConsoleInputBuffer(hIn);
        Write("\x1b[c");
        CheckReport("DA after the flood", "^[?1;0c");
    }

    FlushConsoleInputBuffer(hIn);
    SetConsoleMode(hIn, InMode);
}

START_TEST(ConsoleVirtualTerminal)
{
    COORD Size = { WIDTH, HEIGHT };
    CONSOLE_SCREEN_BUFFER_INFO i;

    hCon = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
    if (hCon == INVALID_HANDLE_VALUE)
    {
        skip("No console (CreateConsoleScreenBuffer failed with error %lu)\n", GetLastError());
        return;
    }
    SetView(0);
    SetConsoleScreenBufferSize(hCon, Size);
    SetView(0);
    i = BufferInfo();
    if (i.dwSize.X != WIDTH || i.dwSize.Y != HEIGHT || i.srWindow.Top != 0 || i.srWindow.Bottom != VIEW - 1 ||
        i.srWindow.Right != WIDTH - 1)
    {
        skip("Could not set up an %dx%d buffer with an %dx%d view (%dx%d, %d..%d)\n",
             WIDTH, HEIGHT, WIDTH, VIEW, i.dwSize.X, i.dwSize.Y, i.srWindow.Top, i.srWindow.Bottom);
        CloseHandle(hCon);
        return;
    }
    hIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, 0, NULL);

    TestModes();
    TestNotInterpreted();
    TestGraphicsRendition();
    TestCursorMoves();
    TestErase();
    TestEraseDisplay();
    TestScrollingRegion();
    TestSaveRestore();
    TestCursorVisibility();
    TestTitle();
    TestSplitSequences();
    TestLineFeed();
    TestWrap();
    TestOtherSequences();
    TestUtf8();
    TestControls();
    TestAdversarial();
    TestReports();

    if (hIn != INVALID_HANDLE_VALUE)
        CloseHandle(hIn);
    CloseHandle(hCon);
}
