/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Stress test for the user-mode PnP event queue
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define THREAD_COUNT 4
#define TOGGLE_COUNT 150
#define TAG_TEST 'tseT'

/* A private interface class, so that no driver listens to it */
static const GUID KmtestPnpEventGuid = {0x5c3a8e21, 0x7d4b, 0x4f0e, {0x9a, 0x61, 0x2b, 0x8f, 0x0c, 0x77, 0xd1, 0x34}};

static UNICODE_STRING SymbolicLinkName;
static LONG ToggleFailures;
static NTSTATUS LastFailure;

static KSTART_ROUTINE ToggleThread;
static
VOID
NTAPI
ToggleThread(
    _In_ PVOID Context)
{
    ULONG i;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Context);

    /* Every call queues an arrival or a removal event for umpnpmgr */
    for (i = 0; i < TOGGLE_COUNT; i++)
    {
        Status = IoSetDeviceInterfaceState(&SymbolicLinkName, (i & 1) == 0);
        if (!NT_SUCCESS(Status))
        {
            InterlockedIncrement(&ToggleFailures);
            LastFailure = Status;
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Registers a persistent interface key, which is left in the registry */
static
NTSTATUS
RegisterTestInterface(
    _Out_ PDEVICE_OBJECT *Pdo)
{
    UNICODE_STRING DriverName = RTL_CONSTANT_STRING(L"\\Driver\\PnpManager");
    UNICODE_STRING ReferenceString = RTL_CONSTANT_STRING(L"KmtestPnpEventQueue");
    PDRIVER_OBJECT DriverObject;
    PDEVICE_OBJECT *DeviceList;
    ULONG Count, i;
    NTSTATUS Status;

    *Pdo = NULL;
    Status = ObReferenceObjectByName(&DriverName,
                                     OBJ_CASE_INSENSITIVE,
                                     NULL,
                                     0,
                                     IoDriverObjectType,
                                     KernelMode,
                                     NULL,
                                     (PVOID*)&DriverObject);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Take a referenced snapshot of the devices of the PnP root driver */
    Status = IoEnumerateDeviceObjectList(DriverObject, NULL, 0, &Count);
    if (Status != STATUS_BUFFER_TOO_SMALL || Count == 0)
    {
        ObDereferenceObject(DriverObject);
        return STATUS_NOT_FOUND;
    }
    DeviceList = ExAllocatePoolWithTag(NonPagedPool, Count * sizeof(*DeviceList), TAG_TEST);
    if (!DeviceList)
    {
        ObDereferenceObject(DriverObject);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = IoEnumerateDeviceObjectList(DriverObject, DeviceList, Count * sizeof(*DeviceList), &Count);
    ObDereferenceObject(DriverObject);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(DeviceList, TAG_TEST);
        return Status;
    }

    /* Use the first root-enumerated physical device object, keep its reference */
    Status = STATUS_NOT_FOUND;
    for (i = 0; i < Count; i++)
    {
        if (*Pdo == NULL && (DeviceList[i]->Flags & DO_BUS_ENUMERATED_DEVICE))
        {
            Status = IoRegisterDeviceInterface(DeviceList[i],
                                               &KmtestPnpEventGuid,
                                               &ReferenceString,
                                               &SymbolicLinkName);
            if (NT_SUCCESS(Status))
            {
                *Pdo = DeviceList[i];
                continue;
            }
        }
        ObDereferenceObject(DeviceList[i]);
    }

    ExFreePoolWithTag(DeviceList, TAG_TEST);
    return Status;
}

START_TEST(IoPnpEventQueue)
{
    PKTHREAD Threads[THREAD_COUNT];
    PDEVICE_OBJECT Pdo;
    LARGE_INTEGER Interval;
    NTSTATUS Status;
    ULONG i;

    Status = RegisterTestInterface(&Pdo);
    if (skip(NT_SUCCESS(Status), "Cannot register a test interface: 0x%lx\n", Status))
        return;

    Status = IoSetDeviceInterfaceState(&SymbolicLinkName, TRUE);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Producers on several threads while umpnpmgr consumes the queue */
    ToggleFailures = 0;
    for (i = 0; i < THREAD_COUNT; i++)
        Threads[i] = KmtStartThread(ToggleThread, NULL);
    for (i = 0; i < THREAD_COUNT; i++)
        KmtFinishThread(Threads[i], NULL);

    ok(ToggleFailures == 0, "%ld toggles failed, last status 0x%lx\n", ToggleFailures, LastFailure);

    /* Leave the interface disabled */
    IoSetDeviceInterfaceState(&SymbolicLinkName, FALSE);

    /* Keep running for 2 seconds while umpnpmgr consumes the events.
     * A corrupted queue bugchecks; a lost event is not detected. */
    Interval.QuadPart = -2 * 10 * 1000 * 1000LL;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);

    RtlFreeUnicodeString(&SymbolicLinkName);
    ObDereferenceObject(Pdo);
}
