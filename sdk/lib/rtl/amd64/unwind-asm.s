/*
 * PROJECT:     ReactOS runtime library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Unwinding related x64 asm functions
 * COPYRIGHT:   Copyright 2025 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

#include <asm.inc>
#include <ksamd64.inc>

.code64

EXTERN RtlpExecuteHandlerForUnwindHandler:PROC

//
// EXCEPTION_DISPOSITION
// RtlpExecuteHandlerForUnwind(
//    _Inout_ PEXCEPTION_RECORD* ExceptionRecord,
//    _In_ PVOID EstablisherFrame,
//    _Inout_ PCONTEXT ContextRecord,
//    _In_ PDISPATCHER_CONTEXT DispatcherContext);
//
PUBLIC RtlpExecuteHandlerForUnwind
#ifdef _USE_ML
RtlpExecuteHandlerForUnwind PROC FRAME:RtlpExecuteHandlerForUnwindHandler
#else
.PROC RtlpExecuteHandlerForUnwind
.seh_handler RtlpExecuteHandlerForUnwindHandler, @unwind, @except
.seh_handlerdata
.long 0 // Count
.seh_code
#endif

    /* Save ExceptionRecord->ExceptionFlags in the home space */
    mov eax, [rcx + ErExceptionFlags]
    mov [rsp + P1Home], eax

    /* Save DispatcherContext in the home space */
    mov [rsp + P4Home], r9

    sub rsp, 40
    .ALLOCSTACK 40
    .ENDPROLOG

    /* Call the language handler */
    call qword ptr [r9 + DcLanguageHandler]

    /* This nop prevents RtlVirtualUnwind from unwinding the epilog,
       which would lead to ignoring the handler */
    nop

    add rsp, 40
    ret

#ifdef _USE_ML
RtlpExecuteHandlerForUnwind ENDP
#else
.ENDP
#endif

//
// PVOID
// RtlpCallConsolidateCallback(
//    _In_ PCONTEXT ContextRecord,
//    _In_ PVOID (*Callback)(PEXCEPTION_RECORD),
//    _In_ PEXCEPTION_RECORD ExceptionRecord);
//
// Calls the STATUS_UNWIND_CONSOLIDATE callback from a frame whose unwind
// information describes the target context: a machine frame holding the
// target Rip and Rsp, and the target's nonvolatile registers saved in the
// frame. An exception or unwind started from inside the callback (for
// example a C++ rethrow from a catch block, which runs as the callback)
// therefore continues in the target frame instead of walking back through
// the frames that the consolidating unwind has already removed.
//
// The unwind codes need no instructions of their own: the values they
// describe are stored after the prolog, before the callback is called.
//
// Frame layout (offsets from Rsp after the prolog):
//   0x000 home space for the callback
//   0x020 rbx, rbp, rsi, rdi, r12, r13, r14, r15
//   0x060 xmm6 - xmm15
//   0x100 MACHINE_FRAME (Rip, SegCs, EFlags, Rsp, SegSs)
//
#define CONSOLIDATE_FRAME_LENGTH HEX(100)

PUBLIC RtlpCallConsolidateCallback
.PROC RtlpCallConsolidateCallback

    /* Space for the MACHINE_FRAME (Rsp is 16-byte aligned after this) */
    sub rsp, MachineFrameLength
    .PUSHFRAME

    /* Home space and the nonvolatile register save area */
    sub rsp, CONSOLIDATE_FRAME_LENGTH
    .ALLOCSTACK CONSOLIDATE_FRAME_LENGTH
    .SAVEREG rbx, HEX(20)
    .SAVEREG rbp, HEX(28)
    .SAVEREG rsi, HEX(30)
    .SAVEREG rdi, HEX(38)
    .SAVEREG r12, HEX(40)
    .SAVEREG r13, HEX(48)
    .SAVEREG r14, HEX(50)
    .SAVEREG r15, HEX(58)
    .SAVEXMM128 xmm6, HEX(60)
    .SAVEXMM128 xmm7, HEX(70)
    .SAVEXMM128 xmm8, HEX(80)
    .SAVEXMM128 xmm9, HEX(90)
    .SAVEXMM128 xmm10, HEX(a0)
    .SAVEXMM128 xmm11, HEX(b0)
    .SAVEXMM128 xmm12, HEX(c0)
    .SAVEXMM128 xmm13, HEX(d0)
    .SAVEXMM128 xmm14, HEX(e0)
    .SAVEXMM128 xmm15, HEX(f0)
    .ENDPROLOG

    /* Fill the machine frame from the target context */
    mov rax, [rcx + CxRip]
    mov [rsp + CONSOLIDATE_FRAME_LENGTH + MfRip], rax
    movzx eax, word ptr [rcx + CxSegCs]
    mov [rsp + CONSOLIDATE_FRAME_LENGTH + MfSegCs], rax
    mov eax, [rcx + CxEFlags]
    mov [rsp + CONSOLIDATE_FRAME_LENGTH + MfEFlags], rax
    mov rax, [rcx + CxRsp]
    mov [rsp + CONSOLIDATE_FRAME_LENGTH + MfRsp], rax
    movzx eax, word ptr [rcx + CxSegSs]
    mov [rsp + CONSOLIDATE_FRAME_LENGTH + MfSegSs], rax

    /* Store the target's nonvolatile registers where the unwind codes say */
    mov rax, [rcx + CxRbx]
    mov [rsp + HEX(20)], rax
    mov rax, [rcx + CxRbp]
    mov [rsp + HEX(28)], rax
    mov rax, [rcx + CxRsi]
    mov [rsp + HEX(30)], rax
    mov rax, [rcx + CxRdi]
    mov [rsp + HEX(38)], rax
    mov rax, [rcx + CxR12]
    mov [rsp + HEX(40)], rax
    mov rax, [rcx + CxR13]
    mov [rsp + HEX(48)], rax
    mov rax, [rcx + CxR14]
    mov [rsp + HEX(50)], rax
    mov rax, [rcx + CxR15]
    mov [rsp + HEX(58)], rax
    movups xmm0, [rcx + CxXmm6]
    movaps [rsp + HEX(60)], xmm0
    movups xmm0, [rcx + CxXmm7]
    movaps [rsp + HEX(70)], xmm0
    movups xmm0, [rcx + CxXmm8]
    movaps [rsp + HEX(80)], xmm0
    movups xmm0, [rcx + CxXmm9]
    movaps [rsp + HEX(90)], xmm0
    movups xmm0, [rcx + CxXmm10]
    movaps [rsp + HEX(a0)], xmm0
    movups xmm0, [rcx + CxXmm11]
    movaps [rsp + HEX(b0)], xmm0
    movups xmm0, [rcx + CxXmm12]
    movaps [rsp + HEX(c0)], xmm0
    movups xmm0, [rcx + CxXmm13]
    movaps [rsp + HEX(d0)], xmm0
    movups xmm0, [rcx + CxXmm14]
    movaps [rsp + HEX(e0)], xmm0
    movups xmm0, [rcx + CxXmm15]
    movaps [rsp + HEX(f0)], xmm0

    /* Call the callback with the exception record */
    mov rcx, r8
    call rdx

    /* This nop keeps RtlVirtualUnwind from taking the return address for
       the start of the epilog, which would unwind to our real caller */
    nop

    add rsp, CONSOLIDATE_FRAME_LENGTH + MachineFrameLength
    ret

#ifdef _USE_ML
RtlpCallConsolidateCallback ENDP
#else
.ENDP
#endif

END
