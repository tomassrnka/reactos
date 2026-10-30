/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test the frame a STATUS_UNWIND_CONSOLIDATE callback runs on
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifdef _M_AMD64

/*
 * A C++ catch block runs as the STATUS_UNWIND_CONSOLIDATE callback of the
 * unwind to the frame that holds the try block. When the catch block throws
 * or rethrows, the new exception must be dispatched from the target frame
 * onwards: the frames between the target and the RtlUnwindEx caller have
 * already been unwound. Windows calls the callback from a frame that unwinds
 * straight to the target context, so a stack walk from inside the callback
 * reaches the target after one frame, and handlers of unwound frames are not
 * called again.
 *
 * Call chain: ConsolidateOuterGuard -> Middle -> ConsolidateStaleGuard ->
 * DoUnwind -> RtlUnwindEx(target = Middle, STATUS_UNWIND_CONSOLIDATE).
 * The callback resumes Middle after its call to ConsolidateStaleGuard, or
 * raises an exception that ConsolidateOuterHandler handles.
 */

#define TEST_EXCEPTION_CODE 0xE0C0FFEE

LONG_PTR ConsolidateOuterGuard(LONG_PTR (*Function)(VOID));
LONG_PTR ConsolidateStaleGuard(LONG_PTR (*Function)(VOID));
extern UCHAR ConsolidateOuterResume[];

static BOOLEAN RaiseInCallback;
static volatile LONG OuterExcept, OuterUnwind, StaleExcept, StaleUnwind, CallbackCalls;
static ULONG64 TargetRip, TargetRsp, TargetRbx, TargetRsi, TargetRdi, TargetR12;
static ULONG64 DoUnwindBegin, StaleGuardBegin;
static BOOLEAN WalkVisitedDoUnwind, WalkVisitedStale, WalkReachedTarget, WalkNonvolatilesOk;
static ULONG WalkFrames;
static LONG_PTR MiddleResult;
static CONTEXT UnwindContext, OuterContext;
static EXCEPTION_RECORD ConsolidateRecord;

EXCEPTION_DISPOSITION
ConsolidateOuterHandler(
    _Inout_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID EstablisherFrame,
    _Inout_ PCONTEXT ContextRecord,
    _In_ PDISPATCHER_CONTEXT DispatcherContext)
{
    if (ExceptionRecord->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
    {
        OuterUnwind++;
        return ExceptionContinueSearch;
    }

    if (ExceptionRecord->ExceptionCode != TEST_EXCEPTION_CODE)
        return ExceptionContinueSearch;

    OuterExcept++;
    RtlUnwindEx(EstablisherFrame,
                ConsolidateOuterResume,
                ExceptionRecord,
                (PVOID)0x77,
                &OuterContext,
                DispatcherContext->HistoryTable);
    return ExceptionContinueSearch;
}

EXCEPTION_DISPOSITION
ConsolidateStaleHandler(
    _Inout_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID EstablisherFrame,
    _Inout_ PCONTEXT ContextRecord,
    _In_ PDISPATCHER_CONTEXT DispatcherContext)
{
    if (ExceptionRecord->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
        StaleUnwind++;
    else
        StaleExcept++;
    return ExceptionContinueSearch;
}

static
VOID
UnwindOneFrame(
    _Inout_ PCONTEXT Context,
    _Out_ PULONG64 EstablisherFrame,
    _Out_ PULONG64 FunctionBegin)
{
    PRUNTIME_FUNCTION FunctionEntry;
    ULONG64 ImageBase;
    PVOID HandlerData;

    FunctionEntry = RtlLookupFunctionEntry(Context->Rip, &ImageBase, NULL);
    if (FunctionEntry == NULL)
    {
        *FunctionBegin = 0;
        Context->Rip = *(PULONG64)Context->Rsp;
        Context->Rsp += sizeof(ULONG64);
        *EstablisherFrame = Context->Rsp;
        return;
    }

    *FunctionBegin = ImageBase + FunctionEntry->BeginAddress;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                     ImageBase,
                     Context->Rip,
                     FunctionEntry,
                     Context,
                     &HandlerData,
                     EstablisherFrame,
                     NULL);
}

static
ULONG64
FunctionBeginOf(
    _In_ PVOID Address)
{
    PRUNTIME_FUNCTION FunctionEntry;
    ULONG64 ImageBase;

    FunctionEntry = RtlLookupFunctionEntry((ULONG64)Address, &ImageBase, NULL);
    return FunctionEntry ? ImageBase + FunctionEntry->BeginAddress : 0;
}

static
PVOID
CALLBACK
ConsolidateCallback(
    _In_ PEXCEPTION_RECORD ExceptionRecord)
{
    CONTEXT Context;
    ULONG64 EstablisherFrame, FunctionBegin;
    ULONG i;

    CallbackCalls++;

    /* Walk the stack from here until the target context shows up */
    RtlCaptureContext(&Context);
    for (i = 0; i < 40; i++)
    {
        UnwindOneFrame(&Context, &EstablisherFrame, &FunctionBegin);
        if (FunctionBegin == DoUnwindBegin)
            WalkVisitedDoUnwind = TRUE;
        if (FunctionBegin == StaleGuardBegin)
            WalkVisitedStale = TRUE;
        if ((Context.Rsp == TargetRsp) && (Context.Rip == TargetRip))
        {
            WalkReachedTarget = TRUE;
            WalkFrames = i + 1;
            WalkNonvolatilesOk = (Context.Rbx == TargetRbx) && (Context.Rsi == TargetRsi) &&
                                 (Context.Rdi == TargetRdi) && (Context.R12 == TargetR12);
            break;
        }
        if ((Context.Rsp > TargetRsp) || (Context.Rip == 0))
            break;
    }

    if (RaiseInCallback)
        RaiseException(TEST_EXCEPTION_CODE, 0, 0, NULL);

    /* Continue in Middle, after the call to ConsolidateStaleGuard */
    return (PVOID)TargetRip;
}

static
DECLSPEC_NOINLINE
LONG_PTR
DoUnwind(VOID)
{
    CONTEXT Context, TargetContext;
    ULONG64 EstablisherFrame, FunctionBegin, TargetFrame;

    /* Unwind DoUnwind and ConsolidateStaleGuard to get Middle's context */
    RtlCaptureContext(&Context);
    UnwindOneFrame(&Context, &EstablisherFrame, &FunctionBegin);
    UnwindOneFrame(&Context, &EstablisherFrame, &FunctionBegin);
    TargetRip = Context.Rip;
    TargetRsp = Context.Rsp;
    TargetRbx = Context.Rbx;
    TargetRsi = Context.Rsi;
    TargetRdi = Context.Rdi;
    TargetR12 = Context.R12;

    /* Unwinding Middle gives its establisher frame */
    TargetContext = Context;
    UnwindOneFrame(&TargetContext, &TargetFrame, &FunctionBegin);

    RtlZeroMemory(&ConsolidateRecord, sizeof(ConsolidateRecord));
    ConsolidateRecord.ExceptionCode = STATUS_UNWIND_CONSOLIDATE;
    ConsolidateRecord.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    ConsolidateRecord.NumberParameters = 1;
    ConsolidateRecord.ExceptionInformation[0] = (ULONG_PTR)ConsolidateCallback;
    RtlUnwindEx((PVOID)TargetFrame,
                (PVOID)TargetRip,
                &ConsolidateRecord,
                (PVOID)0x1234,
                &UnwindContext,
                NULL);
    return -1;
}

static
DECLSPEC_NOINLINE
LONG_PTR
Middle(VOID)
{
    volatile LONG_PTR Result = ConsolidateStaleGuard(DoUnwind);
    MiddleResult = Result;
    return Result + 1;
}

static
VOID
ResetState(
    _In_ BOOLEAN Raise)
{
    RaiseInCallback = Raise;
    OuterExcept = OuterUnwind = StaleExcept = StaleUnwind = CallbackCalls = 0;
    WalkVisitedDoUnwind = WalkVisitedStale = WalkReachedTarget = WalkNonvolatilesOk = FALSE;
    WalkFrames = 0;
    MiddleResult = 0;
}

START_TEST(RtlUnwindExConsolidate)
{
    LONG_PTR Result;

    DoUnwindBegin = FunctionBeginOf((PVOID)DoUnwind);
    StaleGuardBegin = FunctionBeginOf((PVOID)ConsolidateStaleGuard);
    ok(DoUnwindBegin != 0, "No function entry for DoUnwind\n");
    ok(StaleGuardBegin != 0, "No function entry for ConsolidateStaleGuard\n");

    /* The callback returns and execution continues in the target frame */
    ResetState(FALSE);
    Result = ConsolidateOuterGuard(Middle);
    ok_long(CallbackCalls, 1);
    ok(MiddleResult == 0x1234, "MiddleResult = 0x%Ix\n", MiddleResult);
    ok(Result == 0x1235, "Result = 0x%Ix\n", Result);
    ok(WalkReachedTarget, "Stack walk from the callback did not reach the target context\n");
    ok(!WalkVisitedDoUnwind, "Stack walk from the callback went through the RtlUnwindEx caller\n");
    ok(!WalkVisitedStale, "Stack walk from the callback went through an unwound frame\n");
    ok(WalkFrames == 2, "Stack walk took %lu frames from the callback to the target, expected 2\n", WalkFrames);
    ok(WalkNonvolatilesOk, "Nonvolatile registers at the target differ\n");
    ok_long(StaleUnwind, 1);
    ok_long(StaleExcept, 0);
    ok_long(OuterExcept, 0);

    /* The callback raises; the dispatch continues from the target frame */
    ResetState(TRUE);
    Result = ConsolidateOuterGuard(Middle);
    ok_long(CallbackCalls, 1);
    ok(Result == 0x77, "Result = 0x%Ix\n", Result);
    ok(MiddleResult == 0, "MiddleResult = 0x%Ix\n", MiddleResult);
    ok_long(OuterExcept, 1);
    ok_long(OuterUnwind, 1);
    ok_long(StaleExcept, 0);
    ok_long(StaleUnwind, 1);
}

#else

START_TEST(RtlUnwindExConsolidate)
{
    skip("STATUS_UNWIND_CONSOLIDATE frame test is amd64 only\n");
}

#endif
