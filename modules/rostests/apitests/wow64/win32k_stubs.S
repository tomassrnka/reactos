/*
 * Win32k system call stubs of the identical-binary ABI tests (i386 PE32 only).
 * The same executable runs on native i386 and under WoW64. The guest stubs of
 * ntdll call the wow64cpu gate, TEB.WOW32Reserved, and have no fallback, so
 * these stubs keep their own: the gate when it is set, else the native
 * shared-page dispatcher.
 */
#include <asm.inc>

#define KUSER_SHARED_SYSCALL HEX(7ffe0300)

.code
SyscallId = HEX(1000)

MACRO(TEST_LABEL, Name, StackBytes)
    PUBLIC _&Name&@&StackBytes
    _&Name&@&StackBytes:
ENDM
MACRO(TEST_PROC, Name, StackBytes)
    PUBLIC _&Name&@&StackBytes
    .PROC _&Name&@&StackBytes
ENDM

MACRO(TEST_STUB_U, Name, ArgCount)
    TEST_LABEL Zw&Name, %ArgCount * 4
    TEST_PROC Nt&Name, %ArgCount * 4
    StackBytes = 4 * ArgCount
    FPO 0, 0, 0, 0, 0, FRAME_FPO
    mov eax, SyscallId
    cmp dword ptr fs:[HEX(c0)], 0
    je native_&Name
    /* The WOW64 form: xor ecx, ecx (the 33 C9 encoding), lea edx, [esp + 4], call the gate */
    .byte HEX(33), HEX(0C9)
    lea edx, [esp + 4]
    call dword ptr fs:[HEX(c0)]
    ret StackBytes
native_&Name:
    mov edx, KUSER_SHARED_SYSCALL
    call dword ptr [edx]
    ret StackBytes
    .ENDP
    SyscallId = SyscallId + 1
ENDM

#define SVC_(name, count) TEST_STUB_U name, count
#include <w32ksvc32.h>
END
