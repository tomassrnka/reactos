/* ReactOS WoW64 integer exception contexts. SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64p.h"

PVOID Wow64ExceptionDispatcher;

typedef struct _WOW64_APC_FRAME
{
    WOW64_CPU_STATE *Previous;
    CONTEXT *Native;
    WOW64_CONTEXT *Guest;
    LONG PreviousTransition;
} WOW64_APC_FRAME;

PVOID Wow64ApcDispatcher;

NTSTATUS Wow64Continue(const WOW64_CONTEXT *Context, BOOLEAN TestAlert)
{
    CONTEXT Native;
    WOW64_CPU_STATE *Cpu = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU];
    WOW64_APC_FRAME *Apc = Cpu->ApcFrame;
    if ((Context->ContextFlags & WOW64_CONTEXT_FULL) != WOW64_CONTEXT_FULL ||
        (Context->ContextFlags & ~WOW64_CONTEXT_ALL))
        return STATUS_NOT_SUPPORTED;
    if (Apc && Context == Apc->Guest)
    {
        /* Return through the interrupted native syscall before leaving its
         * translator. It owns temporaries which must remain alive for APCs. */
        InterlockedExchange(&Cpu->InTransition, 1);
        Apc->Previous->FsRedirectionDisabled = Cpu->FsRedirectionDisabled;
        Wow64ContextToCpu(Apc->Previous, Context);
        NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU] = Apc->Previous;
        if (Apc->Native->SegCs == 0x23)
        {
            InterlockedExchange(&Apc->Previous->InTransition, 1);
            Wow64ContextToNative(Apc->Native, Context);
        }
        else InterlockedExchange(&Apc->Previous->InTransition, Apc->PreviousTransition);
        return NtContinue(Apc->Native, TestAlert);
    }
    InterlockedExchange(&Cpu->InTransition, 1);
    while (Apc && Context->Esp >= Apc->Guest->Esp)
    {
        /* An exception or longjmp can unwind out of an APC instead of
         * returning to its dispatcher. Discard its native simulation frame. */
        Apc->Previous->FsRedirectionDisabled = Cpu->FsRedirectionDisabled;
        Cpu = Apc->Previous;
        NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU] = Cpu;
        Apc = Cpu->ApcFrame;
    }
    RtlCaptureContext(&Native);
    Wow64ContextToNative(&Native, Context);
    InterlockedExchange(&Cpu->InTransition, 1);
    return NtContinue(&Native, TestAlert);
}

/* The lowest guest stack address a dispatch frame may use. The committed
 * limit (StackLimit) only moves down when the guard page below it is touched,
 * which a frame carved below ESP does on its own: the frame may extend into
 * the rest of the reservation, except its last page. */
static ULONG GuestStackFloor(const TEB32 *Teb32)
{
    ULONG Floor = Teb32->NtTib.StackLimit;
    if (Teb32->DeallocationStack && Teb32->DeallocationStack <= MAXULONG - PAGE_SIZE)
        Floor = min(Floor, Teb32->DeallocationStack + PAGE_SIZE);
    return Floor;
}

/* Touches the guest stack pages from the committed limit down to Low in
 * order, so that each one is the guard page when it is reached and the
 * stack grows page by page instead of being skipped. Raises (under the
 * caller's SEH) when the stack cannot grow any further. A macro, not a call:
 * the frame may lie in pages that are not committed yet. */
#define WOW64_GROW_GUEST_STACK(Teb32, Low) do { \
    ULONG_PTR Page_ = (ULONG_PTR)(Teb32)->NtTib.StackLimit; \
    while (Page_ > ((ULONG_PTR)(Low) & ~((ULONG_PTR)PAGE_SIZE - 1))) \
    { \
        Page_ -= PAGE_SIZE; \
        (void)*(volatile UCHAR *)Page_; \
    } \
} while (0)

/* The native dispatcher supplies its complete interrupted context in R9.
 * Negative routine values encode a guest address; normal native APCs retain
 * the ordinary ntdll dispatch path. */
VOID NTAPI Wow64PrepareForApc(PVOID Arg1, PVOID Arg2, PVOID Arg3, PCONTEXT Native)
{
    struct
    {
        ULONG Routine, Arg1, Arg2, Arg3;
        WOW64_CONTEXT Context;
        ULONG Registration[2];
    } *Guest;
    WOW64_CPU_STATE *Previous = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU];
    WOW64_CPU_STATE Cpu;
    WOW64_CONTEXT Context;
    WOW64_APC_FRAME Apc;
    TEB32 *Teb32 = WOW64_TEB32(NtCurrentTeb());

    /* Encoded guest APCs cannot execute on a native-only helper. Returning
     * drops this APC; ntdll resumes its interrupted native context. */
    if (!Previous) return;
    /* Context APCs must not update either frame while its saved registers
     * are being cloned or ownership is being transferred to/from it. */
    Apc.PreviousTransition = InterlockedExchange(&Previous->InTransition, 1);
    Cpu = *Previous;
    if (Native->SegCs == 0x23) Wow64NativeToContext(&Context, Native);
    else Wow64CpuToContext(&Context, Previous);
    /* Carve the APC frame from the guest ESP. Validate ESP against the guest
     * stack in 32-bit arithmetic before forming the pointer (ProbeForWrite is
     * kernel-only here), so a tiny or wild ESP cannot wrap the subtraction or
     * the range test; the 15-byte margin covers the 16-byte frame alignment.
     * The write then runs under SEH as a second line of defence. A bad ESP
     * terminates this process cleanly instead of writing an unchecked pointer. */
    if (Context.Esp > Teb32->NtTib.StackBase ||
        Context.Esp < GuestStackFloor(Teb32) ||
        Context.Esp - GuestStackFloor(Teb32) < sizeof(*Guest) + 15)
    {
        NtTerminateProcess(NtCurrentProcess(), STATUS_STACK_OVERFLOW);
        return;
    }
    Guest = (PVOID)(ULONG_PTR)((Context.Esp - sizeof(*Guest)) & ~15);
    _SEH2_TRY
    {
        WOW64_GROW_GUEST_STACK(Teb32, Guest);
        Guest->Routine = (ULONG)(-(LONG64)Native->P4Home >> 2);
        Guest->Arg1 = PtrToUlong(Arg1);
        Guest->Arg2 = PtrToUlong(Arg2);
        Guest->Arg3 = PtrToUlong(Arg3);
        Guest->Context = Context;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        NtTerminateProcess(NtCurrentProcess(), _SEH2_GetExceptionCode());
        _SEH2_YIELD(return);
    }
    _SEH2_END;
    Apc.Previous = Previous;
    Apc.Native = Native;
    Apc.Guest = &Guest->Context;
    Cpu.ApcFrame = &Apc;
    Cpu.Esp = PtrToUlong(Guest);
    Cpu.Eip = PtrToUlong(Wow64ApcDispatcher);
    Wow64CpuRun(&Cpu);
}

VOID NTAPI Wow64PrepareForException(PEXCEPTION_RECORD Record, PCONTEXT Context)
{
    struct
    {
        ULONG RecordPointer, ContextPointer;
        EXCEPTION_RECORD32 Record;
        WOW64_CONTEXT Context;
    } Frame;
    CONTEXT Native;
    ULONG i;
    NTSTATUS Status;
    TEB32 *Teb32 = NULL;
    volatile BOOLEAN Grown;

    if (Context->SegCs != 0x23) return;
    Teb32 = WOW64_TEB32(NtCurrentTeb());
    if (!Wow64ExceptionDispatcher ||
        (ULONG_PTR)&Frame < GuestStackFloor(Teb32) ||
        (ULONG_PTR)&Native < GuestStackFloor(Teb32) ||
        (ULONG_PTR)(&Frame + 1) > Teb32->NtTib.StackBase)
        goto Failed;
    /* These locals lie below the kernel's frame on the guest stack: grow the
     * stack over them before they are written */
    Grown = TRUE;
    _SEH2_TRY
    {
        WOW64_GROW_GUEST_STACK(Teb32, min((ULONG_PTR)&Native, (ULONG_PTR)&Frame));
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Grown = FALSE;
    }
    _SEH2_END;
    if (!Grown) goto Failed;

    /* The kernel's amd64 frame already lives on the guest stack. A local
     * frame is below it and cannot overwrite the source context or record.
     * The i386 dispatcher takes two pointers at ESP, with no return address. */
    Native = *Context;
    Wow64NativeToContext(&Frame.Context, Context);
    RtlZeroMemory(&Frame.Record, sizeof(Frame.Record));
    Frame.Record.ExceptionCode = Record->ExceptionCode;
    Frame.Record.ExceptionFlags = Record->ExceptionFlags;
    Frame.Record.ExceptionAddress = PtrToUlong(Record->ExceptionAddress);
    Frame.Record.NumberParameters = min(Record->NumberParameters, EXCEPTION_MAXIMUM_PARAMETERS);
    for (i = 0; i < Frame.Record.NumberParameters; ++i)
        Frame.Record.ExceptionInformation[i] = (ULONG)Record->ExceptionInformation[i];
    Frame.RecordPointer = PtrToUlong(&Frame.Record);
    Frame.ContextPointer = PtrToUlong(&Frame.Context);
    Native.Rip = (ULONG_PTR)Wow64ExceptionDispatcher;
    Native.Rsp = (ULONG_PTR)&Frame;
    Native.SegCs = 0x23;
    Native.SegSs = Native.SegDs = Native.SegEs = 0x2b;
    Native.SegFs = 0x53;
    InterlockedExchange(&((WOW64_CPU_STATE *)NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU])->InTransition, 1);
    Status = NtContinue(&Native, FALSE);
    DbgPrint("WoW64: exception dispatch failed: %08lx\n", Status);
Failed:
    DbgPrint("WoW64: cannot dispatch guest exception %08lx (frame %p, guest stack %08lx-%08lx)\n",
             Record->ExceptionCode, &Frame, Teb32 ? Teb32->NtTib.StackLimit : 0, Teb32 ? Teb32->NtTib.StackBase : 0);
    NtTerminateProcess(NtCurrentProcess(), Record->ExceptionCode);
}

NTSTATUS Wow64RaiseException(const EXCEPTION_RECORD32 *Record, const WOW64_CONTEXT *Context,
                              BOOLEAN FirstChance)
{
    EXCEPTION_RECORD NativeRecord = {0};
    CONTEXT NativeContext;
    ULONG i;
    if (Record->NumberParameters > EXCEPTION_MAXIMUM_PARAMETERS) return STATUS_INVALID_PARAMETER;
    RtlCaptureContext(&NativeContext);
    Wow64ContextToNative(&NativeContext, Context);
    NativeRecord.ExceptionCode = Record->ExceptionCode;
    NativeRecord.ExceptionFlags = Record->ExceptionFlags;
    NativeRecord.ExceptionAddress = PTR32(Record->ExceptionAddress);
    NativeRecord.NumberParameters = Record->NumberParameters;
    for (i = 0; i < Record->NumberParameters; ++i)
        NativeRecord.ExceptionInformation[i] = Record->ExceptionInformation[i];
    InterlockedExchange(&((WOW64_CPU_STATE *)NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU])->InTransition, 1);
    return NtRaiseException(&NativeRecord, &NativeContext, FirstChance);
}
