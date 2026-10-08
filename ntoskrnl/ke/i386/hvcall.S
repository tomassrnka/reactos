/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Hypercall stub for 32-bit callers
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 * REFERENCES:  Microsoft, "Hypervisor Top Level Functional Specification",
 *              v6.0b, 3.7 and 3.8 (x86 register mapping)
 */

#include <asm.inc>

EXTERN _KiHvHypercallPage:DWORD

/* FUNCTIONS ****************************************************************/
.code

/*
 * ULONG64
 * NTAPI
 * KiHvHypercall(ULONG64 Control, ULONG64 Input, ULONG64 Output)
 *
 * The input value goes in EDX:EAX, the input parameter (or its GPA) in
 * EBX:ECX and the output parameter (or its GPA) in EDI:ESI; the result
 * comes back in EDX:EAX.
 */
PUBLIC _KiHvHypercall@24
FUNC _KiHvHypercall@24
    FPO 0, 6, 3, 3, 0, FRAME_FPO

    push ebx
    push esi
    push edi
    mov eax, [esp + 16]
    mov edx, [esp + 20]
    mov ecx, [esp + 24]
    mov ebx, [esp + 28]
    mov esi, [esp + 32]
    mov edi, [esp + 36]
    call dword ptr [_KiHvHypercallPage]
    pop edi
    pop esi
    pop ebx
    ret 24
ENDFUNC

END
