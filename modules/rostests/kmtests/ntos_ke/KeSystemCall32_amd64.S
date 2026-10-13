/* SPDX-License-Identifier: MIT */
#include <asm.inc>

.code64

/* RCX: copied code, RDX: low CSTAR_STATE, R8: safe staging stack. */
PUBLIC CstarInvoke
FUNC CstarInvoke
    push rbx
    .pushreg rbx
    push rbp
    .pushreg rbp
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    .endprolog
    mov rdi, rdx
    mov [rdi], rsp
    lea rax, [rip + CstarNativeReturn]
    mov [rdi + 8], rax
    mov rsp, r8
    push HEX(23)
    push rcx
    mov ebx, HEX(12345678)
    mov ebp, HEX(23456789)
    mov esi, HEX(3456789A)
    /* RETFQ enters the flat ring-3 compatibility segment. */
    .byte HEX(48), HEX(CB)
CstarNativeReturn:
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    ret
ENDFUNC

/*
 * Relocation-free fragment copied below 4 GB and executed with CS=0x23.
 * The following encodings have no REX/address-size prefix, so [rdi] is [edi]
 * in compatibility mode. This also lets ml64 assemble the test fragment.
 */
PUBLIC CstarCode
CstarCode:
    mov eax, [rdi + 16]
    mov esp, [rdi + 20]
    mov edx, HEX(DEADBEEF) /* An argument pointer that must not be read. */
    stc
    std
    syscall
PUBLIC CstarAfterSyscall
CstarAfterSyscall:
    mov [rdi + 24], eax
    mov [rdi + 28], esp
    mov [rdi + 32], ecx
    mov [rdi + 40], ebx
    mov [rdi + 44], ebp
    mov [rdi + 48], esi
    mov [rdi + 52], edi
    mov esp, [rdi + 64]
    pushfq /* PUSHFD in compatibility mode. */
    pop rax /* POP EAX in compatibility mode. */
    mov [rdi + 36], eax
    cld
    .byte HEX(FF), HEX(6F), HEX(38) /* JMP FWORD PTR [EDI+56] */
PUBLIC CstarCode64
CstarCode64:
    mov rsp, [rdi]
    jmp qword ptr [rdi + 8]
PUBLIC CstarCodeEnd
CstarCodeEnd:
END
