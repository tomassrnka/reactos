/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <asm.inc>
.code
EXTERN _ContinueXmm:DWORD
EXTERN _ContinueResult:DWORD
PUBLIC _ContinueFault
_ContinueFault:
    push ebp
    push edi
    push esi
    push ebx
    mov [_ContinueResult + 64], esp
    pxor xmm6, xmm6
    pxor xmm7, xmm7
    xor eax, eax
    mov eax, [eax]
    int 3
PUBLIC _ContinueResume
_ContinueResume:
    mov [_ContinueResult], eax
    mov [_ContinueResult + 4], ebx
    mov [_ContinueResult + 8], ecx
    mov [_ContinueResult + 12], edx
    mov [_ContinueResult + 16], esi
    mov [_ContinueResult + 20], edi
    mov [_ContinueResult + 24], ebp
    pushfd
    pop eax
    mov [_ContinueResult + 28], eax
    movdqu [_ContinueResult + 32], xmm6
    movdqu [_ContinueResult + 48], xmm7
    mov [_ContinueResult + 68], esp
    pop ebx
    pop esi
    pop edi
    pop ebp
    ret
PUBLIC _ContinueSetXmm
_ContinueSetXmm:
    movdqu xmm6, [_ContinueXmm]
    movdqu xmm7, [_ContinueXmm + 16]
    ret
END
