/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that SetConsoleDisplayMode has set the mode when it returns
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <wingdi.h>
#include <winuser.h>

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

/*
 * The switch to full screen takes a while, and the console server waits for
 * it with the console unlocked. Meanwhile a second thread moves the process
 * to another console, whose output handle gets the same value. The display
 * mode call must then fail with ERROR_INVALID_HANDLE instead of reporting the
 * buffer of the other console. Exit code: 0 passed, 1 failed, 2 no overlap.
 */
static volatile LONG Attached;
static LARGE_INTEGER AttachDone;
static DWORD HelperPid;

static
DWORD
WINAPI
MoveThread(
    _In_ LPVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);

    Sleep(20);
    FreeConsole();
    if (AttachConsole(HelperPid))
    {
        QueryPerformanceCounter(&AttachDone);
        InterlockedExchange(&Attached, TRUE);
    }
    return 0;
}

static
int
RunAttachChild(VOID)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE), hThread;
    LARGE_INTEGER Freq, Done;
    COORD Size;
    BOOL Ret;
    DWORD Error;
    int Result;

    hThread = CreateThread(NULL, 0, MoveThread, NULL, 0, NULL);
    if (!hThread)
        return 3;
    Ret = SetConsoleDisplayMode(hOut, CONSOLE_FULLSCREEN_MODE, &Size);
    Error = GetLastError();
    QueryPerformanceCounter(&Done);
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    /* The first console may have been deleted in full screen */
    ChangeDisplaySettingsW(NULL, 0);

    /* Conclusive only if the move ended clearly before the call returned */
    QueryPerformanceFrequency(&Freq);
    if (!Attached || (Done.QuadPart - AttachDone.QuadPart) * 1000 / Freq.QuadPart < 20)
        Result = 2;
    else
        Result = (!Ret && Error == ERROR_INVALID_HANDLE) ? 0 : 1;
    return Result;
}

static
VOID
TestMoveDuringSwitch(
    _In_ PCWSTR FileName)
{
    WCHAR CommandLine[MAX_PATH + 60];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION Helper, Child;
    DWORD Wait, ExitCode;
    ULONG i, Conclusive = 0, Failed = 0;

    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" ConsoleDisplayMode helper", FileName);
    if (!CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, CREATE_NEW_CONSOLE | CREATE_NO_WINDOW,
                        NULL, NULL, &StartupInfo, &Helper))
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
        return;
    }
    CloseHandle(Helper.hThread);
    Sleep(1000);

    StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    StartupInfo.wShowWindow = SW_SHOWNOACTIVATE;
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" ConsoleDisplayMode attach %lu",
                    FileName, Helper.dwProcessId);
    for (i = 0; i < 4 && Conclusive < 2; i++)
    {
        if (!CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, CREATE_NEW_CONSOLE,
                            NULL, NULL, &StartupInfo, &Child))
        {
            break;
        }
        CloseHandle(Child.hThread);
        Wait = WaitForSingleObject(Child.hProcess, CHILD_TIMEOUT_MS);
        ok(Wait == WAIT_OBJECT_0, "The child process did not exit: %lu\n", Wait);
        if (Wait != WAIT_OBJECT_0)
            TerminateProcess(Child.hProcess, 1);
        if (Wait == WAIT_OBJECT_0 && GetExitCodeProcess(Child.hProcess, &ExitCode) && ExitCode <= 1)
        {
            Conclusive++;
            if (ExitCode == 1) Failed++;
        }
        CloseHandle(Child.hProcess);
    }

    TerminateProcess(Helper.hProcess, 0);
    CloseHandle(Helper.hProcess);

    if (!Conclusive)
    {
        skip("The console move never overlapped the display mode switch\n");
        return;
    }
    ok(Failed == 0, "SetConsoleDisplayMode used the console the process moved to, in %lu of %lu runs\n",
       Failed, Conclusive);
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
    if (argc >= 3 && !strcmp(argv[2], "helper"))
    {
        Sleep(CHILD_TIMEOUT_MS);
        return;
    }
    if (argc >= 4 && !strcmp(argv[2], "attach"))
    {
        HelperPid = strtoul(argv[3], NULL, 10);
        ExitProcess(RunAttachChild());
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

    TestMoveDuringSwitch(FileName);
}
