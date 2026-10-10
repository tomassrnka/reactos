/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AMD64 Application Processor (AP) spinup setup
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <asm.inc>
#include <ksamd64.inc>

PUBLIC HalpAPEntry16
PUBLIC HalpAPEntryData
PUBLIC HalpAPEntryJump64
PUBLIC HalpAPEntryLow64
PUBLIC HalpAPEntry16End
PUBLIC HalpAPEntry64

#define DATA(f) (f - HalpAPEntry16)

/*
 * The startup IPI enters HalpAPEntry16 in real mode at a copy of this code in
 * the low stub (CS = stub >> 4, IP = 0). It switches straight to long mode
 * with a temporary GDT and page tables that HalStartNextProcessor builds in
 * the stub: they map the stub one to one and share the kernel's upper half.
 */
.code
.align 16
HalpAPEntry16:
    /* Real-mode code, encoded by hand: ML64 assembles no 16-bit code */
    .byte HEX(FA)                               /* cli */
    .byte HEX(8C), HEX(C8)                      /* mov ax, cs */
    .byte HEX(8E), HEX(D8)                      /* mov ds, ax: address the stub's data */

    /* Load the temporary GDT, its base is the stub's physical address */
    .byte HEX(66), HEX(0F), HEX(01), HEX(16)    /* lgdt (32-bit base) ds:[TempGdtLimit] */
    .word DATA(TempGdtLimit)

    /* Enable PAE with the temporary page tables */
    .byte HEX(66), HEX(A1)                      /* mov eax, ds:[TempCr4] */
    .word DATA(TempCr4)
    .byte HEX(0F), HEX(22), HEX(E0)             /* mov cr4, eax */
    .byte HEX(66), HEX(A1)                      /* mov eax, ds:[TempCr3] */
    .word DATA(TempCr3)
    .byte HEX(0F), HEX(22), HEX(D8)             /* mov cr3, eax */

    /* Enable long mode and no-execute: kernel PTEs carry the NX bit */
    .byte HEX(66), HEX(B9)                      /* mov ecx, MSR_EFER */
    .long MSR_EFER
    .byte HEX(66), HEX(A1)                      /* mov eax, ds:[TempEfer] */
    .word DATA(TempEfer)
    .byte HEX(66), HEX(8B), HEX(16)             /* mov edx, ds:[TempEfer + 4] */
    .word DATA(TempEfer) + 4
    .byte HEX(0F), HEX(30)                      /* wrmsr */

    /* Enable paging and protection together */
    .byte HEX(0F), HEX(20), HEX(C0)             /* mov eax, cr0 */
    .byte HEX(66), HEX(0D)                      /* or eax, 80000001h */
    .long HEX(80000001)
    .byte HEX(0F), HEX(22), HEX(C0)             /* mov cr0, eax */

    /* Far jump to the 64-bit code segment, the target is a linear address */
    .byte HEX(66), HEX(EA)
HalpAPEntryJump64:
    .long 0
    .word 8

.align 8
HalpAPEntryData:
TempGdt:
    .quad HEX(0000000000000000) /* 00: null */
    .quad HEX(00209A0000000000) /* 08: 64-bit code */
    .quad HEX(00CF92000000FFFF) /* 10: data */
TempGdtPad:
    .word 0
TempGdtLimit:
    .word 0
TempGdtBase:
    .long 0
TempCr3:
    .long 0
TempCr4:
    .long 0
TempEfer:
    .quad 0
ProcessorStatePtr:
    .quad 0
Entry64Ptr:
    .quad 0

HalpAPEntryLow64:
    /* Still in the stub. The kernel's upper half is mapped, so continue there */
    mov rsi, [rip + ProcessorStatePtr]
    jmp qword ptr [rip + Entry64Ptr]

HalpAPEntry16End:
    nop

/*
 * VOID HalpAPEntry64(VOID)
 * In: rsi = PKPROCESSOR_STATE of the starting processor
 */
HalpAPEntry64:
    /* Load the processor's own descriptor tables */
    lgdt [rsi + PsGdtr + 6]
    lidt [rsi + PsIdtr + 6]

    /* Load the kernel's control registers, address space and stack */
    mov rsp, [rsi + PsContextFrame + CxRsp]
    mov rax, [rsi + PsCr4]
    mov cr4, rax
    mov rax, [rsi + PsCr3]
    mov cr3, rax
    mov rax, [rsi + PsCr0]
    mov cr0, rax

    /* Reload CS from the kernel GDT with a far return */
    movzx eax, word ptr [rsi + PsContextFrame + CxSegCs]
    push rax
    lea rax, [rip + HalpAPEntry64Reloaded]
    push rax
    .byte HEX(48), HEX(CB) /* retfq */

HalpAPEntry64Reloaded:
    /* Load the data segments */
    mov ax, [rsi + PsContextFrame + CxSegSs]
    mov ss, ax
    mov ax, [rsi + PsContextFrame + CxSegDs]
    mov ds, ax
    mov ax, [rsi + PsContextFrame + CxSegEs]
    mov es, ax
    mov ax, [rsi + PsContextFrame + CxSegFs]
    mov fs, ax
    mov ax, [rsi + PsContextFrame + CxSegGs]
    mov gs, ax

    /* Point GS at the PCR before any kernel code runs */
    mov ecx, MSR_GS_BASE
    mov eax, [rsi + PsSpecialRegisters + SrMsrGsBase]
    mov edx, [rsi + PsSpecialRegisters + SrMsrGsBase + 4]
    wrmsr

    /* Load the TSS */
    ltr word ptr [rsi + PsTr]

    /* Pass the parameter and enter the kernel as if called, on a fresh stack
       (a null return address ends stack walks) */
    mov rsp, [rsi + PsContextFrame + CxRsp]
    mov rcx, [rsi + PsContextFrame + CxRcx]
    mov rax, [rsi + PsContextFrame + CxRip]
    xor ebp, ebp
    push 0
    jmp rax

END
