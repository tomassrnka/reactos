/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Helper for the StackOverflowDispatch test
 */

#include <asm.inc>

.code

EXTERN _StackOverflowDispatchSavedSp:DWORD

/* VOID __cdecl StackOverflowDispatchFault(PVOID StackPointer):
 * fault with the given stack pointer. The test's vectored handler continues
 * at StackOverflowDispatchResume, which returns on the original stack. */
PUBLIC _StackOverflowDispatchFault
_StackOverflowDispatchFault:
    mov eax, [esp + 4]
    mov [_StackOverflowDispatchSavedSp], esp
    mov esp, eax
    xor eax, eax
PUBLIC _StackOverflowDispatchFaultIp
_StackOverflowDispatchFaultIp:
    mov byte ptr [eax], 0
    int 3
PUBLIC _StackOverflowDispatchResume
_StackOverflowDispatchResume:
    mov esp, [_StackOverflowDispatchSavedSp]
    ret

END
