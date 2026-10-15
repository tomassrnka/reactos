/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that switching and closing screen buffers does not hang while the window repaints
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef SW_SHOWNOACTIVATE
#define SW_SHOWNOACTIVATE 4
#endif

/*
 * The child activates a new screen buffer and closes it while it is active,
 * again and again, in a console window that is shown, while a second thread
 * writes to the first buffer: the window repaints all the time, and every
 * switch runs while a repaint waits for the console.
 */
#define ROUNDS              300
#define CHILD_TIMEOUT_MS    240000

static volatile LONG StopWriter;

static
DWORD
WINAPI
WriterThread(
    _In_ LPVOID Parameter)
{
    HANDLE hCon = (HANDLE)Parameter;
    static const CHAR Line[] = "first screen buffer, written while the buffers switch\n";
    DWORD Written;

    while (!StopWriter)
        WriteConsoleA(hCon, Line, sizeof(Line) - 1, &Written, NULL);
    return 0;
}

static
VOID
RunChild(VOID)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE), hCon, hBuffer, hWriter;
    CHAR Line[64];
    DWORD Written;
    ULONG i, Done = 0;

    hCon = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
    hWriter = CreateThread(NULL, 0, WriterThread, hCon, 0, NULL);

    for (i = 0; i < ROUNDS; i++)
    {
        hBuffer = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
        if (hBuffer == INVALID_HANDLE_VALUE)
            break;
        StringCbPrintfA(Line, sizeof(Line), "screen buffer round %lu\n", i);
        WriteConsoleA(hBuffer, Line, (DWORD)strlen(Line), &Written, NULL);
        if (SetConsoleActiveScreenBuffer(hBuffer))
        {
            WriteConsoleA(hBuffer, Line, (DWORD)strlen(Line), &Written, NULL);
            Done++;
        }
        Sleep(1);
        /* Closing the active buffer makes the console switch back to the first one */
        CloseHandle(hBuffer);
        Sleep(1);
    }

    StopWriter = TRUE;
    if (hWriter)
    {
        WaitForSingleObject(hWriter, INFINITE);
        CloseHandle(hWriter);
    }
    CloseHandle(hCon);

    StringCbPrintfA(Line, sizeof(Line), "rounds=%lu\n", Done);
    WriteFile(hOut, Line, (DWORD)strlen(Line), &Written, NULL);
}

START_TEST(ConsoleBufferSwitch)
{
    int argc;
    char **argv;
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 40];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    SECURITY_ATTRIBUTES Sa = { sizeof(Sa), NULL, TRUE };
    HANDLE hRead, hWrite;
    CHAR Output[64];
    DWORD Wait, Count, Used = 0;
    ULONG Rounds = 0;
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
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" ConsoleBufferSwitch child", FileName);

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

    if (Wait != WAIT_OBJECT_0)
        return;
    ok(sscanf(Output, "rounds=%lu", &Rounds) == 1, "Unexpected child output '%s'\n", Output);
    ok(Rounds == ROUNDS, "%lu of %d rounds switched the screen buffer\n", Rounds, ROUNDS);
}
