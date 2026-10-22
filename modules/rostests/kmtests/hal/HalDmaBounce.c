/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite DMA test for a 32-bit bus master and
 *              buffers above 4 GB
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

/* More than 16 pages, so the map registers cannot come from 64 KB runs */
#define TEST_PAGES 20
#define TEST_OFFSET 256
#define TEST_LENGTH (TEST_PAGES * PAGE_SIZE)
/* A transfer that does not start on a page spans one page more */
#define TEST_SPAN (TEST_PAGES + 1)
/* Byte patterns that differ from page to page */
#define PATTERN_WRITE(i) ((UCHAR)((i) * 7 + ((i) >> PAGE_SHIFT) * 31))
#define PATTERN_READ(i) ((UCHAR)((i) * 13 + ((i) >> PAGE_SHIFT) * 29))

typedef struct _SG_RESULT
{
    KEVENT Event;
    PSCATTER_GATHER_LIST List;
} SG_RESULT, *PSG_RESULT;

static DRIVER_LIST_CONTROL ListControl;
static
VOID
NTAPI
ListControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PSCATTER_GATHER_LIST ScatterGather,
    _In_ PVOID Context)
{
    PSG_RESULT Result = Context;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    Result->List = ScatterGather;
    KeSetEvent(&Result->Event, IO_NO_INCREMENT, FALSE);
}

static
BOOLEAN
MemoryAbove4Gb(VOID)
{
    PPHYSICAL_MEMORY_RANGE Ranges;
    BOOLEAN Above = FALSE;
    ULONG i;

    Ranges = MmGetPhysicalMemoryRanges();
    if (!Ranges)
        return FALSE;
    for (i = 0; Ranges[i].NumberOfBytes.QuadPart != 0; i++)
    {
        if ((ULONGLONG)(Ranges[i].BaseAddress.QuadPart +
                        Ranges[i].NumberOfBytes.QuadPart) > 0x100000000ULL)
        {
            Above = TRUE;
        }
    }
    ExFreePool(Ranges);
    return Above;
}

/* What the device sees: compare the elements with the buffer (write) or
   store a pattern in them (read) */
static
VOID
DeviceAccess(
    _In_ PSCATTER_GATHER_LIST List,
    _In_ PUCHAR Buffer,
    _In_ BOOLEAN WriteToDevice)
{
    ULONG i, j, Offset = 0, Mismatch = 0;
    PUCHAR Va;

    for (i = 0; i < List->NumberOfElements; i++)
    {
        PSCATTER_GATHER_ELEMENT Element = &List->Elements[i];

        /* The HAL maps its map buffers uncached: do not alias them cached */
        Va = MmMapIoSpace(Element->Address, Element->Length, MmNonCached);
        ok(Va != NULL, "MmMapIoSpace failed for element %lu\n", i);
        if (!Va)
            return;

        for (j = 0; j < Element->Length; j++)
        {
            if (WriteToDevice)
            {
                if (Va[j] != Buffer[TEST_OFFSET + Offset + j])
                    Mismatch++;
            }
            else
            {
                Va[j] = PATTERN_READ(Offset + j);
            }
        }

        MmUnmapIoSpace(Va, Element->Length);
        Offset += Element->Length;
    }

    if (WriteToDevice)
        ok(Mismatch == 0, "The device would read %lu wrong bytes\n", Mismatch);
}

static
VOID
TestDirection(
    _In_ PDMA_ADAPTER Adapter,
    _In_ PMDL Mdl,
    _In_ PUCHAR Buffer,
    _In_ BOOLEAN WriteToDevice)
{
    SG_RESULT Result;
    NTSTATUS Status;
    KIRQL OldIrql;
    ULONG i, Total = 0, Wrong = 0;
    PUCHAR CurrentVa = (PUCHAR)MmGetMdlVirtualAddress(Mdl) + TEST_OFFSET;
    LARGE_INTEGER Timeout;

    for (i = 0; i < TEST_LENGTH; i++)
        Buffer[TEST_OFFSET + i] = PATTERN_WRITE(i);

    KeInitializeEvent(&Result.Event, NotificationEvent, FALSE);
    Result.List = NULL;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Status = Adapter->DmaOperations->GetScatterGatherList(Adapter,
                                                         KmtDriverObject->DeviceObject,
                                                         Mdl,
                                                         CurrentVa,
                                                         TEST_LENGTH,
                                                         ListControl,
                                                         &Result,
                                                         WriteToDevice);
    KeLowerIrql(OldIrql);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* Map registers may be granted later, from a work item */
    Timeout.QuadPart = -10 * 1000 * 1000 * 10LL;
    Status = KeWaitForSingleObject(&Result.Event, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (Status != STATUS_SUCCESS)
    {
        /* The request is still queued with this stack frame: hang rather
           than return under it */
        KeWaitForSingleObject(&Result.Event, Executive, KernelMode, FALSE, NULL);
    }

    for (i = 0; i < Result.List->NumberOfElements; i++)
    {
        PSCATTER_GATHER_ELEMENT Element = &Result.List->Elements[i];

        ok((ULONGLONG)Element->Address.QuadPart + Element->Length <= 0x100000000ULL,
           "%s element %lu at 0x%I64x, length 0x%lx, is out of reach of a 32-bit device\n",
           WriteToDevice ? "Write" : "Read", i, Element->Address.QuadPart, Element->Length);
        Total += Element->Length;
    }
    ok_eq_ulong(Total, TEST_LENGTH);

    if (Total == TEST_LENGTH)
        DeviceAccess(Result.List, Buffer, WriteToDevice);

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Adapter->DmaOperations->PutScatterGatherList(Adapter, Result.List, WriteToDevice);
    KeLowerIrql(OldIrql);

    /* After a write the buffer is unchanged; after a read it holds what
       the device stored */
    for (i = 0; i < TEST_LENGTH; i++)
    {
        UCHAR Expected = WriteToDevice ? PATTERN_WRITE(i) : PATTERN_READ(i);
        if (Buffer[TEST_OFFSET + i] != Expected)
            Wrong++;
    }
    ok(Wrong == 0, "%s: %lu bytes of the buffer are wrong\n",
       WriteToDevice ? "Write" : "Read", Wrong);
}

START_TEST(HalDmaBounce)
{
    DEVICE_DESCRIPTION Description;
    PDMA_ADAPTER Adapter;
    ULONG MapRegisters = 0, i;
    PHYSICAL_ADDRESS Low, High, Skip;
    PMDL Mdl;
    PUCHAR Buffer;
    PPFN_NUMBER Pfns;
    SIZE_T Size = (TEST_PAGES + 1) * PAGE_SIZE;

    if (skip(MemoryAbove4Gb(), "No memory above 4 GB\n"))
        return;

    RtlZeroMemory(&Description, sizeof(Description));
    Description.Version = DEVICE_DESCRIPTION_VERSION;
    Description.Master = TRUE;
    Description.ScatterGather = TRUE;
    Description.Dma32BitAddresses = TRUE;
    Description.InterfaceType = PCIBus;
    Description.MaximumLength = TEST_LENGTH;

    Adapter = (PDMA_ADAPTER)HalGetAdapter(&Description, &MapRegisters);
    ok(Adapter != NULL, "HalGetAdapter failed\n");
    if (!Adapter)
        return;
    trace("Map registers: %lu\n", MapRegisters);
    if (skip(MapRegisters >= TEST_SPAN, "Only %lu map registers\n", MapRegisters))
    {
        Adapter->DmaOperations->PutDmaAdapter(Adapter);
        return;
    }

    /* Pages that a 32-bit device cannot reach */
    Low.QuadPart = 0x100000000LL;
    High.QuadPart = -1;
    Skip.QuadPart = 0;
    Mdl = MmAllocatePagesForMdlEx(Low, High, Skip, Size, MmCached, 0);
    if (Mdl && MmGetMdlByteCount(Mdl) < Size)
    {
        MmFreePagesFromMdl(Mdl);
        ExFreePool(Mdl);
        Mdl = NULL;
    }
    if (skip(Mdl != NULL, "No pages above 4 GB available\n"))
    {
        Adapter->DmaOperations->PutDmaAdapter(Adapter);
        return;
    }
    /* The transfer needs one map register per page it spans */
    if (Adapter->DmaOperations->Size >= RTL_SIZEOF_THROUGH_FIELD(DMA_OPERATIONS, CalculateScatterGatherList) &&
        Adapter->DmaOperations->CalculateScatterGatherList)
    {
        ULONG ListSize = 0, Needed = 0;
        NTSTATUS Status;

        Status = Adapter->DmaOperations->CalculateScatterGatherList(Adapter, Mdl,
                                                                    (PUCHAR)MmGetMdlVirtualAddress(Mdl) + TEST_OFFSET,
                                                                    TEST_LENGTH, &ListSize, &Needed);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(Needed >= TEST_SPAN, "%lu map registers for a transfer spanning %u pages\n", Needed, TEST_SPAN);
    }

    Pfns = MmGetMdlPfnArray(Mdl);
    for (i = 0; i < TEST_PAGES + 1; i++)
        ok((ULONGLONG)Pfns[i] << PAGE_SHIFT >= 0x100000000ULL, "Page %lu is below 4 GB\n", i);

    Buffer = MmMapLockedPagesSpecifyCache(Mdl, KernelMode, MmCached, NULL, FALSE, NormalPagePriority);
    ok(Buffer != NULL, "Mapping failed\n");
    if (Buffer)
    {
        TestDirection(Adapter, Mdl, Buffer, TRUE);
        TestDirection(Adapter, Mdl, Buffer, FALSE);

        MmUnmapLockedPages(Buffer, Mdl);
    }

    MmFreePagesFromMdl(Mdl);
    ExFreePool(Mdl);
    Adapter->DmaOperations->PutDmaAdapter(Adapter);
}
