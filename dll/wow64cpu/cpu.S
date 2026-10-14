/* ReactOS WoW64 mode transition. SPDX-License-Identifier: GPL-2.0-or-later */
#include <asm.inc>
#include <ksamd64.inc>
EXTERN Wow64Service:QWORD

.code
PUBLIC Wow64CpuRun
.PROC Wow64CpuRun
    sub rsp, 88
    .allocstack 88
    .endprolog
    mov [rcx], rsp
    mov gs:[TeTlsSlots + 8], rcx
    mov r11, rcx
    jmp Wow64CpuReturn
.ENDP

PUBLIC Wow64CpuDispatch
.PROC Wow64CpuDispatch
    .endprolog
    /* The low-address gate enters with the i386 stub's return on ESP.
     * No caller argument is read until we are on the native stack. */
    mov r11, gs:[TeTlsSlots + 8]
    mov [r11 + 8], esp
    mov rsp, [r11]
    mov [r11 + 24], ebx
    mov [r11 + 28], esi
    mov [r11 + 32], edi
    mov [r11 + 36], ebp
    pushfq
    pop rdx
    mov [r11 + 16], edx
    cld
    mov ecx, eax
    mov edx, [r11 + 8]
    /* Return to the stub, which pops its own arguments. */
    mov eax, [rdx]
    mov [r11 + 12], eax
    add dword ptr [r11 + 8], 4
    add edx, 8
    call qword ptr Wow64Service[rip]
    mov r11, gs:[TeTlsSlots + 8]
    mov [r11 + 20], eax
Wow64CpuReturn:
    /* IRET in ring 3 switches only user selectors and restores the 32-bit
     * stack. Kernel transitions occur inside the native ntdll calls. */
    mov rsp, [r11]
    mov qword ptr [rsp + 64], HEX(2b)
    mov eax, [r11 + 8]
    mov [rsp + 56], rax
    mov eax, [r11 + 16]
    mov [rsp + 48], rax
    mov qword ptr [rsp + 40], HEX(23)
    mov eax, [r11 + 12]
    mov [rsp + 32], rax
    mov ebx, [r11 + 24]
    mov esi, [r11 + 28]
    mov edi, [r11 + 32]
    mov ebp, [r11 + 36]
    mov eax, HEX(53)
    mov fs, ax
    mov eax, HEX(2b)
    mov ds, ax
    mov es, ax
    mov eax, [r11 + 20]
    add rsp, 32
    iretq
.ENDP
END
