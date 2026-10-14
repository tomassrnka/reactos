/* ReactOS WoW64 integer exception contexts. SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64p.h"

PVOID Wow64ExceptionDispatcher;

static VOID ContextTo32(WOW64_CONTEXT *To, const CONTEXT *From)
{
    RtlZeroMemory(To, sizeof(*To));
    /* Floating-point/debug context delivery is a separate extension. Do not
     * advertise state that the native exception dispatcher does not capture. */
    To->ContextFlags = WOW64_CONTEXT_FULL;
#define REG(x, y) To->x = (ULONG)From->y
    REG(Eax, Rax); REG(Ebx, Rbx); REG(Ecx, Rcx); REG(Edx, Rdx);
    REG(Esi, Rsi); REG(Edi, Rdi); REG(Ebp, Rbp); REG(Esp, Rsp); REG(Eip, Rip);
    REG(EFlags, EFlags);
    REG(SegCs, SegCs); REG(SegSs, SegSs); REG(SegDs, SegDs);
    REG(SegEs, SegEs); REG(SegFs, SegFs); REG(SegGs, SegGs);
#undef REG
}

static NTSTATUS ContextFrom32(CONTEXT *To, const WOW64_CONTEXT *From)
{
    ULONG Flags = From->ContextFlags;
    if ((Flags & WOW64_CONTEXT_FULL) != WOW64_CONTEXT_FULL ||
        (Flags & ~(WOW64_CONTEXT_FULL)))
        return STATUS_NOT_SUPPORTED;
    RtlCaptureContext(To);
    To->ContextFlags = CONTEXT_AMD64;
    if (Flags & (WOW64_CONTEXT_INTEGER & ~WOW64_CONTEXT_i386))
    {
        To->ContextFlags |= CONTEXT_INTEGER;
#define REG(x, y) To->x = From->y
        REG(Rax, Eax); REG(Rbx, Ebx); REG(Rcx, Ecx); REG(Rdx, Edx);
        REG(Rsi, Esi); REG(Rdi, Edi);
#undef REG
    }
    if (Flags & (WOW64_CONTEXT_CONTROL & ~WOW64_CONTEXT_i386))
    {
        To->ContextFlags |= CONTEXT_CONTROL | CONTEXT_INTEGER;
        To->Rbp = From->Ebp;
        To->Rsp = From->Esp;
        To->Rip = From->Eip;
        To->EFlags = From->EFlags;
        To->SegCs = 0x23;
        To->SegSs = 0x2b;
    }
    if (Flags & (WOW64_CONTEXT_SEGMENTS & ~WOW64_CONTEXT_i386))
    {
        To->ContextFlags |= CONTEXT_SEGMENTS;
        To->SegDs = From->SegDs;
        To->SegEs = From->SegEs;
        To->SegFs = 0x53;
        To->SegGs = From->SegGs;
    }
    return STATUS_SUCCESS;
}

NTSTATUS Wow64Continue(const WOW64_CONTEXT *Context, BOOLEAN TestAlert)
{
    CONTEXT Native;
    NTSTATUS Status = ContextFrom32(&Native, Context);
    if (!NT_SUCCESS(Status)) return Status;
    return NtContinue(&Native, TestAlert);
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
    TEB32 *Teb32;

    if (Context->SegCs != 0x23) return;
    Teb32 = WOW64_TEB32(NtCurrentTeb());
    if (!Wow64ExceptionDispatcher ||
        (ULONG_PTR)&Frame < Teb32->NtTib.StackLimit ||
        (ULONG_PTR)(&Frame + 1) > Teb32->NtTib.StackBase)
        goto Failed;

    /* The kernel's amd64 frame already lives on the guest stack. A local
     * frame is below it and cannot overwrite the source context or record.
     * The i386 dispatcher takes two pointers at ESP, with no return address. */
    Native = *Context;
    ContextTo32(&Frame.Context, Context);
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
    Status = NtContinue(&Native, FALSE);
    DbgPrint("WoW64: exception dispatch failed: %08lx\n", Status);
Failed:
    NtTerminateProcess(NtCurrentProcess(), Record->ExceptionCode);
}
