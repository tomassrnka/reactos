/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test unwinding through code that has an indirect RUNTIME_FUNCTION
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#if defined(_M_AMD64) && !defined(_MSC_VER)

/*
 * MSVC separates cold code from a function's body and gives the separated
 * part an indirect RUNTIME_FUNCTION (UnwindData = RVA of the function's own
 * entry | RUNTIME_FUNCTION_INDIRECT). The .NET Framework's clr.dll has about
 * 10000 of them: its first C++ exception is thrown from such a part. If the
 * dispatcher reads the indirect UnwindData as unwind info, the frames above
 * get wrong registers, and the first /GS handler (__GSHandlerCheck) sees a
 * wrong establisher frame and fails fast (STATUS_STACK_BUFFER_OVERRUN).
 * amd64/RtlUnwindIndirect.S has the functions and their unwind data.
 */

#define IND_RBX 0x1111222233334444ULL
#define IND_RSI 0x5555666677778888ULL
#define IND_CODE 0xE0494E44 /* "IND" */

ULONG64 IndOuter(ULONG64 Rbx, ULONG64 Rsi);
VOID IndPrimary(VOID);
extern UCHAR IndFragment[];
extern UCHAR IndFragmentReturn[];
extern UCHAR IndEpiFragment[];
extern UCHAR IndEpiFragmentPop[];
extern UCHAR IndSelf[];
extern RUNTIME_FUNCTION IndSelfEntry;
extern UCHAR IndOuterReturn[];
extern RUNTIME_FUNCTION IndPrimaryEntryCopy;
extern ULONG IndPrimaryHandlerData;
extern ULONG64 IndOuterFrame;
extern ULONG64 IndCookie;

typedef struct _HANDLER_CALL
{
    ULONG Calls;
    ULONG Flags;
    ULONG64 EstablisherFrame;
    ULONG64 ControlPc;
    PRUNTIME_FUNCTION FunctionEntry;
    PVOID HandlerData;
    ULONG64 Rbx, Rsi, Rbp, Rip, Rsp;
    BOOLEAN CookieOk;
} HANDLER_CALL;

static HANDLER_CALL PrimaryDispatch, PrimaryUnwind, OuterDispatch, OuterUnwind;
static ULONG64 PrimaryFrame;

static
VOID
Record(
    _Out_ HANDLER_CALL *Call,
    _In_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID EstablisherFrame,
    _In_ PDISPATCHER_CONTEXT DispatcherContext)
{
    Call->Calls++;
    Call->Flags = ExceptionRecord->ExceptionFlags;
    Call->EstablisherFrame = (ULONG64)EstablisherFrame;
    Call->ControlPc = DispatcherContext->ControlPc;
    Call->FunctionEntry = DispatcherContext->FunctionEntry;
    Call->HandlerData = DispatcherContext->HandlerData;
    Call->Rbx = DispatcherContext->ContextRecord->Rbx;
    Call->Rsi = DispatcherContext->ContextRecord->Rsi;
    Call->Rbp = DispatcherContext->ContextRecord->Rbp;
    Call->Rip = DispatcherContext->ContextRecord->Rip;
    Call->Rsp = DispatcherContext->ContextRecord->Rsp;
}

EXCEPTION_DISPOSITION
IndPrimaryHandler(
    _Inout_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID EstablisherFrame,
    _Inout_ PCONTEXT ContextRecord,
    _In_ PDISPATCHER_CONTEXT DispatcherContext)
{
    if (ExceptionRecord->ExceptionCode != IND_CODE)
        return ExceptionContinueSearch;

    Record(IS_UNWINDING(ExceptionRecord->ExceptionFlags) ? &PrimaryUnwind : &PrimaryDispatch,
           ExceptionRecord, EstablisherFrame, DispatcherContext);
    return ExceptionContinueSearch;
}

/* Checks the cookie the way __GSHandlerCheck does: the cookie slot is at the
   establisher frame plus the offset in the handler data, and the value it was
   xored with is the establisher frame plus the frame register's offset, read
   from the unwind info of DispatcherContext->FunctionEntry */
EXCEPTION_DISPOSITION
IndOuterHandler(
    _Inout_ PEXCEPTION_RECORD ExceptionRecord,
    _In_ PVOID EstablisherFrame,
    _Inout_ PCONTEXT ContextRecord,
    _In_ PDISPATCHER_CONTEXT DispatcherContext)
{
    HANDLER_CALL *Call;
    PUCHAR UnwindInfo;
    ULONG64 Xor, Slot;
    ULONG CookieOffset;

    if (ExceptionRecord->ExceptionCode != IND_CODE)
        return ExceptionContinueSearch;

    Call = IS_UNWINDING(ExceptionRecord->ExceptionFlags) ? &OuterUnwind : &OuterDispatch;
    Record(Call, ExceptionRecord, EstablisherFrame, DispatcherContext);

    /* Only read the frame when it is the one IndOuter set up */
    if ((ULONG64)EstablisherFrame == IndOuterFrame)
    {
        CookieOffset = *(PULONG)DispatcherContext->HandlerData;
        UnwindInfo = (PUCHAR)(DispatcherContext->ImageBase + DispatcherContext->FunctionEntry->UnwindData);
        Xor = (ULONG64)EstablisherFrame;
        if (UnwindInfo[3] & 0x0F)
            Xor += UnwindInfo[3] & 0xF0;
        Slot = *(PULONG64)((ULONG64)EstablisherFrame + CookieOffset);
        Call->CookieOk = ((Slot ^ Xor) == IndCookie);
    }
    return ExceptionContinueSearch;
}

VOID
IndRaise(VOID)
{
    RaiseException(IND_CODE, 0, 0, NULL);
}

static
VOID
TestLookup(VOID)
{
    PRUNTIME_FUNCTION FunctionEntry;
    ULONG64 ImageBase = 0;

    /* A pc in the separated part gives the function's entry */
    FunctionEntry = RtlLookupFunctionEntry((ULONG64)IndFragmentReturn, &ImageBase, NULL);
    ok(FunctionEntry == &IndPrimaryEntryCopy, "Fragment entry %p, expected %p\n",
       FunctionEntry, &IndPrimaryEntryCopy);
    ok(ImageBase == (ULONG64)GetModuleHandleW(NULL), "ImageBase %p\n", (PVOID)ImageBase);
    if (FunctionEntry != NULL)
    {
        ok(!(FunctionEntry->UnwindData & RUNTIME_FUNCTION_INDIRECT),
           "UnwindData 0x%lx is still indirect\n", FunctionEntry->UnwindData);
    }

    /* The function's own code gives its own entry */
    FunctionEntry = RtlLookupFunctionEntry((ULONG64)IndPrimary, &ImageBase, NULL);
    ok(FunctionEntry != NULL, "No entry for IndPrimary\n");
    if (FunctionEntry != NULL)
    {
        ok(FunctionEntry->BeginAddress == IndPrimaryEntryCopy.BeginAddress &&
           FunctionEntry->UnwindData == IndPrimaryEntryCopy.UnwindData,
           "IndPrimary entry %lx/%lx\n", FunctionEntry->BeginAddress, FunctionEntry->UnwindData);
    }
}

static
VOID
TestVirtualUnwind(VOID)
{
    ULONG64 Stack[16], ImageBase, EstablisherFrame = 0;
    PRUNTIME_FUNCTION FunctionEntry;
    PEXCEPTION_ROUTINE Handler;
    PVOID HandlerData = NULL;
    CONTEXT Context;
    ULONG i;

    /* IndPrimary's frame as the fragment sees it: 0x20 bytes, rbp, rsi, rbx,
       return address */
    for (i = 0; i < ARRAYSIZE(Stack); i++)
        Stack[i] = 0xAAAA000000000000ULL + i;
    Stack[4] = 0x1234; /* rbp */
    Stack[5] = 0x5678; /* rsi */
    Stack[6] = 0x9ABC; /* rbx */
    Stack[7] = (ULONG64)IndOuter + 0x40;

    RtlZeroMemory(&Context, sizeof(Context));
    Context.Rip = (ULONG64)IndFragmentReturn;
    Context.Rsp = (ULONG64)&Stack[0];

    FunctionEntry = RtlLookupFunctionEntry(Context.Rip, &ImageBase, NULL);
    if (FunctionEntry != &IndPrimaryEntryCopy)
    {
        skip("The fragment's entry is not resolved\n");
        return;
    }

    Handler = RtlVirtualUnwind(UNW_FLAG_EHANDLER, ImageBase, Context.Rip, FunctionEntry,
                               &Context, &HandlerData, &EstablisherFrame, NULL);
    ok(Handler == (PEXCEPTION_ROUTINE)IndPrimaryHandler, "Handler %p, expected %p\n",
       Handler, IndPrimaryHandler);
    ok(HandlerData == &IndPrimaryHandlerData, "HandlerData %p, expected %p\n",
       HandlerData, &IndPrimaryHandlerData);
    ok(EstablisherFrame == (ULONG64)&Stack[0], "EstablisherFrame %p, expected %p\n",
       (PVOID)EstablisherFrame, &Stack[0]);
    ok(Context.Rbp == 0x1234, "Rbp 0x%I64x\n", Context.Rbp);
    ok(Context.Rsi == 0x5678, "Rsi 0x%I64x\n", Context.Rsi);
    ok(Context.Rbx == 0x9ABC, "Rbx 0x%I64x\n", Context.Rbx);
    ok(Context.Rip == Stack[7], "Rip %p, expected %p\n", (PVOID)Context.Rip, (PVOID)Stack[7]);
    ok(Context.Rsp == (ULONG64)&Stack[8], "Rsp %p, expected %p\n", (PVOID)Context.Rsp, &Stack[8]);
}

static
VOID
TestFragmentEpilog(VOID)
{
    ULONG64 Stack[16], ImageBase, EstablisherFrame;
    PRUNTIME_FUNCTION FunctionEntry;
    PEXCEPTION_ROUTINE Handler;
    PVOID HandlerData;
    CONTEXT Context;
    ULONG i, Step;
    PUCHAR Pc[2] = { IndEpiFragment, IndEpiFragmentPop };

    /* The frame is released in a separated part: the code that frees it is
       an epilog of IndPrimary, whose range is the part's own range. Once
       the allocation is gone (second step) only the pops remain. */
    for (Step = 0; Step < 2; Step++)
    {
        for (i = 0; i < ARRAYSIZE(Stack); i++)
            Stack[i] = 0xAAAA000000000000ULL + i;
        i = (Step == 0) ? 4 : 0;
        Stack[i + 0] = 0x1234; /* rbp */
        Stack[i + 1] = 0x5678; /* rsi */
        Stack[i + 2] = 0x9ABC; /* rbx */
        Stack[i + 3] = (ULONG64)IndOuter + 0x40;

        RtlZeroMemory(&Context, sizeof(Context));
        Context.Rip = (ULONG64)Pc[Step];
        Context.Rsp = (ULONG64)&Stack[0];
        Context.Rbp = Context.Rsi = Context.Rbx = 0xBAD;

        FunctionEntry = RtlLookupFunctionEntry(Context.Rip, &ImageBase, NULL);
        ok(FunctionEntry == &IndPrimaryEntryCopy, "Step %lu: entry %p, expected %p\n",
           Step, FunctionEntry, &IndPrimaryEntryCopy);
        if (FunctionEntry != &IndPrimaryEntryCopy)
            continue;

        HandlerData = NULL;
        EstablisherFrame = 0;
        Handler = RtlVirtualUnwind(UNW_FLAG_EHANDLER, ImageBase, Context.Rip, FunctionEntry,
                                   &Context, &HandlerData, &EstablisherFrame, NULL);
        ok(Handler == NULL, "Step %lu: handler %p in an epilog\n", Step, Handler);
        ok(Context.Rbp == 0x1234, "Step %lu: Rbp 0x%I64x\n", Step, Context.Rbp);
        ok(Context.Rsi == 0x5678, "Step %lu: Rsi 0x%I64x\n", Step, Context.Rsi);
        ok(Context.Rbx == 0x9ABC, "Step %lu: Rbx 0x%I64x\n", Step, Context.Rbx);
        ok(Context.Rip == Stack[i + 3], "Step %lu: Rip %p, expected %p\n",
           Step, (PVOID)Context.Rip, (PVOID)Stack[i + 3]);
        ok(Context.Rsp == (ULONG64)&Stack[i + 4], "Step %lu: Rsp %p, expected %p\n",
           Step, (PVOID)Context.Rsp, &Stack[i + 4]);
    }
}

static
BOOLEAN
RunsOnReactOS(VOID)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\Software\\ReactOS");
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Key;

    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(NtOpenKey(&Key, KEY_READ, &ObjectAttributes)))
        return FALSE;
    NtClose(Key);
    return TRUE;
}

static
VOID
TestSelfReference(VOID)
{
    ULONG64 ImageBase = 0, EstablisherFrame = 0, Stack[4] = { 0 };
    PRUNTIME_FUNCTION FunctionEntry;
    PEXCEPTION_ROUTINE Handler;
    PVOID HandlerData = (PVOID)(ULONG_PTR)0x1234;
    CONTEXT Context;

    /* Windows is not asked about an entry that points to itself: only the
       data in this file can end the walk through it */
    if (!RunsOnReactOS())
    {
        skip("Self-referencing entry is only checked on ReactOS\n");
        return;
    }

    ok(((ULONG_PTR)&IndSelfEntry & 3) == 0, "Entry is not aligned\n");
    FunctionEntry = RtlLookupFunctionEntry((ULONG64)IndSelf, &ImageBase, NULL);
    ok(FunctionEntry == NULL, "Self-referencing entry resolved to %p\n", FunctionEntry);

    RtlZeroMemory(&Context, sizeof(Context));
    Context.Rip = (ULONG64)IndSelf;
    Context.Rsp = (ULONG64)&Stack[0];
    Handler = RtlVirtualUnwind(UNW_FLAG_EHANDLER, (ULONG64)GetModuleHandleW(NULL), Context.Rip,
                               &IndSelfEntry, &Context, &HandlerData, &EstablisherFrame, NULL);
    ok(Handler == NULL, "Handler %p\n", Handler);
    ok(Context.Rip == (ULONG64)IndSelf, "Rip changed to %p\n", (PVOID)Context.Rip);
    ok(Context.Rsp == (ULONG64)&Stack[0], "Rsp changed to %p\n", (PVOID)Context.Rsp);
    ok(HandlerData == NULL, "HandlerData %p not set\n", HandlerData);
    ok(EstablisherFrame == (ULONG64)&Stack[0], "EstablisherFrame %p not set\n", (PVOID)EstablisherFrame);
}

static ULONG CallbackCalls;
static ULONG64 CallbackPc[16];
static ULONG64 DynBase;
static RUNTIME_FUNCTION DynEntry;

static
PRUNTIME_FUNCTION
CALLBACK
DynCallback(
    _In_ DWORD64 ControlPc,
    _In_ PVOID Context)
{
    if (CallbackCalls < ARRAYSIZE(CallbackPc))
        CallbackPc[CallbackCalls] = ControlPc;
    CallbackCalls++;

    if ((ControlPc >= DynBase + 0x40) && (ControlPc < DynBase + 0x80))
        return &DynEntry;
    return NULL;
}

typedef BOOLEAN (NTAPI *PFN_INSTALL)(ULONG_PTR, ULONG_PTR, ULONG, PGET_RUNTIME_FUNCTION_CALLBACK, PVOID, PCWSTR);
typedef BOOLEAN (NTAPI *PFN_DELETE_TABLE)(PRUNTIME_FUNCTION);

static
VOID
TestDynamicCallback(VOID)
{
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    PFN_INSTALL pInstall = (PFN_INSTALL)GetProcAddress(hNtdll, "RtlInstallFunctionTableCallback");
    PFN_DELETE_TABLE pDelete = (PFN_DELETE_TABLE)GetProcAddress(hNtdll, "RtlDeleteFunctionTable");
    ULONG64 Stack[4], ImageBase, EstablisherFrame;
    PRUNTIME_FUNCTION FunctionEntry;
    PEXCEPTION_ROUTINE Handler;
    PVOID HandlerData;
    CONTEXT Context;
    PUCHAR Mem;
    ULONG Before, i;
    ULONG64 Pc[] = { 0x100, 0x7F0, 0, 0 };
    BOOLEAN Ok;

    if (!pInstall || !pDelete)
    {
        skip("Function table callbacks are not available\n");
        return;
    }

    Mem = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    ok(Mem != NULL, "VirtualAlloc failed\n");
    if (Mem == NULL)
        return;
    DynBase = (ULONG64)Mem;

    /* A leaf function: no unwind codes, the return address is at rsp */
    Mem[0] = 1;             /* version 1, no flags */
    Mem[0x40] = 0xC3;       /* ret */
    DynEntry.BeginAddress = 0x40;
    DynEntry.EndAddress = 0x80;
    DynEntry.UnwindData = 0;

    Ok = pInstall(DynBase | 3, DynBase, 0x1000, DynCallback, NULL, NULL);
    ok(Ok, "RtlInstallFunctionTableCallback failed, %lu\n", GetLastError());
    if (!Ok)
    {
        VirtualFree(Mem, 0, MEM_RELEASE);
        return;
    }

    CallbackCalls = 0;
    ImageBase = 0;
    FunctionEntry = RtlLookupFunctionEntry(DynBase + 0x44, &ImageBase, NULL);
    ok(FunctionEntry == &DynEntry, "Entry %p, expected %p\n", FunctionEntry, &DynEntry);
    ok(ImageBase == DynBase, "ImageBase %p\n", (PVOID)ImageBase);
    ok(CallbackCalls == 1, "Lookup called the callback %lu times\n", CallbackCalls);
    ok(CallbackPc[0] == DynBase + 0x44, "Callback pc %p\n", (PVOID)CallbackPc[0]);

    /* A pc inside the function: RtlVirtualUnwind needs the entry only */
    Before = CallbackCalls;
    Stack[0] = 0x4242;
    RtlZeroMemory(&Context, sizeof(Context));
    Context.Rip = DynBase + 0x44;
    Context.Rsp = (ULONG64)&Stack[0];
    HandlerData = NULL;
    EstablisherFrame = 0;
    Handler = RtlVirtualUnwind(UNW_FLAG_EHANDLER, DynBase, Context.Rip, &DynEntry,
                               &Context, &HandlerData, &EstablisherFrame, NULL);
    ok(Handler == NULL, "Handler %p\n", Handler);
    ok(Context.Rip == 0x4242, "Rip %p\n", (PVOID)Context.Rip);
    ok(Context.Rsp == (ULONG64)&Stack[1], "Rsp %p\n", (PVOID)Context.Rsp);
    ok(CallbackCalls == Before, "RtlVirtualUnwind inside the function called the callback %lu times\n",
       CallbackCalls - Before);

    /* A pc in the table's range but outside the function, and one outside
       the range. Windows measured: RtlLookupFunctionEntry runs the callback
       for the first only, and finds no entry; RtlVirtualUnwind with the
       function's entry runs no callback for either, and unwinds with the
       entry as for any pc in the leaf function */
    Pc[2] = DynBase + 0x200;
    Pc[3] = DynBase + 0x1000 + 0x40;
    for (i = 2; i < ARRAYSIZE(Pc); i++)
    {
        Before = CallbackCalls;
        ImageBase = 0;
        FunctionEntry = RtlLookupFunctionEntry(Pc[i], &ImageBase, NULL);
        ok(FunctionEntry == NULL, "Entry %p for pc %p\n", FunctionEntry, (PVOID)Pc[i]);
        ok(CallbackCalls - Before == ((i == 2) ? 1 : 0),
           "Lookup of pc %p called the callback %lu times\n", (PVOID)Pc[i], CallbackCalls - Before);

        Before = CallbackCalls;
        Stack[0] = 0x4242;
        RtlZeroMemory(&Context, sizeof(Context));
        Context.Rip = Pc[i];
        Context.Rsp = (ULONG64)&Stack[0];
        Handler = RtlVirtualUnwind(UNW_FLAG_EHANDLER, DynBase, Context.Rip, &DynEntry,
                                   &Context, &HandlerData, &EstablisherFrame, NULL);
        ok(CallbackCalls == Before, "RtlVirtualUnwind for pc %p called the callback %lu times\n",
           (PVOID)Pc[i], CallbackCalls - Before);
        ok(Handler == NULL, "Handler %p for pc %p\n", Handler, (PVOID)Pc[i]);
        ok(Context.Rip == 0x4242, "Rip %p for pc %p\n", (PVOID)Context.Rip, (PVOID)Pc[i]);
        ok(Context.Rsp == (ULONG64)&Stack[1], "Rsp %p for pc %p\n", (PVOID)Context.Rsp, (PVOID)Pc[i]);
    }

    ok(pDelete((PRUNTIME_FUNCTION)(DynBase | 3)), "RtlDeleteFunctionTable failed\n");
    VirtualFree(Mem, 0, MEM_RELEASE);
}

static
VOID
TestDispatch(VOID)
{
    ULONG64 ImageBase;
    volatile ULONG64 Result = 0;
    BOOLEAN Caught = FALSE;

    /* A wrong unwind through the fragment takes the process down, so only
       dispatch when the lookup works */
    if (RtlLookupFunctionEntry((ULONG64)IndFragmentReturn, &ImageBase, NULL) != &IndPrimaryEntryCopy)
    {
        skip("The fragment's entry is not resolved\n");
        return;
    }

    _SEH2_TRY
    {
        Result = IndOuter(IND_RBX, IND_RSI);
    }
    _SEH2_EXCEPT(_SEH2_GetExceptionCode() == IND_CODE ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        Caught = TRUE;
    }
    _SEH2_END;

    ok(Caught, "Exception not caught, result 0x%I64x\n", Result);

    /* The separated part is a part of IndPrimary: its handler runs, with
       IndPrimary's entry, and IndPrimary's frame */
    PrimaryFrame = IndOuterFrame - 0x40;
    ok(PrimaryDispatch.Calls == 1, "IndPrimary dispatch calls %lu\n", PrimaryDispatch.Calls);
    ok(PrimaryDispatch.ControlPc == (ULONG64)IndFragmentReturn, "ControlPc %p, expected %p\n",
       (PVOID)PrimaryDispatch.ControlPc, IndFragmentReturn);
    ok(PrimaryDispatch.FunctionEntry == &IndPrimaryEntryCopy, "FunctionEntry %p, expected %p\n",
       PrimaryDispatch.FunctionEntry, &IndPrimaryEntryCopy);
    ok(PrimaryDispatch.HandlerData == &IndPrimaryHandlerData, "HandlerData %p\n",
       PrimaryDispatch.HandlerData);
    ok(PrimaryDispatch.EstablisherFrame == PrimaryFrame, "IndPrimary frame %p, expected %p\n",
       (PVOID)PrimaryDispatch.EstablisherFrame, (PVOID)PrimaryFrame);

    /* IndOuter's frame is found, and the registers IndPrimary saved are back */
    ok(OuterDispatch.Calls == 1, "IndOuter dispatch calls %lu\n", OuterDispatch.Calls);
    ok(OuterDispatch.EstablisherFrame == IndOuterFrame, "IndOuter frame %p, expected %p\n",
       (PVOID)OuterDispatch.EstablisherFrame, (PVOID)IndOuterFrame);
    ok(OuterDispatch.CookieOk, "The /GS style cookie check failed\n");
    /* In the dispatch the context is the one after the frame's virtual
       unwind: for IndPrimary's handler it is IndOuter's, with the registers
       IndPrimary saved, and for IndOuter's handler the test function's */
    ok(PrimaryDispatch.Rbx == IND_RBX, "Rbx 0x%I64x\n", PrimaryDispatch.Rbx);
    ok(PrimaryDispatch.Rsi == IND_RSI, "Rsi 0x%I64x\n", PrimaryDispatch.Rsi);
    ok(PrimaryDispatch.Rbp == IndOuterFrame + 0x20, "Rbp 0x%I64x, expected 0x%I64x\n",
       PrimaryDispatch.Rbp, IndOuterFrame + 0x20);
    ok(PrimaryDispatch.Rip == (ULONG64)IndOuterReturn, "Rip %p, expected %p\n",
       (PVOID)PrimaryDispatch.Rip, IndOuterReturn);
    ok(PrimaryDispatch.Rsp == IndOuterFrame, "Rsp 0x%I64x, expected 0x%I64x\n",
       PrimaryDispatch.Rsp, IndOuterFrame);
    ok(OuterDispatch.Rsp == IndOuterFrame + 0x50, "Rsp 0x%I64x, expected 0x%I64x\n",
       OuterDispatch.Rsp, IndOuterFrame + 0x50);

    /* The unwind to the except block runs IndOuter's termination handler the same way */
    ok(OuterUnwind.Calls == 1, "IndOuter unwind calls %lu\n", OuterUnwind.Calls);
    ok(OuterUnwind.EstablisherFrame == IndOuterFrame, "IndOuter unwind frame %p, expected %p\n",
       (PVOID)OuterUnwind.EstablisherFrame, (PVOID)IndOuterFrame);
    ok(OuterUnwind.CookieOk, "The /GS style cookie check failed in the unwind\n");
    ok(OuterUnwind.Flags & EXCEPTION_UNWINDING, "Unwind flags 0x%lx\n", OuterUnwind.Flags);

    /* In the unwind the context is the frame's own */
    ok(OuterUnwind.Rbx == IND_RBX, "Unwind Rbx 0x%I64x\n", OuterUnwind.Rbx);
    ok(OuterUnwind.Rsi == IND_RSI, "Unwind Rsi 0x%I64x\n", OuterUnwind.Rsi);
    ok(OuterUnwind.Rbp == IndOuterFrame + 0x20, "Unwind Rbp 0x%I64x\n", OuterUnwind.Rbp);
    ok(OuterUnwind.Rip == (ULONG64)IndOuterReturn, "Unwind Rip %p, expected %p\n",
       (PVOID)OuterUnwind.Rip, IndOuterReturn);
    ok(OuterUnwind.Rsp == IndOuterFrame, "Unwind Rsp 0x%I64x\n", OuterUnwind.Rsp);

    /* IndPrimary has only an exception handler */
    ok(PrimaryUnwind.Calls == 0, "IndPrimary unwind calls %lu\n", PrimaryUnwind.Calls);
}

START_TEST(RtlUnwindIndirect)
{
    TestLookup();
    TestVirtualUnwind();
    TestFragmentEpilog();
    TestSelfReference();
    TestDynamicCallback();
    TestDispatch();
}

#else

START_TEST(RtlUnwindIndirect)
{
    skip("The test needs amd64 and a GAS assembler\n");
}

#endif
