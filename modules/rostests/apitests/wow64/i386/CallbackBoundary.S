/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <asm.inc>
.code
EXTERN _NtUserMessageCall@28:PROC
PUBLIC _CallbackBoundary@20
_CallbackBoundary@20:
    push ebp
    mov ebp, esp
    mov esp, [ebp + 8]
    and esp, -16
    push 0
    push HEX(2b1) /* FNID_SENDMESSAGE */
    push dword ptr [ebp + 24]
    push dword ptr [ebp + 20]
    push 1024
    push dword ptr [ebp + 16]
    push dword ptr [ebp + 12]
    call _NtUserMessageCall@28
    mov esp, ebp
    pop ebp
    ret 20
END
