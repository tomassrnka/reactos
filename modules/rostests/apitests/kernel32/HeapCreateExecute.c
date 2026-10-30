/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that HeapCreate(HEAP_CREATE_ENABLE_EXECUTE) returns executable memory
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static DWORD ProtectionOf(PVOID p)
{
    MEMORY_BASIC_INFORMATION Info;
    if (!VirtualQuery(p, &Info, sizeof(Info)))
        return 0;
    return Info.Protect;
}

static void TestHeap(DWORD Options, SIZE_T Size, DWORD Expected)
{
    HANDLE Heap;
    PVOID p;

    Heap = HeapCreate(Options, 0, 0);
    ok(Heap != NULL, "HeapCreate(0x%lx) failed %lu\n", Options, GetLastError());
    if (!Heap)
        return;
    p = HeapAlloc(Heap, 0, Size);
    ok(p != NULL, "HeapAlloc(%Iu) failed\n", Size);
    if (p)
    {
        DWORD Protect = ProtectionOf(p);
        ok(Protect == Expected, "Options 0x%lx size %Iu: protection 0x%lx, expected 0x%lx\n",
           Options, Size, Protect, Expected);
        HeapFree(Heap, 0, p);
    }
    HeapDestroy(Heap);
}

START_TEST(HeapCreateExecute)
{
    /* A small block lives in the heap's first segment, a large one is allocated directly */
    TestHeap(HEAP_CREATE_ENABLE_EXECUTE, 64, PAGE_EXECUTE_READWRITE);
    TestHeap(HEAP_CREATE_ENABLE_EXECUTE, 4 * 1024 * 1024, PAGE_EXECUTE_READWRITE);
    TestHeap(0, 64, PAGE_READWRITE);
    TestHeap(0, 4 * 1024 * 1024, PAGE_READWRITE);
}
