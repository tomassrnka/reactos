/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Helpers for the RtlUnwindExConsolidate test
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <asm.inc>

.code64

EXTERN ConsolidateOuterHandler:PROC
EXTERN ConsolidateStaleHandler:PROC

/* LONG_PTR ConsolidateOuterGuard(LONG_PTR (*Function)(VOID)):
 * calls Function in a frame handled by ConsolidateOuterHandler, which
 * unwinds to ConsolidateOuterResume and returns the unwind's return value. */
PUBLIC ConsolidateOuterGuard
#ifdef _USE_ML
ConsolidateOuterGuard PROC FRAME:ConsolidateOuterHandler
#else
.PROC ConsolidateOuterGuard
.seh_handler ConsolidateOuterHandler, @unwind, @except
#endif
    sub rsp, 40
    .ALLOCSTACK 40
    .ENDPROLOG
    call rcx
PUBLIC ConsolidateOuterResume
ConsolidateOuterResume:
    nop
    add rsp, 40
    ret
#ifdef _USE_ML
ConsolidateOuterGuard ENDP
#else
.ENDP
#endif

/* LONG_PTR ConsolidateStaleGuard(LONG_PTR (*Function)(VOID)):
 * calls Function in a frame handled by ConsolidateStaleHandler, which only
 * counts its calls. */
PUBLIC ConsolidateStaleGuard
#ifdef _USE_ML
ConsolidateStaleGuard PROC FRAME:ConsolidateStaleHandler
#else
.PROC ConsolidateStaleGuard
.seh_handler ConsolidateStaleHandler, @unwind, @except
#endif
    sub rsp, 40
    .ALLOCSTACK 40
    .ENDPROLOG
    call rcx
    nop
    add rsp, 40
    ret
#ifdef _USE_ML
ConsolidateStaleGuard ENDP
#else
.ENDP
#endif

END
