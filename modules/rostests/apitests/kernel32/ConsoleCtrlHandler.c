/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that console control events reach the handlers registered with SetConsoleCtrlHandler
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <winuser.h>

/*
 * Each case runs in a child on a console of its own, so the events never
 * reach the test runner, and the child reports what its handlers saw in its
 * exit code. The console server delivers an event by starting a thread in
 * the process at the control routine kernel32 registered when it connected;
 * that routine calls the handlers, the last registered first, until one
 * returns TRUE, and the default handler ends the process with
 * STATUS_CONTROL_C_EXIT. The same cases run for PE32 images under WoW64.
 */
#define EVENT_WAIT_MS       30000
#define QUIET_MS            2000
#define CHILD_TIMEOUT_MS    120000

static WCHAR FileName[MAX_PATH];
static DWORD MainThreadId;
static HANDLE Delivered;
static LONG CountC, CountBreak, Sequence;

static
BOOL
StartSelf(
    _In_ PCWSTR Mode,
    _In_ DWORD Flags,
    _In_opt_ HANDLE Inherit,
    _Out_ PPROCESS_INFORMATION ProcessInfo)
{
    WCHAR CommandLine[MAX_PATH + 60];
    STARTUPINFOW StartupInfo;

    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" ConsoleCtrlHandler %ls %lu",
                    FileName, Mode, HandleToUlong(Inherit));
    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    return CreateProcessW(FileName, CommandLine, NULL, NULL, Inherit != NULL, Flags,
                          NULL, NULL, &StartupInfo, ProcessInfo);
}

/* Wait until *Count reaches Want; FALSE if it does not within the time */
static
BOOL
WaitCount(
    _In_ volatile LONG *Count,
    _In_ LONG Want)
{
    DWORD Start = GetTickCount();

    while (*Count < Want)
    {
        if (GetTickCount() - Start >= EVENT_WAIT_MS)
            return FALSE;
        WaitForSingleObject(Delivered, 10);
    }
    return TRUE;
}

/* Counts the events on threads other than the main one, handles them all */
static
BOOL
WINAPI
CountHandler(
    _In_ DWORD CtrlType)
{
    if (GetCurrentThreadId() == MainThreadId)
        ExitProcess(9);
    if (CtrlType == CTRL_C_EVENT)
        InterlockedIncrement(&CountC);
    else if (CtrlType == CTRL_BREAK_EVENT)
        InterlockedIncrement(&CountBreak);
    else
        return FALSE;
    SetEvent(Delivered);
    return TRUE;
}

/* Ignores the first Ctrl+C, ends the process with 42 on the second */
static
BOOL
WINAPI
TwiceHandler(
    _In_ DWORD CtrlType)
{
    if (CtrlType != CTRL_C_EVENT)
        return FALSE;
    if (GetCurrentThreadId() == MainThreadId)
        ExitProcess(9);
    if (InterlockedIncrement(&CountC) >= 2)
        ExitProcess(42);
    SetEvent(Delivered);
    return TRUE;
}

static
VOID
Record(
    _In_ LONG Digit)
{
    LONG Old, New;

    do
    {
        Old = Sequence;
        New = Old * 10 + Digit;
    } while (InterlockedCompareExchange(&Sequence, New, Old) != Old);
}

static BOOL WINAPI Handler1(_In_ DWORD CtrlType) { Record(1); SetEvent(Delivered); return TRUE; }
static BOOL WINAPI Handler2(_In_ DWORD CtrlType) { Record(2); SetEvent(Delivered); return TRUE; }
static BOOL WINAPI Handler3(_In_ DWORD CtrlType) { Record(3); return FALSE; }

/* Calls the default handler for every event */
static
BOOL
WINAPI
DeclineHandler(
    _In_ DWORD CtrlType)
{
    return FALSE;
}

/* Ends the process with 70 + the event when the console is closed */
static
BOOL
WINAPI
CloseHandler(
    _In_ DWORD CtrlType)
{
    if (CtrlType == CTRL_CLOSE_EVENT)
        ExitProcess(70 + CtrlType);
    return FALSE;
}

/* The flag that ignores Ctrl+C is inherited (from a runner started in a
 * process group of its own, for one): clear it */
static
VOID
EnableCtrlC(VOID)
{
    if (!SetConsoleCtrlHandler(NULL, FALSE))
        ExitProcess(18);
}

/* Child: Ctrl+C ignored once, the second one ends the process from the handler */
static
VOID
TwiceChild(VOID)
{
    EnableCtrlC();
    SetConsoleCtrlHandler(TwiceHandler, TRUE);
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0))
        ExitProcess(1);
    if (!WaitCount(&CountC, 1))
        ExitProcess(2);
    Sleep(QUIET_MS);
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0))
        ExitProcess(3);
    Sleep(EVENT_WAIT_MS);
    ExitProcess(4);
}

/* Child: handlers run last registered first, until one returns TRUE.
 * Exit code: the order of the first event, then of the second, as digits. */
static
VOID
OrderChild(VOID)
{
    LONG First;

    EnableCtrlC();
    SetConsoleCtrlHandler(Handler1, TRUE);
    SetConsoleCtrlHandler(Handler2, TRUE);
    SetConsoleCtrlHandler(Handler3, TRUE);
    if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0))
        ExitProcess(1);
    if (!WaitCount(&Sequence, 1))
        ExitProcess(2);
    Sleep(QUIET_MS);
    First = InterlockedExchange(&Sequence, 0);
    SetConsoleCtrlHandler(Handler2, FALSE);
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0))
        ExitProcess(3);
    if (!WaitCount(&Sequence, 1))
        ExitProcess(4);
    Sleep(QUIET_MS);
    ExitProcess(First * 100 + Sequence);
}

/* Child: no handler takes the event, the default one ends the process */
static
VOID
DefaultChild(
    _In_ BOOL Register,
    _In_ DWORD CtrlType)
{
    EnableCtrlC();
    if (Register)
        SetConsoleCtrlHandler(DeclineHandler, TRUE);
    if (!GenerateConsoleCtrlEvent(CtrlType, 0))
        ExitProcess(1);
    Sleep(EVENT_WAIT_MS);
    ExitProcess(2);
}

/* Child: SetConsoleCtrlHandler(NULL, TRUE) ignores Ctrl+C, not Ctrl+Break.
 * Exit code: Ctrl+C events seen while ignored, Ctrl+Break seen, Ctrl+C seen after. */
static
VOID
IgnoreChild(VOID)
{
    LONG IgnoredC;

    /* With no handler, an ignored Ctrl+C must not end the process */
    if (!SetConsoleCtrlHandler(NULL, TRUE))
        ExitProcess(1);
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0))
        ExitProcess(2);
    Sleep(QUIET_MS);
    SetConsoleCtrlHandler(CountHandler, TRUE);
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0))
        ExitProcess(3);
    if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0))
        ExitProcess(4);
    if (!WaitCount(&CountBreak, 1))
        ExitProcess(5);
    Sleep(QUIET_MS);
    IgnoredC = CountC;
    if (!SetConsoleCtrlHandler(NULL, FALSE))
        ExitProcess(6);
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0))
        ExitProcess(7);
    if (!WaitCount(&CountC, IgnoredC + 1))
        ExitProcess(8);
    Sleep(QUIET_MS);
    ExitProcess(100 + IgnoredC * 100 + CountBreak * 10 + (CountC - IgnoredC));
}

/* Grandchild in a process group of its own, on the group child's console.
 * Exit code: 100 + Ctrl+C seen * 10 + Ctrl+Break seen. */
static
VOID
MemberChild(
    _In_ HANDLE Ready)
{
    SetConsoleCtrlHandler(CountHandler, TRUE);
    SetEvent(Ready);
    WaitCount(&CountBreak, 1);
    Sleep(QUIET_MS);
    ExitProcess(100 + CountC * 10 + CountBreak);
}

/* Child: an event sent to a process group reaches that group only, and
 * Ctrl+C is disabled in a group started with CREATE_NEW_PROCESS_GROUP.
 * Exit code: the member's exit code * 100 + own Ctrl+C * 10 + own Ctrl+Break. */
static
VOID
GroupChild(VOID)
{
    SECURITY_ATTRIBUTES Attributes = { sizeof(Attributes), NULL, TRUE };
    PROCESS_INFORMATION ProcessInfo;
    HANDLE Ready;
    DWORD MemberCode;

    EnableCtrlC();
    SetConsoleCtrlHandler(CountHandler, TRUE);
    Ready = CreateEventW(&Attributes, TRUE, FALSE, NULL);
    if (!Ready)
        ExitProcess(1);
    if (!StartSelf(L"member", CREATE_NEW_PROCESS_GROUP, Ready, &ProcessInfo))
        ExitProcess(2);
    CloseHandle(ProcessInfo.hThread);
    if (WaitForSingleObject(Ready, EVENT_WAIT_MS) != WAIT_OBJECT_0)
        ExitProcess(3);
    /* Every process on the console, the member included */
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0))
        ExitProcess(4);
    if (!WaitCount(&CountC, 1))
        ExitProcess(5);
    /* The member's group only */
    if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, ProcessInfo.dwProcessId))
        ExitProcess(6);
    if (WaitForSingleObject(ProcessInfo.hProcess, EVENT_WAIT_MS * 2) != WAIT_OBJECT_0)
        ExitProcess(7);
    GetExitCodeProcess(ProcessInfo.hProcess, &MemberCode);
    Sleep(QUIET_MS);
    ExitProcess(MemberCode * 100 + CountC * 10 + CountBreak);
}

/* Grandchild on the inherit child's console. Exit code: 100 + Ctrl+C seen. */
static
VOID
HeirChild(VOID)
{
    SetConsoleCtrlHandler(CountHandler, TRUE);
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) ||
        !GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0))
        ExitProcess(1);
    if (!WaitCount(&CountBreak, 1))
        ExitProcess(2);
    Sleep(QUIET_MS);
    ExitProcess(100 + CountC);
}

/* Child: a child inherits the flag that ignores Ctrl+C.
 * Exit code: the heir's exit code with the flag set * 1000 + without it. */
static
VOID
InheritChild(VOID)
{
    PROCESS_INFORMATION ProcessInfo;
    DWORD Codes[2] = { 0, 0 };
    ULONG i;

    SetConsoleCtrlHandler(CountHandler, TRUE);
    for (i = 0; i < 2; i++)
    {
        if (!SetConsoleCtrlHandler(NULL, i == 0))
            ExitProcess(1);
        if (!StartSelf(L"heir", 0, NULL, &ProcessInfo))
            ExitProcess(2);
        CloseHandle(ProcessInfo.hThread);
        if (WaitForSingleObject(ProcessInfo.hProcess, EVENT_WAIT_MS * 2) != WAIT_OBJECT_0)
            ExitProcess(3);
        GetExitCodeProcess(ProcessInfo.hProcess, &Codes[i]);
        CloseHandle(ProcessInfo.hProcess);
    }
    ExitProcess(Codes[0] * 1000 + Codes[1]);
}

/* Child: closing the console window sends CTRL_CLOSE_EVENT */
static
VOID
CloseChild(
    _In_ BOOL Register)
{
    HWND Window = GetConsoleWindow();

    if (!Window)
        ExitProcess(1);
    if (Register)
        SetConsoleCtrlHandler(CloseHandler, TRUE);
    if (!PostMessageW(Window, WM_CLOSE, 0, 0))
        ExitProcess(2);
    Sleep(EVENT_WAIT_MS);
    ExitProcess(3);
}

static
VOID
RunChild(
    _In_ PCWSTR Mode,
    _In_ DWORD Expected)
{
    PROCESS_INFORMATION ProcessInfo;
    DWORD Wait, ExitCode = 0;

    if (!StartSelf(Mode, CREATE_NEW_CONSOLE, NULL, &ProcessInfo))
    {
        skip("CreateProcessW failed with %lu\n", GetLastError());
        return;
    }
    CloseHandle(ProcessInfo.hThread);
    Wait = WaitForSingleObject(ProcessInfo.hProcess, CHILD_TIMEOUT_MS);
    ok(Wait == WAIT_OBJECT_0, "The %ls child did not exit within %lu ms\n", Mode, CHILD_TIMEOUT_MS);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(ProcessInfo.hProcess, 1);
    }
    else
    {
        GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
        ok(ExitCode == Expected, "The %ls child exited with %lu (0x%lx), expected %lu (0x%lx)\n",
           Mode, ExitCode, ExitCode, Expected, Expected);
    }
    CloseHandle(ProcessInfo.hProcess);
}

START_TEST(ConsoleCtrlHandler)
{
    int argc;
    char **argv;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    MainThreadId = GetCurrentThreadId();
    Delivered = CreateEventW(NULL, FALSE, FALSE, NULL);

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3)
    {
        if (!strcmp(argv[2], "twice"))
            TwiceChild();
        if (!strcmp(argv[2], "order"))
            OrderChild();
        if (!strcmp(argv[2], "default"))
            DefaultChild(TRUE, CTRL_C_EVENT);
        if (!strcmp(argv[2], "nohandler"))
            DefaultChild(FALSE, CTRL_BREAK_EVENT);
        if (!strcmp(argv[2], "ignore"))
            IgnoreChild();
        if (!strcmp(argv[2], "group"))
            GroupChild();
        if (argc >= 4 && !strcmp(argv[2], "member"))
            MemberChild(ULongToHandle(strtoul(argv[3], NULL, 10)));
        if (!strcmp(argv[2], "inherit"))
            InheritChild();
        if (!strcmp(argv[2], "heir"))
            HeirChild();
        if (!strcmp(argv[2], "close"))
            CloseChild(TRUE);
        if (!strcmp(argv[2], "closedefault"))
            CloseChild(FALSE);
    }

    RunChild(L"twice", 42);
    /* Break: 3 declines, 2 takes it; C after removing 2: 3 declines, 1 takes it */
    RunChild(L"order", 3231);
    RunChild(L"default", STATUS_CONTROL_C_EXIT);
    RunChild(L"nohandler", STATUS_CONTROL_C_EXIT);
    /* Ctrl+C ignored: not seen; Ctrl+Break seen; Ctrl+C seen once the flag is cleared */
    RunChild(L"ignore", 111);
    /* The member saw only its Break (101); the group child only its Ctrl+C */
    RunChild(L"group", 10110);
    /* The heir ignores Ctrl+C while its parent does, sees it once the parent does not */
    RunChild(L"inherit", 100101);
    RunChild(L"close", 70 + CTRL_CLOSE_EVENT);
    RunChild(L"closedefault", STATUS_CONTROL_C_EXIT);
}
