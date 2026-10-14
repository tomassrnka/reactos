/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Tests for HeapWalk and RtlWalkHeap
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expectations are what Windows Server 2008 R2 and Windows 10 report for the same calls. The
 * layout of a heap with its region, its blocks and its uncommitted range follows the checks that
 * the Wine project has for HeapWalk (modules/rostests/winetests/kernel32/heap.c, see the copyright
 * holders there); the rest was measured on the two systems.
 */

#include "precomp.h"

#include <versionhelpers.h>

/* The entry as ntdll reports it. The sizes of the region differ from PROCESS_HEAP_ENTRY in 64 bits only in cbData. */
typedef struct
{
    PVOID lpData;
    SIZE_T cbData;
    BYTE cbOverhead;
    BYTE iRegionIndex;
    WORD wFlags;
    union
    {
        struct
        {
            SIZE_T Settable;
            USHORT TagIndex;
            USHORT AllocatorBackTraceIndex;
            DWORD Reserved[2];
        } Block;
        struct
        {
            DWORD dwCommittedSize;
            DWORD dwUnCommittedSize;
            PVOID lpFirstBlock;
            PVOID lpLastBlock;
        } Region;
    };
} RTL_ENTRY;

#define RTL_BUSY            0x0001
#define RTL_REGION          0x0002
#define RTL_BLOCK           0x0010
#define RTL_UNCOMMITTED     0x0100 /* 0x1000 on Windows 10 */
#define RTL_UNCOMMITTED_10  0x1000
#define RTL_SETTABLE_FLAG1  0x0020

#define HEAP_SETTABLE_USER_VALUE_ 0x00000100
#define HEAP_SETTABLE_USER_FLAG1_ 0x00000200

#define MAX_ENTRIES 4096
#define VIRTUAL_SIZE (5 * 0x8000 * sizeof(void *)) /* more than a heap segment keeps */

static NTSTATUS (NTAPI *pRtlWalkHeap)(HANDLE, PVOID);
static PVOID (NTAPI *pRtlAllocateHeap)(HANDLE, ULONG, SIZE_T);
static BOOLEAN (NTAPI *pRtlSetUserValueHeap)(HANDLE, ULONG, PVOID, PVOID);
static BOOLEAN (NTAPI *pRtlSetUserFlagsHeap)(HANDLE, ULONG, PVOID, ULONG, ULONG);

static DWORD WalkAll(HANDLE Heap, PROCESS_HEAP_ENTRY *Entries, DWORD Max)
{
    PROCESS_HEAP_ENTRY Entry;
    DWORD Count = 0;
    BOOL Ret;

    memset(&Entry, 0xcd, sizeof(Entry));
    Entry.lpData = NULL;
    SetLastError(0xdeadbeef);
    while ((Ret = HeapWalk(Heap, &Entry)))
    {
        if (Count < Max)
            Entries[Count] = Entry;
        Count++;
        if (Count > 100000)
            break;
    }
    ok(!Ret, "The walk does not end\n");
    ok_err(ERROR_NO_MORE_ITEMS);
    return Count;
}

static DWORD RtlWalkAll(HANDLE Heap, RTL_ENTRY *Entries, DWORD Max)
{
    RTL_ENTRY Entry;
    NTSTATUS Status;
    DWORD Count = 0;

    memset(&Entry, 0xcd, sizeof(Entry));
    Entry.lpData = NULL;
    while (!(Status = pRtlWalkHeap(Heap, &Entry)))
    {
        if (Count < Max)
            Entries[Count] = Entry;
        Count++;
        if (Count > 100000)
            break;
    }
    ok_hex(Status, STATUS_NO_MORE_ENTRIES);
    return Count;
}

static void CheckBlock(const PROCESS_HEAP_ENTRY *Entry, PVOID Pointer, SIZE_T Size)
{
    ok(Entry->lpData == Pointer, "lpData is %p, expected %p\n", Entry->lpData, Pointer);
    ok(Entry->cbData == Size, "cbData is %lu, expected %Iu\n", Entry->cbData, Size);
    ok(Entry->wFlags == PROCESS_HEAP_ENTRY_BUSY, "wFlags is %x\n", Entry->wFlags);
    ok(Entry->iRegionIndex == 0, "iRegionIndex is %u\n", Entry->iRegionIndex);
}

static void test_fresh_heap(void)
{
    PROCESS_HEAP_ENTRY Entries[16];
    RTL_ENTRY RtlEntries[16];
    HANDLE Heap;
    DWORD Count, RtlCount, i;

    Heap = HeapCreate(0, 0, 0);
    ok(Heap != NULL, "HeapCreate failed with %lu\n", GetLastError());
    if (Heap == NULL)
        return;

    Count = WalkAll(Heap, Entries, ARRAYSIZE(Entries));
    RtlCount = RtlWalkAll(Heap, RtlEntries, ARRAYSIZE(RtlEntries));
    ok(Count == 3, "%lu entries\n", Count);
    ok(RtlCount == Count, "ntdll reports %lu entries, kernel32 %lu\n", RtlCount, Count);

    if (Count == 3 && RtlCount == 3)
    {
        /* The region: the header of the heap */
        ok(Entries[0].wFlags == PROCESS_HEAP_REGION, "wFlags is %x\n", Entries[0].wFlags);
        ok(Entries[0].lpData == Heap, "lpData is %p, the heap is %p\n", Entries[0].lpData, Heap);
        ok(Entries[0].cbData != 0 && Entries[0].cbData <= 0x1000, "cbData is %lx\n", Entries[0].cbData);
        ok(Entries[0].cbOverhead == 0, "cbOverhead is %u\n", Entries[0].cbOverhead);
        ok(Entries[0].iRegionIndex == 0, "iRegionIndex is %u\n", Entries[0].iRegionIndex);
        ok(Entries[0].Region.dwCommittedSize != 0, "dwCommittedSize is 0\n");
        ok(Entries[0].Region.dwUnCommittedSize != 0, "dwUnCommittedSize is 0\n");
        ok((BYTE *)Entries[0].Region.lpFirstBlock > (BYTE *)Entries[0].lpData + Entries[0].cbData &&
           (BYTE *)Entries[0].Region.lpFirstBlock <= (BYTE *)Entries[0].lpData + Entries[0].cbData + 4 * sizeof(void *),
           "lpFirstBlock is %p after %p and %lx\n", Entries[0].Region.lpFirstBlock, Entries[0].lpData, Entries[0].cbData);
        ok((BYTE *)Entries[0].Region.lpLastBlock == (BYTE *)Entries[2].lpData + Entries[2].cbData,
           "lpLastBlock is %p, the uncommitted range ends at %p\n", Entries[0].Region.lpLastBlock, (BYTE *)Entries[2].lpData + Entries[2].cbData);

        /* The free block, which is all that the heap holds */
        ok(Entries[1].wFlags == 0, "wFlags is %x\n", Entries[1].wFlags);
        ok(Entries[1].lpData != NULL, "lpData is NULL\n");
        ok(Entries[1].cbData != 0, "cbData is 0\n");
        ok(Entries[1].cbOverhead == 4 * sizeof(void *), "cbOverhead is %u\n", Entries[1].cbOverhead);
        ok(Entries[1].iRegionIndex == 0, "iRegionIndex is %u\n", Entries[1].iRegionIndex);

        /* What is committed ends where the uncommitted range begins */
        ok(Entries[2].wFlags == PROCESS_HEAP_UNCOMMITTED_RANGE, "wFlags is %x\n", Entries[2].wFlags);
        ok(Entries[2].lpData == (BYTE *)Entries[0].lpData + Entries[0].Region.dwCommittedSize,
           "lpData is %p, expected %p\n", Entries[2].lpData, (BYTE *)Entries[0].lpData + Entries[0].Region.dwCommittedSize);
        /* Windows 10 leaves out the last page of the region */
        ok(Entries[2].cbData == Entries[0].Region.dwUnCommittedSize || Entries[2].cbData + 0x1000 == Entries[0].Region.dwUnCommittedSize,
           "cbData is %lx, the region says %lx\n", Entries[2].cbData, Entries[0].Region.dwUnCommittedSize);
        ok(Entries[2].cbOverhead == 0, "cbOverhead is %u\n", Entries[2].cbOverhead);
        ok(Entries[2].iRegionIndex == 0, "iRegionIndex is %u\n", Entries[2].iRegionIndex);

        /* The same entries from ntdll */
        for (i = 0; i < 3; i++)
        {
            ok(RtlEntries[i].lpData == Entries[i].lpData, "%lu: lpData is %p, expected %p\n", i, RtlEntries[i].lpData, Entries[i].lpData);
            ok(RtlEntries[i].cbData == Entries[i].cbData, "%lu: cbData is %Ix, expected %lx\n", i, RtlEntries[i].cbData, Entries[i].cbData);
            ok(RtlEntries[i].cbOverhead == Entries[i].cbOverhead, "%lu: cbOverhead is %u\n", i, RtlEntries[i].cbOverhead);
        }
        ok(RtlEntries[0].wFlags == RTL_REGION, "wFlags is %x\n", RtlEntries[0].wFlags);
        ok(RtlEntries[0].Region.dwCommittedSize == Entries[0].Region.dwCommittedSize, "dwCommittedSize is %lx\n", RtlEntries[0].Region.dwCommittedSize);
        ok(RtlEntries[0].Region.dwUnCommittedSize == Entries[0].Region.dwUnCommittedSize, "dwUnCommittedSize is %lx\n", RtlEntries[0].Region.dwUnCommittedSize);
        ok(RtlEntries[0].Region.lpFirstBlock == Entries[0].Region.lpFirstBlock, "lpFirstBlock is %p\n", RtlEntries[0].Region.lpFirstBlock);
        ok(RtlEntries[0].Region.lpLastBlock == Entries[0].Region.lpLastBlock, "lpLastBlock is %p\n", RtlEntries[0].Region.lpLastBlock);
        ok(RtlEntries[1].wFlags == 0, "wFlags is %x\n", RtlEntries[1].wFlags);
        ok(RtlEntries[2].wFlags == RTL_UNCOMMITTED || RtlEntries[2].wFlags == RTL_UNCOMMITTED_10, "wFlags is %x\n", RtlEntries[2].wFlags);
    }

    /* The end is reported again */
    {
        PROCESS_HEAP_ENTRY Last = Entries[2];

        SetLastError(0xdeadbeef);
        ok(!HeapWalk(Heap, &Last), "HeapWalk after the end succeeded\n");
        ok_err(ERROR_NO_MORE_ITEMS);
    }

    ok(HeapDestroy(Heap), "HeapDestroy failed with %lu\n", GetLastError());
}

static void test_blocks(void)
{
    PROCESS_HEAP_ENTRY Entries[32];
    RTL_ENTRY RtlEntries[32];
    PVOID Block[3];
    HANDLE Heap;
    DWORD Count, RtlCount, i;
    static const SIZE_T Sizes[3] = { 100, 123, 456 };

    Heap = HeapCreate(0, 0, 0);
    ok(Heap != NULL, "HeapCreate failed with %lu\n", GetLastError());
    if (Heap == NULL)
        return;

    for (i = 0; i < 3; i++)
    {
        Block[i] = HeapAlloc(Heap, HEAP_ZERO_MEMORY, Sizes[i]);
        ok(Block[i] != NULL, "HeapAlloc failed with %lu\n", GetLastError());
    }

    Count = WalkAll(Heap, Entries, ARRAYSIZE(Entries));
    RtlCount = RtlWalkAll(Heap, RtlEntries, ARRAYSIZE(RtlEntries));
    ok(Count == 6, "%lu entries\n", Count);
    ok(RtlCount == Count, "ntdll reports %lu entries, kernel32 %lu\n", RtlCount, Count);

    if (Count == 6 && RtlCount == 6)
    {
        ok(Entries[0].wFlags == PROCESS_HEAP_REGION, "wFlags is %x\n", Entries[0].wFlags);

        for (i = 0; i < 3; i++)
        {
            winetest_push_context("block %lu", i);
            CheckBlock(&Entries[1 + i], Block[i], Sizes[i]);
            ok(RtlEntries[1 + i].wFlags == RTL_BUSY, "wFlags is %x\n", RtlEntries[1 + i].wFlags);
            ok(RtlEntries[1 + i].lpData == Block[i], "lpData is %p\n", RtlEntries[1 + i].lpData);
            ok(RtlEntries[1 + i].cbData == Sizes[i], "cbData is %Ix\n", RtlEntries[1 + i].cbData);
            ok(RtlEntries[1 + i].cbOverhead == Entries[1 + i].cbOverhead, "cbOverhead is %u\n", RtlEntries[1 + i].cbOverhead);
            ok(Entries[1 + i].cbOverhead < 4 * sizeof(void *), "cbOverhead is %u\n", Entries[1 + i].cbOverhead);
            winetest_pop_context();
        }

        /* The overhead is what the block takes besides its data, and the next block starts where this one ends */
        ok((BYTE *)Entries[2].lpData == (BYTE *)Entries[1].lpData + Entries[1].cbData + Entries[1].cbOverhead,
           "Block 1 is at %p, block 0 ends at %p\n", Entries[2].lpData, (BYTE *)Entries[1].lpData + Entries[1].cbData + Entries[1].cbOverhead);
        ok((BYTE *)Entries[3].lpData == (BYTE *)Entries[2].lpData + Entries[2].cbData + Entries[2].cbOverhead,
           "Block 2 is at %p, block 1 ends at %p\n", Entries[3].lpData, (BYTE *)Entries[2].lpData + Entries[2].cbData + Entries[2].cbOverhead);

        /* The free block follows, and the entry of the heap in front of the data of the next block is not part of it */
        ok(Entries[4].wFlags == 0, "wFlags is %x\n", Entries[4].wFlags);
        ok((BYTE *)Entries[4].lpData == (BYTE *)Entries[3].lpData + Entries[3].cbData + Entries[3].cbOverhead + 2 * sizeof(void *),
           "The free block is at %p, the block before ends at %p\n", Entries[4].lpData, (BYTE *)Entries[3].lpData + Entries[3].cbData + Entries[3].cbOverhead);
        ok(Entries[4].cbOverhead == 4 * sizeof(void *), "cbOverhead is %u\n", Entries[4].cbOverhead);
        ok(Entries[5].wFlags == PROCESS_HEAP_UNCOMMITTED_RANGE, "wFlags is %x\n", Entries[5].wFlags);
    }

    /* A block that is freed is a free block, or part of one */
    ok(HeapFree(Heap, 0, Block[1]), "HeapFree failed with %lu\n", GetLastError());
    Count = WalkAll(Heap, Entries, ARRAYSIZE(Entries));
    ok(Count == 6, "%lu entries\n", Count);
    if (Count == 6)
    {
        ok(Entries[1].wFlags == PROCESS_HEAP_ENTRY_BUSY && Entries[1].lpData == Block[0], "wFlags is %x, lpData is %p\n", Entries[1].wFlags, Entries[1].lpData);
        ok(Entries[2].wFlags == 0, "wFlags is %x\n", Entries[2].wFlags);
        ok(Entries[3].wFlags == PROCESS_HEAP_ENTRY_BUSY && Entries[3].lpData == Block[2], "wFlags is %x, lpData is %p\n", Entries[3].wFlags, Entries[3].lpData);
    }

    ok(HeapFree(Heap, 0, Block[0]), "HeapFree failed with %lu\n", GetLastError());
    ok(HeapFree(Heap, 0, Block[2]), "HeapFree failed with %lu\n", GetLastError());
    Count = WalkAll(Heap, Entries, ARRAYSIZE(Entries));
    ok(Count == 3, "%lu entries after all blocks were freed\n", Count);

    ok(HeapDestroy(Heap), "HeapDestroy failed with %lu\n", GetLastError());
}

/* A block of no bytes is a block with all its overhead */
static void test_zero_size(void)
{
    PROCESS_HEAP_ENTRY Entries[8];
    PVOID Block;
    HANDLE Heap;
    DWORD Count;

    Heap = HeapCreate(0, 0, 0);
    ok(Heap != NULL, "HeapCreate failed with %lu\n", GetLastError());
    if (Heap == NULL)
        return;

    Block = HeapAlloc(Heap, 0, 0);
    ok(Block != NULL, "HeapAlloc failed with %lu\n", GetLastError());
    Count = WalkAll(Heap, Entries, ARRAYSIZE(Entries));
    ok(Count == 4, "%lu entries\n", Count);
    if (Count == 4)
    {
        CheckBlock(&Entries[1], Block, 0);
        ok(Entries[1].cbOverhead != 0, "cbOverhead is 0\n");
    }

    ok(HeapDestroy(Heap), "HeapDestroy failed with %lu\n", GetLastError());
}

static void test_virtual_block(void)
{
    PROCESS_HEAP_ENTRY Entries[16];
    RTL_ENTRY RtlEntries[16];
    PVOID Big[2], Small;
    HANDLE Heap;
    DWORD Count, RtlCount, i;

    Heap = HeapCreate(0, 0, 0);
    ok(Heap != NULL, "HeapCreate failed with %lu\n", GetLastError());
    if (Heap == NULL)
        return;

    Big[0] = HeapAlloc(Heap, HEAP_ZERO_MEMORY, VIRTUAL_SIZE);
    Small = HeapAlloc(Heap, 0, 64);
    Big[1] = HeapAlloc(Heap, HEAP_ZERO_MEMORY, VIRTUAL_SIZE);
    ok(Big[0] != NULL && Big[1] != NULL && Small != NULL, "HeapAlloc failed with %lu\n", GetLastError());

    Count = WalkAll(Heap, Entries, ARRAYSIZE(Entries));
    RtlCount = RtlWalkAll(Heap, RtlEntries, ARRAYSIZE(RtlEntries));
    ok(Count == 6, "%lu entries\n", Count);
    ok(RtlCount == Count, "ntdll reports %lu entries, kernel32 %lu\n", RtlCount, Count);

    /* The blocks that are too big for the heap come after what the heap keeps itself, in the order of their allocation */
    if (Count == 6 && RtlCount == 6)
    {
        ok(Entries[0].wFlags == PROCESS_HEAP_REGION, "wFlags is %x\n", Entries[0].wFlags);
        CheckBlock(&Entries[1], Small, 64);
        ok(Entries[2].wFlags == 0, "wFlags is %x\n", Entries[2].wFlags);
        ok(Entries[3].wFlags == PROCESS_HEAP_UNCOMMITTED_RANGE, "wFlags is %x\n", Entries[3].wFlags);

        for (i = 0; i < 2; i++)
        {
            PROCESS_HEAP_ENTRY *Entry = &Entries[4 + i];

            winetest_push_context("big block %lu", i);
            ok(Entry->lpData == Big[i], "lpData is %p, expected %p\n", Entry->lpData, Big[i]);
            ok(Entry->cbData == VIRTUAL_SIZE, "cbData is %lx\n", Entry->cbData);
            /* Windows 7 marks it as shared with DDE too, Windows 10 does not; ReactOS answers as Windows 7 does */
            ok(Entry->wFlags == PROCESS_HEAP_ENTRY_BUSY || Entry->wFlags == (PROCESS_HEAP_ENTRY_BUSY | PROCESS_HEAP_ENTRY_DDESHARE),
               "wFlags is %x\n", Entry->wFlags);
            if (IsReactOS())
                ok(Entry->wFlags == (PROCESS_HEAP_ENTRY_BUSY | PROCESS_HEAP_ENTRY_DDESHARE), "wFlags is %x\n", Entry->wFlags);
            ok(Entry->iRegionIndex == 64, "iRegionIndex is %u\n", Entry->iRegionIndex);
            ok(Entry->cbOverhead == 0 || Entry->cbOverhead == 8 * sizeof(void *), "cbOverhead is %u\n", Entry->cbOverhead);
            ok((RtlEntries[4 + i].wFlags & (RTL_BUSY | RTL_BLOCK)) == (RTL_BUSY | RTL_BLOCK), "wFlags is %x\n", RtlEntries[4 + i].wFlags);
            winetest_pop_context();
        }
    }

    /* The big blocks go away with their heap, or when they are freed */
    ok(HeapFree(Heap, 0, Big[0]), "HeapFree failed with %lu\n", GetLastError());
    Count = WalkAll(Heap, Entries, ARRAYSIZE(Entries));
    ok(Count == 5, "%lu entries after a big block was freed\n", Count);
    if (Count == 5)
        ok(Entries[4].lpData == Big[1], "lpData is %p, expected %p\n", Entries[4].lpData, Big[1]);

    ok(HeapFree(Heap, 0, Big[1]), "HeapFree failed with %lu\n", GetLastError());
    Count = WalkAll(Heap, Entries, ARRAYSIZE(Entries));
    ok(Count == 4, "%lu entries after both big blocks were freed\n", Count);

    ok(HeapDestroy(Heap), "HeapDestroy failed with %lu\n", GetLastError());
}

/* The blocks of the heap are in the segments, and a heap that grows has several */
static void test_segments(void)
{
    static PROCESS_HEAP_ENTRY Entries[MAX_ENTRIES];
    PVOID Block[64];
    HANDLE Heap;
    DWORD Count, Regions = 0, Busy = 0, Uncommitted = 0, Allocated, Freed = 0, i;
    ULONG_PTR Segment = 0;

    Heap = HeapCreate(0, 0, 0);
    ok(Heap != NULL, "HeapCreate failed with %lu\n", GetLastError());
    if (Heap == NULL)
        return;

    memset(Block, 0, sizeof(Block));
    for (i = 0; i < ARRAYSIZE(Block); i++)
    {
        Block[i] = HeapAlloc(Heap, 0, 0x70000);
        if (Block[i] == NULL)
            break;
    }
    Allocated = i;
    ok(Allocated == ARRAYSIZE(Block), "HeapAlloc failed for block %lu with %lu\n", Allocated, GetLastError());

    Count = WalkAll(Heap, Entries, ARRAYSIZE(Entries));
    ok(Count < ARRAYSIZE(Entries), "%lu entries\n", Count);
    if (Count < ARRAYSIZE(Entries))
    {
        for (i = 0; i < Count; i++)
        {
            if (Entries[i].wFlags == PROCESS_HEAP_REGION)
            {
                Regions++;
                Segment = (ULONG_PTR)Entries[i].lpData;
                ok(Entries[i].Region.dwCommittedSize + Entries[i].Region.dwUnCommittedSize == (ULONG_PTR)Entries[i].Region.lpLastBlock - Segment ||
                   Entries[i].Region.dwCommittedSize + Entries[i].Region.dwUnCommittedSize == (ULONG_PTR)Entries[i].Region.lpLastBlock - Segment + 0x1000,
                   "Region %lu: %lx committed and %lx uncommitted in %Ix\n", Regions,
                   Entries[i].Region.dwCommittedSize, Entries[i].Region.dwUnCommittedSize,
                   (ULONG_PTR)Entries[i].Region.lpLastBlock - Segment);
            }
            else if (Entries[i].wFlags == PROCESS_HEAP_ENTRY_BUSY)
            {
                Busy++;
                ok(Entries[i].cbData == 0x70000, "Block %lu has %lx bytes\n", Busy, Entries[i].cbData);
                ok((ULONG_PTR)Entries[i].lpData > Segment, "Block %lu at %p is not in the region at %Ix\n", Busy, Entries[i].lpData, Segment);
            }
            else if (Entries[i].wFlags == PROCESS_HEAP_UNCOMMITTED_RANGE)
            {
                Uncommitted++;
            }
        }

        ok(Regions >= 2, "%lu regions\n", Regions);
        ok(Busy == Allocated, "%lu blocks, %lu were allocated\n", Busy, Allocated);
        ok(Uncommitted >= 1, "%lu uncommitted ranges\n", Uncommitted);
    }

    /* A heap whose segments are made of what is left after blocks were freed */
    for (i = 0; i < Allocated; i += 2)
    {
        ok(HeapFree(Heap, 0, Block[i]), "HeapFree failed with %lu\n", GetLastError());
        Freed++;
    }

    Count = WalkAll(Heap, Entries, ARRAYSIZE(Entries));
    ok(Count < ARRAYSIZE(Entries), "%lu entries\n", Count);
    for (i = 0, Busy = 0; i < min(Count, ARRAYSIZE(Entries)); i++)
    {
        if (Entries[i].wFlags == PROCESS_HEAP_ENTRY_BUSY)
            Busy++;
    }
    ok(Busy == Allocated - Freed, "%lu blocks are left\n", Busy);

    ok(HeapDestroy(Heap), "HeapDestroy failed with %lu\n", GetLastError());
}

static void test_user_info(void)
{
    RTL_ENTRY Entries[16];
    PVOID Settable, Plain;
    HANDLE Heap;
    DWORD Count;

    if (!pRtlAllocateHeap || !pRtlSetUserValueHeap || !pRtlSetUserFlagsHeap)
    {
        skip("The heap functions of ntdll are not there\n");
        return;
    }

    Heap = HeapCreate(0, 0, 0);
    ok(Heap != NULL, "HeapCreate failed with %lu\n", GetLastError());
    if (Heap == NULL)
        return;

    Settable = pRtlAllocateHeap(Heap, HEAP_SETTABLE_USER_VALUE_ | HEAP_SETTABLE_USER_FLAG1_, 64);
    Plain = pRtlAllocateHeap(Heap, 0, 64);
    ok(Settable != NULL && Plain != NULL, "RtlAllocateHeap failed\n");

    Count = RtlWalkAll(Heap, Entries, ARRAYSIZE(Entries));
    ok(Count == 5, "%lu entries\n", Count);
    if (Count == 5)
    {
        /* A block that can carry a value and flags says so, and has room for them in its overhead */
        ok(Entries[1].lpData == Settable, "lpData is %p, expected %p\n", Entries[1].lpData, Settable);
        ok(Entries[1].wFlags == (RTL_BUSY | RTL_BLOCK | RTL_SETTABLE_FLAG1), "wFlags is %x\n", Entries[1].wFlags);
        ok(Entries[1].cbOverhead > Entries[2].cbOverhead, "cbOverhead is %u, a plain block has %u\n", Entries[1].cbOverhead, Entries[2].cbOverhead);
        ok(Entries[2].lpData == Plain, "lpData is %p, expected %p\n", Entries[2].lpData, Plain);
        ok(Entries[2].wFlags == RTL_BUSY, "wFlags is %x\n", Entries[2].wFlags);
    }

    ok(pRtlSetUserValueHeap(Heap, 0, Settable, (PVOID)(ULONG_PTR)0x12345678), "RtlSetUserValueHeap failed\n");
    ok(pRtlSetUserFlagsHeap(Heap, 0, Settable, 0, 0x400 | 0x800), "RtlSetUserFlagsHeap failed\n");

    Count = RtlWalkAll(Heap, Entries, ARRAYSIZE(Entries));
    ok(Count == 5, "%lu entries\n", Count);
    if (Count == 5)
    {
        ok(Entries[1].wFlags == (RTL_BUSY | RTL_BLOCK | 0xe0), "wFlags is %x\n", Entries[1].wFlags);
        ok(Entries[1].Block.Settable == 0x12345678, "Settable is %Ix\n", Entries[1].Block.Settable);
        ok(Entries[2].wFlags == RTL_BUSY, "wFlags is %x\n", Entries[2].wFlags);
    }

    ok(HeapDestroy(Heap), "HeapDestroy failed with %lu\n", GetLastError());
}

/* The walk goes on after the block that it stood at was freed */
static void test_free_under_cursor(void)
{
    PROCESS_HEAP_ENTRY Entry;
    PVOID Block[3];
    HANDLE Heap;
    DWORD i;

    Heap = HeapCreate(0, 0, 0);
    ok(Heap != NULL, "HeapCreate failed with %lu\n", GetLastError());
    if (Heap == NULL)
        return;

    for (i = 0; i < 3; i++)
        Block[i] = HeapAlloc(Heap, 0, 100);

    memset(&Entry, 0, sizeof(Entry));
    ok(HeapWalk(Heap, &Entry), "HeapWalk failed with %lu\n", GetLastError());
    ok(Entry.wFlags == PROCESS_HEAP_REGION, "wFlags is %x\n", Entry.wFlags);
    ok(HeapWalk(Heap, &Entry), "HeapWalk failed with %lu\n", GetLastError());
    ok(Entry.lpData == Block[0], "lpData is %p, expected %p\n", Entry.lpData, Block[0]);

    ok(HeapFree(Heap, 0, Block[0]), "HeapFree failed with %lu\n", GetLastError());
    ok(HeapWalk(Heap, &Entry), "HeapWalk failed with %lu\n", GetLastError());
    ok(Entry.lpData == Block[1], "lpData is %p, expected %p\n", Entry.lpData, Block[1]);
    ok(Entry.wFlags == PROCESS_HEAP_ENTRY_BUSY, "wFlags is %x\n", Entry.wFlags);

    ok(HeapDestroy(Heap), "HeapDestroy failed with %lu\n", GetLastError());
}

/* A heap that is locked by whoever walks it, while others allocate and free */
typedef struct
{
    HANDLE Heap;
    volatile LONG Stop;
    DWORD Seed;
} WORKER;

static DWORD WINAPI Worker(PVOID Parameter)
{
    WORKER *Worker = Parameter;
    PVOID Block[48] = { 0 };
    DWORD Seed = Worker->Seed, Index, Size;

    while (!Worker->Stop)
    {
        Seed = Seed * 1103515245 + 12345;
        Index = (Seed >> 16) % ARRAYSIZE(Block);
        if (Block[Index] != NULL)
        {
            HeapFree(Worker->Heap, 0, Block[Index]);
            Block[Index] = NULL;
            continue;
        }

        /* Mostly small, some that make the heap commit and decommit, a few that are too big for a segment */
        Size = 16 + ((Seed >> 8) % 1000);
        if ((Seed & 0xf0) == 0xf0)
            Size = 0x8000 + ((Seed >> 8) % 0x20000);
        if ((Seed & 0xff0) == 0xff0)
            Size = 0x90000;
        Block[Index] = HeapAlloc(Worker->Heap, 0, Size);
    }

    for (Index = 0; Index < ARRAYSIZE(Block); Index++)
    {
        if (Block[Index] != NULL)
            HeapFree(Worker->Heap, 0, Block[Index]);
    }

    return 0;
}

static void test_locked_walk(void)
{
    static PROCESS_HEAP_ENTRY Entries[MAX_ENTRIES];
    WORKER Workers[3];
    HANDLE Threads[3];
    DWORD Tick, Walks = 0, Count, i, Bad = 0, Busy, Error;
    PCSTR Why = NULL;

    for (i = 0; i < ARRAYSIZE(Workers); i++)
    {
        Workers[i].Heap = i == 0 ? HeapCreate(0, 0, 0) : Workers[0].Heap;
        Workers[i].Stop = 0;
        Workers[i].Seed = 1 + i * 7919;
    }
    ok(Workers[0].Heap != NULL, "HeapCreate failed with %lu\n", GetLastError());
    if (Workers[0].Heap == NULL)
        return;

    for (i = 0; i < ARRAYSIZE(Workers); i++)
    {
        Threads[i] = CreateThread(NULL, 0, Worker, &Workers[i], 0, NULL);
        ok(Threads[i] != NULL, "CreateThread failed with %lu\n", GetLastError());
    }

    Tick = GetTickCount();
    while (GetTickCount() - Tick < 3000)
    {
        PROCESS_HEAP_ENTRY Entry;

        /* The caller has to lock the heap for the whole walk, as the documentation says */
        if (!HeapLock(Workers[0].Heap))
            Bad++;

        memset(&Entry, 0, sizeof(Entry));
        Count = 0;
        SetLastError(0xdeadbeef);
        while (HeapWalk(Workers[0].Heap, &Entry))
        {
            if (Count == 0 && Entry.wFlags != PROCESS_HEAP_REGION)
                Why = "the first entry is not a region";
            if (Entry.wFlags == PROCESS_HEAP_ENTRY_BUSY && (Entry.lpData == NULL || Entry.cbData == 0))
                Why = "a busy block has no data";
            Count++;
            if (Count > 100000)
                break;
        }
        Error = GetLastError();
        if (Error != ERROR_NO_MORE_ITEMS || Count < 3 || Count > 100000)
            Why = "the walk does not end properly";

        if (Why)
        {
            if (Bad == 0)
                ok(0, "Walk %lu: %s, error %lu after %lu entries, the last at %p flags %x\n", Walks, Why, Error, Count, Entry.lpData, Entry.wFlags);
            Bad++;
            Why = NULL;
        }

        if (!HeapUnlock(Workers[0].Heap))
            Bad++;
        Walks++;
    }

    ok(Walks > 10, "Only %lu walks\n", Walks);
    ok(Bad == 0, "%lu walks were wrong\n", Bad);

    for (i = 0; i < ARRAYSIZE(Workers); i++)
        Workers[i].Stop = 1;

    for (i = 0; i < ARRAYSIZE(Workers); i++)
    {
        ok_int(WaitForSingleObject(Threads[i], 20000), WAIT_OBJECT_0);
        CloseHandle(Threads[i]);
    }

    /* Everything was freed. What Windows keeps for the small blocks of the low fragmentation heap shows as blocks. */
    Count = WalkAll(Workers[0].Heap, Entries, ARRAYSIZE(Entries));
    for (i = 0, Busy = 0; i < min(Count, ARRAYSIZE(Entries)); i++)
    {
        if (Entries[i].wFlags & PROCESS_HEAP_ENTRY_BUSY)
            Busy++;
    }
    if (IsReactOS())
        ok(Busy == 0, "%lu blocks are left\n", Busy);

    ok(HeapDestroy(Workers[0].Heap), "HeapDestroy failed with %lu\n", GetLastError());
}

/* Windows does not promise anything for a walk without the lock; ReactOS takes the lock for every step */
static void test_unlocked_walk(void)
{
    WORKER Workers[3];
    HANDLE Threads[3];
    DWORD Tick, Walks = 0, Count, i;
    PROCESS_HEAP_ENTRY Entry;

    for (i = 0; i < ARRAYSIZE(Workers); i++)
    {
        Workers[i].Heap = i == 0 ? HeapCreate(0, 0, 0) : Workers[0].Heap;
        Workers[i].Stop = 0;
        Workers[i].Seed = 3 + i * 104729;
    }
    ok(Workers[0].Heap != NULL, "HeapCreate failed with %lu\n", GetLastError());
    if (Workers[0].Heap == NULL)
        return;

    for (i = 0; i < ARRAYSIZE(Workers); i++)
        Threads[i] = CreateThread(NULL, 0, Worker, &Workers[i], 0, NULL);

    Tick = GetTickCount();
    while (GetTickCount() - Tick < 3000)
    {
        memset(&Entry, 0, sizeof(Entry));
        Count = 0;
        while (HeapWalk(Workers[0].Heap, &Entry) && Count < 100000)
            Count++;
        Walks++;
    }
    ok(Walks > 10, "Only %lu walks\n", Walks);

    for (i = 0; i < ARRAYSIZE(Workers); i++)
        Workers[i].Stop = 1;

    for (i = 0; i < ARRAYSIZE(Workers); i++)
    {
        ok_int(WaitForSingleObject(Threads[i], 20000), WAIT_OBJECT_0);
        CloseHandle(Threads[i]);
    }

    ok(HeapDestroy(Workers[0].Heap), "HeapDestroy failed with %lu\n", GetLastError());
}

/* Windows faults for these; ReactOS reports an error */
static void test_bad_parameters(void)
{
    PROCESS_HEAP_ENTRY Entry;
    RTL_ENTRY RtlEntry;
    HANDLE Heap;
    BOOL Ret;

    Heap = HeapCreate(0, 0, 0);
    ok(Heap != NULL, "HeapCreate failed with %lu\n", GetLastError());
    if (Heap == NULL)
        return;

    memset(&Entry, 0, sizeof(Entry));
    SetLastError(0xdeadbeef);
    Ret = HeapWalk(NULL, &Entry);
    ok(!Ret, "HeapWalk succeeded for no heap\n");
    ok_err(ERROR_INVALID_HANDLE);

    SetLastError(0xdeadbeef);
    Ret = HeapWalk((HANDLE)(ULONG_PTR)0x12340000, &Entry);
    ok(!Ret, "HeapWalk succeeded for a heap that is not one\n");
    ok_err(ERROR_INVALID_HANDLE);

    SetLastError(0xdeadbeef);
    Ret = HeapWalk(Heap, NULL);
    ok(!Ret, "HeapWalk succeeded without an entry\n");
    ok_err(ERROR_INVALID_PARAMETER);

    memset(&RtlEntry, 0, sizeof(RtlEntry));
    ok_hex(pRtlWalkHeap(NULL, &RtlEntry), STATUS_INVALID_HANDLE);
    ok_hex(pRtlWalkHeap(Heap, NULL), STATUS_INVALID_PARAMETER);

    /* The entry stands for a place that is not in the heap */
    memset(&Entry, 0, sizeof(Entry));
    Entry.lpData = (PVOID)(ULONG_PTR)0x1234560;
    Entry.wFlags = PROCESS_HEAP_ENTRY_BUSY;
    SetLastError(0xdeadbeef);
    Ret = HeapWalk(Heap, &Entry);
    ok(!Ret, "HeapWalk succeeded for a place that is not in the heap\n");
    ok_err(ERROR_INVALID_PARAMETER);

    ok(HeapDestroy(Heap), "HeapDestroy failed with %lu\n", GetLastError());

    /* A heap that is gone */
    memset(&Entry, 0, sizeof(Entry));
    SetLastError(0xdeadbeef);
    Ret = HeapWalk(Heap, &Entry);
    ok(!Ret, "HeapWalk succeeded for a heap that was destroyed\n");
    ok_err(ERROR_INVALID_HANDLE);
}

START_TEST(HeapWalk)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");

    pRtlWalkHeap = (PVOID)GetProcAddress(Ntdll, "RtlWalkHeap");
    pRtlAllocateHeap = (PVOID)GetProcAddress(Ntdll, "RtlAllocateHeap");
    pRtlSetUserValueHeap = (PVOID)GetProcAddress(Ntdll, "RtlSetUserValueHeap");
    pRtlSetUserFlagsHeap = (PVOID)GetProcAddress(Ntdll, "RtlSetUserFlagsHeap");
    ok(pRtlWalkHeap != NULL, "RtlWalkHeap is not exported\n");
    if (!pRtlWalkHeap)
        return;

    test_fresh_heap();
    test_blocks();
    test_zero_size();
    test_virtual_block();
    test_segments();
    test_user_info();
    test_free_under_cursor();
    test_locked_walk();

    if (IsReactOS())
    {
        test_unlocked_walk();
        test_bad_parameters();
    }
    else
    {
        skip("Windows faults for a bad parameter, and does not promise anything for a walk without the lock\n");
    }
}
