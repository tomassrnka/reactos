/* SPDX-License-Identifier: GPL-2.0-or-later
 * Independent native TEB address oracle, using GS rather than guest FS. */
.intel_syntax noprefix
.text
.code32
.globl _GuestStateNativeTeb
_GuestStateNativeTeb:
    .byte 0xea
    .long .Lnative_teb
    .word 0x33
.code64
.Lnative_teb:
    mov esp, esp
    mov rax, QWORD PTR gs:0x30
    lea rdx, [rip + .Lcompat_teb]
    push 0x23
    push rdx
    retfq
.code32
.Lcompat_teb:
    ret
