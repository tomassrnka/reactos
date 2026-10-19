/* SPDX-License-Identifier: GPL-2.0-or-later
 * Test-only native syscall bridge. This lets the registration test run before
 * wow64win and guest user32 callback dispatch are available. */
.intel_syntax noprefix
.text
.code32
.globl _ClientPfnCall64
_ClientPfnCall64:
    push ebp
    push ebx
    push esi
    push edi
    mov eax, [esp + 20]
    mov edi, [esp + 24]
    mov esi, [esp + 28]
    /* Far jump to the amd64 user code selector. */
    .byte 0xea
    .long .Lnative
    .word 0x33
.code64
.Lnative:
    mov esp, esp
    mov edi, edi
    mov esi, esi
    mov ebp, esp
    and rsp, -16
    sub rsp, 64
    /* The native dispatcher needs only the thread-startup callback here. */
    mov rbx, QWORD PTR gs:0x60
    mov rdx, [rbx + 0x58]
    mov [rsp + 56], rdx
    mov [rbx + 0x58], rsi
    mov rdx, [rdi + 32]
    mov [rsp + 40], rdx
    mov rdx, [rdi + 40]
    mov [rsp + 48], rdx
    mov rcx, [rdi]
    mov rdx, [rdi + 8]
    mov r8, [rdi + 16]
    mov r9, [rdi + 24]
    mov r10, rcx
    syscall
    mov rdx, [rsp + 56]
    mov [rbx + 0x58], rdx
    mov esp, ebp
    lea rdx, [rip + .Lcompat]
    push 0x23
    push rdx
    retfq
.code32
.Lcompat:
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

/* Called by native ntdll's KiUserCallbackDispatcher; never called as i386. */
.code64
.globl _ClientPfnCallback64
_ClientPfnCallback64:
#define DEFINE_USER32_CALLBACK(id, value, fn) .set id, value
#include <u32cb.h>
#undef DEFINE_USER32_CALLBACK
    mov eax, 0xc0000002 /* STATUS_NOT_IMPLEMENTED for unexpected callbacks */
    cmp r8d, USER32_CALLBACK_CLIENTTHREADSTARTUP
    jne .Lcallback_done
    xor eax, eax
.Lcallback_done:
    ret
