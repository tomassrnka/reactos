/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Helper for the BadSegmentFault test
 */

#include <asm.inc>

.code64

/* VOID BadSegmentFault(VOID): load a writable DPL3 data selector into SS and
 * fault, so the trap frame carries SS != KGDT64_R3_DATA. The test's vectored
 * handler continues at BadSegmentResume; SS has been corrected by the kernel. */
PUBLIC BadSegmentFault
.PROC BadSegmentFault
    .ENDPROLOG
    mov ax, HEX(53)   /* KGDT64_R3_CMTEB | RPL_MASK */
    mov ss, ax
    xor eax, eax
PUBLIC BadSegmentFaultIp
BadSegmentFaultIp:
    mov byte ptr [rax], 0
    int 3
PUBLIC BadSegmentResume
BadSegmentResume:
    ret
.ENDP

END
