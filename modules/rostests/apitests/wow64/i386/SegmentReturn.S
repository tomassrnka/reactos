#include <asm.inc>
.code
PUBLIC _SegmentReloadTrap
_SegmentReloadTrap:
    /* Reloading the same selector discards the base set through the MSR.
     * An exception return must restore the native TEB before its dispatcher
     * and the next WoW64 transition use GS. */
    push eax
    mov ax, gs
    mov gs, ax
    pop eax
    int 3
PUBLIC _SegmentReloadResume
_SegmentReloadResume:
    ret
END
