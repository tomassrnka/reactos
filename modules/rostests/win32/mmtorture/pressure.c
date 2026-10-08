/*
 * PROJECT:     ReactOS memory manager torture suite
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Workload (a): memory pressure. Private allocators, file
 *              mappings, copy-on-write views of files and pagefile-backed
 *              sections touch, verify and free memory; every page carries
 *              a pattern that is checked after it may have been paged out
 *              and in again
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "mmtorture.h"

#define MAX_BLOCKS 512

typedef enum _KIND
{
    KindPrivate,
    KindFile,
    KindCow,
    KindPagefile,
    KindMax
} KIND;

static const char *KindName[KindMax] = { "private", "file", "cow", "pagefile" };

typedef struct _BLOCK
{
    PUCHAR Base;
    SIZE_T Pages;
    ULONGLONG Seed;
    ULONGLONG CowSeed;
    HANDLE File;
    HANDLE Section;
} BLOCK;

typedef struct _PRESSURE_THREAD
{
    ULONG Index;
    SIZE_T TargetBytes;
    KIND Kind;
    LONG Slot;
} PRESSURE_THREAD;

static WCHAR Dir[MAX_PATH];
static volatile LONG FileCounter;

static BOOL
CheckRange(PUCHAR Base, SIZE_T FirstPage, SIZE_T Pages, ULONGLONG Seed, MMT_RNG *Rng, const char *What)
{
    SIZE_T i, Full = MmtRandRange(Rng, 0, (ULONG)Pages - 1);

    for (i = 0; i < Pages; i++)
    {
        if (!MmtCheckPage(Base + i * PAGE_SIZE, Seed, FirstPage + i, i == Full, What))
        {
            MmtFail("%s: view %p, first page %lu, %lu pages", What, Base, (ULONG)FirstPage, (ULONG)Pages);
            return FALSE;
        }
    }
    return TRUE;
}

static VOID
FillRange(PUCHAR Base, SIZE_T FirstPage, SIZE_T Pages, ULONGLONG Seed)
{
    SIZE_T i;

    for (i = 0; i < Pages; i++)
        MmtFillPage(Base + i * PAGE_SIZE, Seed, FirstPage + i);
}

static VOID
FreeBlock(BLOCK *B)
{
    if (B->Base && B->Section)
    {
        /* The long-lived copy-on-write view */
        if (!UnmapViewOfFile(B->Base))
            MmtFail("UnmapViewOfFile(%p) failed %lu", B->Base, GetLastError());
    }
    else if (B->Base)
    {
        if (!VirtualFree(B->Base, 0, MEM_RELEASE))
            MmtFail("VirtualFree(%p) failed %lu", B->Base, GetLastError());
    }
    if (B->Section)
        CloseHandle(B->Section);
    if (B->File)
        CloseHandle(B->File);
    ZeroMemory(B, sizeof(*B));
}

static BOOL
NewBlock(BLOCK *B, KIND Kind, MMT_RNG *Rng, LONG Slot)
{
    WCHAR Path[MAX_PATH];
    PUCHAR View;

    ZeroMemory(B, sizeof(*B));
    B->Seed = ((ULONGLONG)MmtRand(Rng) << 32) | MmtRand(Rng);
    if (Kind == KindPrivate)
    {
        B->Pages = MmtRandRange(Rng, 16, 1024);
        B->Base = VirtualAlloc(NULL, B->Pages * PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!B->Base)
        {
            B->Pages = 0;
            return FALSE;
        }
        FillRange(B->Base, 0, B->Pages, B->Seed);
        MmtProgress(Slot);
        return TRUE;
    }

    B->Pages = MmtRandRange(Rng, 256, Kind == KindCow ? 2048 : 4096);
    if (Kind != KindPagefile)
    {
        /* A file of its own, deleted when the last handle and section go */
        _snwprintf(Path, _countof(Path), L"%s\\pr-%lu-%ld.bin", Dir, GetCurrentProcessId(), InterlockedIncrement(&FileCounter));
        B->File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (B->File == INVALID_HANDLE_VALUE)
        {
            MmtLog("pressure: cannot create %S: %lu", Path, GetLastError());
            B->File = NULL;
            B->Pages = 0;
            return FALSE;
        }
    }
    B->Section = CreateFileMappingW(B->File ? B->File : INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                    (DWORD)(B->Pages * PAGE_SIZE), NULL);
    View = B->Section ? MapViewOfFile(B->Section, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    if (!View)
    {
        FreeBlock(B);
        return FALSE;
    }
    FillRange(View, 0, B->Pages, B->Seed);
    if (!UnmapViewOfFile(View))
        MmtFail("UnmapViewOfFile(%p) failed %lu", View, GetLastError());

    if (Kind == KindCow)
    {
        /* A copy-on-write view whose every page gets a private copy: those pages can only go to the paging file */
        B->CowSeed = B->Seed ^ 0xC0C0C0C0C0C0C0C0ULL;
        B->Base = MapViewOfFile(B->Section, FILE_MAP_COPY, 0, 0, 0);
        if (!B->Base)
        {
            MmtLog("pressure: copy-on-write view failed %lu", GetLastError());
            FreeBlock(B);
            return FALSE;
        }
        FillRange(B->Base, 0, B->Pages, B->CowSeed);
    }
    MmtProgress(Slot);
    return TRUE;
}

static VOID
VisitSectionBlock(BLOCK *B, MMT_RNG *Rng, LONG Slot, const char *What)
{
    PUCHAR View;
    SIZE_T Offset, Pages;
    ULONG Action = MmtRandRange(Rng, 0, 9);

    /* Map a 64 KB aligned window of the section */
    Offset = (MmtRandRange(Rng, 0, (ULONG)(B->Pages - 1)) * PAGE_SIZE) & ~(SIZE_T)0xFFFF;
    Pages = MmtRandRange(Rng, 16, 1024);
    Pages = min(B->Pages - Offset / PAGE_SIZE, Pages);
    View = MapViewOfFile(B->Section, Action < 7 ? FILE_MAP_READ : FILE_MAP_WRITE, 0, (DWORD)Offset, Pages * PAGE_SIZE);
    if (!View)
    {
        MmtLog("%s: MapViewOfFile window failed %lu", What, GetLastError());
        return;
    }
    CheckRange(View, Offset / PAGE_SIZE, Pages, B->Seed, Rng, What);
    if (Action >= 7)
    {
        /* Rewrite the window with the same pattern: the pages are dirty again */
        FillRange(View, Offset / PAGE_SIZE, Pages, B->Seed);
    }
    if (!UnmapViewOfFile(View))
        MmtFail("UnmapViewOfFile(%p) failed %lu", View, GetLastError());
    MmtProgress(Slot);
}

static VOID
VisitCowBlock(BLOCK *B, MMT_RNG *Rng, LONG Slot)
{
    SIZE_T First = MmtRandRange(Rng, 0, (ULONG)B->Pages - 1), Count;

    /* The private copies keep their contents; the file keeps its own */
    Count = MmtRandRange(Rng, 1, 256);
    Count = min(B->Pages - First, Count);
    CheckRange(B->Base + First * PAGE_SIZE, First, Count, B->CowSeed, Rng, "cow-private");
    if (MmtRandRange(Rng, 0, 3) == 0)
        VisitSectionBlock(B, Rng, Slot, "cow-file");
    MmtProgress(Slot);
}

static VOID
VisitPrivateBlock(BLOCK *B, MMT_RNG *Rng, LONG Slot)
{
    ULONG Action = MmtRandRange(Rng, 0, 9);
    SIZE_T First, Count, i;

    if (!CheckRange(B->Base, 0, B->Pages, B->Seed, Rng, "private"))
        return;
    if (Action == 0 && B->Pages > 4)
    {
        /* Decommit a range, commit it again: it must read back as zeros */
        First = MmtRandRange(Rng, 0, (ULONG)B->Pages - 2);
        Count = MmtRandRange(Rng, 1, (ULONG)(B->Pages - First));
        if (!VirtualFree(B->Base + First * PAGE_SIZE, Count * PAGE_SIZE, MEM_DECOMMIT))
        {
            MmtFail("decommit %p+%lu failed %lu", B->Base, (ULONG)First, GetLastError());
            return;
        }
        if (!VirtualAlloc(B->Base + First * PAGE_SIZE, Count * PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE))
        {
            /* Out of memory: give the whole block back */
            FreeBlock(B);
            return;
        }
        for (i = First; i < First + Count; i++)
        {
            if (!MmtIsZeroPage(B->Base + i * PAGE_SIZE))
            {
                MmtFail("recommitted page %p is not zero", B->Base + i * PAGE_SIZE);
                break;
            }
            MmtFillPage(B->Base + i * PAGE_SIZE, B->Seed, i);
        }
    }
    else if (Action == 1)
    {
        /* MEM_RESET leaves the contents undefined: write them again */
        First = MmtRandRange(Rng, 0, (ULONG)B->Pages - 1);
        Count = MmtRandRange(Rng, 1, (ULONG)(B->Pages - First));
        if (!VirtualAlloc(B->Base + First * PAGE_SIZE, Count * PAGE_SIZE, MEM_RESET, PAGE_READWRITE))
            MmtFail("MEM_RESET %p+%lu failed %lu", B->Base, (ULONG)First, GetLastError());
        FillRange(B->Base + First * PAGE_SIZE, First, Count, B->Seed);
    }
    MmtProgress(Slot);
}

static VOID
VisitBlock(BLOCK *B, KIND Kind, MMT_RNG *Rng, LONG Slot)
{
    switch (Kind)
    {
        case KindPrivate: VisitPrivateBlock(B, Rng, Slot); break;
        case KindCow: VisitCowBlock(B, Rng, Slot); break;
        default: VisitSectionBlock(B, Rng, Slot, KindName[Kind]); break;
    }
}

static DWORD WINAPI
PressureThread(PVOID Context)
{
    PRESSURE_THREAD *T = Context;
    static BLOCK BlocksArray[32][MAX_BLOCKS];
    BLOCK *Blocks = BlocksArray[T->Index % 32];
    MMT_RNG Rng;
    SIZE_T Used = 0;
    ULONG i, Failures = 0;

    MmtRngInit(&Rng, GetTickCount() ^ (T->Index * 7919) ^ (GetCurrentProcessId() << 16));
    while (!MmtShouldStop())
    {
        ULONG Pick = MmtRandRange(&Rng, 0, MAX_BLOCKS - 1);
        BLOCK *B = &Blocks[Pick];
        SIZE_T Pages;

        if (B->Pages == 0)
        {
            if (Used >= T->TargetBytes)
                continue;
            if (NewBlock(B, T->Kind, &Rng, T->Slot))
            {
                Used += B->Pages * PAGE_SIZE;
                Failures = 0;
            }
            else if (++Failures % 64 == 1)
            {
                MmtLog("pressure: %s allocation failed (%lu): %lu, used %lu MB", KindName[T->Kind], Failures,
                       GetLastError(), (ULONG)(Used >> 20));
                Sleep(200);
            }
            continue;
        }

        Pages = B->Pages;
        if (Used >= T->TargetBytes && MmtRandRange(&Rng, 0, 3) == 0)
        {
            /* Verify one last time and give it back */
            VisitBlock(B, T->Kind, &Rng, T->Slot);
            if (B->Pages)
                FreeBlock(B);
            Used -= Pages * PAGE_SIZE;
            MmtProgress(T->Slot);
            continue;
        }
        VisitBlock(B, T->Kind, &Rng, T->Slot);
        if (B->Pages == 0)
            Used -= Pages * PAGE_SIZE;
    }

    for (i = 0; i < MAX_BLOCKS; i++)
    {
        if (Blocks[i].Pages)
            FreeBlock(&Blocks[i]);
    }
    return 0;
}

static DWORD WINAPI
HeapThread(PVOID Context)
{
    LONG Slot = (LONG)(ULONG_PTR)Context;
    HANDLE Heap = HeapCreate(0, 0, 0);
    PVOID Blocks[1024] = {0};
    SIZE_T Sizes[1024] = {0};
    MMT_RNG Rng;
    ULONG n = 0;

    MmtRngInit(&Rng, GetTickCount() ^ 0xC0FFEE ^ GetCurrentProcessId());
    while (!MmtShouldStop())
    {
        ULONG i = MmtRandRange(&Rng, 0, 1023);
        if (Blocks[i])
        {
            PUCHAR P = Blocks[i];
            if (P[0] != (UCHAR)i || P[Sizes[i] - 1] != (UCHAR)(i ^ 0x5A))
                MmtFail("heap block %p of %lu bytes damaged", P, (ULONG)Sizes[i]);
            HeapFree(Heap, 0, Blocks[i]);
            Blocks[i] = NULL;
        }
        else
        {
            Sizes[i] = MmtRandRange(&Rng, 2, (MmtRand(&Rng) & 7) ? 4096 : 600 * 1024);
            Blocks[i] = HeapAlloc(Heap, 0, Sizes[i]);
            if (Blocks[i])
            {
                PUCHAR P = Blocks[i];
                memset(P, 0x11, Sizes[i]);
                P[0] = (UCHAR)i;
                P[Sizes[i] - 1] = (UCHAR)(i ^ 0x5A);
            }
        }
        if ((++n & 255) == 0)
            MmtProgress(Slot);
    }
    HeapDestroy(Heap);
    return 0;
}

/* pressure PRIVATE_MB FILE_MB THREADS [COW_MB] [DIR] [PAGEFILE_SECTION_MB] */
int
MmtPressureMain(int argc, char **argv)
{
    ULONG Mb[KindMax], Threads = MmtArgUlong(argc, argv, 4, 4), i, k, Count = 0, Kinds = 0;
    ULONG PerKind[KindMax] = {0};
    PRESSURE_THREAD Context[32];
    HANDLE Handles[33];
    LONG Slot;

    Mb[KindPrivate] = MmtArgUlong(argc, argv, 2, 32);
    Mb[KindFile] = MmtArgUlong(argc, argv, 3, 128);
    Mb[KindCow] = MmtArgUlong(argc, argv, 5, 0);
    Mb[KindPagefile] = MmtArgUlong(argc, argv, 7, 0);
    MultiByteToWideChar(CP_ACP, 0, argc > 6 ? argv[6] : "C:\\mmt", -1, Dir, _countof(Dir));
    CreateDirectoryW(Dir, NULL);
    MmtOpenShared(FALSE);
    for (k = 0; k < KindMax; k++)
        Kinds += Mb[k] != 0;
    if (!Kinds)
        return 2;
    Threads = max(Kinds, min(Threads, 32));
    MmtLog("pressure private=%lu file=%lu cow=%lu pagefile=%lu MB threads=%lu dir=%S",
           Mb[KindPrivate], Mb[KindFile], Mb[KindCow], Mb[KindPagefile], Threads, Dir);
    Slot = MmtAllocSlot("pressure");

    /* Threads go round-robin over the kinds that have a size */
    for (i = 0, k = 0; i < Threads; i++)
    {
        while (!Mb[k % KindMax])
            k++;
        Context[i].Kind = (KIND)(k % KindMax);
        PerKind[Context[i].Kind]++;
        k++;
    }
    for (i = 0; i < Threads; i++)
    {
        Context[i].Index = i;
        Context[i].Slot = Slot;
        Context[i].TargetBytes = ((SIZE_T)Mb[Context[i].Kind] << 20) / PerKind[Context[i].Kind];
        Handles[Count] = CreateThread(NULL, 0, PressureThread, &Context[i], 0, NULL);
        if (Handles[Count])
            Count++;
    }
    Handles[Count] = CreateThread(NULL, 0, HeapThread, (PVOID)(ULONG_PTR)Slot, 0, NULL);
    if (Handles[Count])
        Count++;
    WaitForMultipleObjects(Count, Handles, TRUE, INFINITE);
    MmtLog("pressure done");
    return 0;
}
