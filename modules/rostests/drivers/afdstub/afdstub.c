/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Stub datagram transport for AFD tests. Opens like \Device\Udp
 *              (address file, max-datagram query, receive datagram), but a
 *              datagram send can be kept in flight and then completed, or
 *              cancelled, on request of the test. The real transports
 *              complete every send inside the call, so AFD's handling of a
 *              send that is still in the transport never runs against them.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include <ntddk.h>
#include <tdikrnl.h>
#include "afdstub.h"

#define STUB_TAG 'bStA'

#define FILE_CONTROL 1
#define FILE_ADDRESS 2

typedef struct _STUB_FILE
{
    ULONG Type;
} STUB_FILE, *PSTUB_FILE;

typedef struct _STUB_GLOBALS
{
    KSPIN_LOCK Lock;
    LIST_ENTRY HeldSends;       /* IRPs, arrival order */
    LIST_ENTRY HeldReceives;
    ULONG Sequence;
    AFDSTUB_SET Mode;
    AFDSTUB_STATE State;
} STUB_GLOBALS;

static STUB_GLOBALS Globals;
static PDEVICE_OBJECT StubDevice;

/* Irp->Tail.Overlay.DriverContext of a held IRP */
#define IRP_SEQ(Irp)        ((ULONG_PTR)(Irp)->Tail.Overlay.DriverContext[0])
#define IRP_FIRST(Irp)      ((ULONG_PTR)(Irp)->Tail.Overlay.DriverContext[1])
#define IRP_HONORS(Irp)     ((Irp)->Tail.Overlay.DriverContext[2])
#define IRP_FILE(Irp)       ((Irp)->Tail.Overlay.DriverContext[3])

static
VOID
CompleteIrp(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static
ULONG_PTR
SendLengthOf(PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    return ((PTDI_REQUEST_KERNEL_SENDDG)&IrpSp->Parameters)->SendLength;
}

static
VOID
NTAPI
StubCancel(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&Globals.Lock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    Globals.State.Held--;
    Globals.State.CancelledByCancel++;
    KeReleaseSpinLock(&Globals.Lock, OldIrql);

    CompleteIrp(Irp, STATUS_CANCELLED, 0);
}

/* Completes the held sends of FileObject (NULL: any) that can still be taken
 * from the list: all of them, or the Index-th in arrival order. A send whose
 * cancel routine is already running is left to it. Returns how many. */
static
ULONG
CompleteHeld(PVOID FileObject, ULONG Index, NTSTATUS Status)
{
    KIRQL OldIrql;
    LIST_ENTRY Done;
    PLIST_ENTRY Entry, Next;
    ULONG Pos = 0, Count = 0;

    InitializeListHead(&Done);
    KeAcquireSpinLock(&Globals.Lock, &OldIrql);
    for (Entry = Globals.HeldSends.Flink; Entry != &Globals.HeldSends; Entry = Next)
    {
        PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        Next = Entry->Flink;

        if (FileObject && IRP_FILE(Irp) != FileObject)
            continue;
        if (Index != AFDSTUB_ALL && Pos++ != Index)
            continue;
        if (IRP_HONORS(Irp) && IoSetCancelRoutine(Irp, NULL) == NULL)
            continue;   /* IoCancelIrp got it first */

        RemoveEntryList(Entry);
        Globals.State.Held--;
        Count++;
        InsertTailList(&Done, Entry);
    }
    KeReleaseSpinLock(&Globals.Lock, OldIrql);

    while (!IsListEmpty(&Done))
    {
        PIRP Irp = CONTAINING_RECORD(RemoveHeadList(&Done), IRP, Tail.Overlay.ListEntry);
        if (NT_SUCCESS(Status))
            CompleteIrp(Irp, STATUS_SUCCESS, SendLengthOf(Irp));
        else
            CompleteIrp(Irp, Status, 0);
    }
    return Count;
}

static
NTSTATUS
StubSendDatagram(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    KIRQL OldIrql;
    PUCHAR Data;
    ULONG_PTR Length = SendLengthOf(Irp);
    ULONG First = 0;

    if (Irp->MdlAddress && Length)
    {
        Data = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
        if (Data)
            First = Data[0];
    }

    KeAcquireSpinLock(&Globals.Lock, &OldIrql);
    Globals.State.SendsSeen++;
    /* Like tcpip, a send that finishes at once is completed inside the call
     * and the call still returns STATUS_PENDING */
    if (Globals.Mode.HoldSends == AFDSTUB_FAIL)
    {
        KeReleaseSpinLock(&Globals.Lock, OldIrql);
        IoMarkIrpPending(Irp);
        CompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        return STATUS_PENDING;
    }
    if (!Globals.Mode.HoldSends)
    {
        Globals.State.SendsInline++;
        KeReleaseSpinLock(&Globals.Lock, OldIrql);
        IoMarkIrpPending(Irp);
        CompleteIrp(Irp, STATUS_SUCCESS, Length);
        return STATUS_PENDING;
    }

    Irp->Tail.Overlay.DriverContext[0] = (PVOID)(ULONG_PTR)++Globals.Sequence;
    Irp->Tail.Overlay.DriverContext[1] = (PVOID)(ULONG_PTR)First;
    Irp->Tail.Overlay.DriverContext[2] = (PVOID)(ULONG_PTR)(Globals.Mode.HonorCancel != 0);
    Irp->Tail.Overlay.DriverContext[3] = IrpSp->FileObject;
    IoMarkIrpPending(Irp);
    InsertTailList(&Globals.HeldSends, &Irp->Tail.Overlay.ListEntry);
    Globals.State.Held++;
    if (IRP_HONORS(Irp))
    {
        /* Set under our lock: a cancel that races finds the IRP in the list */
        IoSetCancelRoutine(Irp, StubCancel);
        if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL))
        {
            RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
            Globals.State.Held--;
            KeReleaseSpinLock(&Globals.Lock, OldIrql);
            CompleteIrp(Irp, STATUS_CANCELLED, 0);
            return STATUS_PENDING;
        }
    }
    KeReleaseSpinLock(&Globals.Lock, OldIrql);
    return STATUS_PENDING;
}

static
VOID
NTAPI
StubRecvCancel(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    IoReleaseCancelSpinLock(Irp->CancelIrql);
    KeAcquireSpinLock(&Globals.Lock, &OldIrql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&Globals.Lock, OldIrql);
    CompleteIrp(Irp, STATUS_CANCELLED, 0);
}

static
NTSTATUS
StubReceiveDatagram(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    KIRQL OldIrql;

    IRP_FILE(Irp) = IrpSp->FileObject;
    IoMarkIrpPending(Irp);
    KeAcquireSpinLock(&Globals.Lock, &OldIrql);
    InsertTailList(&Globals.HeldReceives, &Irp->Tail.Overlay.ListEntry);
    IoSetCancelRoutine(Irp, StubRecvCancel);
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL))
    {
        RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
        KeReleaseSpinLock(&Globals.Lock, OldIrql);
        CompleteIrp(Irp, STATUS_CANCELLED, 0);
        return STATUS_PENDING;
    }
    KeReleaseSpinLock(&Globals.Lock, OldIrql);
    return STATUS_PENDING;
}

static
VOID
CancelReceives(PVOID FileObject)
{
    KIRQL OldIrql;
    LIST_ENTRY Done;
    PLIST_ENTRY Entry, Next;

    InitializeListHead(&Done);
    KeAcquireSpinLock(&Globals.Lock, &OldIrql);
    for (Entry = Globals.HeldReceives.Flink; Entry != &Globals.HeldReceives; Entry = Next)
    {
        PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        Next = Entry->Flink;
        if (IRP_FILE(Irp) != FileObject || IoSetCancelRoutine(Irp, NULL) == NULL)
            continue;
        RemoveEntryList(Entry);
        InsertTailList(&Done, Entry);
    }
    KeReleaseSpinLock(&Globals.Lock, OldIrql);

    while (!IsListEmpty(&Done))
    {
        PIRP Irp = CONTAINING_RECORD(RemoveHeadList(&Done), IRP, Tail.Overlay.ListEntry);
        CompleteIrp(Irp, STATUS_CANCELLED, 0);
    }
}

static
NTSTATUS
NTAPI
StubInternalControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status = STATUS_NOT_IMPLEMENTED;
    ULONG_PTR Information = 0;

    UNREFERENCED_PARAMETER(DeviceObject);

    switch (IrpSp->MinorFunction)
    {
        case TDI_SEND_DATAGRAM:
            return StubSendDatagram(Irp, IrpSp);

        case TDI_RECEIVE_DATAGRAM:
            return StubReceiveDatagram(Irp, IrpSp);

        case TDI_SET_EVENT_HANDLER:
            Status = STATUS_SUCCESS;
            break;

        case TDI_QUERY_INFORMATION:
        {
            PTDI_REQUEST_KERNEL_QUERY_INFORMATION Query =
                (PTDI_REQUEST_KERNEL_QUERY_INFORMATION)&IrpSp->Parameters;

            if (Query->QueryType == TDI_QUERY_MAX_DATAGRAM_INFO && Irp->MdlAddress &&
                MmGetMdlByteCount(Irp->MdlAddress) >= sizeof(TDI_MAX_DATAGRAM_INFO))
            {
                PTDI_MAX_DATAGRAM_INFO Info =
                    MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
                if (Info)
                {
                    Info->MaxDatagramSize = 4096;
                    Information = sizeof(*Info);
                    Status = STATUS_SUCCESS;
                }
                else
                    Status = STATUS_INSUFFICIENT_RESOURCES;
            }
            else
                Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }

        default:
            break;
    }

    CompleteIrp(Irp, Status, Information);
    return Status;
}

static
NTSTATUS
NTAPI
StubCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_FULL_EA_INFORMATION Ea = Irp->AssociatedIrp.SystemBuffer;
    PSTUB_FILE File;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    File = ExAllocatePoolWithTag(NonPagedPool, sizeof(*File), STUB_TAG);
    if (!File)
    {
        CompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (!Ea)
        File->Type = FILE_CONTROL;
    else if (Ea->EaNameLength == TDI_TRANSPORT_ADDRESS_LENGTH &&
             RtlCompareMemory(Ea->EaName, TdiTransportAddress, TDI_TRANSPORT_ADDRESS_LENGTH) ==
                 TDI_TRANSPORT_ADDRESS_LENGTH)
        File->Type = FILE_ADDRESS;
    else
    {
        ExFreePoolWithTag(File, STUB_TAG);
        CompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
        return STATUS_INVALID_PARAMETER;
    }

    if (File->Type == FILE_ADDRESS)
    {
        KeAcquireSpinLock(&Globals.Lock, &OldIrql);
        Globals.State.AddressFilesOpen++;
        KeReleaseSpinLock(&Globals.Lock, OldIrql);
    }
    IrpSp->FileObject->FsContext = File;
    CompleteIrp(Irp, STATUS_SUCCESS, FILE_OPENED);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
StubCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PSTUB_FILE File = IrpSp->FileObject->FsContext;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (File && File->Type == FILE_ADDRESS)
    {
        PLIST_ENTRY Entry;
        BOOLEAN Found = FALSE;
        BOOLEAN Complete;

        KeAcquireSpinLock(&Globals.Lock, &OldIrql);
        for (Entry = Globals.HeldSends.Flink; Entry != &Globals.HeldSends; Entry = Entry->Flink)
        {
            if (IRP_FILE(CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry)) == IrpSp->FileObject)
                Found = TRUE;
        }
        if (Found)
            Globals.State.CleanupWhileHeld++;
        Complete = Globals.Mode.CompleteOnCleanup != 0;
        Globals.State.AddressFilesOpen--;
        KeReleaseSpinLock(&Globals.Lock, OldIrql);

        if (Found && Complete)
            CompleteHeld(IrpSp->FileObject, AFDSTUB_ALL, STATUS_CANCELLED);
        CancelReceives(IrpSp->FileObject);
    }

    CompleteIrp(Irp, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
StubClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    UNREFERENCED_PARAMETER(DeviceObject);

    if (IrpSp->FileObject->FsContext)
        ExFreePoolWithTag(IrpSp->FileObject->FsContext, STUB_TAG);
    IrpSp->FileObject->FsContext = NULL;
    CompleteIrp(Irp, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
StubDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PSTUB_FILE File = IrpSp->FileObject->FsContext;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG In = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG Out = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Information = 0;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!File || File->Type != FILE_CONTROL)
    {
        CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_AFDSTUB_SET:
            if (In < sizeof(AFDSTUB_SET))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            KeAcquireSpinLock(&Globals.Lock, &OldIrql);
            Globals.Mode = *(PAFDSTUB_SET)Buffer;
            if (!Globals.State.Held)
            {
                /* A new run: restart the counters, keep the files that are open */
                ULONG Open = Globals.State.AddressFilesOpen;
                RtlZeroMemory(&Globals.State, sizeof(Globals.State));
                Globals.State.AddressFilesOpen = Open;
                Globals.Sequence = 0;
            }
            KeReleaseSpinLock(&Globals.Lock, OldIrql);
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_AFDSTUB_QUERY:
        {
            PAFDSTUB_STATE State = Buffer;
            PLIST_ENTRY Entry;

            if (Out < sizeof(*State))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            KeAcquireSpinLock(&Globals.Lock, &OldIrql);
            *State = Globals.State;
            State->Reported = 0;
            for (Entry = Globals.HeldSends.Flink;
                 Entry != &Globals.HeldSends && State->Reported < AFDSTUB_MAX_REPORTED;
                 Entry = Entry->Flink)
            {
                PIRP Held = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
                PAFDSTUB_HELD Report = &State->Entry[State->Reported++];

                Report->Sequence = (ULONG)IRP_SEQ(Held);
                Report->Length = (ULONG)SendLengthOf(Held);
                Report->FirstByte = (ULONG)IRP_FIRST(Held);
                Report->CancelRequested = Held->Cancel;
            }
            KeReleaseSpinLock(&Globals.Lock, OldIrql);
            Information = sizeof(*State);
            Status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_AFDSTUB_COMPLETE:
        {
            PAFDSTUB_COMPLETE Req = Buffer;
            ULONG Count;

            if (In < sizeof(*Req))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            Count = CompleteHeld(NULL, Req->Index, Req->Status);
            KeAcquireSpinLock(&Globals.Lock, &OldIrql);
            Globals.State.CompletedByTest += Count;
            KeReleaseSpinLock(&Globals.Lock, OldIrql);
            Status = Count ? STATUS_SUCCESS : STATUS_NOT_FOUND;
            break;
        }

        default:
            break;
    }

    CompleteIrp(Irp, Status, Information);
    return Status;
}

static
VOID
NTAPI
StubUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    if (StubDevice)
        IoDeleteDevice(StubDevice);
}

NTSTATUS
NTAPI
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(AFDSTUB_DEVICE_NAME);
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);

    KeInitializeSpinLock(&Globals.Lock);
    InitializeListHead(&Globals.HeldSends);
    InitializeListHead(&Globals.HeldReceives);
    Globals.Mode.CompleteOnCleanup = 1;

    Status = IoCreateDevice(DriverObject, 0, &Name, FILE_DEVICE_NETWORK,
                            FILE_DEVICE_SECURE_OPEN, FALSE, &StubDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    DriverObject->MajorFunction[IRP_MJ_CREATE] = StubCreate;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = StubCleanup;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = StubClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = StubDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = StubInternalControl;
    DriverObject->DriverUnload = StubUnload;
    StubDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
