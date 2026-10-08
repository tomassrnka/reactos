/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for RtlFailFast and __fastfail (INT 29h)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef PF_FASTFAIL_AVAILABLE
#define PF_FASTFAIL_AVAILABLE 23
#endif

#define CHILD_RETURNED 0x7E57DEAD

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
BOOL
StartChild(
    _In_ ULONG Code,
    _In_ DWORD CreationFlags,
    _Out_ PPROCESS_INFORMATION ProcessInfo)
{
    char FileName[MAX_PATH];
    char CommandLine[MAX_PATH + 64];
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };

    if (!GetModuleFileNameA(NULL, FileName, _countof(FileName)))
        return FALSE;
    StringCbPrintfA(CommandLine, sizeof(CommandLine), "\"%s\" RtlFailFast child %lu", FileName, Code);
    return CreateProcessA(NULL, CommandLine, NULL, NULL, FALSE, CreationFlags, NULL, NULL,
                          &StartupInfo, ProcessInfo);
}

static
VOID
TestExitCode(
    _In_ ULONG Code)
{
    PROCESS_INFORMATION ProcessInfo;
    DWORD Wait, ExitCode = 0;

    if (!StartChild(Code, 0, &ProcessInfo))
    {
        ok(FALSE, "CreateProcessA failed: %lu\n", GetLastError());
        return;
    }

    Wait = WaitForSingleObject(ProcessInfo.hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "Code %lu: wait returned %lu\n", Code, Wait);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(ProcessInfo.hProcess, 1);
        WaitForSingleObject(ProcessInfo.hProcess, INFINITE);
    }

    ok(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode), "GetExitCodeProcess failed: %lu\n", GetLastError());
    ok(ExitCode != CHILD_RETURNED, "Code %lu: the fast fail returned to the caller\n", Code);
#ifdef _M_AMD64
    /* Pre-existing amd64 bug: the BaseProcessStartup handler does not catch it, and csrss ends the
       process with STATUS_ABANDONED */
    todo_if(is_reactos())
#endif
    ok(ExitCode == STATUS_STACK_BUFFER_OVERRUN, "Code %lu: exit code 0x%lx, expected 0x%lx\n",
       Code, ExitCode, STATUS_STACK_BUFFER_OVERRUN);

    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
}

static
VOID
TestExceptionRecord(
    _In_ ULONG Code)
{
    PROCESS_INFORMATION ProcessInfo;
    DEBUG_EVENT Event;
    DWORD ContinueStatus;
    BOOL SawFastFail = FALSE;
    PEXCEPTION_RECORD Record;
    UCHAR Instruction[2];
    SIZE_T Read;

    if (!StartChild(Code, DEBUG_ONLY_THIS_PROCESS, &ProcessInfo))
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

            if (Record->ExceptionCode == STATUS_STACK_BUFFER_OVERRUN && !SawFastFail)
            {
                SawFastFail = TRUE;
                ok(Record->ExceptionFlags & EXCEPTION_NONCONTINUABLE,
                   "ExceptionFlags 0x%lx, expected EXCEPTION_NONCONTINUABLE\n", Record->ExceptionFlags);
                ok(Record->NumberParameters == 1, "NumberParameters %lu, expected 1\n", Record->NumberParameters);
                if (Record->NumberParameters >= 1)
                    ok(Record->ExceptionInformation[0] == Code, "ExceptionInformation[0] %Iu, expected %lu\n",
                       Record->ExceptionInformation[0], Code);
                ok(ReadProcessMemory(ProcessInfo.hProcess, Record->ExceptionAddress, Instruction,
                                     sizeof(Instruction), &Read) &&
                   Instruction[0] == 0xCD && Instruction[1] == 0x29,
                   "ExceptionAddress %p does not point to INT 29h\n", Record->ExceptionAddress);
                /* ReactOS dispatches it as a first chance exception, so handlers in the process still run */
                todo_if(is_reactos())
                ok(!Event.u.Exception.dwFirstChance, "Fast fail reported as a first chance exception\n");
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

        ContinueDebugEvent(Event.dwProcessId, Event.dwThreadId, ContinueStatus);
        if (Event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
            break;
    }

    ok(SawFastFail, "No STATUS_STACK_BUFFER_OVERRUN exception was reported\n");

    WaitForSingleObject(ProcessInfo.hProcess, 30000);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
}

START_TEST(RtlFailFast)
{
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 4 && !strcmp(argv[2], "child"))
    {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
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
    TestExceptionRecord(FAST_FAIL_CORRUPT_LIST_ENTRY);
}
