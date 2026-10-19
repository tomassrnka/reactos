/* ReactOS WoW64 mode transition. SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_NO_STATUS
#include <windef.h>
#define NTOS_MODE_USER
#include <ndk/ntndk.h>
#include <reactos/wow64.h>

WOW64_SERVICE Wow64Service;
extern VOID Wow64CpuDispatch(VOID);

NTSTATUS NTAPI
Wow64CpuInitialize(WOW64_SERVICE Service, PVOID *Gate)
{
    UCHAR *Code = NULL;
    SIZE_T Size = PAGE_SIZE;
    ULONG OldProtect;
    NTSTATUS Status;

    /* A compatibility far jump has a 32-bit offset. Its destination is a
     * small low-address trampoline; the DLL itself may reside above 4 GB. */
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), (PVOID *)&Code,
                                     0xFFFFFFFF, &Size, MEM_RESERVE | MEM_COMMIT,
                                     PAGE_READWRITE);
    if (!NT_SUCCESS(Status)) return Status;
    Code[0] = 0xff; Code[1] = 0x2d;
    *(ULONG *)(Code + 2) = PtrToUlong(Code + 32);
    *(ULONG *)(Code + 32) = PtrToUlong(Code + 16);
    *(USHORT *)(Code + 36) = 0x33; /* KGDT64_R3_CODE | RPL_MASK */
    Code[16] = 0x49; Code[17] = 0xbb; /* mov r11, imm64 */
    *(ULONG64 *)(Code + 18) = (ULONG64)Wow64CpuDispatch;
    Code[26] = 0x41; Code[27] = 0xff; Code[28] = 0xe3; /* jmp r11 */
    Status = NtProtectVirtualMemory(NtCurrentProcess(), (PVOID *)&Code, &Size,
                                    PAGE_EXECUTE_READ, &OldProtect);
    if (!NT_SUCCESS(Status))
    {
        Size = 0;
        NtFreeVirtualMemory(NtCurrentProcess(), (PVOID *)&Code, &Size, MEM_RELEASE);
        return Status;
    }
    NtFlushInstructionCache(NtCurrentProcess(), Code, PAGE_SIZE);
    Wow64Service = Service;
    *Gate = Code;
    return STATUS_SUCCESS;
}
