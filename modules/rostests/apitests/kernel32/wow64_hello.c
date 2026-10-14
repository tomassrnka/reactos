/* PE32 phase-1 console fixture. SPDX-License-Identifier: LGPL-2.1-or-later */
#define WIN32_NO_STATUS
#include <windows.h>
#define NTOS_MODE_USER
#include <ndk/ntndk.h>

/* Use the ordinary i386 kernel32 import library, with no CRT dependency. */
void __cdecl mainCRTStartup(void)
{
    static const char Text[] = "Hello from 32-bit ReactOS!\r\n";
    DWORD Written = 0;
    BOOL Wow64 = FALSE;
    PVOID Memory;
    SIZE_T Size = 4096;
    ULONG OldProtect;
    NTSTATUS Status;
    if (!IsWow64Process(GetCurrentProcess(), &Wow64)) ExitProcess(10);
    if (Wow64 && GetCommandLineW()[lstrlenW(GetCommandLineW()) - 1] == L'!')
        *(volatile ULONG *)0 = 1; /* Verify fail-closed guest hardware faults. */
    Memory = VirtualAlloc(NULL, Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!Memory) ExitProcess(13);
    *(ULONG *)Memory = 0x12345678;
    if (!VirtualProtect(Memory, Size, PAGE_READONLY, &OldProtect) ||
        OldProtect != PAGE_READWRITE || *(ULONG *)Memory != 0x12345678) ExitProcess(14);
    if (!VirtualFree(Memory, 0, MEM_RELEASE)) ExitProcess(15);
    if (Wow64)
    {
        Status = NtAllocateVirtualMemory(NtCurrentProcess(), (PVOID *)1, 0, &Size,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (Status != STATUS_ACCESS_VIOLATION) ExitProcess(16);
    }
    if (WriteFile(INVALID_HANDLE_VALUE, Text, sizeof(Text) - 1, &Written, NULL) ||
        GetLastError() != ERROR_INVALID_HANDLE) ExitProcess(17);
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), Text, sizeof(Text) - 1, &Written, NULL))
        ExitProcess(11);
    if (Written != sizeof(Text) - 1) ExitProcess(12);
    /* Distinguish native i386 control from an actual WoW64 process. */
    ExitProcess(Wow64 ? 32 : 0);
}
