/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS TCP/IP protocol driver
 * FILE:        tcpip/addrchange.c
 * PURPOSE:     IOCTL_IP_ADDCHANGE_NOTIFY_REQUEST: pending requests that
 *              complete when the IPv4 address table changes
 */

#include "precomp.h"

/*
 * Requests wait in a cancel-safe queue. A change of the address table (an
 * address set or deleted, an interface registered or removed, a link going
 * up or down) completes every waiting request with STATUS_SUCCESS. A
 * request completes with STATUS_CANCELLED when the caller cancels it or
 * closes the last handle of its file object (see AddrChangeCleanup).
 */

static IO_CSQ AddrChangeCsq;
static LIST_ENTRY AddrChangeQueue;
static KSPIN_LOCK AddrChangeLock;

static
VOID
NTAPI
AddrChangeCsqInsertIrp(PIO_CSQ Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    InsertTailList(&AddrChangeQueue, &Irp->Tail.Overlay.ListEntry);
}

static
VOID
NTAPI
AddrChangeCsqRemoveIrp(PIO_CSQ Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

static
PIRP
NTAPI
AddrChangeCsqPeekNextIrp(PIO_CSQ Csq, PIRP Irp, PVOID PeekContext)
{
    PLIST_ENTRY Entry;
    PIRP Next;

    UNREFERENCED_PARAMETER(Csq);

    Entry = Irp ? Irp->Tail.Overlay.ListEntry.Flink : AddrChangeQueue.Flink;
    for (; Entry != &AddrChangeQueue; Entry = Entry->Flink)
    {
        Next = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        if (!PeekContext ||
            IoGetCurrentIrpStackLocation(Next)->FileObject == (PFILE_OBJECT)PeekContext)
            return Next;
    }

    return NULL;
}

static
VOID
NTAPI
AddrChangeCsqAcquireLock(PIO_CSQ Csq, PKIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeAcquireSpinLock(&AddrChangeLock, Irql);
}

static
VOID
NTAPI
AddrChangeCsqReleaseLock(PIO_CSQ Csq, KIRQL Irql)
{
    UNREFERENCED_PARAMETER(Csq);
    KeReleaseSpinLock(&AddrChangeLock, Irql);
}

static
VOID
NTAPI
AddrChangeCsqCompleteCanceledIrp(PIO_CSQ Csq, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

VOID
AddrChangeInitialize(VOID)
{
    InitializeListHead(&AddrChangeQueue);
    KeInitializeSpinLock(&AddrChangeLock);
    IoCsqInitialize(&AddrChangeCsq,
                    AddrChangeCsqInsertIrp,
                    AddrChangeCsqRemoveIrp,
                    AddrChangeCsqPeekNextIrp,
                    AddrChangeCsqAcquireLock,
                    AddrChangeCsqReleaseLock,
                    AddrChangeCsqCompleteCanceledIrp);
}

/* Complete every waiting request; callable at IRQL <= DISPATCH_LEVEL */
VOID
AddrChangeNotify(VOID)
{
    PIRP Irp;

    while ((Irp = IoCsqRemoveNextIrp(&AddrChangeCsq, NULL)) != NULL)
    {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
    }
}

/* IOCTL_IP_ADDCHANGE_NOTIFY_REQUEST: queue the request until the next change */
NTSTATUS
DispAddrChangeNotifyRequest(PIRP Irp)
{
    /* The request carries no data; this is a plain pend, so any buffers are ignored */
    Irp->IoStatus.Information = 0;
    IoCsqInsertIrp(&AddrChangeCsq, Irp, NULL);
    return STATUS_PENDING;
}

/* Cancel the waiting requests of one file object; returns how many */
static
ULONG
AddrChangeCancelFile(PFILE_OBJECT FileObject)
{
    PIRP Pending;
    ULONG Count = 0;

    while ((Pending = IoCsqRemoveNextIrp(&AddrChangeCsq, FileObject)) != NULL)
    {
        Pending->IoStatus.Status = STATUS_CANCELLED;
        Pending->IoStatus.Information = 0;
        IoCompleteRequest(Pending, IO_NO_INCREMENT);
        Count++;
    }

    return Count;
}

/* IOCTL_IP_ADDCHANGE_CANCEL_REQUEST: lets any thread cancel, which CancelIo cannot */
NTSTATUS
DispAddrChangeCancelRequest(PIRP Irp)
{
    Irp->IoStatus.Information = 0;
    AddrChangeCancelFile(IoGetCurrentIrpStackLocation(Irp)->FileObject);
    return STATUS_SUCCESS;
}

/* IRP_MJ_CLEANUP: the last handle of a file object closed; cancel its waiting requests */
NTSTATUS
NTAPI
TiDispatchCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    AddrChangeCancelFile(IoGetCurrentIrpStackLocation(Irp)->FileObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}
