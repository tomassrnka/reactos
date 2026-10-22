/*
 * PROJECT:     ReactOS cmlib regression tests
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Regression test for the hive free-cell-list bounds/alignment
 *              validation in HvpCreateHiveFreeCellList() (sdk/lib/cmlib/hivecell.c).
 *
 *              A crafted primary hive whose single HBIN ends with an undersized,
 *              unaligned free cell (Size == 4 at bin offset 0xFFC) makes the
 *              unhardened HvpAddFree() write its 4-byte free-list link one ULONG
 *              past the end of the bin allocation (an out-of-bounds kernel write
 *              reachable through RegLoadKey with SeRestorePrivilege).
 *
 *              This is a host test: it links the real cmlib sources and drives
 *              HvInitialize(HINIT_MEMORY). Every cmlib allocation is served from
 *              a guard-page arena whose usable region ends flush against a
 *              PROT_NONE page, so the out-of-bounds link write faults
 *              deterministically on an unfixed tree (fail-before). On a fixed
 *              tree the crafted hive is rejected with STATUS_REGISTRY_CORRUPT and
 *              a well-formed hive still initializes successfully (pass-after).
 */

#include <cmlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>

/* HvpHiveHeaderChecksum is internal to cmlib; declare it for the fixture. */
ULONG CMAPI HvpHiveHeaderChecksum(PHBASE_BLOCK BaseBlock);

/* ------------------------------------------------------------------ */
/* Minimal RTL bitmap shims (cmlib only needs these for the success    */
/* path; they are never reached while a corrupt hive is rejected).     */
/* ------------------------------------------------------------------ */
VOID NTAPI RtlInitializeBitMap(PRTL_BITMAP Header, PULONG Buffer, ULONG SizeOfBitMap)
{
    Header->SizeOfBitMap = SizeOfBitMap;
    Header->Buffer = Buffer;
}
VOID NTAPI RtlClearAllBits(PRTL_BITMAP Header)
{
    if (Header->Buffer)
        memset(Header->Buffer, 0x00, ((Header->SizeOfBitMap + 31) / 32) * sizeof(ULONG));
}
VOID NTAPI RtlSetAllBits(PRTL_BITMAP Header)
{
    if (Header->Buffer)
        memset(Header->Buffer, 0xFF, ((Header->SizeOfBitMap + 31) / 32) * sizeof(ULONG));
}

/* ------------------------------------------------------------------ */
/* Stubs for cmlib references that are not exercised on the            */
/* HvInitialize(HINIT_MEMORY) parse path driven by this fixture.       */
/* ------------------------------------------------------------------ */
VOID NTAPI RtlSetBits(PRTL_BITMAP Header, ULONG StartingIndex, ULONG NumberToSet)
{
    (void)Header; (void)StartingIndex; (void)NumberToSet;
}
ULONG NTAPI RtlFindSetBits(PRTL_BITMAP Header, ULONG NumberToFind, ULONG HintIndex)
{
    (void)Header; (void)NumberToFind; (void)HintIndex;
    return 0xFFFFFFFF;
}
LONG NTAPI RtlCompareUnicodeString(PCUNICODE_STRING s1, PCUNICODE_STRING s2, BOOLEAN CaseInSensitive)
{
    (void)s1; (void)s2; (void)CaseInSensitive;
    return 0;
}
WCHAR NTAPI RtlUpcaseUnicodeChar(WCHAR Source)
{
    return Source;
}
VOID NTAPI KeQuerySystemTime(PLARGE_INTEGER CurrentTime)
{
    if (CurrentTime)
        CurrentTime->QuadPart = 0;
}
VOID NTAPI
KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2, ULONG_PTR P3, ULONG_PTR P4)
{
    (void)Code; (void)P1; (void)P2; (void)P3; (void)P4;
    fprintf(stderr, "unexpected KeBugCheckEx\n");
    abort();
}
PVOID NTAPI CmpAllocate(SIZE_T Size, BOOLEAN Paged, ULONG Tag)
{
    (void)Paged; (void)Tag;
    return malloc((size_t)Size);
}
VOID NTAPI CmpFree(PVOID Ptr, ULONG Quota)
{
    (void)Quota;
    free(Ptr);
}

BOOLEAN CMAPI CmIsSelfHealEnabled(BOOLEAN FixHive)
{
    (void)FixHive;
    return FALSE;
}
BOOLEAN CMAPI HvSyncHiveFromRecover(PHHIVE RegistryHive)
{
    (void)RegistryHive;
    return TRUE;
}
PHBIN CMAPI HvpAddBin(PHHIVE RegistryHive, ULONG Size, HSTORAGE_TYPE Storage)
{
    (void)RegistryHive; (void)Size; (void)Storage;
    return NULL;
}


/* ------------------------------------------------------------------ */
/* Guard-page allocator: the returned block ends flush against a       */
/* PROT_NONE page so any one-past-the-end write faults.                */
/* ------------------------------------------------------------------ */
static PVOID CMAPI TestAllocate(SIZE_T Size, BOOLEAN Paged, ULONG Tag)
{
    long pg = sysconf(_SC_PAGESIZE);
    size_t npages = (Size + (size_t)pg - 1) / (size_t)pg;
    size_t total;
    char *base, *ptr;

    (void)Paged; (void)Tag;
    if (npages == 0)
        npages = 1;
    total = (npages + 1) * (size_t)pg;

    base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return NULL;

    /* Last page is the guard. */
    if (mprotect(base + npages * (size_t)pg, (size_t)pg, PROT_NONE) != 0)
        return NULL;

    /* Place the block so its end abuts the guard page, keeping 16-byte
       alignment (page-sized requests stay page aligned). */
    ptr = base + npages * (size_t)pg - Size;
    ptr = (char *)((uintptr_t)ptr & ~(uintptr_t)0xF);
    return ptr;
}

static VOID CMAPI TestFree(PVOID Ptr, ULONG Quota)
{
    (void)Ptr; (void)Quota;   /* arena is intentionally leaked for the test */
}

static void BuildBaseBlock(PHBASE_BLOCK bb, ULONG HiveLength)
{
    memset(bb, 0, sizeof(*bb));
    bb->Signature = HV_HBLOCK_SIGNATURE;
    bb->Sequence1 = 1;
    bb->Sequence2 = 1;
    bb->Major     = HSYS_MAJOR;
    bb->Minor     = HSYS_MINOR;
    bb->Type      = HFILE_TYPE_PRIMARY;
    bb->Format    = HBASE_FORMAT_MEMORY;
    bb->Cluster   = 1;
    bb->RootCell  = 0x20;
    bb->Length    = HiveLength;
    bb->CheckSum  = HvpHiveHeaderChecksum(bb);
}

/* Craft the malicious hive from the Astra finding. */
static NTSTATUS RunCrafted(void)
{
    unsigned char *img = calloc(1, 0x2000);
    PHBASE_BLOCK bb = (PHBASE_BLOCK)img;
    PHBIN bin = (PHBIN)(img + HBLOCK_SIZE);
    HHIVE hive;
    NTSTATUS st;

    BuildBaseBlock(bb, HBLOCK_SIZE);

    bin->Signature  = HV_HBIN_SIGNATURE;
    bin->FileOffset = 0;
    bin->Size       = HBLOCK_SIZE;

    /* Used cell (negative size) spanning 0x20..0xFFC. */
    *(LONG *)((unsigned char *)bin + 0x20)  = -(LONG)0xFDC;
    /* Undersized free cell at the tail: 4-byte link would land at bin+0x1000. */
    *(LONG *)((unsigned char *)bin + 0xFFC) = 4;

    st = HvInitialize(&hive, HINIT_MEMORY, 0, HFILE_TYPE_PRIMARY, bb,
                      TestAllocate, TestFree, NULL, NULL, NULL, NULL, 1, NULL);
    return st;
}

/* Craft a well-formed hive (one free cell filling the bin). */
static NTSTATUS RunWellFormed(void)
{
    unsigned char *img = calloc(1, 0x2000);
    PHBASE_BLOCK bb = (PHBASE_BLOCK)img;
    PHBIN bin = (PHBIN)(img + HBLOCK_SIZE);
    HHIVE hive;
    NTSTATUS st;

    BuildBaseBlock(bb, HBLOCK_SIZE);

    bin->Signature  = HV_HBIN_SIGNATURE;
    bin->FileOffset = 0;
    bin->Size       = HBLOCK_SIZE;

    /* Single free cell, 8-byte aligned, filling the bin (0x20..0x1000). */
    *(LONG *)((unsigned char *)bin + 0x20) = (LONG)(HBLOCK_SIZE - 0x20);

    st = HvInitialize(&hive, HINIT_MEMORY, 0, HFILE_TYPE_PRIMARY, bb,
                      TestAllocate, TestFree, NULL, NULL, NULL, NULL, 1, NULL);
    return st;
}

int main(void)
{
    NTSTATUS st;
    int failed = 0;

    st = RunCrafted();
    printf("[crafted]   HvInitialize -> 0x%08lx\n", (unsigned long)st);
    if (st == STATUS_REGISTRY_CORRUPT)
    {
        printf("[crafted]   PASS: malicious hive rejected, no out-of-bounds link write\n");
    }
    else
    {
        printf("[crafted]   FAIL: expected STATUS_REGISTRY_CORRUPT (0x%08lx)\n",
               (unsigned long)STATUS_REGISTRY_CORRUPT);
        failed = 1;
    }

    st = RunWellFormed();
    printf("[wellformed] HvInitialize -> 0x%08lx\n", (unsigned long)st);
    if (st == STATUS_SUCCESS)
    {
        printf("[wellformed] PASS: valid hive still initializes\n");
    }
    else
    {
        printf("[wellformed] FAIL: valid hive unexpectedly rejected\n");
        failed = 1;
    }

    printf(failed ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return failed;
}
