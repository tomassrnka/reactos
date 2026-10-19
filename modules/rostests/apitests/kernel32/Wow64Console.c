/* Console and shutdown-parameter calls of a 32-bit console program: screen buffer information
 * after output, input/output code pages, and process shutdown parameters (all CSR requests,
 * translated by WoW64 on 64-bit systems). SPDX-License-Identifier: LGPL-2.1-or-later */
#include "precomp.h"

/* Child failure bits, reported in the exit code */
#define CHILD_DONE          0x100
#define CHILD_INFO          0x001
#define CHILD_INFO_VALUES   0x002
#define CHILD_WRITE         0x004
#define CHILD_CURSOR        0x008
#define CHILD_GETCP         0x010
#define CHILD_SETCP         0x020
#define CHILD_SETCP_VALUE   0x040
#define CHILD_ATTRIBUTES    0x080

/* Runs in a new console of its own: output to the console moves the cursor that
 * GetConsoleScreenBufferInfo reports, as it does for cmd.exe and other console programs. */
static DWORD Child(void)
{
    HANDLE Out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO Before, After;
    DWORD Bits = 0, Written = 0;
    UINT InputCp, OutputCp;

    if (!GetConsoleScreenBufferInfo(Out, &Before))
        return CHILD_DONE | CHILD_INFO;
    if (Before.dwSize.X <= 0 || Before.dwSize.Y <= 0 ||
        Before.srWindow.Left < 0 || Before.srWindow.Top < 0 ||
        Before.srWindow.Right < Before.srWindow.Left || Before.srWindow.Bottom < Before.srWindow.Top ||
        Before.srWindow.Right >= Before.dwSize.X || Before.srWindow.Bottom >= Before.dwSize.Y ||
        Before.dwMaximumWindowSize.X <= 0 || Before.dwMaximumWindowSize.Y <= 0 ||
        Before.dwCursorPosition.X < 0 || Before.dwCursorPosition.X >= Before.dwSize.X ||
        Before.dwCursorPosition.Y < 0 || Before.dwCursorPosition.Y >= Before.dwSize.Y)
        Bits |= CHILD_INFO_VALUES;
    /* Start a line, then write a few characters without a line break */
    if (!WriteConsoleA(Out, "\r\nabcde", 7, &Written, NULL) || Written != 7)
        Bits |= CHILD_WRITE;
    if (!GetConsoleScreenBufferInfo(Out, &After))
        Bits |= CHILD_INFO;
    else
    {
        /* The line break scrolls the buffer instead when the cursor is on its last line */
        if (After.dwCursorPosition.X != 5 ||
            (After.dwCursorPosition.Y != Before.dwCursorPosition.Y + 1 &&
             Before.dwCursorPosition.Y + 1 < Before.dwSize.Y))
            Bits |= CHILD_CURSOR;
        if (After.wAttributes != Before.wAttributes || !After.wAttributes ||
            After.dwSize.X != Before.dwSize.X || After.dwSize.Y != Before.dwSize.Y)
            Bits |= CHILD_ATTRIBUTES;
    }

    InputCp = GetConsoleCP();
    OutputCp = GetConsoleOutputCP();
    if (!InputCp || !OutputCp)
        Bits |= CHILD_GETCP;
    /* Switching the output code page also asks the console for its language */
    if (!SetConsoleOutputCP(1252) || !SetConsoleCP(1252))
        Bits |= CHILD_SETCP;
    else if (GetConsoleOutputCP() != 1252 || GetConsoleCP() != 1252)
        Bits |= CHILD_SETCP_VALUE;
    if (OutputCp && InputCp &&
        (!SetConsoleOutputCP(OutputCp) || !SetConsoleCP(InputCp) ||
         GetConsoleOutputCP() != OutputCp || GetConsoleCP() != InputCp))
        Bits |= CHILD_SETCP_VALUE;
    return CHILD_DONE | Bits;
}

static void TestShutdownParameters(void)
{
    DWORD SavedLevel = 0xdead, SavedFlags = 0xdead, Level = 0xdead, Flags = 0xdead;
    BOOL Ret;

    Ret = GetProcessShutdownParameters(&SavedLevel, &SavedFlags);
    ok(Ret, "GetProcessShutdownParameters failed: %lu\n", GetLastError());
    ok(SavedLevel == 0x280, "Level %#lx\n", SavedLevel);
    ok(SavedFlags == 0, "Flags %#lx\n", SavedFlags);

    Ret = SetProcessShutdownParameters(0x3ff, SHUTDOWN_NORETRY);
    ok(Ret, "SetProcessShutdownParameters failed: %lu\n", GetLastError());
    Ret = GetProcessShutdownParameters(&Level, &Flags);
    ok(Ret, "GetProcessShutdownParameters failed: %lu\n", GetLastError());
    ok(Level == 0x3ff, "Level %#lx\n", Level);
    ok(Flags == SHUTDOWN_NORETRY, "Flags %#lx\n", Flags);

    Ret = SetProcessShutdownParameters(SavedLevel, SavedFlags);
    ok(Ret, "SetProcessShutdownParameters (restore) failed: %lu\n", GetLastError());
}

START_TEST(Wow64Console)
{
    char Application[MAX_PATH], Command[MAX_PATH + 32];
    STARTUPINFOA Startup = {sizeof(Startup)};
    PROCESS_INFORMATION Process;
    DWORD Code = 0, Wait;
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "child"))
        ExitProcess(Child());
    /* Generic console behaviour; the PE32 build run on a 64-bit system tests the WoW64 translation */
    {
        BOOL Wow64 = FALSE;
        IsWow64Process(GetCurrentProcess(), &Wow64);
        trace("Running %s WoW64\n", Wow64 ? "under" : "without");
    }

    TestShutdownParameters();

    ok(GetModuleFileNameA(NULL, Application, MAX_PATH) != 0, "GetModuleFileNameA failed\n");
    StringCbPrintfA(Command, sizeof(Command), "\"%s\" Wow64Console child", Application);
    Startup.dwFlags = STARTF_USESHOWWINDOW;
    Startup.wShowWindow = 7; /* SW_SHOWMINNOACTIVE */
    if (!CreateProcessA(Application, Command, NULL, NULL, FALSE, CREATE_NEW_CONSOLE,
                        NULL, NULL, &Startup, &Process))
    {
        ok(0, "CreateProcessA failed: %lu\n", GetLastError());
        return;
    }
    Wait = WaitForSingleObject(Process.hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "Wait %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
        TerminateProcess(Process.hProcess, 99);
    ok(GetExitCodeProcess(Process.hProcess, &Code), "GetExitCodeProcess failed\n");
    ok(Code == CHILD_DONE, "Child result %#lx (0x100 = no failure)\n", Code);
    ok(!(Code & CHILD_INFO), "GetConsoleScreenBufferInfo failed\n");
    ok(!(Code & CHILD_INFO_VALUES), "Screen buffer information out of range\n");
    ok(!(Code & CHILD_WRITE), "WriteConsoleA failed\n");
    ok(!(Code & CHILD_CURSOR), "Cursor position after output is wrong\n");
    ok(!(Code & CHILD_ATTRIBUTES), "Attributes or buffer size changed by the output\n");
    ok(!(Code & CHILD_GETCP), "GetConsoleCP/GetConsoleOutputCP returned 0\n");
    ok(!(Code & CHILD_SETCP), "SetConsoleOutputCP/SetConsoleCP failed\n");
    ok(!(Code & CHILD_SETCP_VALUE), "Code page not changed or not restored\n");
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
}
