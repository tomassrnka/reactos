
#ifdef _M_ARM

#include <ksarm.h>

    GBLA SyscallId

SyscallId SETA 0

	MACRO
	STUB_K $Name
	LCLS ZwFuncName
	LCLS ZwFuncEndName
ZwFuncName    SETS "Zw$Name"
ZwFuncEndName SETS "$ZwFuncName":CC:"_end"
	ALIGN 2
	EXPORT $ZwFuncName [FUNC]
$ZwFuncName
	ROUT
    mov r12, #SyscallId
    svc #1
    bx lr
$ZwFuncEndName
    MEND

	MACRO
	STUB_U $Name
	LCLS NtFuncName
	LCLS NtFuncEndName
	LCLS ZwFuncName
	LCLS ZwFuncEndName
NtFuncName    SETS "Nt$Name"
NtFuncEndName SETS "$NtFuncName":CC:"_end"
ZwFuncName    SETS "Zw$Name"
ZwFuncEndName SETS "$ZwFuncName":CC:"_end"
	ALIGN 2
	EXPORT $NtFuncName [FUNC]
$NtFuncName
	EXPORT $ZwFuncName [FUNC]
$ZwFuncName
	ROUT
    mov r12, #SyscallId
    svc #1
    bx lr
$NtFuncEndName
$ZwFuncEndName
    MEND

#else

#include <asm.inc>

SyscallId = 0

#ifdef _M_IX86
#define KUSER_SHARED_SYSCALL HEX(7ffe0300)
#define KGDT_R0_CODE 8
#ifdef SYSWOW64_GUEST
#include <ntverp.h>
/* System-call hooks pick the stub layout they accept by the reported Windows
 * version (the Chromium sandbox: Wow64W10ResolverThunk on 10, Wow64W8ResolverThunk
 * on 6.2 and 6.3, Wow64ResolverThunk below), so the WOW64 stubs follow
 * VERBUMP_TIER. Every form enters the gate in TEB.WOW32Reserved with the stub's
 * return address on the stack; the gate reads the arguments above it.
 *   Windows 10: mov eax, id; mov edx, Wow64SystemServiceCall; call edx; ret n; nop
 *   Windows 8:  mov eax, id; call dword ptr fs:[0C0h]; ret n; nop
 *   NT 5.2 x64: mov eax, id; xor ecx, ecx; lea edx, [esp + 4];
 *               call dword ptr fs:[0C0h]; ret n */
#if VERBUMP_TIER == 100
/* One local Wow64SystemServiceCall per object, before its first stub:
 * jmp dword ptr fs:[0C0h]. (asm.inc turns endif into .endif.) */
MACRO(WOW64_SERVICE_CALL)
    .ifndef Wow64SystemServiceCallLocal
Wow64SystemServiceCallLocal:
    jmp dword ptr fs:[HEX(c0)]
    endif
ENDM
#endif
#endif
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
    StackBytes = 4 * ArgCount
    FPO 0, 0, 0, 0, 0, FRAME_FPO
    mov eax, SyscallId
#if defined(SYSWOW64_GUEST) && VERBUMP_TIER == 100
    mov edx, offset Wow64SystemServiceCallLocal
    call edx
    ret StackBytes
    nop
#elif defined(SYSWOW64_GUEST) && VERBUMP_TIER == 62
    call dword ptr fs:[HEX(c0)]
    ret StackBytes
    nop
#elif defined(SYSWOW64_GUEST)
    .byte HEX(33), HEX(0C9) /* xor ecx, ecx in this encoding */
    lea edx, [esp + 4]
    call dword ptr fs:[HEX(c0)]
    ret StackBytes
#else
    mov edx, KUSER_SHARED_SYSCALL
    call dword ptr [edx]
    ret StackBytes
#endif
ENDM
MACRO(STUBCODE_K, Name, SyscallId, ArgCount)
    StackBytes = 4 * &ArgCount
    FPO 0, 0, 0, 0, 0, FRAME_FPO
    mov eax, SyscallId
    lea edx, [esp + 4]
    pushfd
    push KGDT_R0_CODE
    call _KiSystemService
    ret StackBytes
ENDM
#elif defined(_M_AMD64)
/* Windows layout (mov r10, rcx encoded 4C 8B D1; mov eax, id; syscall; ret), one stub per 16 bytes */
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
    .ENDPROLOG
    .byte HEX(4C), HEX(8B), HEX(0D1)
    mov eax, SyscallId
    syscall
    ret
ENDM
MACRO(STUBCODE_K, Name, SyscallId, ArgCount)
    .ENDPROLOG
    EXTERN Nt&Name:PROC
    lea rax, Nt&Name[rip]
    mov r10, ArgCount * 8
    jmp KiZwSystemService
ENDM
#elif defined(_M_PPC)
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
    stwu 1,-16(1)
    mflr 0
    stw  0,0(1)
    li   0, SyscallId
    sc
    lwz 0,0(1)
    mtlr 0
    addi 1,1,16
    blr
ENDM
#define STUBCODE_K STUBCODE_U
#elif defined(_M_MIPS)
MACRO(STUBCODE_U, Name, SyscallId, ArgCount)
    li $8, KUSER_SHARED_SYSCALL
    lw $8,0($8)
    j $8
    nop
ENDM
MACRO(STUBCODE_K, Name, SyscallId, ArgCount)
    j KiSystemService
    nop
ENDM
#else
#error unsupported architecture
#endif

#ifdef _M_IX86
MACRO(MAKE_LABEL, Name, StackBytes)
    PUBLIC _&Name&@&StackBytes
    _&Name&@&StackBytes:
ENDM
MACRO(START_PROC, Name, StackBytes)
    PUBLIC _&Name&@&StackBytes
    .PROC _&Name&@&StackBytes
ENDM
#else
MACRO(MAKE_LABEL, Name, StackBytes)
    PUBLIC &Name
    &Name:
ENDM
MACRO(START_PROC, Name, StackBytes)
    PUBLIC &Name
    .PROC &Name
ENDM
#endif

MACRO(STUB_U, Name, ArgCount)
#ifdef _M_AMD64
    align 16
#endif
#if defined(SYSWOW64_GUEST) && VERBUMP_TIER == 100
    WOW64_SERVICE_CALL
#endif
    MAKE_LABEL Zw&Name, %ArgCount * 4
    START_PROC Nt&Name, %ArgCount * 4
    STUBCODE_U Name, SyscallId, %ArgCount
    .ENDP
    SyscallId = SyscallId + 1
ENDM

MACRO(STUB_K, Name, ArgCount)
    START_PROC Zw&Name, %ArgCount * 4
    STUBCODE_K Name, SyscallId, %ArgCount
    .ENDP
    SyscallId = SyscallId + 1
ENDM

#endif
