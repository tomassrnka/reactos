/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for RtlFailFast and __fastfail (INT 29h)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <pseh/pseh2.h>

#ifndef PF_FASTFAIL_AVAILABLE
#define PF_FASTFAIL_AVAILABLE 23
#endif

#define CHILD_RETURNED 0x7E57DEAD
#define CHILD_VEH_RAN 0x7E57BEEF
#define CHILD_FILTER_RAN 0x7E57F00D
#define CHILD_SEH_RAN 0x7E57C0DE
#define CHILD_SETUP_FAILED 0x7E57BAD0

#if defined(__GNUC__) && (defined(_M_IX86) || defined(_M_AMD64))
/* Raw INT 29h with a defined return path, so a fast fail that returns reaches CHILD_RETURNED */
static
VOID
__attribute__((noinline))
FailFastChild(
    _In_ ULONG Code)
{
    __asm__ __volatile__("int $0x29" : : "c"(Code) : "memory");
}
#else
static
VOID
FailFastChild(
    _In_ ULONG Code)
{
    RtlFailFast(Code);
}
#endif

static
LONG
WINAPI
ChildVectoredHandler(
    _In_ PEXCEPTION_POINTERS ExceptionInfo)
{
    if (ExceptionInfo->ExceptionRecord->ExceptionCode == STATUS_STACK_BUFFER_OVERRUN)
        ExitProcess(CHILD_VEH_RAN);
    return EXCEPTION_CONTINUE_SEARCH;
}

static
LONG
WINAPI
ChildUnhandledFilter(
    _In_ PEXCEPTION_POINTERS ExceptionInfo)
{
    ExitProcess(CHILD_FILTER_RAN);
}

static
LONG
ChildSehFilter(VOID)
{
    ExitProcess(CHILD_SEH_RAN);
}

/* A vectored handler, a frame-based handler and the unhandled exception filter
   each end the child with their own exit code if the fast fail reaches them */
static
VOID
FailFastWithHandlers(
    _In_ ULONG Code)
{
    if (!AddVectoredExceptionHandler(TRUE, ChildVectoredHandler))
        ExitProcess(CHILD_SETUP_FAILED);
    SetUnhandledExceptionFilter(ChildUnhandledFilter);

    _SEH2_TRY
    {
        FailFastChild(Code);
    }
    _SEH2_EXCEPT(ChildSehFilter())
    {
    }
    _SEH2_END;
}

static
BOOL
StartChild(
    _In_ PCSTR Mode,
    _In_ ULONG Code,
    _In_ DWORD CreationFlags,
    _Out_ PPROCESS_INFORMATION ProcessInfo)
{
    char FileName[MAX_PATH];
    char CommandLine[MAX_PATH + 64];
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };

    if (!GetModuleFileNameA(NULL, FileName, _countof(FileName)))
        return FALSE;
    StringCbPrintfA(CommandLine, sizeof(CommandLine), "\"%s\" RtlFailFast %s %lu", FileName, Mode, Code);
    return CreateProcessA(NULL, CommandLine, NULL, NULL, FALSE, CreationFlags, NULL, NULL,
                          &StartupInfo, ProcessInfo);
}

static
DWORD
RunChild(
    _In_ PCSTR Mode,
    _In_ ULONG Code)
{
    PROCESS_INFORMATION ProcessInfo;
    DWORD Wait, ExitCode = 0;

    if (!StartChild(Mode, Code, 0, &ProcessInfo))
    {
        ok(FALSE, "CreateProcessA failed: %lu\n", GetLastError());
        return 0;
    }

    Wait = WaitForSingleObject(ProcessInfo.hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "%s %lu: wait returned %lu\n", Mode, Code, Wait);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(ProcessInfo.hProcess, 1);
        WaitForSingleObject(ProcessInfo.hProcess, INFINITE);
    }

    ok(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode), "GetExitCodeProcess failed: %lu\n", GetLastError());
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
    return ExitCode;
}

static
VOID
TestExitCode(
    _In_ ULONG Code)
{
    DWORD ExitCode = RunChild("child", Code);

    ok(ExitCode != CHILD_RETURNED, "Code %lu: the fast fail returned to the caller\n", Code);
    ok(ExitCode == STATUS_STACK_BUFFER_OVERRUN, "Code %lu: exit code 0x%lx, expected 0x%lx\n",
       Code, ExitCode, STATUS_STACK_BUFFER_OVERRUN);
}

/* A fast fail skips the exception handlers of the process */
static
VOID
TestHandlersBypassed(
    _In_ ULONG Code)
{
    DWORD ExitCode = RunChild("handlers", Code);

    ok(ExitCode != CHILD_SETUP_FAILED, "Code %lu: AddVectoredExceptionHandler failed\n", Code);
    ok(ExitCode != CHILD_VEH_RAN, "Code %lu: a vectored exception handler ran\n", Code);
    ok(ExitCode != CHILD_SEH_RAN, "Code %lu: a frame-based exception handler ran\n", Code);
    ok(ExitCode != CHILD_FILTER_RAN, "Code %lu: the unhandled exception filter ran\n", Code);
    ok(ExitCode != CHILD_RETURNED, "Code %lu: the fast fail returned to the caller\n", Code);
    ok(ExitCode == STATUS_STACK_BUFFER_OVERRUN, "Code %lu: exit code 0x%lx, expected 0x%lx\n",
       Code, ExitCode, STATUS_STACK_BUFFER_OVERRUN);
}

static
VOID
TestExceptionRecord(
    _In_ ULONG Code,
    _In_ ULONG ContinueCount)
{
    PROCESS_INFORMATION ProcessInfo;
    DEBUG_EVENT Event;
    DWORD ContinueStatus, ExitCode = 0;
    BOOL SawFastFail = FALSE;
    ULONG FastFails = 0, FirstChanceEvents = 0;
    PVOID FirstAddress = NULL;
    DWORD FirstThreadId = 0;
    PEXCEPTION_RECORD Record;
    UCHAR Instruction[2];
    SIZE_T Read;

    if (!StartChild("child", Code, DEBUG_ONLY_THIS_PROCESS, &ProcessInfo))
    {
        ok(FALSE, "CreateProcessA failed: %lu\n", GetLastError());
        return;
    }

    for (;;)
    {
        if (!WaitForDebugEvent(&Event, 30000))
        {
            ok(FALSE, "WaitForDebugEvent failed: %lu\n", GetLastError());
            TerminateProcess(ProcessInfo.hProcess, 1);
            break;
        }

        ContinueStatus = DBG_CONTINUE;
        if (Event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT)
        {
            Record = &Event.u.Exception.ExceptionRecord;
            if (Record->ExceptionCode != STATUS_BREAKPOINT)
                ContinueStatus = DBG_EXCEPTION_NOT_HANDLED;

            if (Record->ExceptionCode == STATUS_STACK_BUFFER_OVERRUN)
            {
                FastFails++;
                trace("Fast fail event %lu: first chance %lu, flags 0x%lx, address %p\n", FastFails,
                      Event.u.Exception.dwFirstChance, Record->ExceptionFlags, Record->ExceptionAddress);
                if (FastFails <= ContinueCount)
                    ContinueStatus = DBG_CONTINUE;

                /* Every event, the repeated ones too, is the same second chance fast fail */
                if (Event.u.Exception.dwFirstChance)
                    FirstChanceEvents++;
                ok(Record->ExceptionFlags & EXCEPTION_NONCONTINUABLE,
                   "ExceptionFlags 0x%lx, expected EXCEPTION_NONCONTINUABLE\n", Record->ExceptionFlags);
                ok(Record->NumberParameters == 1, "NumberParameters %lu, expected 1\n", Record->NumberParameters);
                if (Record->NumberParameters >= 1)
                    ok(Record->ExceptionInformation[0] == Code, "ExceptionInformation[0] %Iu, expected %lu\n",
                       Record->ExceptionInformation[0], Code);
                if (!SawFastFail)
                {
                    SawFastFail = TRUE;
                    FirstAddress = Record->ExceptionAddress;
                    FirstThreadId = Event.dwThreadId;
                    ok(ReadProcessMemory(ProcessInfo.hProcess, Record->ExceptionAddress, Instruction,
                                         sizeof(Instruction), &Read) &&
                       Instruction[0] == 0xCD && Instruction[1] == 0x29,
                       "ExceptionAddress %p does not point to INT 29h\n", Record->ExceptionAddress);
                }
                else
                {
                    ok(Record->ExceptionAddress == FirstAddress, "Fast fail event %lu at %p, the first at %p\n",
                       FastFails, Record->ExceptionAddress, FirstAddress);
                    ok(Event.dwThreadId == FirstThreadId, "Fast fail event %lu in thread %lu, the first in %lu\n",
                       FastFails, Event.dwThreadId, FirstThreadId);
                }
            }
            else if (Record->ExceptionCode != STATUS_BREAKPOINT &&
                     Record->ExceptionCode != STATUS_STACK_BUFFER_OVERRUN)
            {
                trace("Unexpected exception 0x%lx\n", Record->ExceptionCode);
            }
        }
        else if (Event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && Event.u.LoadDll.hFile)
        {
            CloseHandle(Event.u.LoadDll.hFile);
        }
        else if (Event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT && Event.u.CreateProcessInfo.hFile)
        {
            CloseHandle(Event.u.CreateProcessInfo.hFile);
        }

        if (Event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
            ExitCode = Event.u.ExitProcess.dwExitCode;
        ContinueDebugEvent(Event.dwProcessId, Event.dwThreadId, ContinueStatus);
        if (Event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
            break;
    }

    ok(SawFastFail, "No STATUS_STACK_BUFFER_OVERRUN exception was reported\n");
    ok(FirstChanceEvents == 0, "%lu first chance fast fail events\n", FirstChanceEvents);
    trace("Continued %lu: %lu fast fail events, exit code 0x%lx\n", ContinueCount, FastFails, ExitCode);
    ok(FastFails == ContinueCount + 1, "Continued %lu: %lu fast fail events\n", ContinueCount, FastFails);
    ok(ExitCode == STATUS_STACK_BUFFER_OVERRUN, "Continued %lu: exit code 0x%lx, expected 0x%lx\n",
       ContinueCount, ExitCode, STATUS_STACK_BUFFER_OVERRUN);

    WaitForSingleObject(ProcessInfo.hProcess, 30000);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
}

START_TEST(RtlFailFast)
{
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 4 && (!strcmp(argv[2], "child") || !strcmp(argv[2], "handlers")))
    {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        if (!strcmp(argv[2], "handlers"))
            FailFastWithHandlers(strtoul(argv[3], NULL, 0));
        else
            FailFastChild(strtoul(argv[3], NULL, 0));
        ExitProcess(CHILD_RETURNED);
    }

#if !defined(_M_IX86) && !defined(_M_AMD64)
    skip("The test checks the x86 INT 29h form of fast fail only\n");
    return;
#endif

    if (!is_reactos() && !IsProcessorFeaturePresent(PF_FASTFAIL_AVAILABLE))
    {
        skip("Fast fail is not available\n");
        return;
    }

    TestExitCode(FAST_FAIL_CORRUPT_LIST_ENTRY);
    TestExitCode(FAST_FAIL_INVALID_ARG);
    TestExitCode(FAST_FAIL_FATAL_APP_EXIT);
    TestHandlersBypassed(FAST_FAIL_CORRUPT_LIST_ENTRY);
    TestExceptionRecord(FAST_FAIL_CORRUPT_LIST_ENTRY, 0);
    TestExceptionRecord(FAST_FAIL_INVALID_ARG, 1);
}
