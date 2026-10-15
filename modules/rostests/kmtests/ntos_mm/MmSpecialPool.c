/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Kernel-Mode Test Suite special pool test
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

/*
 * Special pool is chosen at boot. This test expects, in
 * HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management:
 *   PoolTag         REG_DWORD  0x70536D4B ("KmSp")
 *   SpecialPoolTags REG_SZ     "KmS? Kmz* KmP"
 *   PoolTagOverruns REG_DWORD  1 or absent (guard page after the data), 0 (guard page before it)
 * Without that configuration the test skips. The hidden tests make a bad access on purpose and
 * must stop the system at once: -MmSpecialPoolOverrun and -MmSpecialPoolUnderrun with
 * PAGE_FAULT_BEYOND_END_OF_ALLOCATION (0xCD); -MmSpecialPoolUseAfterFree, -MmSpecialPoolUseAfterFreeDpc
 * (at DISPATCH_LEVEL) and -MmSpecialPoolUseAfterFreeNoInts (interrupts off) with
 * PAGE_FAULT_IN_FREED_SPECIAL_POOL (0xCC), not a hang or another code.
 *
 * The hidden test -MmSpecialPoolMalformed checks that malformed values are ignored. Boot it with
 * one of these instead of the configuration above:
 *   M1: PoolTag REG_BINARY 4B 6D 53 70 ("KmSp"), PoolTagOverruns REG_BINARY 00, SpecialPoolTags REG_SZ "ABCDEFGH Kmz*"
 *   M2: SpecialPoolTags REG_DWORD 0x2A (would read as "*", every tag), nothing else
 *   M3: PoolTag REG_DWORD of 8 bytes 2A 00 00 00 00 00 00 00 (its first four bytes read as "*"),
 *       PoolTagOverruns REG_DWORD of 8 bytes, all zero (its first four bytes would select underruns)
 * Only "Kmz*" in M1 may select anything. The user-mode test MmSpecialPoolUser (kmtest.exe) checks
 * that the same addresses touched from user mode give an access violation and no bugcheck.
 */

#define TAG_EXACT       'pSmK' /* "KmSp", PoolTag */
#define TAG_ANY_CHAR    'xSmK' /* "KmSx", matches "KmS?" */
#define TAG_ANY_REST    'AzmK' /* "KmzA", matches "Kmz*" */
#define TAG_PADDED      ' PmK' /* "KmP ", matches "KmP" padded with a space */
#define TAG_NOT_PADDED  'xPmK' /* "KmPx", not selected */
#define TAG_OTHER       'xTmK' /* "KmTx", not selected */

#define EXPECTED_POOL_TAG 0x70536D4BUL
#define EXPECTED_TAG_LIST L"KmS? Kmz* KmP"

/* Special pool places the data at a POOL_HEADER boundary next to the guard page */
#define SPECIAL_POOL_ALIGNMENT (2 * sizeof(PVOID))

static BOOLEAN CatchOverruns = TRUE;

static
BOOLEAN
QueryMemoryManagementValue(
    _In_ PCWSTR Name,
    _Out_writes_bytes_(BufferSize) PVOID Buffer,
    _In_ ULONG BufferSize,
    _Out_ PULONG DataSize,
    _Out_ PULONG Type)
{
    UNICODE_STRING KeyName = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management");
    UNICODE_STRING ValueName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UCHAR InfoBuffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + 256];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)InfoBuffer;
    HANDLE KeyHandle;
    NTSTATUS Status;
    ULONG ResultLength;

    *DataSize = 0;
    *Type = REG_NONE;
    InitializeObjectAttributes(&ObjectAttributes, &KeyName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwOpenKey(&KeyHandle, KEY_QUERY_VALUE, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return FALSE;

    RtlInitUnicodeString(&ValueName, Name);
    Status = ZwQueryValueKey(KeyHandle, &ValueName, KeyValuePartialInformation, Info, sizeof(InfoBuffer), &ResultLength);
    ZwClose(KeyHandle);
    if (!NT_SUCCESS(Status) || Info->DataLength > BufferSize)
        return FALSE;

    RtlCopyMemory(Buffer, Info->Data, Info->DataLength);
    *DataSize = Info->DataLength;
    *Type = Info->Type;
    return TRUE;
}

static
BOOLEAN
IsConfigured(VOID)
{
    ULONG PoolTag = 0, Overruns = 1, DataSize, Type;
    WCHAR List[64] = { 0 };
    BOOLEAN HavePoolTag, HaveList;

    HavePoolTag = QueryMemoryManagementValue(L"PoolTag", &PoolTag, sizeof(PoolTag), &DataSize, &Type) &&
                  Type == REG_DWORD && PoolTag == EXPECTED_POOL_TAG;
    HaveList = QueryMemoryManagementValue(L"SpecialPoolTags", List, sizeof(List) - sizeof(WCHAR), &DataSize, &Type) &&
               (Type == REG_SZ || Type == REG_MULTI_SZ) &&
               wcscmp(List, EXPECTED_TAG_LIST) == 0;
    if (QueryMemoryManagementValue(L"PoolTagOverruns", &Overruns, sizeof(Overruns), &DataSize, &Type) &&
        Type == REG_DWORD)
    {
        CatchOverruns = (Overruns != 0);
    }

    if (HavePoolTag != HaveList)
        trace("Special pool is only partly configured (PoolTag %d, SpecialPoolTags %d)\n", HavePoolTag, HaveList);
    return HavePoolTag && HaveList;
}

/* Data next to an unmapped guard page, at the end of its page or at its start */
static
BOOLEAN
LooksSpecial(
    _In_ PVOID Buffer,
    _In_ SIZE_T Size)
{
    ULONG_PTR Start = (ULONG_PTR)Buffer;
    ULONG_PTR PageEnd = (ULONG_PTR)PAGE_ALIGN(Buffer) + PAGE_SIZE;

    if (CatchOverruns)
    {
        return (Start + Size <= PageEnd) &&
               (PageEnd - (Start + Size) < SPECIAL_POOL_ALIGNMENT) &&
               !MmIsAddressValid((PVOID)PageEnd);
    }

    return (PAGE_ALIGN(Buffer) == Buffer) &&
           !MmIsAddressValid((PUCHAR)Buffer - 1);
}

static
VOID
TestSelected(
    _In_ POOL_TYPE PoolType,
    _In_ ULONG Tag,
    _In_ SIZE_T Size)
{
    PUCHAR Buffer;

    Buffer = ExAllocatePoolWithTag(PoolType, Size, Tag);
    ok(Buffer != NULL, "[%d %.4s %Iu] allocation failed\n", PoolType, (PCSTR)&Tag, Size);
    if (!Buffer)
        return;

    ok(LooksSpecial(Buffer, Size), "[%d %.4s %Iu] %p is not next to a guard page\n", PoolType, (PCSTR)&Tag, Size, Buffer);
    ok(MmIsAddressValid(Buffer) && MmIsAddressValid(Buffer + Size - 1), "[%d %.4s %Iu] %p is not mapped\n", PoolType, (PCSTR)&Tag, Size, Buffer);
    RtlFillMemory(Buffer, Size, 0x5A);
    ExFreePoolWithTag(Buffer, Tag);
    ok(!MmIsAddressValid(Buffer), "[%d %.4s %Iu] %p is still mapped after the free\n", PoolType, (PCSTR)&Tag, Size, Buffer);
}

static
VOID
TestNotSelected(
    _In_ POOL_TYPE PoolType,
    _In_ ULONG Tag)
{
    PVOID Buffers[8];
    ULONG i, Special = 0;

    /* An ordinary block can sit at the end of a page by chance, but not eight times in a row */
    for (i = 0; i < RTL_NUMBER_OF(Buffers); i++)
    {
        Buffers[i] = ExAllocatePoolWithTag(PoolType, 40, Tag);
        ok(Buffers[i] != NULL, "[%d %.4s] allocation %lu failed\n", PoolType, (PCSTR)&Tag, i);
        if (Buffers[i] && LooksSpecial(Buffers[i], 40))
            Special++;
    }
    ok(Special < RTL_NUMBER_OF(Buffers), "[%d %.4s] %lu of %lu allocations came from special pool\n",
       PoolType, (PCSTR)&Tag, Special, (ULONG)RTL_NUMBER_OF(Buffers));
    for (i = 0; i < RTL_NUMBER_OF(Buffers); i++)
    {
        if (Buffers[i])
            ExFreePoolWithTag(Buffers[i], Tag);
    }
}

static
VOID
TestLookaside(VOID)
{
    NPAGED_LOOKASIDE_LIST NonPagedList;
    PAGED_LOOKASIDE_LIST PagedList;
    PUCHAR Entry;

    /* A lookaside list must not keep a freed special pool entry mapped */
    ExInitializeNPagedLookasideList(&NonPagedList, NULL, NULL, 0, 72, TAG_EXACT, 0);
    Entry = ExAllocateFromNPagedLookasideList(&NonPagedList);
    ok(Entry != NULL, "nonpaged lookaside allocation failed\n");
    if (Entry)
    {
        ok(LooksSpecial(Entry, 72), "nonpaged lookaside entry %p is not next to a guard page\n", Entry);
        ExFreeToNPagedLookasideList(&NonPagedList, Entry);
        ok(!MmIsAddressValid(Entry), "nonpaged lookaside entry %p is still mapped after the free\n", Entry);
    }
    ExDeleteNPagedLookasideList(&NonPagedList);

    ExInitializePagedLookasideList(&PagedList, NULL, NULL, 0, 72, TAG_ANY_REST, 0);
    Entry = ExAllocateFromPagedLookasideList(&PagedList);
    ok(Entry != NULL, "paged lookaside allocation failed\n");
    if (Entry)
    {
        ok(LooksSpecial(Entry, 72), "paged lookaside entry %p is not next to a guard page\n", Entry);
        ExFreeToPagedLookasideList(&PagedList, Entry);
        ok(!MmIsAddressValid(Entry), "paged lookaside entry %p is still mapped after the free\n", Entry);
    }
    ExDeletePagedLookasideList(&PagedList);
}

START_TEST(MmSpecialPool)
{
    static const SIZE_T Sizes[] = { 1, 24, 100, 2000, PAGE_SIZE - SPECIAL_POOL_ALIGNMENT };
    static const ULONG SelectedTags[] = { TAG_EXACT, TAG_ANY_CHAR, TAG_ANY_REST, TAG_PADDED };
    ULONG t, s;
    PVOID Big;

    if (!IsConfigured())
    {
        skip(FALSE, "Special pool is not configured: set PoolTag=0x70536D4B and SpecialPoolTags=\"%ls\", then reboot\n",
             EXPECTED_TAG_LIST);
        return;
    }
    trace("Special pool catches %s\n", CatchOverruns ? "overruns" : "underruns");

    for (t = 0; t < RTL_NUMBER_OF(SelectedTags); t++)
    {
        for (s = 0; s < RTL_NUMBER_OF(Sizes); s++)
        {
            TestSelected(NonPagedPool, SelectedTags[t], Sizes[s]);
            TestSelected(PagedPool, SelectedTags[t], Sizes[s]);
        }
    }

    TestNotSelected(NonPagedPool, TAG_OTHER);
    TestNotSelected(PagedPool, TAG_OTHER);
    TestNotSelected(NonPagedPool, TAG_NOT_PADDED);

    /* Cache-aligned requests and requests larger than a page stay in ordinary pool */
    Big = ExAllocatePoolWithTag(NonPagedPoolCacheAligned, 100, TAG_EXACT);
    ok(Big != NULL, "cache-aligned allocation failed\n");
    if (Big)
    {
        ok(!LooksSpecial(Big, 100), "cache-aligned allocation %p came from special pool\n", Big);
        ExFreePoolWithTag(Big, TAG_EXACT);
    }
    Big = ExAllocatePoolWithTag(NonPagedPool, 3 * PAGE_SIZE, TAG_EXACT);
    ok(Big != NULL, "large allocation failed\n");
    if (Big)
    {
        ok(MmIsAddressValid((PUCHAR)Big + 3 * PAGE_SIZE - 1), "large allocation %p is not mapped\n", Big);
        ExFreePoolWithTag(Big, TAG_EXACT);
    }

    TestLookaside();
}

static
PUCHAR
AllocateForFault(VOID)
{
    PUCHAR Buffer;

    if (!IsConfigured())
    {
        skip(FALSE, "Special pool is not configured\n");
        return NULL;
    }
    Buffer = ExAllocatePoolWithTag(NonPagedPool, 24, TAG_EXACT);
    ok(Buffer != NULL, "allocation failed\n");
    if (Buffer && !LooksSpecial(Buffer, 24))
    {
        ok(0, "%p is not next to a guard page\n", Buffer);
        ExFreePoolWithTag(Buffer, TAG_EXACT);
        return NULL;
    }
    return Buffer;
}

START_TEST(MmSpecialPoolOverrun)
{
    PUCHAR Buffer = AllocateForFault();
    PUCHAR PageEnd;

    if (!Buffer)
        return;
    if (!CatchOverruns)
    {
        skip(FALSE, "Special pool catches underruns (PoolTagOverruns is 0)\n");
        ExFreePoolWithTag(Buffer, TAG_EXACT);
        return;
    }

    PageEnd = (PUCHAR)PAGE_ALIGN(Buffer) + PAGE_SIZE;
    trace("Writing %p, the byte after the page of %p\n", PageEnd, Buffer);
    *(volatile UCHAR *)PageEnd = 0xF1;
    ok(0, "The write after the allocation did not fault\n");
    ExFreePoolWithTag(Buffer, TAG_EXACT);
}

START_TEST(MmSpecialPoolUnderrun)
{
    PUCHAR Buffer = AllocateForFault();

    if (!Buffer)
        return;
    if (CatchOverruns)
    {
        skip(FALSE, "Special pool catches overruns (set PoolTagOverruns to 0)\n");
        ExFreePoolWithTag(Buffer, TAG_EXACT);
        return;
    }

    trace("Writing %p, the byte before %p\n", Buffer - 1, Buffer);
    *(volatile UCHAR *)(Buffer - 1) = 0xF1;
    ok(0, "The write before the allocation did not fault\n");
    ExFreePoolWithTag(Buffer, TAG_EXACT);
}

START_TEST(MmSpecialPoolUseAfterFree)
{
    PUCHAR Buffer = AllocateForFault();

    if (!Buffer)
        return;

    ExFreePoolWithTag(Buffer, TAG_EXACT);
    trace("Writing %p after its free\n", Buffer);
    *(volatile UCHAR *)Buffer = 0xF1;
    ok(0, "The write after the free did not fault\n");
}

START_TEST(MmSpecialPoolUseAfterFreeDpc)
{
    PUCHAR Buffer = AllocateForFault();
    KIRQL OldIrql;

    if (!Buffer)
        return;

    ExFreePoolWithTag(Buffer, TAG_EXACT);
    trace("Reading %p after its free, at DISPATCH_LEVEL\n", Buffer);
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    (void)*(volatile UCHAR *)Buffer;
    KeLowerIrql(OldIrql);
    ok(0, "The read after the free did not fault\n");
}

START_TEST(MmSpecialPoolUseAfterFreeNoInts)
{
    PUCHAR Buffer = AllocateForFault();

    if (!Buffer)
        return;

    ExFreePoolWithTag(Buffer, TAG_EXACT);
    trace("Reading %p after its free, with interrupts off\n", Buffer);
    _disable();
    (void)*(volatile UCHAR *)Buffer;
    _enable();
    ok(0, "The read after the free did not fault\n");
}

/* Selected tags and the ones a mis-parse could produce: the long token, its halves, "*" and the wrongly typed PoolTag */
START_TEST(MmSpecialPoolMalformed)
{
    static const struct { ULONG Tag; PCSTR Why; } NotSelected[] =
    {
        { 'pSmK', "PoolTag of the wrong type" },
        { 'DCBA', "first four characters of a long token" },
        { 'HGFE', "last four characters of a long token" },
        { 'wZyX', "an unrelated tag (\"*\" from a REG_DWORD)" },
    };
    ULONG DataSize, Type, i, Special, Raw[4];
    WCHAR List[64] = { 0 };
    BOOLEAN ExpectKmz, M2, M3;
    PVOID Buffer;

    ExpectKmz = QueryMemoryManagementValue(L"SpecialPoolTags", List, sizeof(List) - sizeof(WCHAR), &DataSize, &Type) &&
                (Type == REG_SZ) && (wcscmp(List, L"ABCDEFGH Kmz*") == 0);
    M2 = QueryMemoryManagementValue(L"SpecialPoolTags", Raw, sizeof(Raw), &DataSize, &Type) && (Type == REG_DWORD);
    M3 = QueryMemoryManagementValue(L"PoolTag", Raw, sizeof(Raw), &DataSize, &Type) &&
         (Type == REG_DWORD) && (DataSize > sizeof(ULONG));
    if (!ExpectKmz && !M2 && !M3)
    {
        skip(FALSE, "Not booted with a malformed configuration (M1, M2 or M3, see the file header)\n");
        return;
    }

    /* PoolTagOverruns of the wrong type or size must leave the default: data at the end of the page */
    CatchOverruns = TRUE;
    trace("Configuration %s\n", ExpectKmz ? "M1" : (M2 ? "M2" : "M3"));

    for (i = 0; i < RTL_NUMBER_OF(NotSelected); i++)
    {
        Buffer = ExAllocatePoolWithTag(NonPagedPool, 40, NotSelected[i].Tag);
        ok(Buffer != NULL, "[%.4s] allocation failed\n", (PCSTR)&NotSelected[i].Tag);
        if (!Buffer)
            continue;
        Special = LooksSpecial(Buffer, 40);
        ExFreePoolWithTag(Buffer, NotSelected[i].Tag);
        if (Special)
        {
            /* An ordinary block may end a page by chance; a second one rules that out */
            Buffer = ExAllocatePoolWithTag(NonPagedPool, 56, NotSelected[i].Tag);
            Special = Buffer && LooksSpecial(Buffer, 56);
            if (Buffer) ExFreePoolWithTag(Buffer, NotSelected[i].Tag);
        }
        ok(!Special, "[%.4s] came from special pool: %s\n", (PCSTR)&NotSelected[i].Tag, NotSelected[i].Why);
    }

    if (ExpectKmz)
    {
        TestSelected(NonPagedPool, TAG_ANY_REST, 40);
        TestSelected(PagedPool, TAG_ANY_REST, 2000);
    }
}
