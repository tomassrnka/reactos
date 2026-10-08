/*
 * PROJECT:     ReactOS Kernel-Mode Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for granting a queued DMA adapter channel
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

typedef struct _DMA_GRANT
{
    KEVENT Event;
    PVOID MapRegisterBase;
    IO_ALLOCATION_ACTION Action;
    LONG Calls;
} DMA_GRANT, *PDMA_GRANT;

/* Static: a request the HAL never grants keeps pointing here */
static DMA_GRANT First, Second;
static BOOLEAN Leaked;

static DRIVER_CONTROL GrantControl;

static
IO_ALLOCATION_ACTION
NTAPI
GrantControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID MapRegisterBase,
    _In_ PVOID Context)
{
    PDMA_GRANT Grant = Context;
    IO_ALLOCATION_ACTION Action = Grant->Action;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    Grant->MapRegisterBase = MapRegisterBase;
    InterlockedIncrement(&Grant->Calls);
    KeSetEvent(&Grant->Event, IO_NO_INCREMENT, FALSE);
    return Action;
}

static
VOID
FreeChannel(
    _In_ PDMA_ADAPTER Adapter)
{
    KIRQL OldIrql;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Adapter->DmaOperations->FreeAdapterChannel(Adapter);
    KeLowerIrql(OldIrql);
}

static
NTSTATUS
AllocateChannel(
    _In_ PDMA_ADAPTER Adapter,
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG NumberOfMapRegisters,
    _In_ PDMA_GRANT Grant)
{
    NTSTATUS Status;
    KIRQL OldIrql;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Status = Adapter->DmaOperations->AllocateAdapterChannel(Adapter,
                                                            DeviceObject,
                                                            NumberOfMapRegisters,
                                                            GrantControl,
                                                            Grant);
    KeLowerIrql(OldIrql);
    return Status;
}

/*
 * Hold the channel, queue a second request for NumberOfMapRegisters behind
 * it and free the channel. Both control routines keep the channel, so the
 * HAL is done with the adapter once the test has freed it. Returns FALSE if
 * a request was never granted; the HAL then still holds it and the caller
 * must leak the adapter and the device objects.
 */
static
BOOLEAN
QueueBehindBusyChannel(
    _In_ PDMA_ADAPTER Adapter,
    _In_ PDEVICE_OBJECT Device[2],
    _In_ ULONG NumberOfMapRegisters,
    _Out_ PBOOLEAN Passed)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    BOOLEAN Queued;

    *Passed = FALSE;
    RtlZeroMemory(&First, sizeof(First));
    RtlZeroMemory(&Second, sizeof(Second));
    KeInitializeEvent(&First.Event, NotificationEvent, FALSE);
    KeInitializeEvent(&Second.Event, NotificationEvent, FALSE);
    First.Action = KeepObject;
    Second.Action = KeepObject;
    Timeout.QuadPart = -5 * 1000 * 1000 * 10LL;

    Status = AllocateChannel(Adapter, Device[0], 1, &First);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = KeWaitForSingleObject(&First.Event, Executive, KernelMode, FALSE, &Timeout);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (Status != STATUS_SUCCESS)
        return FALSE;

    Status = AllocateChannel(Adapter, Device[1], NumberOfMapRegisters, &Second);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_long(Second.Calls, 0L);
    Queued = (Status == STATUS_SUCCESS && Second.Calls == 0);

    FreeChannel(Adapter);

    Status = KeWaitForSingleObject(&Second.Event, Executive, KernelMode, FALSE, &Timeout);
    ok(Status == STATUS_SUCCESS, "Queued request for %lu map registers was not granted, Status 0x%lx\n",
       NumberOfMapRegisters, Status);
    if (Status != STATUS_SUCCESS)
        return FALSE;

    FreeChannel(Adapter);
    ok_eq_long(First.Calls, 1L);
    ok_eq_long(Second.Calls, 1L);
    trace("%lu map registers: MapRegisterBase %p, %p\n",
          NumberOfMapRegisters, First.MapRegisterBase, Second.MapRegisterBase);
    *Passed = (Queued && First.Calls == 1 && Second.Calls == 1);
    if (First.MapRegisterBase == NULL)
    {
        ok(Second.MapRegisterBase == NULL, "Queued request for %lu map registers got map registers %p\n",
           NumberOfMapRegisters, Second.MapRegisterBase);
        *Passed = *Passed && (Second.MapRegisterBase == NULL);
    }
    return TRUE;
}

/*
 * A 32-bit PCI scatter/gather bus master needs no map registers, so a
 * request queued behind a busy channel must be granted with none when the
 * channel is freed, even when it asks for more map registers than exist.
 */
static
VOID
TestQueuedChannelGrant(VOID)
{
    DEVICE_DESCRIPTION Description;
    PDMA_ADAPTER Adapter;
    PDEVICE_OBJECT Device[2] = { NULL, NULL };
    ULONG NumberOfMapRegisters = 0;
    BOOLEAN Passed;
    NTSTATUS Status;
    ULONG i;

    RtlZeroMemory(&Description, sizeof(Description));
    Description.Version = DEVICE_DESCRIPTION_VERSION;
    Description.Master = TRUE;
    Description.ScatterGather = TRUE;
    Description.Dma32BitAddresses = TRUE;
    Description.InterfaceType = PCIBus;
    Description.MaximumLength = 1024 * 1024;

    ok(!Leaked, "An earlier run left a request with the HAL\n");
    if (Leaked)
        return;

    Adapter = IoGetDmaAdapter(NULL, &Description, &NumberOfMapRegisters);
    if (skip(Adapter != NULL, "No DMA adapter\n"))
        return;
    trace("NumberOfMapRegisters = %lu\n", NumberOfMapRegisters);
    ok(NumberOfMapRegisters != 0, "No map registers allowed\n");

    for (i = 0; i < RTL_NUMBER_OF(Device); i++)
    {
        Status = IoCreateDevice(KmtDriverObject, 0, NULL, FILE_DEVICE_UNKNOWN,
                                0, FALSE, &Device[i]);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!NT_SUCCESS(Status))
            goto Cleanup;
    }

    /* A request that fits: a wrong grant gets map registers, which the HAL
     * takes back when the test frees the channel */
    if (!QueueBehindBusyChannel(Adapter, Device, 1, &Passed))
    {
        Leaked = TRUE;
        return;
    }

    /* More than the HAL has: a wrong grant parks it at the head of the master
     * adapter queue for good, stalling every adapter that needs map registers */
    if (!skip(Passed, "Queued grant of one map register failed\n"))
    {
        if (!QueueBehindBusyChannel(Adapter, Device, NumberOfMapRegisters, &Passed))
        {
            Leaked = TRUE;
            return;
        }
    }

Cleanup:
    for (i = 0; i < RTL_NUMBER_OF(Device); i++)
    {
        if (Device[i])
            IoDeleteDevice(Device[i]);
    }
    Adapter->DmaOperations->PutDmaAdapter(Adapter);
}

START_TEST(HalDma)
{
    TestQueuedChannelGrant();
}
