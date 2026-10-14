/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Test-only i386 -> amd64 bridge for ReactOS phase-2 WoW64.
 * Uses the native stack saved by wow64cpu (WOW64_TLS_CPU slot 1).
 * Function 0 returns native ntdll; function 1 copies native memory;
 * other values call a native function with four integer arguments.
 */
.text
.code32
.globl _SharedViewCall64
_SharedViewCall64:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl 20(%esp), %edi
    movl %esp, %esi
    ljmp $0x33, $native_entry
.code64
native_entry:
    movq %gs:0x1488, %rax
    movq (%rax), %rsp
    pushq %r12
    subq $40, %rsp
    movl %esi, %r12d
    movl %edi, %ebx
    movq (%rbx), %rax
    testq %rax, %rax
    jz native_ntdll
    cmpq $1, %rax
    je native_copy
    movq 8(%rbx), %rcx
    movq 16(%rbx), %rdx
    movq 24(%rbx), %r8
    movq 32(%rbx), %r9
    call *%rax
    jmp native_done
native_ntdll:
    movq %gs:0x60, %rax
    movq 0x18(%rax), %rax
    movq 0x10(%rax), %rax
    movq (%rax), %rax
    movq 0x30(%rax), %rax
    jmp native_done
native_copy:
    movq 8(%rbx), %rdi
    movq 16(%rbx), %rsi
    movq 24(%rbx), %rcx
    cld
    rep movsb
    xorl %eax, %eax
native_done:
    movq %rax, 40(%rbx)
    movl %r12d, %edx
    addq $40, %rsp
    popq %r12
    movl %edx, %esp
    pushq $0x23
    leaq guest_return(%rip), %rax
    pushq %rax
    lretq
.code32
guest_return:
    movw $0x53, %ax
    movw %ax, %fs
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
