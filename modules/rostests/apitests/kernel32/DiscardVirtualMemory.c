/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for DiscardVirtualMemory (Windows 8.1 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows 10 22H2 (x64 and WOW64). What the pages hold afterwards is not tested.
 */

#include "precomp.h"

typedef DWORD (WINAPI *PFN_DiscardVirtualMemory)(PVOID, SIZE_T);

#define PENDING_ERROR 0xdeadbeef

static PFN_DiscardVirtualMemory pDiscardVirtualMemory;

static void Check(PVOID Address, SIZE_T Size, DWORD Expected, int Line)
{
    DWORD Result;

    SetLastError(PENDING_ERROR);
    Result = pDiscardVirtualMemory(Address, Size);
    ok(Result == Expected, "line %d: got %lu, expected %lu\n", Line, Result, Expected);
    /* a failure does not touch the last error */
    if (Expected != ERROR_SUCCESS)
        ok(GetLastError() == PENDING_ERROR, "line %d: the last error became %lu\n", Line, GetLastError());
}

#define CHECK(a, s, e) Check(a, s, e, __LINE__)

START_TEST(DiscardVirtualMemory)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    PUCHAR Memory, Reserved;
    DWORD Old;

    pDiscardVirtualMemory = (PFN_DiscardVirtualMemory)GetProcAddress(Kernel32, "DiscardVirtualMemory");
    /* Windows 8 and older have none: it came with Windows 8.1 */
    if (!pDiscardVirtualMemory)
    {
        pDiscardVirtualMemory = (PFN_DiscardVirtualMemory)GetProcAddress(LoadLibraryW(L"kernelbase.dll"), "DiscardVirtualMemory");
        /* ReactOS has it from export level 0x601, which a kernel32 with the Windows 7 functions shows */
        ok(pDiscardVirtualMemory || !(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")), "DiscardVirtualMemory is not exported\n");
        if (!pDiscardVirtualMemory)
        {
            skip("DiscardVirtualMemory is not exported\n");
            return;
        }
    }

    Memory = VirtualAlloc(NULL, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok(Memory != NULL, "VirtualAlloc failed\n");
    if (!Memory)
        return;
    memset(Memory, 0xaa, 0x4000);

    /* whole pages of committed memory that can be written */
    CHECK(Memory, 0x1000, ERROR_SUCCESS);
    CHECK(Memory + 0x1000, 0x2000, ERROR_SUCCESS);
    CHECK(Memory, 0x4000, ERROR_SUCCESS);

    /* anything else is refused */
    CHECK(Memory, 0, ERROR_INVALID_PARAMETER);
    CHECK(NULL, 0, ERROR_INVALID_PARAMETER);
    CHECK(NULL, 0x1000, ERROR_INVALID_PARAMETER);
    CHECK(Memory + 1, 0x1000, ERROR_INVALID_PARAMETER);
    CHECK(Memory, 1, ERROR_INVALID_PARAMETER);
    CHECK(Memory + 0x100, 0x100, ERROR_INVALID_PARAMETER);
    CHECK(Memory, 0x5000, ERROR_INVALID_PARAMETER);
    CHECK(Memory + 0x3000, 0x2000, ERROR_INVALID_PARAMETER);
    CHECK((PVOID)(ULONG_PTR)0x10, 0x1000, ERROR_INVALID_PARAMETER);
    CHECK((PVOID)~(ULONG_PTR)0, 0x1000, ERROR_INVALID_PARAMETER);

    /* a page that cannot be written, or is not committed */
    ok(VirtualProtect(Memory, 0x1000, PAGE_READONLY, &Old), "VirtualProtect failed\n");
    CHECK(Memory, 0x1000, ERROR_INVALID_PARAMETER);
    CHECK(Memory, 0x2000, ERROR_INVALID_PARAMETER);
    ok(VirtualProtect(Memory, 0x1000, PAGE_NOACCESS, &Old), "VirtualProtect failed\n");
    CHECK(Memory, 0x1000, ERROR_INVALID_PARAMETER);
    ok(VirtualProtect(Memory, 0x1000, PAGE_READWRITE, &Old), "VirtualProtect failed\n");
    CHECK(Memory, 0x1000, ERROR_SUCCESS);
    ok(VirtualFree(Memory + 0x3000, 0x1000, MEM_DECOMMIT), "VirtualFree failed\n");
    CHECK(Memory + 0x3000, 0x1000, ERROR_INVALID_PARAMETER);
    CHECK(Memory, 0x4000, ERROR_INVALID_PARAMETER);
    CHECK(Memory, 0x3000, ERROR_SUCCESS);

    Reserved = VirtualAlloc(NULL, 0x2000, MEM_RESERVE, PAGE_READWRITE);
    ok(Reserved != NULL, "VirtualAlloc failed\n");
    if (Reserved)
    {
        CHECK(Reserved, 0x1000, ERROR_INVALID_PARAMETER);
        ok(VirtualFree(Reserved, 0, MEM_RELEASE), "VirtualFree failed\n");
    }

    ok(VirtualFree(Memory, 0, MEM_RELEASE), "VirtualFree failed\n");
    CHECK(Memory, 0x1000, ERROR_INVALID_PARAMETER);
}
