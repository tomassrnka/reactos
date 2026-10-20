/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that a thread context cannot load reserved MXCSR bits
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifdef _M_AMD64
#include <xmmintrin.h>

/* A valid, non-default value so we can see the restore actually happen. */
#define DIRTY_MXCSR 0x00003F80UL

/*
 * The MXCSR bits this processor supports. LDMXCSR/FXRSTOR fault on any bit
 * outside this mask, which is exactly what the kernel must keep out of a
 * thread context. The kernel derives it from the same FXSAVE MXCSR_MASK (and
 * uses 0xFFBF when that is zero), so the sanitized value must be the requested
 * value AND this mask.
 */
static ULONG ProcessorMxCsrMask;

static
ULONG
GetProcessorMxCsrMask(VOID)
{
    CONTEXT Context;

    RtlZeroMemory(&Context, sizeof(Context));
    Context.ContextFlags = CONTEXT_FLOATING_POINT;
    RtlCaptureContext(&Context);
    return Context.FltSave.MxCsr_Mask ? Context.FltSave.MxCsr_Mask : 0xFFBF;
}

static
DWORD
WINAPI
MxCsrThread(LPVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);
    return _mm_getcsr();
}

/*
 * NtContinue on the current thread. RtlCaptureContext gives a resume point;
 * a guard flag tells the first pass (set the bad MXCSR and continue) from the
 * resumed pass (check what we actually run with). Before the kernel fix the
 * reserved bits reach LDMXCSR and fault in kernel mode on the return to user
 * mode, so this never comes back.
 */
static
VOID
TestContinue(ULONG Value)
{
    static CONTEXT Context;
    static volatile LONG Pass;
    static volatile ULONG RunMxCsr;
    ULONG Expected = Value & ProcessorMxCsrMask;

    Pass = 0;
    _mm_setcsr(DIRTY_MXCSR);
    RtlCaptureContext(&Context);
    if (InterlockedCompareExchange(&Pass, 1, 0) == 0)
    {
        Context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;
        Context.MxCsr = Value;
        Context.FltSave.MxCsr = Value;
        NtContinue(&Context, FALSE);
        ok(0, "NtContinue(0x%08lx) returned\n", Value);
        return;
    }

    RunMxCsr = _mm_getcsr();
    trace("NtContinue(0x%08lx) -> 0x%08lx (mask 0x%08lx)\n", Value, RunMxCsr, ProcessorMxCsrMask);
    ok((RunMxCsr & ~ProcessorMxCsrMask) == 0,
       "NtContinue(0x%08lx) kept unsupported MXCSR bits: 0x%08lx\n", Value, RunMxCsr);
    ok(RunMxCsr == Expected,
       "NtContinue(0x%08lx) ran with 0x%08lx, expected 0x%08lx\n", Value, RunMxCsr, Expected);
    _mm_setcsr(0x1F80);
}

/* SetThreadContext on another, suspended thread. */
static
VOID
TestSetContext(ULONG Value)
{
    CONTEXT Context;
    HANDLE Thread;
    DWORD WaitResult, ExitMxCsr;
    ULONG Expected = Value & ProcessorMxCsrMask;
    BOOL Success, SetSucceeded;

    Thread = CreateThread(NULL, 0, MxCsrThread, NULL, CREATE_SUSPENDED, NULL);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Thread)
        return;

    RtlZeroMemory(&Context, sizeof(Context));
    Context.ContextFlags = CONTEXT_FLOATING_POINT;
    Success = GetThreadContext(Thread, &Context);
    ok(Success, "GetThreadContext failed: %lu\n", GetLastError());

    Context.ContextFlags = CONTEXT_FLOATING_POINT;
    Context.MxCsr = Value;
    Context.FltSave.MxCsr = Value;
    SetSucceeded = SetThreadContext(Thread, &Context);
    ok(SetSucceeded, "SetThreadContext failed: %lu\n", GetLastError());

    RtlZeroMemory(&Context, sizeof(Context));
    Context.ContextFlags = CONTEXT_FLOATING_POINT;
    Success = GetThreadContext(Thread, &Context);
    ok(Success, "GetThreadContext failed: %lu\n", GetLastError());
    if (Success)
    {
        trace("SetThreadContext(0x%08lx) -> context 0x%08lx (mask 0x%08lx)\n", Value, Context.MxCsr, ProcessorMxCsrMask);
        ok((Context.MxCsr & ~ProcessorMxCsrMask) == 0,
           "GetThreadContext reported unsupported MXCSR bits: 0x%08lx\n", Context.MxCsr);
        ok(Context.MxCsr == Expected,
           "GetThreadContext reported 0x%08lx, expected 0x%08lx\n", Context.MxCsr, Expected);
    }

    /* The worker must run on (no kernel fault) with the sanitized value. */
    WaitResult = ResumeThread(Thread);
    ok(WaitResult != (DWORD)-1, "ResumeThread failed: %lu\n", GetLastError());
    WaitResult = WaitForSingleObject(Thread, 10000);
    ok(WaitResult == WAIT_OBJECT_0, "Worker did not finish: %lu\n", WaitResult);
    if (WaitResult == WAIT_OBJECT_0)
    {
        Success = GetExitCodeThread(Thread, &ExitMxCsr);
        ok(Success, "GetExitCodeThread failed: %lu\n", GetLastError());
        if (Success && SetSucceeded)
        {
            ok((ExitMxCsr & ~ProcessorMxCsrMask) == 0,
               "Worker ran with unsupported MXCSR bits: 0x%08lx\n", ExitMxCsr);
            ok(ExitMxCsr == Expected,
               "Worker ran with 0x%08lx, expected 0x%08lx\n", ExitMxCsr, Expected);
        }
    }
    CloseHandle(Thread);
}
#endif /* _M_AMD64 */

START_TEST(MxCsrMask)
{
#ifndef _M_AMD64
    skip("MXCSR context test requires amd64\n");
#else
    ULONG OriginalMxCsr = _mm_getcsr();

    ProcessorMxCsrMask = GetProcessorMxCsrMask();
    trace("Processor MXCSR_MASK 0x%08lx\n", ProcessorMxCsrMask);

    TestContinue(0x80001F80);
    TestContinue(0xFFFF1F80);
    TestSetContext(0x80001F80);
    TestSetContext(0xFFFF1F80);

    _mm_setcsr(OriginalMxCsr);
#endif
}
