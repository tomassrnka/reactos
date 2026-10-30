/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     LsaLookupSids/LsaLookupNames2 return each result as one heap block that LsaFreeMemory frees whole
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ndk/rtlfuncs.h>
#include <ndk/umtypes.h>
#include <ntsecapi.h>

#define LOOKUP_ITERATIONS   10000
#define WARMUP_ITERATIONS   200

static SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
static SID_IDENTIFIER_AUTHORITY WorldAuthority = {SECURITY_WORLD_SID_AUTHORITY};

typedef struct _HEAP_TOTALS
{
    SIZE_T Blocks;
    SIZE_T Bytes;
} HEAP_TOTALS;

/* Busy blocks and bytes in the process heap */
static void SnapshotHeap(HEAP_TOTALS *Totals)
{
    HANDLE Heap = GetProcessHeap();
    PROCESS_HEAP_ENTRY Entry;

    Totals->Blocks = 0;
    Totals->Bytes = 0;
    HeapLock(Heap);
    Entry.lpData = NULL;
    while (HeapWalk(Heap, &Entry))
    {
        if ((Entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) && !(Entry.wFlags & PROCESS_HEAP_REGION))
        {
            Totals->Blocks++;
            Totals->Bytes += Entry.cbData;
        }
    }
    HeapUnlock(Heap);
}

static BOOL InBlock(PVOID Block, SIZE_T BlockSize, PVOID Pointer, SIZE_T Length)
{
    PUCHAR Start = Block, P = Pointer;

    return P >= Start && Length <= BlockSize && (SIZE_T)(P - Start) <= BlockSize - Length;
}

/* The list is one block, in Windows order (header, trust array, data), and its size covers all of it */
static void CheckDomainList(PLSA_REFERENCED_DOMAIN_LIST List, PCSTR What)
{
    SIZE_T Size, i;

    if (List == NULL)
        return;

    ok(HeapValidate(GetProcessHeap(), 0, List), "%s: list %p is not a heap block\n", What, List);
    Size = HeapSize(GetProcessHeap(), 0, List);
    ok(Size != (SIZE_T)-1, "%s: HeapSize failed\n", What);
    if (List->Entries == 0)
        return;

    ok(List->Domains != NULL, "%s: no trust array\n", What);
    if (List->Domains == NULL)
        return;
    ok((PUCHAR)List->Domains >= (PUCHAR)(List + 1), "%s: trust array %p below the header %p\n", What, List->Domains, List);
    ok(InBlock(List, Size, List->Domains, List->Entries * sizeof(LSA_TRUST_INFORMATION)),
       "%s: trust array %p is outside the %Iu byte list at %p\n", What, List->Domains, Size, List);
    for (i = 0; i < List->Entries; i++)
    {
        PLSA_TRUST_INFORMATION Trust = &List->Domains[i];

        if (Trust->Name.Buffer != NULL)
            ok(InBlock(List, Size, Trust->Name.Buffer, Trust->Name.MaximumLength),
               "%s: domain %Iu name %p is outside the list\n", What, i, Trust->Name.Buffer);
        if (Trust->Sid != NULL)
            ok(InBlock(List, Size, Trust->Sid, RtlLengthSid(Trust->Sid)),
               "%s: domain %Iu SID %p is outside the list\n", What, i, Trust->Sid);
    }
}

static void CheckNames(PLSA_TRANSLATED_NAME Names, ULONG Count, PCSTR What)
{
    SIZE_T Size, i;

    if (Names == NULL)
        return;
    ok(HeapValidate(GetProcessHeap(), 0, Names), "%s: names %p is not a heap block\n", What, Names);
    Size = HeapSize(GetProcessHeap(), 0, Names);
    ok(InBlock(Names, Size, Names, Count * sizeof(*Names)), "%s: HeapSize %Iu too small for the array\n", What, Size);
    for (i = 0; i < Count; i++)
    {
        if (Names[i].Name.Buffer != NULL)
            ok(InBlock(Names, Size, Names[i].Name.Buffer, Names[i].Name.MaximumLength),
               "%s: name %Iu buffer %p is outside the %Iu byte block at %p\n", What, i, Names[i].Name.Buffer, Size, Names);
    }
}

static void CheckSids2(PLSA_TRANSLATED_SID2 Sids, ULONG Count, PCSTR What)
{
    SIZE_T Size, i;

    if (Sids == NULL)
        return;
    ok(HeapValidate(GetProcessHeap(), 0, Sids), "%s: SIDs %p is not a heap block\n", What, Sids);
    Size = HeapSize(GetProcessHeap(), 0, Sids);
    ok(InBlock(Sids, Size, Sids, Count * sizeof(*Sids)), "%s: HeapSize %Iu too small for the array\n", What, Size);
    for (i = 0; i < Count; i++)
    {
        if (Sids[i].Sid != NULL)
            ok(InBlock(Sids, Size, Sids[i].Sid, RtlLengthSid(Sids[i].Sid)),
               "%s: SID %Iu at %p is outside the %Iu byte block at %p\n", What, i, Sids[i].Sid, Size, Sids);
    }
}

static void InitWellKnownSids(PSID *Sids)
{
    BOOL Ok;

    Ok = AllocateAndInitializeSid(&NtAuthority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &Sids[0]);
    ok(Ok, "AllocateAndInitializeSid failed: %lu\n", GetLastError());
    Ok = AllocateAndInitializeSid(&WorldAuthority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &Sids[1]);
    ok(Ok, "AllocateAndInitializeSid failed: %lu\n", GetLastError());
    Ok = AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &Sids[2]);
    ok(Ok, "AllocateAndInitializeSid failed: %lu\n", GetLastError());
}

static void SetName(PLSA_UNICODE_STRING String, PCWSTR Text)
{
    String->Buffer = (PWSTR)Text;
    String->Length = (USHORT)(wcslen(Text) * sizeof(WCHAR));
    String->MaximumLength = String->Length + sizeof(WCHAR);
}

static void FreeLsa(PVOID Block)
{
    NTSTATUS Status;

    if (Block == NULL)
        return;
    Status = LsaFreeMemory(Block);
    ok(Status == STATUS_SUCCESS, "LsaFreeMemory(%p) returned 0x%lx\n", Block, Status);
}

static void LookupSidsOnce(LSA_HANDLE Policy, PSID *Sids, BOOL Check)
{
    PLSA_REFERENCED_DOMAIN_LIST Domains;
    PLSA_TRANSLATED_NAME Names;
    NTSTATUS Status;

    Status = LsaLookupSids(Policy, 3, Sids, &Domains, &Names);
    ok(Status == STATUS_SUCCESS || Status == STATUS_SOME_NOT_MAPPED, "LsaLookupSids returned 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;
    if (Check)
    {
        CheckDomainList(Domains, "LsaLookupSids");
        CheckNames(Names, 3, "LsaLookupSids");
    }
    FreeLsa(Domains);
    FreeLsa(Names);
}

static void LookupNames2Once(LSA_HANDLE Policy, PLSA_UNICODE_STRING Names, ULONG Count, BOOL Check,
                             NTSTATUS Expected1, NTSTATUS Expected2)
{
    PLSA_REFERENCED_DOMAIN_LIST Domains;
    PLSA_TRANSLATED_SID2 Sids;
    NTSTATUS Status;

    Status = LsaLookupNames2(Policy, 0, Count, Names, &Domains, &Sids);
    ok(Status == Expected1 || Status == Expected2, "LsaLookupNames2 returned 0x%lx, expected 0x%lx\n", Status, Expected1);
    if (Status != STATUS_SUCCESS && Status != STATUS_SOME_NOT_MAPPED && Status != STATUS_NONE_MAPPED)
    {
        ok(Domains == NULL && Sids == NULL, "Output on failure: %p %p\n", Domains, Sids);
        return;
    }
    if (Check)
    {
        CheckDomainList(Domains, "LsaLookupNames2");
        CheckSids2(Sids, Count, "LsaLookupNames2");
    }
    FreeLsa(Domains);
    FreeLsa(Sids);
}

static const PCSTR KindNames[5] =
{
    "LsaLookupSids", "LsaLookupNames2 (all mapped)", "LsaLookupNames2 (some mapped)",
    "LsaLookupNames2 (none mapped)", "LsaLookupNames"
};

static void LookupKindOnce(LSA_HANDLE Policy, ULONG Kind, PSID *Sids, PLSA_UNICODE_STRING Names,
                           PLSA_UNICODE_STRING Mixed, PLSA_UNICODE_STRING Bogus)
{
    switch (Kind)
    {
        case 0:
            LookupSidsOnce(Policy, Sids, FALSE);
            break;
        case 1:
            LookupNames2Once(Policy, Names, 3, FALSE, STATUS_SUCCESS, STATUS_SUCCESS);
            break;
        case 2:
            LookupNames2Once(Policy, Mixed, 3, FALSE, STATUS_SOME_NOT_MAPPED, STATUS_SOME_NOT_MAPPED);
            break;
        case 3:
            LookupNames2Once(Policy, Bogus, 1, FALSE, STATUS_NONE_MAPPED, STATUS_NONE_MAPPED);
            break;
        default:
        {
            PLSA_REFERENCED_DOMAIN_LIST Domains;
            PLSA_TRANSLATED_SID LegacySids;
            NTSTATUS Status = LsaLookupNames(Policy, 3, Names, &Domains, &LegacySids);

            ok(Status == STATUS_SUCCESS, "LsaLookupNames returned 0x%lx\n", Status);
            FreeLsa(Domains);
            FreeLsa(LegacySids);
            break;
        }
    }
}

START_TEST(LsaLookupLeak)
{
    LSA_OBJECT_ATTRIBUTES Attributes = {sizeof(Attributes)};
    LSA_HANDLE Policy;
    NTSTATUS Status;
    PSID Sids[3];
    LSA_UNICODE_STRING Names[3], Mixed[3], Bogus[1];
    HEAP_TOTALS Before, After;
    ULONG i, Kind;

    Status = LsaOpenPolicy(NULL, &Attributes, POLICY_LOOKUP_NAMES, &Policy);
    if (Status != STATUS_SUCCESS)
    {
        skip("LsaOpenPolicy failed: 0x%lx\n", Status);
        return;
    }

    InitWellKnownSids(Sids);
    SetName(&Names[0], L"SYSTEM");
    SetName(&Names[1], L"Everyone");
    SetName(&Names[2], L"BUILTIN\\Administrators");
    SetName(&Mixed[0], L"Everyone");
    SetName(&Mixed[1], L"NoSuchPrincipalLsaLookupLeak");
    SetName(&Mixed[2], L"BUILTIN\\Users");
    SetName(&Bogus[0], L"NoSuchPrincipalLsaLookupLeak");

    /* Layout and error paths */
    LookupSidsOnce(Policy, Sids, TRUE);
    LookupNames2Once(Policy, Names, 3, TRUE, STATUS_SUCCESS, STATUS_SUCCESS);
    LookupNames2Once(Policy, Mixed, 3, TRUE, STATUS_SOME_NOT_MAPPED, STATUS_SOME_NOT_MAPPED);
    LookupNames2Once(Policy, Bogus, 1, TRUE, STATUS_NONE_MAPPED, STATUS_NONE_MAPPED);

    {
        PLSA_REFERENCED_DOMAIN_LIST Domains;
        PLSA_TRANSLATED_SID LegacySids;

        Status = LsaLookupNames(Policy, 3, Names, &Domains, &LegacySids);
        ok(Status == STATUS_SUCCESS, "LsaLookupNames returned 0x%lx\n", Status);
        CheckDomainList(Domains, "LsaLookupNames");
        if (LegacySids != NULL)
            ok(HeapValidate(GetProcessHeap(), 0, LegacySids), "LsaLookupNames SIDs not a heap block\n");
        FreeLsa(Domains);
        FreeLsa(LegacySids);
    }

    /* Leak: LsaFreeMemory on the results must return everything. Each kind of lookup is measured on its own. */
    for (Kind = 0; Kind < 5; Kind++)
    {
        for (i = 0; i < WARMUP_ITERATIONS; i++)
            LookupKindOnce(Policy, Kind, Sids, Names, Mixed, Bogus);

        SnapshotHeap(&Before);
        for (i = 0; i < LOOKUP_ITERATIONS; i++)
            LookupKindOnce(Policy, Kind, Sids, Names, Mixed, Bogus);
        SnapshotHeap(&After);

        trace("%s: process heap busy before: %Iu blocks, %Iu bytes; after %d lookups: %Iu blocks, %Iu bytes\n",
              KindNames[Kind], Before.Blocks, Before.Bytes, LOOKUP_ITERATIONS, After.Blocks, After.Bytes);
        /* A leak costs at least one block per lookup; allow slack for the RPC runtime's own caches */
        ok(After.Blocks < Before.Blocks + 100, "%s: heap blocks grew from %Iu to %Iu\n",
           KindNames[Kind], Before.Blocks, After.Blocks);
        ok(After.Bytes < Before.Bytes + 32 * 1024, "%s: heap bytes grew from %Iu to %Iu\n",
           KindNames[Kind], Before.Bytes, After.Bytes);
    }

    for (i = 0; i < 3; i++)
        FreeSid(Sids[i]);
    LsaClose(Policy);
}
