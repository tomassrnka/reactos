/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for RaiseFailFastException
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <ndk/psfuncs.h>

#ifndef STATUS_FAIL_FAST_EXCEPTION
#define STATUS_FAIL_FAST_EXCEPTION ((NTSTATUS)0xC0000602L)
#endif

#ifndef FAIL_FAST_GENERATE_EXCEPTION_ADDRESS
#define FAIL_FAST_GENERATE_EXCEPTION_ADDRESS 0x1
#endif
#ifndef FAIL_FAST_NO_HARD_ERROR_DLG
#define FAIL_FAST_NO_HARD_ERROR_DLG 0x2
#endif

#define TEST_EXCEPTION_CODE 0xE0F0F001
#define EXIT_CODE_RESUMED 0xBAD0001
#define EXIT_CODE_RETURNED 0xBAD0002

typedef VOID WINAPI FN_RaiseFailFastException(PEXCEPTION_RECORD, PCONTEXT, DWORD);

static FN_RaiseFailFastException *pRaiseFailFastException;

static
DWORD
WINAPI
ResumeTarget(
    _In_ PVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);
    NtTerminateProcess(NtCurrentProcess(), EXIT_CODE_RESUMED);
    return 0;
}

/* Pass the start context of a suspended thread: a continued exception that
   resumed it would run ResumeTarget */
static
VOID
RaiseWithOtherContext(VOID)
{
    CONTEXT Context;
    HANDLE Thread;

    Thread = CreateThread(NULL, 0, ResumeTarget, NULL, CREATE_SUSPENDED, NULL);
    if (!Thread)
        ExitProcess(EXIT_CODE_RETURNED);
    RtlZeroMemory(&Context, sizeof(Context));
    Context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(Thread, &Context))
        ExitProcess(EXIT_CODE_RETURNED);
    pRaiseFailFastException(NULL, &Context, FAIL_FAST_NO_HARD_ERROR_DLG);
}

static
VOID
RunChild(
    _In_ PCSTR Mode)
{
    EXCEPTION_RECORD ExceptionRecord;

    if (!strcmp(Mode, "context"))
    {
        RaiseWithOtherContext();
    }
    else if (!strcmp(Mode, "record"))
    {
        RtlZeroMemory(&ExceptionRecord, sizeof(ExceptionRecord));
        ExceptionRecord.ExceptionCode = TEST_EXCEPTION_CODE;
        pRaiseFailFastException(&ExceptionRecord, NULL,
                                FAIL_FAST_GENERATE_EXCEPTION_ADDRESS | FAIL_FAST_NO_HARD_ERROR_DLG);
    }
    else
    {
        pRaiseFailFastException(NULL, NULL, FAIL_FAST_NO_HARD_ERROR_DLG);
    }
    ExitProcess(EXIT_CODE_RETURNED);
}

static
VOID
TestChild(
    _In_ PCSTR Mode,
    _In_ BOOL Debug,
    _In_ DWORD ExpectedCode)
{
    CHAR FileName[MAX_PATH];
    CHAR CommandLine[MAX_PATH + 64];
    STARTUPINFOA StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    DEBUG_EVENT Event;
    DWORD ExitCode = 0, Continue, Seen = 0, Start, KillTime = 0;
    BOOL Done = FALSE, Killed = FALSE;

    GetModuleFileNameA(NULL, FileName, sizeof(FileName));
    StringCbPrintfA(CommandLine, sizeof(CommandLine), "\"%s\" RaiseFailFastException child %s", FileName, Mode);
    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    if (!CreateProcessA(FileName, CommandLine, NULL, NULL, FALSE,
                        Debug ? DEBUG_ONLY_THIS_PROCESS : 0,
                        NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
        return;
    }
    CloseHandle(ProcessInfo.hThread);

    if (!Debug)
    {
        if (WaitForSingleObject(ProcessInfo.hProcess, 30000) != WAIT_OBJECT_0)
        {
            ok(0, "%s: child did not exit\n", Mode);
            TerminateProcess(ProcessInfo.hProcess, 1);
            WaitForSingleObject(ProcessInfo.hProcess, 5000);
        }
        GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
        Done = TRUE;
    }

    /* A child that keeps raising, or stops, is ended; its exit event is
       still awaited so that it cannot end the next case */
    Start = GetTickCount();
    while (!Done)
    {
        if (!Killed && (GetTickCount() - Start > 30000 || Seen > 2))
        {
            ok(0, "%s: child did not end (%lu fail fast exceptions)\n", Mode, Seen);
            TerminateProcess(ProcessInfo.hProcess, 1);
            Killed = TRUE;
            KillTime = GetTickCount();
        }
        if (Killed && GetTickCount() - KillTime > 10000)
        {
            ok(0, "%s: no exit event after termination\n", Mode);
            DebugActiveProcessStop(ProcessInfo.dwProcessId);
            break;
        }
        if (!WaitForDebugEvent(&Event, 1000))
            continue;

        Continue = DBG_CONTINUE;
        switch (Event.dwDebugEventCode)
        {
            case CREATE_PROCESS_DEBUG_EVENT:
                if (Event.u.CreateProcessInfo.hFile)
                    CloseHandle(Event.u.CreateProcessInfo.hFile);
                break;

            case LOAD_DLL_DEBUG_EVENT:
                if (Event.u.LoadDll.hFile)
                    CloseHandle(Event.u.LoadDll.hFile);
                break;

            case EXCEPTION_DEBUG_EVENT:
                if (Event.dwProcessId == ProcessInfo.dwProcessId &&
                    Event.u.Exception.ExceptionRecord.ExceptionCode == ExpectedCode)
                {
                    Seen++;
                    trace("%s: first chance %lu, flags 0x%lx\n", Mode,
                          Event.u.Exception.dwFirstChance,
                          Event.u.Exception.ExceptionRecord.ExceptionFlags);
                    ok(Event.u.Exception.ExceptionRecord.ExceptionFlags & EXCEPTION_NONCONTINUABLE,
                       "%s: exception flags 0x%lx\n", Mode, Event.u.Exception.ExceptionRecord.ExceptionFlags);
                    /* On Windows a debugger that continues the exception resumes the
                       process (Windows 10: the call returns, or the supplied context
                       runs; Server 2008 R2: it raises again), so the debugger hands
                       the exception back. ReactOS ends the process either way. */
                    if (!is_reactos())
                        Continue = DBG_EXCEPTION_NOT_HANDLED;
                }
                else if (Event.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT)
                {
                    Continue = DBG_EXCEPTION_NOT_HANDLED;
                }
                break;

            case EXIT_PROCESS_DEBUG_EVENT:
                if (Event.dwProcessId == ProcessInfo.dwProcessId)
                {
                    ExitCode = Event.u.ExitProcess.dwExitCode;
                    Done = TRUE;
                }
                break;
        }
        ContinueDebugEvent(Event.dwProcessId, Event.dwThreadId, Continue);
    }

    /* The process ends. On ReactOS this holds when the debugger continued the
       exception, which is a ReactOS rule that Windows does not follow */
#if defined(_M_IX86) || defined(_M_AMD64)
    if (Debug)
        ok(Seen >= 1, "%s: the debugger did not see exception 0x%lx\n", Mode, ExpectedCode);
#endif
    ok(ExitCode == ExpectedCode, "%s, %s: exit code 0x%lx, expected 0x%lx\n",
       Mode, Debug ? "debugged" : "not debugged", ExitCode, ExpectedCode);
    CloseHandle(ProcessInfo.hProcess);
}

START_TEST(RaiseFailFastException)
{
    int argc;
    char **argv;

    pRaiseFailFastException = (FN_RaiseFailFastException *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                          "RaiseFailFastException");
    if (!pRaiseFailFastException)
    {
        skip("RaiseFailFastException is not available\n");
        return;
    }

    argc = winetest_get_mainargs(&argv);
    if (argc >= 4 && !strcmp(argv[2], "child"))
    {
        RunChild(argv[3]);
        return;
    }

    TestChild("default", FALSE, STATUS_FAIL_FAST_EXCEPTION);
    TestChild("record", FALSE, TEST_EXCEPTION_CODE);
    TestChild("context", FALSE, STATUS_FAIL_FAST_EXCEPTION);
    TestChild("default", TRUE, STATUS_FAIL_FAST_EXCEPTION);
    TestChild("record", TRUE, TEST_EXCEPTION_CODE);
    TestChild("context", TRUE, STATUS_FAIL_FAST_EXCEPTION);
}
