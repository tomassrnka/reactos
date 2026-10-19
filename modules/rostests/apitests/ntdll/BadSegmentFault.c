/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that the bad-SS access violation record does not leak kernel stack
 */

#include "precomp.h"

#ifdef _M_AMD64
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>

/*
 * When a user fault is taken with a stack segment other than the flat DPL3
 * data selector, the kernel delivers STATUS_ACCESS_VIOLATION from a locally
 * built EXCEPTION_RECORD. That record is copied to the user stack in full, so
 * its unused ExceptionInformation slots must be zeroed, or they leak kernel
 * stack bytes to user mode. The faulting case runs in a child process, which
 * first churns the kernel dispatch stack with a known pattern, so an
 * unzeroed record is seen as non-zero.
 */

VOID BadSegmentFault(VOID);
extern UCHAR BadSegmentResume[];

#define CHILD_NO_HANDLER    0xE1
#define CHILD_NO_FAULT      0xE2
#define CHILD_WRONG_CODE    0xE3
#define CHILD_OK            0x00

static EXCEPTION_RECORD FaultRecord;
static ULONG Faults;
static volatile LONG InBadSegment;

static
LONG
CALLBACK
BadSegmentHandler(PEXCEPTION_POINTERS Pointers)
{
    /* The synthetic bad-SS access violation carries no meaningful address
     * (it is the leak being tested), so it is identified by the guard flag */
    if (!InBadSegment || Pointers->ExceptionRecord->ExceptionCode != (DWORD)STATUS_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    Faults++;
    FaultRecord = *Pointers->ExceptionRecord;
    Pointers->ContextRecord->Rip = (ULONG64)BadSegmentResume;
    /* The kernel corrects the trap frame SS but not the context copied to
     * user mode, so restore the flat data selector before continuing. */
    Pointers->ContextRecord->SegSs = 0x2B; /* KGDT64_R3_DATA | RPL_MASK */
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Churn the kernel exception-dispatch stack region with real, kernel-dispatched
 * faults (a write to a bad address goes through the trap handler and
 * KiDispatchExceptionToUser), so the dispatch record of an unfixed kernel is
 * built over non-zero stack contents rather than a chance run of zeros. */
static
VOID
ChurnKernelStack(VOID)
{
    volatile ULONG_PTR Address;
    ULONG i;

    for (i = 0; i < 8; i++)
    {
        _SEH2_TRY
        {
            Address = (ULONG_PTR)0xA5A5A5A5A5A50000ULL | (i << 4);
            *(volatile CHAR *)Address = (CHAR)0xA5;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
        }
        _SEH2_END;
    }
}

static
DWORD
RunChild(VOID)
{
    ULONG i;

    if (!AddVectoredExceptionHandler(1, BadSegmentHandler))
        return CHILD_NO_HANDLER;
    ChurnKernelStack();
    Faults = 0;
    RtlFillMemory(&FaultRecord, sizeof(FaultRecord), 0x5A);
    InterlockedExchange(&InBadSegment, 1);
    BadSegmentFault();
    InterlockedExchange(&InBadSegment, 0);
    if (Faults != 1)
        return CHILD_NO_FAULT;
    if (FaultRecord.ExceptionCode != (DWORD)STATUS_ACCESS_VIOLATION)
        return CHILD_WRONG_CODE;
    /* Encode what the record carried so a run can be read from the exit code:
     * 0x100 = a non-zero ExceptionInformation slot (the leak),
     * 0x200 = a non-zero ExceptionAddress, 0x400 = NumberParameters != 0,
     * 0x800 = a non-zero chained ExceptionRecord, 0x1000 = any other non-zero
     * byte (e.g. the padding). A fixed kernel returns 0. */
    {
        ULONG Code = 0;
        for (i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
            if (FaultRecord.ExceptionInformation[i] != 0)
                Code |= 0x100;
        if (FaultRecord.ExceptionAddress != NULL)
            Code |= 0x200;
        if (FaultRecord.NumberParameters != 0)
            Code |= 0x400;
        if (FaultRecord.ExceptionRecord != NULL)
            Code |= 0x800;
        /* Compare the record byte for byte, so the padding after
         * NumberParameters is checked too */
        {
            EXCEPTION_RECORD Expected;
            RtlZeroMemory(&Expected, sizeof(Expected));
            Expected.ExceptionCode = STATUS_ACCESS_VIOLATION;
            Expected.ExceptionFlags = FaultRecord.ExceptionFlags;
            Expected.NumberParameters = FaultRecord.NumberParameters;
            if (memcmp(&FaultRecord, &Expected, sizeof(Expected)) != 0)
                Code |= 0x1000;
        }
        return Code;
    }
}

START_TEST(BadSegmentFault)
{
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 64];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    DWORD ExitCode = 0xFFFFFFFF;
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "child"))
    {
        ExitProcess(RunChild());
        return;
    }

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" BadSegmentFault child", FileName);
    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    if (!CreateProcessW(FileName, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        ok(0, "CreateProcess failed with %lu\n", GetLastError());
        return;
    }
    CloseHandle(ProcessInfo.hThread);
    if (WaitForSingleObject(ProcessInfo.hProcess, 30000) != WAIT_OBJECT_0)
    {
        ok(0, "The child did not end\n");
        TerminateProcess(ProcessInfo.hProcess, 0xDEAD);
        WaitForSingleObject(ProcessInfo.hProcess, 10000);
    }
    GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
    CloseHandle(ProcessInfo.hProcess);
    ok(ExitCode == CHILD_OK, "The bad-SS access violation record: child exit code 0x%lx "
       "(non-zero = leaked kernel stack; 0x100 info, 0x200 address, 0x800 chained, "
       "0x1000 other bytes)\n", ExitCode);
}
#else /* _M_AMD64 */

START_TEST(BadSegmentFault)
{
    skip("The bad stack segment path is amd64 only\n");
}

#endif /* _M_AMD64 */
