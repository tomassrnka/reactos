/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for heaps that check the tail of their blocks
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static const SIZE_T Sizes[] = { 1, 7, 8, 9, 15, 16, 17, 24, 0x28, 33, 64, 100, 0x98, 1000, 4000 };

static VOID
TestHeap(_In_ ULONG HeapFlags, _In_ PCSTR Name)
{
    PUCHAR Blocks[ARRAYSIZE(Sizes)];
    PUCHAR NewBlock;
    HANDLE Heap;
    SIZE_T i, j;
    BOOLEAN Ok;

    Heap = RtlCreateHeap(HeapFlags | HEAP_GROWABLE, NULL, 0, 0, NULL, NULL);
    ok(Heap != NULL, "%s: RtlCreateHeap failed\n", Name);
    if (!Heap)
        return;

    for (i = 0; i < ARRAYSIZE(Sizes); i++)
    {
        Blocks[i] = RtlAllocateHeap(Heap, 0, Sizes[i]);
        ok(Blocks[i] != NULL, "%s: allocation of %Iu bytes failed\n", Name, Sizes[i]);
        if (Blocks[i])
            RtlFillMemory(Blocks[i], Sizes[i], (UCHAR)(i + 1));
    }

    /* A block written only within its size is valid, and so is the heap */
    for (i = 0; i < ARRAYSIZE(Sizes); i++)
    {
        if (Blocks[i])
            ok(RtlValidateHeap(Heap, 0, Blocks[i]), "%s: block of %Iu bytes is not valid\n", Name, Sizes[i]);
    }
    ok(RtlValidateHeap(Heap, 0, NULL), "%s: heap is not valid\n", Name);

    /* Such blocks can be reallocated, keep their data, and be freed */
    for (i = 0; i < ARRAYSIZE(Sizes); i++)
    {
        if (!Blocks[i])
            continue;
        NewBlock = RtlReAllocateHeap(Heap, 0, Blocks[i], Sizes[i] + 13);
        ok(NewBlock != NULL, "%s: reallocation of %Iu bytes failed\n", Name, Sizes[i]);
        if (!NewBlock)
            continue;
        Blocks[i] = NewBlock;
        for (j = 0, Ok = TRUE; j < Sizes[i]; j++)
            Ok = Ok && NewBlock[j] == (UCHAR)(i + 1);
        ok(Ok, "%s: reallocation of %Iu bytes lost the data\n", Name, Sizes[i]);
        RtlFillMemory(NewBlock, Sizes[i] + 13, (UCHAR)(i + 1));
        ok(RtlValidateHeap(Heap, 0, NewBlock), "%s: reallocated block of %Iu bytes is not valid\n", Name, Sizes[i]);
    }
    ok(RtlValidateHeap(Heap, 0, NULL), "%s: heap is not valid after the reallocations\n", Name);

    for (i = 0; i < ARRAYSIZE(Sizes); i++)
    {
        if (Blocks[i])
            ok(RtlFreeHeap(Heap, 0, Blocks[i]), "%s: freeing the block of %Iu bytes failed\n", Name, Sizes[i]);
    }
    ok(RtlValidateHeap(Heap, 0, NULL), "%s: heap is not valid after the frees\n", Name);

    /* A write just past the size is caught (Windows may end the process on a corruption it detects) */
    if (is_reactos())
    {
        NewBlock = RtlAllocateHeap(Heap, 0, 0x28);
        ok(NewBlock != NULL, "%s: allocation failed\n", Name);
        if (NewBlock)
        {
            NewBlock[0x28] = 0;
            ok(!RtlValidateHeap(Heap, 0, NewBlock), "%s: a block written past its size is valid\n", Name);
        }
    }

    RtlDestroyHeap(Heap);
}

START_TEST(RtlHeapTailCheck)
{
    TestHeap(HEAP_TAIL_CHECKING_ENABLED, "tail");
    TestHeap(HEAP_TAIL_CHECKING_ENABLED | HEAP_FREE_CHECKING_ENABLED, "tail+free");
}
