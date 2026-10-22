/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Helper for the StackOverflowDispatch test
 */

#include <asm.inc>

.code64

EXTERN StackOverflowDispatchSavedSp:QWORD

/* VOID StackOverflowDispatchFault(PVOID StackPointer):
 * fault with the given stack pointer. The test's vectored handler continues
 * at StackOverflowDispatchResume, which returns on the original stack. */
PUBLIC StackOverflowDispatchFault
.PROC StackOverflowDispatchFault
    .ENDPROLOG
    mov StackOverflowDispatchSavedSp[rip], rsp
    mov rsp, rcx
    xor eax, eax
PUBLIC StackOverflowDispatchFaultIp
StackOverflowDispatchFaultIp:
    mov byte ptr [rax], 0
    int 3
PUBLIC StackOverflowDispatchResume
StackOverflowDispatchResume:
    mov rsp, StackOverflowDispatchSavedSp[rip]
    ret
.ENDP

END
