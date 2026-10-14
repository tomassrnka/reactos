/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <asm.inc>
#include <ksamd64.inc>
.code
EXTERN RtlCaptureContext:PROC
EXTERN NtContinue:PROC
EXTERN NtRaiseException:PROC
EXTERN NativeContinueRecord:BYTE
EXTERN NativeContinueContext:BYTE
EXTERN ContinueXmm:DWORD
EXTERN ContinueResult:DWORD
PUBLIC NativeContinuePartial
.PROC NativeContinuePartial
    push rbp
    .pushreg rbp
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push r12
    .pushreg r12
    push r13
    .pushreg r13
    push r14
    .pushreg r14
    push r15
    .pushreg r15
    push rbx
    .pushreg rbx
    sub rsp, HEX(D8)
    .allocstack HEX(D8)
    movaps [rsp + HEX(30)], xmm6
    .savexmm128 xmm6, HEX(30)
    movaps [rsp + HEX(40)], xmm7
    .savexmm128 xmm7, HEX(40)
    movaps [rsp + HEX(50)], xmm8
    .savexmm128 xmm8, HEX(50)
    movaps [rsp + HEX(60)], xmm9
    .savexmm128 xmm9, HEX(60)
    movaps [rsp + HEX(70)], xmm10
    .savexmm128 xmm10, HEX(70)
    movaps [rsp + HEX(80)], xmm11
    .savexmm128 xmm11, HEX(80)
    movaps [rsp + HEX(90)], xmm12
    .savexmm128 xmm12, HEX(90)
    movaps [rsp + HEX(A0)], xmm13
    .savexmm128 xmm13, HEX(A0)
    movaps [rsp + HEX(B0)], xmm14
    .savexmm128 xmm14, HEX(B0)
    movaps [rsp + HEX(C0)], xmm15
    .savexmm128 xmm15, HEX(C0)
    .endprolog
    mov [rsp + HEX(20)], ecx
    mov [rsp + HEX(24)], edx
    lea rcx, NativeContinueContext[rip]
    call RtlCaptureContext
    lea rcx, NativeContinueContext[rip]
    mov eax, [rsp + HEX(20)]
    mov [rcx + CxContextFlags], eax
    lea rax, ContinueNativeResume[rip]
    mov [rcx + CxRip], rax
    mov [rcx + CxRsp], rsp
    mov qword ptr [rcx + CxRbx], HEX(12345678)
    mov ebx, HEX(87654321)
    movdqu xmm6, ContinueXmm[rip]
    movdqu xmm7, ContinueXmm + 16[rip]
    movdqu [rcx + CxXmm6], xmm6
    movdqu [rcx + CxXmm7], xmm7
    cmp dword ptr [rsp + HEX(24)], 0
    jnz NativeRaise
    xor edx, edx
    call NtContinue
    int 3
NativeRaise:
    mov rdx, rcx
    lea rcx, NativeContinueRecord[rip]
    mov r8d, 1
    call NtRaiseException
    int 3
ContinueNativeResume:
    mov ContinueResult[rip], ebx
    movdqu ContinueResult + 32[rip], xmm6
    movdqu ContinueResult + 48[rip], xmm7
    movaps xmm6, [rsp + HEX(30)]
    movaps xmm7, [rsp + HEX(40)]
    movaps xmm8, [rsp + HEX(50)]
    movaps xmm9, [rsp + HEX(60)]
    movaps xmm10, [rsp + HEX(70)]
    movaps xmm11, [rsp + HEX(80)]
    movaps xmm12, [rsp + HEX(90)]
    movaps xmm13, [rsp + HEX(A0)]
    movaps xmm14, [rsp + HEX(B0)]
    movaps xmm15, [rsp + HEX(C0)]
    add rsp, HEX(D8)
    pop rbx
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbp
    ret
.ENDP
END
