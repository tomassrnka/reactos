/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NDIS User I/O driver
 * FILE:        readwrite.c
 * PURPOSE:     Handles IRP_MJ_READ and IRP_MJ_WRITE
 * PROGRAMMERS: Cameron Gutman (cameron.gutman@reactos.org)
 */

#include "ndisuio.h"

#define NDEBUG
#include <debug.h>

static
VOID
NTAPI
ReadIrpCancel(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    /* The read holds a reference on the adapter (NduDispatchRead) while its cancel routine is set. The
       read takes the cancel spin lock to remove the routine before it drops that reference, so the
       adapter stays as long as this holds the lock: it is released only once the adapter is not used */
    PNDISUIO_ADAPTER_CONTEXT AdapterContext = Irp->Tail.Overlay.DriverContext[0];
    PNDISUIO_PACKET_ENTRY PacketEntry;

    /* Indicate a 0-byte packet on the queue to cancel the read */
    PacketEntry = ExAllocatePool(NonPagedPool, sizeof(NDISUIO_PACKET_ENTRY));
    if (PacketEntry)
    {
        PacketEntry->PacketLength = 0;

        ExInterlockedInsertHeadList(&AdapterContext->PacketList,
                                    &PacketEntry->ListEntry,
                                    &AdapterContext->Spinlock);

        KeSetEvent(&AdapterContext->PacketReadEvent, IO_NO_INCREMENT, FALSE);
    }

    /* Release the cancel spin lock */
    IoReleaseCancelSpinLock(Irp->CancelIrql);
}

NTSTATUS
NTAPI
NduDispatchRead(PDEVICE_OBJECT DeviceObject,
                PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNDISUIO_ADAPTER_CONTEXT AdapterContext;
    BOOLEAN WriteOnly = FALSE;
    KIRQL OldIrql, OldCancelIrql;
    NTSTATUS Status;
    PLIST_ENTRY ListEntry;
    PNDISUIO_PACKET_ENTRY PacketEntry = NULL;
    ULONG BytesCopied = 0;
    PVOID WaitObjects[2];

    ASSERT(DeviceObject == GlobalDeviceObject);

    /* The adapter stays until this read is done; WriteOnly is read under the list lock inside */
    AdapterContext = ReferenceFileAdapter(IrpSp->FileObject, &WriteOnly);
    if (AdapterContext == NULL || WriteOnly)
    {
        if (AdapterContext != NULL)
            DereferenceAdapterContext(AdapterContext);

        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return STATUS_INVALID_PARAMETER;
    }
    Irp->Tail.Overlay.DriverContext[0] = AdapterContext;
    WaitObjects[0] = &AdapterContext->PacketReadEvent;
    WaitObjects[1] = &AdapterContext->UnbindingEvent;

    /* Make the read cancellable */
    IoAcquireCancelSpinLock(&OldCancelIrql);
    IoSetCancelRoutine(Irp, ReadIrpCancel);
    if (Irp->Cancel)
    {
        /* Cancelled before the routine was set: nothing called it, and it must not stay set */
        IoSetCancelRoutine(Irp, NULL);
        IoReleaseCancelSpinLock(OldCancelIrql);

        DereferenceAdapterContext(AdapterContext);

        /* Indicate a 0 byte read */
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return STATUS_SUCCESS;
    }
    IoReleaseCancelSpinLock(OldCancelIrql);

    while (TRUE)
    {
        KeAcquireSpinLock(&AdapterContext->Spinlock, &OldIrql);

        /* Check if we have a packet */
        if (IsListEmpty(&AdapterContext->PacketList))
        {
            KeReleaseSpinLock(&AdapterContext->Spinlock, OldIrql);

            /* Wait for a packet (in the context of the calling user thread), or the unbind */
            Status = KeWaitForMultipleObjects(2,
                                              WaitObjects,
                                              WaitAny,
                                              UserRequest,
                                              UserMode,
                                              TRUE,
                                              NULL,
                                              NULL);
            if (Status == STATUS_WAIT_1)
                Status = STATUS_DEVICE_REMOVED;
            if (Status != STATUS_WAIT_0)
            {
                /* Remove the cancel routine */
                IoAcquireCancelSpinLock(&OldCancelIrql);
                IoSetCancelRoutine(Irp, NULL);
                IoReleaseCancelSpinLock(OldCancelIrql);

                break;
            }
        }
        else
        {
            /* Remove the first packet in the list */
            ListEntry = RemoveHeadList(&AdapterContext->PacketList);
            PacketEntry = CONTAINING_RECORD(ListEntry, NDISUIO_PACKET_ENTRY, ListEntry);

            /* Release the adapter lock: the cancel routine takes it under the cancel spin lock */
            KeReleaseSpinLock(&AdapterContext->Spinlock, OldIrql);

            /* Remove the cancel routine */
            IoAcquireCancelSpinLock(&OldCancelIrql);
            IoSetCancelRoutine(Irp, NULL);
            IoReleaseCancelSpinLock(OldCancelIrql);

            /* And we're done with this loop */
            Status = STATUS_SUCCESS;
            break;
        }
    }

    /* Check if we got a packet */
    if (PacketEntry != NULL)
    {
        /* Find the right amount of bytes to copy */
        BytesCopied = PacketEntry->PacketLength;
        if (BytesCopied > IrpSp->Parameters.Read.Length)
            BytesCopied = IrpSp->Parameters.Read.Length;

        /* Copy the packet */
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,
                      &PacketEntry->PacketData[0],
                      BytesCopied);

        /* Free the packet entry */
        ExFreePool(PacketEntry);
    }
    else
    {
        /* Something failed */
        BytesCopied = 0;
    }

    DereferenceAdapterContext(AdapterContext);

    /* Complete the IRP */
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = BytesCopied;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return Status;
}

NTSTATUS
NTAPI
NduDispatchWrite(PDEVICE_OBJECT DeviceObject,
                 PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PNDISUIO_ADAPTER_CONTEXT AdapterContext;
    PNDIS_PACKET Packet;
    NDIS_STATUS Status;
    ULONG BytesCopied = 0;

    ASSERT(DeviceObject == GlobalDeviceObject);

    /* The adapter stays until this write is done */
    AdapterContext = ReferenceFileAdapter(IrpSp->FileObject, NULL);
    if (AdapterContext == NULL)
    {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        return STATUS_INVALID_PARAMETER;
    }

    /* Create a packet and buffer descriptor for this user buffer */
    Packet = CreatePacketFromPoolBuffer(AdapterContext,
                                        Irp->AssociatedIrp.SystemBuffer,
                                        IrpSp->Parameters.Write.Length);
    if (Packet)
    {
        /* Send it via NDIS */
        NdisSend(&Status,
                 AdapterContext->BindingHandle,
                 Packet);

        /* Wait for the send */
        if (Status == NDIS_STATUS_PENDING)
        {
            KeWaitForSingleObject(&AdapterContext->AsyncEvent,
                                  Executive,
                                  KernelMode,
                                  FALSE,
                                  NULL);
            Status = AdapterContext->AsyncStatus;
        }

        /* Check if it succeeded */
        if (Status == NDIS_STATUS_SUCCESS)
            BytesCopied = IrpSp->Parameters.Write.Length;

        CleanupAndFreePacket(Packet, FALSE);
    }
    else
    {
        /* No memory */
        Status = STATUS_NO_MEMORY;
    }

    DereferenceAdapterContext(AdapterContext);

    /* Complete the IRP */
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = BytesCopied;
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);

    return Status;
}
