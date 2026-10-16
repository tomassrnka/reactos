/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/net/afd/afd/info.c
 * PURPOSE:          Ancillary functions driver
 * PROGRAMMER:       Art Yerkes (ayerkes@speakeasy.net)
 * UPDATE HISTORY:
 * 20040708 Created
 */

#include "afd.h"

NTSTATUS NTAPI
AfdGetInfo( PDEVICE_OBJECT DeviceObject, PIRP Irp,
            PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS Status = STATUS_SUCCESS;
    PAFD_INFO InfoReq = LockRequest(Irp, IrpSp, TRUE, NULL);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PLIST_ENTRY CurrentEntry;

    UNREFERENCED_PARAMETER(DeviceObject);

    AFD_DbgPrint(MID_TRACE,("Called %p %x\n", InfoReq,
                            InfoReq ? InfoReq->InformationClass : 0));

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    if (!InfoReq)
        return UnlockAndMaybeComplete(FCB, STATUS_NO_MEMORY, Irp, 0);

    _SEH2_TRY {
        switch( InfoReq->InformationClass ) {
        case AFD_INFO_RECEIVE_WINDOW_SIZE:
            InfoReq->Information.Ulong = FCB->Recv.NewWindow ? FCB->Recv.NewSize : FCB->Recv.Size;
            break;

        case AFD_INFO_SEND_WINDOW_SIZE:
            InfoReq->Information.Ulong = FCB->Send.NewWindow ? FCB->Send.NewSize : FCB->Send.Size;
            AFD_DbgPrint(MID_TRACE,("Send window size %u\n", FCB->Send.Size));
            break;

        case AFD_INFO_GROUP_ID_TYPE:
            InfoReq->Information.LargeInteger.u.HighPart = FCB->GroupType;
            InfoReq->Information.LargeInteger.u.LowPart = FCB->GroupID;
            AFD_DbgPrint(MID_TRACE, ("Group ID: %u Group Type: %u\n", FCB->GroupID, FCB->GroupType));
            break;

        case AFD_INFO_BLOCKING_MODE:
            InfoReq->Information.Boolean = FCB->NonBlocking;
            break;

    case AFD_INFO_INLINING_MODE:
        InfoReq->Information.Boolean = FCB->OobInline;
        break;

    case AFD_INFO_RECEIVE_CONTENT_SIZE:
        InfoReq->Information.Ulong = FCB->Recv.Content - FCB->Recv.BytesUsed;
        break;

        case AFD_INFO_SENDS_IN_PROGRESS:
            InfoReq->Information.Ulong = 0;

            /* Count the queued sends */
            CurrentEntry = FCB->PendingIrpList[FUNCTION_SEND].Flink;
            while (CurrentEntry != &FCB->PendingIrpList[FUNCTION_SEND])
            {
                 InfoReq->Information.Ulong++;
                 CurrentEntry = CurrentEntry->Flink;
            }

        /* This needs to count too because when this is dispatched
         * the user-mode IRP has already been completed and therefore
         * will NOT be in our pending IRP list. We count this as one send
         * outstanding although it could be multiple since we batch sends
         * when waiting for the in flight request to return, so this number
         * may not be accurate but it really doesn't matter that much since
         * it's more or less a zero/non-zero comparison to determine whether
         * we can shutdown the socket
         */
        if (FCB->SendIrp.InFlightRequest)
            InfoReq->Information.Ulong++;
        break;

        default:
            AFD_DbgPrint(MIN_TRACE,("Unknown info id %x\n",
                                    InfoReq->InformationClass));
            Status = STATUS_INVALID_PARAMETER;
            break;
        }
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        AFD_DbgPrint(MIN_TRACE,("Exception executing GetInfo\n"));
        Status = STATUS_INVALID_PARAMETER;
    } _SEH2_END;

    AFD_DbgPrint(MID_TRACE,("Returning %x\n", Status));

    return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
}

/*
 * A new window size is asked for: the replacement window is allocated now, so that running out of
 * memory fails the request, and put in place by AfdApplyWindowResize once the transport does not
 * hold the window. Asking for the current size cancels a request that waits.
 */
NTSTATUS
AfdRequestWindowResize(PAFD_DATA_WINDOW Data, UINT Size)
{
    PCHAR NewWindow;

    if (Size == Data->Size)
    {
        if (Data->NewWindow)
            ExFreePoolWithTag(Data->NewWindow, TAG_AFD_DATA_BUFFER);
        Data->NewWindow = NULL;
        Data->NewSize = 0;
        return STATUS_SUCCESS;
    }

    if (Data->NewWindow && Data->NewSize == Size)
        return STATUS_SUCCESS;

    NewWindow = ExAllocatePoolWithTag(PagedPool, Size, TAG_AFD_DATA_BUFFER);
    if (!NewWindow)
        return STATUS_NO_MEMORY;

    if (Data->NewWindow)
        ExFreePoolWithTag(Data->NewWindow, TAG_AFD_DATA_BUFFER);
    Data->NewWindow = NewWindow;
    Data->NewSize = Size;

    return STATUS_SUCCESS;
}

/*
 * Puts the replacement window in place, keeping the Used bytes that start at Offset in the old
 * one. No request references a window, only a wire buffer, so freeing the old one is safe; the callers
 * still wait for the transport to be idle, because the size of a request in flight was set from the
 * old size. A window too small for the data keeps the request until the data has gone. Returns TRUE
 * if the window was replaced.
 */
BOOLEAN
AfdApplyWindowResize(PAFD_DATA_WINDOW Data, UINT Offset, UINT Used)
{
    PCHAR OldWindow;

    if (!Data->NewWindow || Used > Data->NewSize)
        return FALSE;

    if (Used)
        RtlCopyMemory(Data->NewWindow, Data->Window + Offset, Used);

    OldWindow = Data->Window;
    Data->Window = Data->NewWindow;
    Data->Size = Data->NewSize;
    Data->NewWindow = NULL;
    Data->NewSize = 0;

    if (OldWindow)
        ExFreePoolWithTag(OldWindow, TAG_AFD_DATA_BUFFER);

    return TRUE;
}

/*
 * A stream socket keeps the unread data, at the start of the new window; a datagram socket copies
 * each datagram out of the window at once and has nothing to keep.
 */
VOID
AfdApplyRecvResize(PAFD_FCB FCB)
{
    UINT Offset = 0, Used = 0;

    if (!FCB->Recv.NewWindow)
        return;

    if (!(FCB->Flags & AFD_ENDPOINT_CONNECTIONLESS))
    {
        Offset = FCB->Recv.BytesUsed;
        Used = FCB->Recv.Content - Offset;
    }

    if (AfdApplyWindowResize(&FCB->Recv, Offset, Used) && !(FCB->Flags & AFD_ENDPOINT_CONNECTIONLESS))
    {
        FCB->Recv.Content = Used;
        FCB->Recv.BytesUsed = 0;
    }
}

#define AFD_WIRE_SHIFT 10
#define AFD_WIRE_CLASSES 7

static KSPIN_LOCK AfdWireLock;
static LIST_ENTRY AfdWireFree[AFD_WIRE_CLASSES];

VOID
AfdInitWires(VOID)
{
    UINT i;

    KeInitializeSpinLock(&AfdWireLock);
    for (i = 0; i < AFD_WIRE_CLASSES; i++)
        InitializeListHead(&AfdWireFree[i]);
}

/* A wire buffer of at least Length bytes, which is at most AFD_WIRE_MAX */
PAFD_WIRE_BUFFER
AfdAcquireWire(UINT Length)
{
    PAFD_WIRE_BUFFER Wire;
    PLIST_ENTRY Entry = NULL;
    KIRQL OldIrql;
    UINT Class = 0;

    ASSERT(Length <= AFD_WIRE_MAX);
    while (((UINT)1 << (AFD_WIRE_SHIFT + Class)) < Length)
        Class++;

    KeAcquireSpinLock(&AfdWireLock, &OldIrql);
    if (!IsListEmpty(&AfdWireFree[Class]))
        Entry = RemoveHeadList(&AfdWireFree[Class]);
    KeReleaseSpinLock(&AfdWireLock, OldIrql);

    if (Entry)
        return CONTAINING_RECORD(Entry, AFD_WIRE_BUFFER, Link);

    Wire = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Wire), TAG_AFD_DATA_BUFFER);
    if (!Wire)
        return NULL;

    Wire->Class = Class;
    Wire->Data = ExAllocatePoolWithTag(PagedPool, (SIZE_T)1 << (AFD_WIRE_SHIFT + Class), TAG_AFD_DATA_BUFFER);
    if (!Wire->Data)
    {
        ExFreePoolWithTag(Wire, TAG_AFD_DATA_BUFFER);
        return NULL;
    }

    return Wire;
}

VOID
AfdReleaseWire(PAFD_WIRE_BUFFER Wire)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&AfdWireLock, &OldIrql);
    InsertHeadList(&AfdWireFree[Wire->Class], &Wire->Link);
    KeReleaseSpinLock(&AfdWireLock, OldIrql);
}

PAFD_WIRE_BUFFER
AfdTakeWire(PAFD_DATA_WINDOW Data)
{
    return InterlockedExchangePointer((PVOID *)&Data->Wire, NULL);
}

NTSTATUS NTAPI
AfdSetInfo( PDEVICE_OBJECT DeviceObject, PIRP Irp,
            PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS Status = STATUS_SUCCESS;
    PAFD_INFO InfoReq = LockRequest(Irp, IrpSp, FALSE, NULL);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!SocketAcquireStateLock(FCB)) return LostSocket(Irp);

    if (!InfoReq)
        return UnlockAndMaybeComplete(FCB, STATUS_NO_MEMORY, Irp, 0);

    _SEH2_TRY {
        switch (InfoReq->InformationClass) {
            case AFD_INFO_BLOCKING_MODE:
                AFD_DbgPrint(MID_TRACE,("Blocking mode set to %u\n", InfoReq->Information.Boolean));
                FCB->NonBlocking = InfoReq->Information.Boolean;
                break;
            case AFD_INFO_INLINING_MODE:
                FCB->OobInline = InfoReq->Information.Boolean;
                break;
            case AFD_INFO_RECEIVE_WINDOW_SIZE:
                if (FCB->SharedData.State == SOCKET_STATE_CONNECTED ||
                    FCB->Flags & AFD_ENDPOINT_CONNECTIONLESS)
                {
                    /* FIXME: likely not right, check tcpip.sys for TDI_QUERY_MAX_DATAGRAM_INFO */
                    if (InfoReq->Information.Ulong > 0 && InfoReq->Information.Ulong < 0xFFFF)
                    {
                        Status = AfdRequestWindowResize(&FCB->Recv, InfoReq->Information.Ulong);

                        /* While a receive is in flight the transport writes into the window */
                        if (NT_SUCCESS(Status) && !FCB->ReceiveIrp.InFlightRequest)
                            AfdApplyRecvResize(FCB);
                    }
                    else
                    {
                        Status = STATUS_SUCCESS;
                    }
                }
                else
                {
                    Status = STATUS_INVALID_PARAMETER;
                }
                break;
            case AFD_INFO_SEND_WINDOW_SIZE:
                if (FCB->SharedData.State == SOCKET_STATE_CONNECTED ||
                    FCB->Flags & AFD_ENDPOINT_CONNECTIONLESS)
                {
                    if (InfoReq->Information.Ulong > 0 && InfoReq->Information.Ulong < 0xFFFF)
                    {
                        Status = AfdRequestWindowResize(&FCB->Send, InfoReq->Information.Ulong);

                        /* While a send is in flight the transport reads the window; datagrams do not use it */
                        if (NT_SUCCESS(Status) &&
                            (!FCB->SendIrp.InFlightRequest || (FCB->Flags & AFD_ENDPOINT_CONNECTIONLESS)))
                        {
                            AfdApplyWindowResize(&FCB->Send, 0, FCB->Send.BytesUsed);
                        }
                    }
                    else
                    {
                        Status = STATUS_SUCCESS;
                    }
                }
                else
                {
                    Status = STATUS_INVALID_PARAMETER;
                }
                break;
            default:
                AFD_DbgPrint(MIN_TRACE,("Unknown request %u\n", InfoReq->InformationClass));
                break;
        }
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        AFD_DbgPrint(MIN_TRACE,("Exception executing SetInfo\n"));
        Status = STATUS_INVALID_PARAMETER;
    } _SEH2_END;

    AFD_DbgPrint(MID_TRACE,("Returning %x\n", Status));

    return UnlockAndMaybeComplete(FCB, Status, Irp, 0);
}

NTSTATUS NTAPI
AfdGetSockName( PDEVICE_OBJECT DeviceObject, PIRP Irp,
                PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PMDL Mdl = NULL;

    UNREFERENCED_PARAMETER(DeviceObject);
    ASSERT(Irp->MdlAddress == NULL);

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    if( FCB->AddressFile.Object == NULL && FCB->Connection.Object == NULL ) {
         return UnlockAndMaybeComplete( FCB, STATUS_INVALID_PARAMETER, Irp, 0 );
    }

    Mdl = IoAllocateMdl( Irp->UserBuffer,
                         IrpSp->Parameters.DeviceIoControl.OutputBufferLength,
                         FALSE,
                         FALSE,
                         NULL );

    if( Mdl != NULL ) {
        _SEH2_TRY {
            MmProbeAndLockPages( Mdl, Irp->RequestorMode, IoModifyAccess );
        } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
            AFD_DbgPrint(MIN_TRACE, ("MmProbeAndLockPages() failed.\n"));
            Status = _SEH2_GetExceptionCode();
        } _SEH2_END;

        if( NT_SUCCESS(Status) ) {
                Status = TdiQueryInformation( FCB->Connection.Object
                                                ? FCB->Connection.Object
                                                : FCB->AddressFile.Object,
                                              TDI_QUERY_ADDRESS_INFO,
                                              Mdl );
        }

        /* Check if MmProbeAndLockPages or TdiQueryInformation failed and
         * clean up Mdl */
        if (!NT_SUCCESS(Status) && Irp->MdlAddress != Mdl)
            IoFreeMdl(Mdl);
    } else
        Status = STATUS_INSUFFICIENT_RESOURCES;

    return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
}

NTSTATUS NTAPI
AfdGetPeerName( PDEVICE_OBJECT DeviceObject, PIRP Irp,
                PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS Status;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;

    UNREFERENCED_PARAMETER(DeviceObject);

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    if (FCB->RemoteAddress == NULL) {
        AFD_DbgPrint(MIN_TRACE,("Invalid parameter\n"));
        return UnlockAndMaybeComplete( FCB, STATUS_INVALID_PARAMETER, Irp, 0 );
    }

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength >= TaLengthOfTransportAddress(FCB->RemoteAddress))
    {
        RtlCopyMemory(Irp->UserBuffer, FCB->RemoteAddress, TaLengthOfTransportAddress(FCB->RemoteAddress));
        Status = STATUS_SUCCESS;
    }
    else
    {
        AFD_DbgPrint(MIN_TRACE,("Buffer too small\n"));
        Status = STATUS_BUFFER_TOO_SMALL;
    }

    return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
}
