/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for VirtualAllocExNuma
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef LPVOID (WINAPI *PFN_VirtualAllocExNuma)(HANDLE, LPVOID, SIZE_T, DWORD, DWORD, DWORD);

#define NO_PREFERRED_NODE 0xFFFFFFFF

typedef BOOL (WINAPI *PFN_GetNumaHighestNodeNumber)(PULONG);

static PFN_VirtualAllocExNuma pVirtualAllocExNuma;

static void CheckAlloc(PCSTR Name, DWORD Node)
{
    LPVOID Memory;

    SetLastError(0xdeadbeef);
    Memory = pVirtualAllocExNuma(GetCurrentProcess(), NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, Node);
    ok(Memory != NULL, "%s: failed with %lu\n", Name, GetLastError());
    if (Memory)
    {
        /* committed and writable */
        *(volatile char *)Memory = 1;
        ok(VirtualFree(Memory, 0, MEM_RELEASE), "%s: VirtualFree failed with %lu\n", Name, GetLastError());
    }
}

static void CheckFailure(PCSTR Name, HANDLE Process, SIZE_T Size, DWORD Protect, DWORD Node, DWORD ExpectedError)
{
    LPVOID Memory;

    SetLastError(0xdeadbeef);
    Memory = pVirtualAllocExNuma(Process, NULL, Size, MEM_COMMIT | MEM_RESERVE, Protect, Node);
    ok(Memory == NULL && GetLastError() == ExpectedError, "%s: %p, error %lu, expected error %lu\n", Name, Memory, GetLastError(), ExpectedError);
    if (Memory)
        VirtualFree(Memory, 0, MEM_RELEASE);
}

START_TEST(VirtualAllocExNuma)
{
    ULONG Highest = 0;
    PFN_GetNumaHighestNodeNumber pGetNumaHighestNodeNumber;

    pVirtualAllocExNuma = (PFN_VirtualAllocExNuma)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "VirtualAllocExNuma");
    if (!pVirtualAllocExNuma)
    {
        ok(!is_reactos(), "VirtualAllocExNuma is not exported by kernel32.dll\n");
        skip("VirtualAllocExNuma is not available\n");
        return;
    }

    pGetNumaHighestNodeNumber = (PFN_GetNumaHighestNodeNumber)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetNumaHighestNodeNumber");
    ok(pGetNumaHighestNodeNumber && pGetNumaHighestNodeNumber(&Highest), "GetNumaHighestNodeNumber failed with %lu\n", GetLastError());

    CheckAlloc("node 0", 0);
    CheckAlloc("the highest node", Highest);
    CheckAlloc("no preferred node", NO_PREFERRED_NODE);

    /* a node that does not exist */
    CheckFailure("the node after the highest", GetCurrentProcess(), 0x1000, PAGE_READWRITE, Highest + 1, ERROR_INVALID_PARAMETER);
    CheckFailure("node 5", GetCurrentProcess(), 0x1000, PAGE_READWRITE, Highest + 5, ERROR_INVALID_PARAMETER);
    CheckFailure("node 0xFFFFFFFE", GetCurrentProcess(), 0x1000, PAGE_READWRITE, 0xFFFFFFFE, ERROR_INVALID_PARAMETER);

    /* the other arguments are those of VirtualAllocEx */
    CheckFailure("size 0", GetCurrentProcess(), 0, PAGE_READWRITE, 0, ERROR_INVALID_PARAMETER);
    CheckFailure("protection 0", GetCurrentProcess(), 0x1000, 0, 0, ERROR_INVALID_PARAMETER);
    CheckFailure("invalid process", (HANDLE)(ULONG_PTR)0xdeadbeef, 0x1000, PAGE_READWRITE, 0, ERROR_INVALID_HANDLE);
}
