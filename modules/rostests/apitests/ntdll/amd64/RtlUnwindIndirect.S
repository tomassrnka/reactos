/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Helper for the RtlUnwindIndirect test: functions with
 *              hand-written unwind data, including an indirect entry
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/*
 * MSVC (and PGO) separate cold code from a function's body. The separated
 * part gets a RUNTIME_FUNCTION whose UnwindData is the RVA of the function's
 * own RUNTIME_FUNCTION plus one (RUNTIME_FUNCTION_INDIRECT): it unwinds with
 * the function's unwind data, as a part of its body. The unwind data here is
 * written by hand, because GCC does not emit indirect entries; this file is
 * GAS only (the test is not built with MSVC).
 *
 * IndOuter      frame pointer function (rbp = rsp + 0x20 after the prolog,
 *               FrameOffset 2), saves rbp/rbx/rsi, keeps a cookie as /GS
 *               does (cookie ^ (EstablisherFrame + FrameOffset * 16)) and
 *               has an exception and termination handler that checks it
 *               like __GSHandlerCheck. It calls IndPrimary.
 * IndPrimary    saves rbx/rsi/rbp, has a handler, clobbers those registers
 *               and jumps to IndFragment.
 * IndFragment   a separated part of IndPrimary (indirect entry): calls
 *               IndRaise, which raises an exception.
 */

    .intel_syntax noprefix
    .text

    .globl IndPrimary
    .globl IndOuter
    .globl IndFragment
    .globl IndFragmentReturn
    .globl IndOuterReturn
    .globl IndPrimaryEntryCopy
    .globl IndPrimaryHandlerData
    .globl IndOuterFrame
    .globl IndCookie

/* VOID IndPrimary(VOID) */
    .p2align 4
IndPrimary:
    push rbx
    push rsi
    push rbp
    sub rsp, 0x20
IndPrimaryEndProlog:
    mov rbx, 0x0BADBADBADBAD001
    mov rsi, 0x0BADBADBADBAD002
    mov rbp, 0x0BADBADBADBAD003
    jmp IndFragment
    /* Not reached: the epilog of the body */
    add rsp, 0x20
    pop rbp
    pop rsi
    pop rbx
    ret
IndPrimaryEnd:

/* ULONG64 IndOuter(ULONG64 Rbx, ULONG64 Rsi): returns Rbx ^ Rsi when the
   callee returns, which it does not */
    .p2align 4
IndOuter:
    push rbp
    push rbx
    push rsi
    sub rsp, 0x30
    lea rbp, [rsp + 0x20]
IndOuterEndProlog:
    mov IndOuterFrame[rip], rsp
    mov rbx, rcx
    mov rsi, rdx
    mov rax, IndCookie[rip]
    xor rax, rbp
    mov [rbp + 8], rax
    call IndPrimary
IndOuterReturn:
    nop
    mov rax, rbx
    xor rax, rsi
    lea rsp, [rbp + 0x10]
    pop rsi
    pop rbx
    pop rbp
    ret
IndOuterEnd:

/* The separated part of IndPrimary */
    .p2align 4
IndFragment:
    call IndRaise
IndFragmentReturn:
    int3
IndFragmentEnd:

    .data
    .p2align 3
IndOuterFrame:
    .quad 0
IndCookie:
    .quad 0x2B992DDFA232

/* Unwind data */
    .section .xdata, "dr"
    .p2align 2
IndPrimaryUnwind:
    .byte 1 | (1 << 3)                   /* version 1, UNW_FLAG_EHANDLER */
    .byte IndPrimaryEndProlog - IndPrimary
    .byte 4                              /* codes */
    .byte 0                              /* no frame register */
    .byte IndPrimaryEndProlog - IndPrimary, 0x32  /* UWOP_ALLOC_SMALL 0x20 */
    .byte 3, 0x50                        /* UWOP_PUSH_NONVOL rbp */
    .byte 2, 0x60                        /* UWOP_PUSH_NONVOL rsi */
    .byte 1, 0x30                        /* UWOP_PUSH_NONVOL rbx */
    .rva IndPrimaryHandler
IndPrimaryHandlerData:
    .long 0x1D1D1D1D

    .p2align 2
IndOuterUnwind:
    .byte 1 | (3 << 3)                   /* UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER */
    .byte IndOuterEndProlog - IndOuter
    .byte 5
    .byte 5 | (2 << 4)                   /* rbp, FrameOffset 2 (0x20) */
    .byte IndOuterEndProlog - IndOuter, 0x03      /* UWOP_SET_FPREG */
    .byte 7, 0x52                        /* UWOP_ALLOC_SMALL 0x30 */
    .byte 3, 0x60                        /* UWOP_PUSH_NONVOL rsi */
    .byte 2, 0x30                        /* UWOP_PUSH_NONVOL rbx */
    .byte 1, 0x50                        /* UWOP_PUSH_NONVOL rbp */
    .byte 0, 0                           /* padding to an even count */
    .rva IndOuterHandler
    .long 0x28                           /* cookie offset from the establisher frame */

/* A copy of IndPrimary's entry, which the indirect entry points to */
    .section .rdata, "dr"
    .p2align 2
IndPrimaryEntryCopy:
    .rva IndPrimary, IndPrimaryEnd, IndPrimaryUnwind

    .section .pdata, "dr"
    .p2align 2
    .rva IndPrimary, IndPrimaryEnd, IndPrimaryUnwind
    .rva IndOuter, IndOuterEnd, IndOuterUnwind
    .rva IndFragment, IndFragmentEnd, IndPrimaryEntryCopy + 1
