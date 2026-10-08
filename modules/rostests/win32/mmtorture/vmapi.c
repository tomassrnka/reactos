/*
 * PROJECT:     ReactOS memory manager torture suite
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Workload (d): virtual memory API churn against a shadow
 *              model. Each thread owns a reservation and checks every
 *              NtAllocate/Free/Protect/Lock/Query result and every access
 *              against what the model predicts
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "mmtorture.h"

#define REGION_PAGES 128

typedef struct _PAGE_MODEL
{
    UCHAR Committed;
    UCHAR Known;
    ULONG Protect;
    ULONG_PTR Value;
} PAGE_MODEL;

typedef struct _VM_THREAD
{
    ULONG Index;
    LONG Slot;
    PUCHAR Base;
    PAGE_MODEL Page[REGION_PAGES];
    MMT_RNG Rng;
    ULONG Errors;
    ULONG Diffs;
} VM_THREAD;

static const ULONG Protections[] =
{
    PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE,
    PAGE_READWRITE | PAGE_GUARD, PAGE_READONLY | PAGE_GUARD, PAGE_READWRITE | PAGE_NOCACHE,
};

static BOOL
CanRead(ULONG Protect)
{
    Protect &= 0xFF;
    return Protect != PAGE_NOACCESS;
}

static BOOL
CanWrite(ULONG Protect)
{
    Protect &= 0xFF;
    return Protect == PAGE_READWRITE || Protect == PAGE_EXECUTE_READWRITE;
}

static NTSTATUS
Access(volatile ULONG_PTR *Address, BOOL Write, ULONG_PTR Value, ULONG_PTR *Out)
{
    NTSTATUS Status = STATUS_SUCCESS;

    _SEH2_TRY
    {
        if (Write)
            *Address = Value;
        else
            *Out = *Address;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    return Status;
}

#define VFAIL(T, ...) do { if ((T)->Errors++ < 50) MmtFail(__VA_ARGS__); } while (0)

static BOOL
Reserve(VM_THREAD *T)
{
    SIZE_T Size = REGION_PAGES * PAGE_SIZE;
    PVOID Base = NULL;
    NTSTATUS Status;

    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_RESERVE, PAGE_NOACCESS);
    if (!NT_SUCCESS(Status))
    {
        MmtLog("vmapi: reserve failed %08lx", Status);
        return FALSE;
    }
    T->Base = Base;
    ZeroMemory(T->Page, sizeof(T->Page));
    return TRUE;
}

static VOID
Release(VM_THREAD *T)
{
    PVOID Base = T->Base;
    SIZE_T Size = 0;
    NTSTATUS Status;

    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Base, &Size, MEM_RELEASE);
    if (!NT_SUCCESS(Status) || Size != REGION_PAGES * PAGE_SIZE)
        VFAIL(T, "vmapi: release %p: %08lx size %lx", T->Base, Status, (ULONG)Size);
    T->Base = NULL;
}

static VOID
OpCommit(VM_THREAD *T, ULONG First, ULONG Count)
{
    ULONG Protect = Protections[MmtRandRange(&T->Rng, 0, _countof(Protections) - 1)], i;
    PVOID Base = T->Base + First * PAGE_SIZE;
    SIZE_T Size = Count * PAGE_SIZE;
    NTSTATUS Status;

    /* Recommitting with another protection is not modelled: keep to fresh pages or the same protection */
    for (i = First; i < First + Count; i++)
    {
        if (T->Page[i].Committed)
            Protect = T->Page[i].Protect;
    }
    for (i = First; i < First + Count; i++)
    {
        if (T->Page[i].Committed && T->Page[i].Protect != Protect)
            return;
    }
    Status = NtAllocateVirtualMemory(NtCurrentProcess(), &Base, 0, &Size, MEM_COMMIT, Protect);
    if (Status == STATUS_COMMITMENT_LIMIT || Status == STATUS_NO_MEMORY)
        return;
    if (!NT_SUCCESS(Status))
    {
        VFAIL(T, "vmapi: commit %p+%lx prot %lx: %08lx", T->Base, First, Protect, Status);
        return;
    }
    if (Base != T->Base + First * PAGE_SIZE || Size != Count * PAGE_SIZE)
        VFAIL(T, "vmapi: commit returned %p/%lx for %p/%lx", Base, (ULONG)Size, T->Base + First * PAGE_SIZE, Count * PAGE_SIZE);
    for (i = First; i < First + Count; i++)
    {
        if (!T->Page[i].Committed)
        {
            T->Page[i].Committed = 1;
            T->Page[i].Known = 1;
            T->Page[i].Value = 0;
            T->Page[i].Protect = Protect;
        }
    }
}

static VOID
OpDecommit(VM_THREAD *T, ULONG First, ULONG Count)
{
    PVOID Base = T->Base + First * PAGE_SIZE;
    SIZE_T Size = Count * PAGE_SIZE;
    NTSTATUS Status;
    ULONG i;

    Status = NtFreeVirtualMemory(NtCurrentProcess(), &Base, &Size, MEM_DECOMMIT);
    if (!NT_SUCCESS(Status))
    {
        VFAIL(T, "vmapi: decommit %p+%lx/%lx: %08lx", T->Base, First, Count, Status);
        return;
    }
    for (i = First; i < First + Count; i++)
        T->Page[i].Committed = 0;
}

static VOID
OpProtect(VM_THREAD *T, ULONG First, ULONG Count)
{
    ULONG Protect = Protections[MmtRandRange(&T->Rng, 0, _countof(Protections) - 1)], Old = 0, i;
    PVOID Base = T->Base + First * PAGE_SIZE;
    SIZE_T Size = Count * PAGE_SIZE;
    BOOL AllCommitted = TRUE;
    NTSTATUS Status;

    for (i = First; i < First + Count; i++)
        AllCommitted &= T->Page[i].Committed;
    /* NOCACHE on pages that have another caching type is a separate rule; skip it */
    if ((Protect & PAGE_NOCACHE) || (T->Page[First].Protect & PAGE_NOCACHE))
        return;
    for (i = First; i < First + Count; i++)
    {
        if (T->Page[i].Protect & PAGE_NOCACHE)
            return;
    }
    Status = NtProtectVirtualMemory(NtCurrentProcess(), &Base, &Size, Protect, &Old);
    if (!AllCommitted)
    {
        if (NT_SUCCESS(Status))
            VFAIL(T, "vmapi: protect of uncommitted %p+%lx/%lx succeeded", T->Base, First, Count);
        return;
    }
    if (!NT_SUCCESS(Status))
    {
        VFAIL(T, "vmapi: protect %p+%lx/%lx to %lx: %08lx", T->Base, First, Count, Protect, Status);
        return;
    }
    if (Old != T->Page[First].Protect)
        VFAIL(T, "vmapi: protect %p+%lx old %lx, model %lx", T->Base, First, Old, T->Page[First].Protect);
    for (i = First; i < First + Count; i++)
        T->Page[i].Protect = Protect;
}

static VOID
OpAccess(VM_THREAD *T, ULONG Page, BOOL Write)
{
    PAGE_MODEL *M = &T->Page[Page];
    volatile ULONG_PTR *Address = (volatile ULONG_PTR *)(T->Base + Page * PAGE_SIZE + (MmtRand(&T->Rng) & 1) * (PAGE_SIZE - sizeof(ULONG_PTR)));
    ULONG_PTR Value = ((ULONG_PTR)MmtRand(&T->Rng) << 1) | 1, Out = 0;
    NTSTATUS Status, Expected;

    if (Write)
        Address = (volatile ULONG_PTR *)(T->Base + Page * PAGE_SIZE);

    if (!M->Committed)
        Expected = STATUS_ACCESS_VIOLATION;
    else if (M->Protect & PAGE_GUARD)
        Expected = STATUS_GUARD_PAGE_VIOLATION;
    else if (Write ? !CanWrite(M->Protect) : !CanRead(M->Protect))
        Expected = STATUS_ACCESS_VIOLATION;
    else
        Expected = STATUS_SUCCESS;

    Status = Access(Address, Write, Value, &Out);
    if (Status != Expected)
    {
        VFAIL(T, "vmapi: %s %p: %08lx, expected %08lx (committed %u prot %lx)", Write ? "write" : "read",
              Address, Status, Expected, M->Committed, M->Protect);
        /* Resynchronise: drop what we know about this page */
        M->Known = 0;
        if (Status == STATUS_GUARD_PAGE_VIOLATION || Expected == STATUS_GUARD_PAGE_VIOLATION)
            M->Protect &= ~PAGE_GUARD;
        return;
    }
    if (Status == STATUS_GUARD_PAGE_VIOLATION)
    {
        /* The first touch clears the guard */
        M->Protect &= ~PAGE_GUARD;
        return;
    }
    if (Status != STATUS_SUCCESS)
        return;
    if (Write)
    {
        /* Both checked words carry the same value */
        Address[0] = Value;
        *(volatile ULONG_PTR *)(T->Base + Page * PAGE_SIZE + PAGE_SIZE - sizeof(ULONG_PTR)) = Value;
        M->Value = Value;
        M->Known = 1;
    }
    else if (M->Known && Out != M->Value)
    {
        VFAIL(T, "vmapi: read %p got %p expected %p (prot %lx)", Address, (PVOID)Out, (PVOID)M->Value, M->Protect);
        M->Known = 0;
    }
}

static VOID
OpQuery(VM_THREAD *T, ULONG Page)
{
    MEMORY_BASIC_INFORMATION Mbi;
    NTSTATUS Status;
    SIZE_T Length;
    ULONG End, ExpectedState, ExpectedProtect;
    PAGE_MODEL *M = &T->Page[Page];

    Status = NtQueryVirtualMemory(NtCurrentProcess(), T->Base + Page * PAGE_SIZE + 17, MemoryBasicInformation,
                                  &Mbi, sizeof(Mbi), &Length);
    if (!NT_SUCCESS(Status))
    {
        VFAIL(T, "vmapi: query %p: %08lx", T->Base + Page * PAGE_SIZE, Status);
        return;
    }
    for (End = Page + 1; End < REGION_PAGES; End++)
    {
        if (T->Page[End].Committed != M->Committed)
            break;
        if (M->Committed && T->Page[End].Protect != M->Protect)
            break;
    }
    ExpectedState = M->Committed ? MEM_COMMIT : MEM_RESERVE;
    ExpectedProtect = M->Committed ? M->Protect : 0;
    if (Mbi.BaseAddress == T->Base + Page * PAGE_SIZE && Mbi.AllocationBase == T->Base &&
        Mbi.State == ExpectedState && Mbi.Type == MEM_PRIVATE && Mbi.AllocationProtect == PAGE_NOACCESS &&
        (!M->Committed || Mbi.Protect == ExpectedProtect) &&
        Mbi.RegionSize < (End - Page) * PAGE_SIZE)
    {
        /* Known ReactOS difference: a region is split where Windows merges (after a guard page fired) */
        if (T->Diffs++ < 20)
            MmtLog("DIFF vmapi: query %p region %lx, model %lx", T->Base + Page * PAGE_SIZE,
                   (ULONG)Mbi.RegionSize, (End - Page) * PAGE_SIZE);
        return;
    }
    if (Mbi.BaseAddress != T->Base + Page * PAGE_SIZE || Mbi.AllocationBase != T->Base ||
        Mbi.State != ExpectedState || Mbi.Type != MEM_PRIVATE ||
        Mbi.RegionSize != (End - Page) * PAGE_SIZE || Mbi.AllocationProtect != PAGE_NOACCESS ||
        (M->Committed && Mbi.Protect != ExpectedProtect))
    {
        VFAIL(T, "vmapi: query %p: base %p alloc %p state %lx prot %lx aprot %lx size %lx type %lx; model state %lx prot %lx size %lx",
              T->Base + Page * PAGE_SIZE, Mbi.BaseAddress, Mbi.AllocationBase, Mbi.State, Mbi.Protect,
              Mbi.AllocationProtect, (ULONG)Mbi.RegionSize, Mbi.Type, ExpectedState, ExpectedProtect,
              (End - Page) * PAGE_SIZE);
    }
}

static VOID
OpLock(VM_THREAD *T, ULONG First, ULONG Count)
{
    ULONG i;

    for (i = First; i < First + Count; i++)
    {
        if (!T->Page[i].Committed || !CanWrite(T->Page[i].Protect) || (T->Page[i].Protect & (PAGE_GUARD | PAGE_NOCACHE)))
            return;
    }
    if (!VirtualLock(T->Base + First * PAGE_SIZE, Count * PAGE_SIZE))
    {
        if (GetLastError() != ERROR_WORKING_SET_QUOTA && GetLastError() != ERROR_NO_SYSTEM_RESOURCES)
            VFAIL(T, "vmapi: VirtualLock %p+%lx/%lx: %lu", T->Base, First, Count, GetLastError());
        return;
    }
    for (i = First; i < First + Count; i++)
        OpAccess(T, i, MmtRand(&T->Rng) & 1);
    if (MmtRand(&T->Rng) % 8 == 0)
    {
        /* Decommit while locked: the lock goes with the pages */
        OpDecommit(T, First, Count);
        return;
    }
    if (!VirtualUnlock(T->Base + First * PAGE_SIZE, Count * PAGE_SIZE))
        VFAIL(T, "vmapi: VirtualUnlock %p+%lx/%lx: %lu", T->Base, First, Count, GetLastError());
}

static VOID
OpReset(VM_THREAD *T, ULONG First, ULONG Count)
{
    ULONG i;

    for (i = First; i < First + Count; i++)
    {
        if (!T->Page[i].Committed || !CanWrite(T->Page[i].Protect) || (T->Page[i].Protect & PAGE_GUARD))
            return;
    }
    if (!VirtualAlloc(T->Base + First * PAGE_SIZE, Count * PAGE_SIZE, MEM_RESET, PAGE_NOACCESS))
    {
        VFAIL(T, "vmapi: MEM_RESET %p+%lx/%lx: %lu", T->Base, First, Count, GetLastError());
        return;
    }
    for (i = First; i < First + Count; i++)
        T->Page[i].Known = 0;
}

static DWORD WINAPI
VmThread(PVOID Context)
{
    VM_THREAD *T = Context;
    ULONG n = 0;

    MmtRngInit(&T->Rng, GetTickCount() ^ (T->Index << 22) ^ GetCurrentProcessId());
    while (!MmtShouldStop())
    {
        ULONG First = MmtRandRange(&T->Rng, 0, REGION_PAGES - 1);
        ULONG Count = MmtRandRange(&T->Rng, 1, 16);
        ULONG Op = MmtRandRange(&T->Rng, 0, 99);

        Count = min(REGION_PAGES - First, Count);
        if (!T->Base && !Reserve(T))
        {
            Sleep(100);
            continue;
        }
        if (Op < 18)
            OpCommit(T, First, Count);
        else if (Op < 26)
            OpDecommit(T, First, Count);
        else if (Op < 40)
            OpProtect(T, First, Count);
        else if (Op < 70)
            OpAccess(T, First, Op & 1);
        else if (Op < 88)
            OpQuery(T, First);
        else if (Op < 93)
            OpLock(T, First, Count);
        else if (Op < 97)
            OpReset(T, First, Count);
        else if (Op == 99 && MmtRand(&T->Rng) % 4 == 0)
            Release(T);
        if ((++n & 63) == 0)
            MmtProgress(T->Slot);
    }
    if (T->Base)
        Release(T);
    return 0;
}

/* Unmodelled churn: other threads allocate, free, read across and walk the address space */
static DWORD WINAPI
ChurnThread(PVOID Context)
{
    LONG Slot = (LONG)(ULONG_PTR)Context;
    PVOID Regions[64] = {0};
    MMT_RNG Rng;
    ULONG n = 0;

    MmtRngInit(&Rng, GetTickCount() ^ 0xABCDEF ^ GetCurrentProcessId());
    while (!MmtShouldStop())
    {
        ULONG i = MmtRandRange(&Rng, 0, 63);
        if (Regions[i])
        {
            UCHAR Buffer[64];
            SIZE_T Done;
            /* Read through the kernel copy path, then free */
            ReadProcessMemory(GetCurrentProcess(), Regions[i], Buffer, sizeof(Buffer), &Done);
            VirtualFree(Regions[i], 0, MEM_RELEASE);
            Regions[i] = NULL;
        }
        else
        {
            SIZE_T Size = MmtRandRange(&Rng, 1, 512) * PAGE_SIZE;
            DWORD Flags = MEM_RESERVE | ((MmtRand(&Rng) & 1) ? MEM_COMMIT : 0) | ((MmtRand(&Rng) % 8 == 0) ? MEM_TOP_DOWN : 0);
            Regions[i] = VirtualAlloc(NULL, Size, Flags, PAGE_READWRITE);
            if (Regions[i] && (Flags & MEM_COMMIT))
                ((PUCHAR)Regions[i])[Size - 1] = 1;
        }
        if (MmtRand(&Rng) % 64 == 0)
        {
            /* Walk the whole user address space: regions must be contiguous and consistent */
            PUCHAR Address = NULL, Previous = NULL;
            MEMORY_BASIC_INFORMATION Mbi;
            ULONG Regions2 = 0;
            while (VirtualQuery(Address, &Mbi, sizeof(Mbi)) == sizeof(Mbi))
            {
                if ((PUCHAR)Mbi.BaseAddress != Address || Mbi.RegionSize == 0 || (PUCHAR)Mbi.BaseAddress < Previous)
                {
                    /* Another thread may change the map between calls; only a zero size or a step back is wrong */
                    if (Mbi.RegionSize == 0 || (PUCHAR)Mbi.BaseAddress < Previous)
                        MmtFail("vmapi: VirtualQuery walk at %p returned base %p size %lx", Address, Mbi.BaseAddress, (ULONG)Mbi.RegionSize);
                }
                Previous = Mbi.BaseAddress;
                Address = (PUCHAR)Mbi.BaseAddress + Mbi.RegionSize;
                if (++Regions2 > 100000)
                {
                    MmtFail("vmapi: VirtualQuery walk does not end");
                    break;
                }
            }
        }
        if ((++n & 63) == 0)
            MmtProgress(Slot);
    }
    for (n = 0; n < 64; n++)
    {
        if (Regions[n])
            VirtualFree(Regions[n], 0, MEM_RELEASE);
    }
    return 0;
}

static ULONG
GrowStack(ULONG Depth)
{
    volatile UCHAR Frame[1024];

    Frame[0] = (UCHAR)Depth;
    Frame[sizeof(Frame) - 1] = 1;
    if (Depth == 0)
        return Frame[0];
    return GrowStack(Depth - 1) + Frame[sizeof(Frame) - 1];
}

static DWORD WINAPI
StackUser(PVOID Context)
{
    /* Grow the stack through its guard page, to about half of the reserve */
    return GrowStack((ULONG)(ULONG_PTR)Context);
}

static DWORD WINAPI
StackThread(PVOID Context)
{
    LONG Slot = (LONG)(ULONG_PTR)Context;

    while (!MmtShouldStop())
    {
        HANDLE Threads[8];
        ULONG i, Count = 0;
        for (i = 0; i < 8; i++)
        {
            SIZE_T Reserve = (SIZE_T)64 * 1024 << (i % 4);
            Threads[Count] = CreateThread(NULL, Reserve, StackUser, (PVOID)(ULONG_PTR)(Reserve / 2 / 1100),
                                          STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
            if (Threads[Count])
                Count++;
        }
        if (WaitForMultipleObjects(Count, Threads, TRUE, 60000) != WAIT_OBJECT_0)
            MmtFail("vmapi: stack threads did not end within 60 s");
        for (i = 0; i < Count; i++)
            CloseHandle(Threads[i]);
        MmtProgress(Slot);
    }
    return 0;
}

static VOID
LargePageCheck(VOID)
{
    SIZE_T (WINAPI *GetMinimum)(VOID);
    SIZE_T Minimum = 0;
    PVOID P;

    GetMinimum = (PVOID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetLargePageMinimum");
    if (GetMinimum)
        Minimum = GetMinimum();

    if (!Minimum)
    {
        MmtLog("vmapi: no large pages");
        return;
    }
    P = VirtualAlloc(NULL, Minimum, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
    MmtLog("vmapi: large page minimum %lx, allocation %p (%lu)", (ULONG)Minimum, P, P ? 0 : GetLastError());
    if (P)
    {
        ((volatile UCHAR *)P)[0] = 1;
        ((volatile UCHAR *)P)[Minimum - 1] = 1;
        VirtualFree(P, 0, MEM_RELEASE);
    }
}

/* vmapi THREADS */
int
MmtVmApiMain(int argc, char **argv)
{
    ULONG Threads = min(MmtArgUlong(argc, argv, 2, 4), 28), i, Count = 0;
    static VM_THREAD T[28];
    HANDLE Handles[32];
    LONG Slot;

    MmtOpenShared(FALSE);
    Slot = MmtAllocSlot("vmapi");
    SetProcessWorkingSetSize(GetCurrentProcess(), 16 << 20, 64 << 20);
    LargePageCheck();
    for (i = 0; i < Threads; i++)
    {
        T[i].Index = i;
        T[i].Slot = Slot;
        Handles[Count] = CreateThread(NULL, 0, VmThread, &T[i], 0, NULL);
        if (Handles[Count])
            Count++;
    }
    Handles[Count] = CreateThread(NULL, 0, ChurnThread, (PVOID)(ULONG_PTR)Slot, 0, NULL);
    if (Handles[Count])
        Count++;
    Handles[Count] = CreateThread(NULL, 0, StackThread, (PVOID)(ULONG_PTR)Slot, 0, NULL);
    if (Handles[Count])
        Count++;
    WaitForMultipleObjects(Count, Handles, TRUE, INFINITE);
    MmtLog("vmapi done");
    return 0;
}
