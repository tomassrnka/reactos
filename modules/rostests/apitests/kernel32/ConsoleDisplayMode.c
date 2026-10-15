/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that SetConsoleDisplayMode has set the mode when it returns
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef SW_SHOWNOACTIVATE
#define SW_SHOWNOACTIVATE 4
#endif

#define ROUNDS              3
#define CHILD_TIMEOUT_MS    120000
#define FULLSCREEN_FLAGS    (CONSOLE_FULLSCREEN | CONSOLE_FULLSCREEN_HARDWARE)

/*
 * The child needs a shown console window of its own: a hidden console has no
 * display mode to change. It reports what GetConsoleDisplayMode returns right
 * after each SetConsoleDisplayMode.
 */
static
VOID
RunChild(VOID)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE), hCon;
    CHAR Line[128];
    DWORD Written, Mode, Error;
    ULONG i, Full = 0, Windowed = 0;
    COORD Size;

    hCon = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
    if (hCon == INVALID_HANDLE_VALUE)
    {
        StringCbPrintfA(Line, sizeof(Line), "noconsole\n");
        WriteFile(hOut, Line, (DWORD)strlen(Line), &Written, NULL);
        return;
    }

    for (i = 0; i < ROUNDS; i++)
    {
        if (!SetConsoleDisplayMode(hCon, CONSOLE_FULLSCREEN_MODE, &Size))
        {
            Error = GetLastError();
            StringCbPrintfA(Line, sizeof(Line), "unsupported %lu\n", Error);
            WriteFile(hOut, Line, (DWORD)strlen(Line), &Written, NULL);
            CloseHandle(hCon);
            return;
        }
        if (GetConsoleDisplayMode(&Mode) && (Mode & FULLSCREEN_FLAGS))
            Full++;

        SetConsoleDisplayMode(hCon, CONSOLE_WINDOWED_MODE, &Size);
        if (GetConsoleDisplayMode(&Mode) && !(Mode & FULLSCREEN_FLAGS))
            Windowed++;
    }
    CloseHandle(hCon);

    StringCbPrintfA(Line, sizeof(Line), "full=%lu windowed=%lu\n", Full, Windowed);
    WriteFile(hOut, Line, (DWORD)strlen(Line), &Written, NULL);
}

START_TEST(ConsoleDisplayMode)
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
    ULONG Full = 0, Windowed = 0;
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
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" ConsoleDisplayMode child", FileName);

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

    if (!strncmp(Output, "noconsole", 9) || !strncmp(Output, "unsupported", 11))
    {
        /* Windows 7 and later have no full-screen console */
        skip("No full-screen console: %s", Output);
        return;
    }
    if (sscanf(Output, "full=%lu windowed=%lu", &Full, &Windowed) != 2)
    {
        ok(0, "Unexpected child output '%s'\n", Output);
        return;
    }
    ok(Full == ROUNDS, "GetConsoleDisplayMode reported full screen after %lu of %d switches\n", Full, ROUNDS);
    ok(Windowed == ROUNDS, "GetConsoleDisplayMode reported windowed after %lu of %d switches back\n", Windowed, ROUNDS);
}
