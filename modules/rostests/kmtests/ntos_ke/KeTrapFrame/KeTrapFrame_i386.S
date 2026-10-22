/* SPDX-License-Identifier: MIT */
#include <asm.inc>
.code
EXTERN _RtlCaptureContext@4:PROC

/* RtlCaptureContext on i386 captures its caller's caller. Supply that frame
 * explicitly so the saved context resumes immediately after this cdecl call. */
PUBLIC _CaptureTrapContext
_CaptureTrapContext:
    push ebp
    mov ebp, esp
    push dword ptr [ebp + 8]
    call _RtlCaptureContext@4
    pop ebp
    ret
END
