/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/net/afd/afd/write.c
 * PURPOSE:          Ancillary functions driver
 * PROGRAMMER:       Art Yerkes (ayerkes@speakeasy.net)
 * UPDATE HISTORY:
 * 20040708 Created
 */

#include "afd.h"

/*
 * Send requests of a stream socket queue in FCB->PendingIrpList[FUNCTION_SEND] in the order they
 * came. A request whose data has been copied into the send window keeps the number of its bytes
 * the transport has not taken yet in DriverContext[3]; it completes when that reaches 0. A request
 * still waiting for room has 0 there. Copied requests always come before waiting ones, so the
 * window holds the requests' data in queue order.
 *
 * IoStatus.Information counts the bytes of a request copied so far. A request with blocking
 * semantics (DriverContext[2] set) copies all its data, a part at a time as room frees up, and
 * completes once the transport has taken all of it; the others complete with what fitted.
 */
#define SEND_BYTES_IN_WINDOW(Irp) ((UINT)(ULONG_PTR)(Irp)->Tail.Overlay.DriverContext[3])
#define SEND_COPIES_ALL(Irp) ((Irp)->Tail.Overlay.DriverContext[2] != NULL)

static VOID
SetSendBytesInWindow(PIRP Irp, UINT Bytes)
{
    Irp->Tail.Overlay.DriverContext[3] = (PVOID)(ULONG_PTR)Bytes;
}

static UINT
AfdSendLength(PAFD_SEND_INFO SendReq)
{
    UINT i, Length = 0;

    for (i = 0; i < SendReq->BufferCount; i++)
        Length += SendReq->BufferArray[i].len;

    return Length;
}

/* TRUE if the request still has data that is not in the window */
static BOOLEAN
AfdSendHasMoreToCopy(PIRP Irp)
{
    PAFD_SEND_INFO SendReq;

    if (SEND_BYTES_IN_WINDOW(Irp) == 0 && Irp->IoStatus.Information == 0)
        return TRUE;

    if (!SEND_COPIES_ALL(Irp))
        return FALSE;

    SendReq = GetLockedData(Irp, IoGetCurrentIrpStackLocation(Irp));
    return Irp->IoStatus.Information < AfdSendLength(SendReq);
}

static BOOLEAN
AfdSendQueueHasWaiter(PAFD_FCB FCB)
{
    PIRP LastIrp;

    if (IsListEmpty(&FCB->PendingIrpList[FUNCTION_SEND]))
        return FALSE;

    LastIrp = CONTAINING_RECORD(FCB->PendingIrpList[FUNCTION_SEND].Blink, IRP, Tail.Overlay.ListEntry);
    return AfdSendHasMoreToCopy(LastIrp);
}

/* Copy the request's data from Offset on into the free part of the window; returns the bytes copied */
static UINT
AfdCopySendData(PAFD_FCB FCB, PAFD_SEND_INFO SendReq, UINT Offset)
{
    PAFD_MAPBUF Map = (PAFD_MAPBUF)(SendReq->BufferArray + SendReq->BufferCount);
    UINT SpaceAvail = FCB->Send.Size - FCB->Send.BytesUsed;
    UINT TotalBytesCopied = 0, BytesCopied, i;
    PCHAR Address;

    for (i = 0; SpaceAvail > 0 && i < SendReq->BufferCount; i++)
    {
        if (Offset >= SendReq->BufferArray[i].len)
        {
            Offset -= SendReq->BufferArray[i].len;
            continue;
        }

        if (!Map[i].Mdl)
            break;

        Address = MmGetSystemAddressForMdlSafe(Map[i].Mdl, NormalPagePriority);
        if (!Address)
            break;

        BytesCopied = MIN(SendReq->BufferArray[i].len - Offset, SpaceAvail);
        RtlCopyMemory(FCB->Send.Window + FCB->Send.BytesUsed, Address + Offset, BytesCopied);

        Offset = 0;
        TotalBytesCopied += BytesCopied;
        SpaceAvail -= BytesCopied;
        FCB->Send.BytesUsed += BytesCopied;
    }

    return TotalBytesCopied;
}

static VOID
AfdCompleteSendIrp(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PAFD_SEND_INFO SendReq = GetLockedData(Irp, IrpSp);

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    SetSendBytesInWindow(Irp, 0);
    AfdClearCancelRoutine(Irp);
    UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);
    if (Irp->MdlAddress) UnlockRequest(Irp, IrpSp);
    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
}

/* Fail every queued send request and drop the data in the window */
static VOID
AfdFailSendQueue(PAFD_FCB FCB, NTSTATUS Status)
{
    PLIST_ENTRY NextIrpEntry;
    PIRP NextIrp;

    while (!IsListEmpty(&FCB->PendingIrpList[FUNCTION_SEND]))
    {
        NextIrpEntry = RemoveHeadList(&FCB->PendingIrpList[FUNCTION_SEND]);
        NextIrp = CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);
        AfdCompleteSendIrp(NextIrp, Status, 0);
    }

    FCB->Send.BytesUsed = 0;
    FCB->SendBytesUnowned = 0;
}

/* Copy waiting requests into the free part of the window, in queue order */
static VOID
AfdFillSendWindow(PAFD_FCB FCB)
{
    PLIST_ENTRY Entry, NextEntry;
    PIRP NextIrp;
    PAFD_SEND_INFO SendReq;
    UINT SpaceAvail, SendLength, BytesCopied;

    for (Entry = FCB->PendingIrpList[FUNCTION_SEND].Flink;
         Entry != &FCB->PendingIrpList[FUNCTION_SEND];
         Entry = NextEntry)
    {
        NextEntry = Entry->Flink;
        NextIrp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);

        if (!AfdSendHasMoreToCopy(NextIrp))
            continue;

        SendReq = GetLockedData(NextIrp, IoGetCurrentIrpStackLocation(NextIrp));
        SendLength = AfdSendLength(SendReq);

        if (SendLength == 0)
        {
            RemoveEntryList(Entry);
            AfdCompleteSendIrp(NextIrp, STATUS_SUCCESS, 0);
            continue;
        }

        SpaceAvail = FCB->Send.Size - FCB->Send.BytesUsed;
        if (SpaceAvail == 0)
            break;

        /* Blocking requests that fit in the window wait for room for all their data */
        if (NextIrp->IoStatus.Information == 0 && SendLength > SpaceAvail &&
            SendLength <= FCB->Send.Size && SEND_COPIES_ALL(NextIrp))
        {
            break;
        }

        BytesCopied = AfdCopySendData(FCB, SendReq, (UINT)NextIrp->IoStatus.Information);
        if (BytesCopied == 0)
            break;

        NextIrp->IoStatus.Information += BytesCopied;
        SetSendBytesInWindow(NextIrp, SEND_BYTES_IN_WINDOW(NextIrp) + BytesCopied);

        if (AfdSendHasMoreToCopy(NextIrp))
            break;
    }
}

static VOID
AfdUpdateSendPollState(PAFD_FCB FCB)
{
    if (FCB->Send.Size - FCB->Send.BytesUsed != 0 && !FCB->SendClosed &&
        IsListEmpty(&FCB->PendingIrpList[FUNCTION_SEND]))
    {
        FCB->PollState |= AFD_EVENT_SEND;
        FCB->PollStatus[FD_WRITE_BIT] = STATUS_SUCCESS;
        PollReeval( FCB->DeviceExt, FCB->FileObject );
    }
    else
    {
        FCB->PollState &= ~AFD_EVENT_SEND;
    }
}

static IO_COMPLETION_ROUTINE SendComplete;

/*
 * The transport often completes a send at once, calling SendComplete from inside TdiSend, and
 * SendComplete starts the next send. Nested calls only ask the outermost one to send again, so a
 * long stream of sends does not overflow the kernel stack.
 */
static VOID
AfdStartSend(PAFD_FCB FCB)
{
    NTSTATUS Status;
    UINT Completions;

    if (FCB->SendStarting)
    {
        FCB->SendRestart = TRUE;
        return;
    }

    FCB->SendStarting = TRUE;
    do
    {
        FCB->SendRestart = FALSE;
        if (!FCB->SendIrp.InFlightRequest && FCB->Send.BytesUsed)
        {
            Completions = FCB->SendCompletions;
            Status = TdiSend(&FCB->SendIrp.InFlightRequest,
                             FCB->Connection.Object,
                             0,
                             FCB->Send.Window,
                             FCB->Send.BytesUsed,
                             SendComplete,
                             FCB);

            /* The request never reached the transport, so SendComplete will not run for it */
            if (!NT_SUCCESS(Status) && Completions == FCB->SendCompletions)
            {
                FCB->SendIrp.InFlightRequest = NULL;
                AfdFailSendQueue(FCB, Status);
                RetryDisconnectCompletion(FCB);
            }
        }
    } while (FCB->SendRestart);
    FCB->SendStarting = FALSE;
}

/*
 * Called by the cancel routine for a queued send request of a stream socket, before it removes
 * the request. Data already in the window cannot be taken back, since the transport may have part
 * of it: it stays in the stream. Returns the byte count the cancelled request reports: 0 when
 * all its data was copied, as Windows reports for a send it still delivers, and the bytes that
 * were copied when only part of it was.
 */
UINT
AfdCancelStreamSend(PAFD_FCB FCB, PIRP Irp)
{
    UINT InWindow = SEND_BYTES_IN_WINDOW(Irp);
    PLIST_ENTRY Previous = Irp->Tail.Overlay.ListEntry.Blink;
    PAFD_SEND_INFO SendReq = GetLockedData(Irp, IoGetCurrentIrpStackLocation(Irp));
    UINT Copied = (UINT)Irp->IoStatus.Information;
    UINT SendLength = AfdSendLength(SendReq);
    PIRP PreviousIrp;

    if (InWindow != 0)
    {
        /* The previous request's data comes just before it in the window */
        if (Previous == &FCB->PendingIrpList[FUNCTION_SEND])
        {
            FCB->SendBytesUnowned += InWindow;
        }
        else
        {
            PreviousIrp = CONTAINING_RECORD(Previous, IRP, Tail.Overlay.ListEntry);
            ASSERT(SEND_BYTES_IN_WINDOW(PreviousIrp) != 0);
            SetSendBytesInWindow(PreviousIrp, SEND_BYTES_IN_WINDOW(PreviousIrp) + InWindow);
        }

        SetSendBytesInWindow(Irp, 0);
    }

    return (Copied < SendLength) ? Copied : 0;
}

static NTSTATUS NTAPI SendComplete
( PDEVICE_OBJECT DeviceObject,
  PIRP Irp,
  PVOID Context ) {
    NTSTATUS Status = Irp->IoStatus.Status;
    PAFD_FCB FCB = (PAFD_FCB)Context;
    PLIST_ENTRY NextIrpEntry;
    PIRP NextIrp;
    UINT Sent, Taken, InWindow;

    UNREFERENCED_PARAMETER(DeviceObject);

    /*
     * The Irp parameter passed in is the IRP of the stream between AFD and
     * TDI driver. It's not very useful to us. We need the IRPs of the stream
     * between usermode and AFD. Those are chained from
     * FCB->PendingIrpList[FUNCTION_SEND] and you'll see them in the code
     * below as "NextIrp" ('cause they are the next usermode IRP to be
     * processed).
     */

    AFD_DbgPrint(MID_TRACE,("Called, status %x, %u bytes used\n",
                            Irp->IoStatus.Status,
                            Irp->IoStatus.Information));

    FCB->SendCompletions++;

    if( !SocketAcquireStateLock( FCB ) )
        return STATUS_FILE_CLOSED;

    ASSERT(FCB->SendIrp.InFlightRequest == Irp);
    FCB->SendIrp.InFlightRequest = NULL;
    /* Request is not in flight any longer */

    if( FCB->SharedData.State == SOCKET_STATE_CLOSED ) {
        /* Cleanup our IRP queue because the FCB is being destroyed */
        AfdFailSendQueue(FCB, STATUS_FILE_CLOSED);

        RetryDisconnectCompletion(FCB);

        SocketStateUnlock( FCB );
        return STATUS_FILE_CLOSED;
    }

    if( !NT_SUCCESS(Status) ) {
        /* Complete all following send IRPs with error */
        AfdFailSendQueue(FCB, Status);

        RetryDisconnectCompletion(FCB);

        SocketStateUnlock( FCB );

        return STATUS_SUCCESS;
    }

    /* The transport took the first bytes of the window */
    Sent = (UINT)Irp->IoStatus.Information;
    ASSERT(Sent <= FCB->Send.BytesUsed);
    Sent = MIN(Sent, FCB->Send.BytesUsed);

    RtlMoveMemory( FCB->Send.Window,
                   FCB->Send.Window + Sent,
                   FCB->Send.BytesUsed - Sent );
    FCB->Send.BytesUsed -= Sent;

    /* The transport has let go of the window, so a size asked for meanwhile can take effect */
    AfdApplyWindowResize(&FCB->Send, FCB->Send.BytesUsed);

    /* They belong to cancelled requests first, then to the queued requests in order */
    Taken = MIN(Sent, FCB->SendBytesUnowned);
    FCB->SendBytesUnowned -= Taken;
    Sent -= Taken;

    while (Sent > 0 && !IsListEmpty(&FCB->PendingIrpList[FUNCTION_SEND]))
    {
        NextIrpEntry = FCB->PendingIrpList[FUNCTION_SEND].Flink;
        NextIrp = CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);

        InWindow = SEND_BYTES_IN_WINDOW(NextIrp);
        ASSERT(InWindow != 0);
        if (InWindow == 0)
            break;

        /* If we didn't get enough, keep waiting */
        if (InWindow > Sent)
        {
            SetSendBytesInWindow(NextIrp, InWindow - Sent);
            Sent = 0;
            break;
        }

        Sent -= InWindow;

        /* A blocking request with data still to copy stays at the head */
        if (AfdSendHasMoreToCopy(NextIrp))
        {
            SetSendBytesInWindow(NextIrp, 0);
            break;
        }

        RemoveEntryList(NextIrpEntry);

        ASSERT(NextIrp->IoStatus.Information != 0);
        AfdCompleteSendIrp(NextIrp, STATUS_SUCCESS, NextIrp->IoStatus.Information);
    }

    ASSERT(Sent == 0);

    AfdFillSendWindow(FCB);

    AfdUpdateSendPollState(FCB);

    /* Some data is still waiting */
    if( FCB->Send.BytesUsed )
    {
        AfdStartSend(FCB);
    }
    else
    {
        /* Nothing is waiting so try to complete a pending disconnect */
        RetryDisconnectCompletion(FCB);
    }

    SocketStateUnlock( FCB );

    return STATUS_SUCCESS;
}

static IO_COMPLETION_ROUTINE PacketSocketSendComplete;
static NTSTATUS NTAPI PacketSocketSendComplete
( PDEVICE_OBJECT DeviceObject,
  PIRP Irp,
  PVOID Context ) {
    PAFD_FCB FCB = (PAFD_FCB)Context;
    PLIST_ENTRY NextIrpEntry;
    PIRP NextIrp;
    PAFD_SEND_INFO SendReq;

    UNREFERENCED_PARAMETER(DeviceObject);

    AFD_DbgPrint(MID_TRACE,("Called, status %x, %u bytes used\n",
                            Irp->IoStatus.Status,
                            Irp->IoStatus.Information));

    if( !SocketAcquireStateLock( FCB ) )
        return STATUS_FILE_CLOSED;

    ASSERT(FCB->SendIrp.InFlightRequest == Irp);
    FCB->SendIrp.InFlightRequest = NULL;
    /* Request is not in flight any longer */

    if( FCB->SharedData.State == SOCKET_STATE_CLOSED ) {
        /* Cleanup our IRP queue because the FCB is being destroyed */
        while( !IsListEmpty( &FCB->PendingIrpList[FUNCTION_SEND] ) ) {
            NextIrpEntry = RemoveHeadList(&FCB->PendingIrpList[FUNCTION_SEND]);
            NextIrp = CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);
            SendReq = GetLockedData(NextIrp, IoGetCurrentIrpStackLocation(NextIrp));
            NextIrp->IoStatus.Status = STATUS_FILE_CLOSED;
            NextIrp->IoStatus.Information = 0;
            AfdClearCancelRoutine(NextIrp);
            UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);
            UnlockRequest( NextIrp, IoGetCurrentIrpStackLocation( NextIrp ) );
            IoCompleteRequest( NextIrp, IO_NETWORK_INCREMENT );
        }
        SocketStateUnlock( FCB );
        return STATUS_FILE_CLOSED;
    }

    ASSERT(!IsListEmpty(&FCB->PendingIrpList[FUNCTION_SEND]));

    /* TDI spec guarantees FIFO ordering on IRPs */
    NextIrpEntry = RemoveHeadList(&FCB->PendingIrpList[FUNCTION_SEND]);
    NextIrp = CONTAINING_RECORD(NextIrpEntry, IRP, Tail.Overlay.ListEntry);

    SendReq = GetLockedData(NextIrp, IoGetCurrentIrpStackLocation(NextIrp));

    NextIrp->IoStatus.Status = Irp->IoStatus.Status;
    NextIrp->IoStatus.Information = Irp->IoStatus.Information;

    AfdClearCancelRoutine(NextIrp);

    UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);

    UnlockRequest(NextIrp, IoGetCurrentIrpStackLocation(NextIrp));

    IoCompleteRequest(NextIrp, IO_NETWORK_INCREMENT);

    FCB->PollState |= AFD_EVENT_SEND;
    FCB->PollStatus[FD_WRITE_BIT] = STATUS_SUCCESS;
    PollReeval(FCB->DeviceExt, FCB->FileObject);

    SocketStateUnlock(FCB);

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
AfdConnectedSocketWriteData(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                            PIO_STACK_LOCATION IrpSp, BOOLEAN Short) {
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PAFD_SEND_INFO SendReq;
    UINT TotalBytesCopied = 0, SpaceAvail = 0, SendLength;
    BOOLEAN Waiter, Blocking;
    KPROCESSOR_MODE LockMode;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Short);

    AFD_DbgPrint(MID_TRACE,("Called on %p\n", FCB));

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    FCB->EventSelectDisabled &= ~AFD_EVENT_SEND;

    if( FCB->Flags & AFD_ENDPOINT_CONNECTIONLESS )
    {
        PAFD_SEND_INFO_UDP SendReq;
        PTDI_CONNECTION_INFORMATION TargetAddress;

        /* Check that the socket is bound */
        if( FCB->SharedData.State != SOCKET_STATE_BOUND || !FCB->RemoteAddress )
        {
            AFD_DbgPrint(MIN_TRACE,("Invalid parameter\n"));
            return UnlockAndMaybeComplete( FCB, STATUS_INVALID_PARAMETER, Irp,
                                           0 );
        }

        if( !(SendReq = LockRequest( Irp, IrpSp, FALSE, &LockMode )) )
            return UnlockAndMaybeComplete( FCB, STATUS_NO_MEMORY, Irp, 0 );

        /* Must lock buffers before handing off user data */
        SendReq->BufferArray = LockBuffers( SendReq->BufferArray,
                                            SendReq->BufferCount,
                                            NULL, NULL,
                                            FALSE, FALSE, LockMode );

        if( !SendReq->BufferArray ) {
            return UnlockAndMaybeComplete( FCB, STATUS_ACCESS_VIOLATION,
                                           Irp, 0 );
        }

        Status = TdiBuildConnectionInfo( &TargetAddress, FCB->RemoteAddress );

        if( NT_SUCCESS(Status) ) {
            FCB->PollState &= ~AFD_EVENT_SEND;

            Status = QueueUserModeIrp(FCB, Irp, FUNCTION_SEND);
            if (Status == STATUS_PENDING)
            {
                if (SendReq->BufferCount > 1)
                {
                    AFD_DbgPrint(MIN_TRACE,("WARN: More than one buffer %ld\n", SendReq->BufferCount));
                }
                Status = TdiSendDatagram(&FCB->SendIrp.InFlightRequest,
                                         FCB->AddressFile.Object,
                                         SendReq->BufferArray[0].buf,
                                         SendReq->BufferArray[0].len,
                                         TargetAddress,
                                         PacketSocketSendComplete,
                                         FCB);
                if (Status != STATUS_PENDING)
                {
                    NT_VERIFY(RemoveHeadList(&FCB->PendingIrpList[FUNCTION_SEND]) == &Irp->Tail.Overlay.ListEntry);
                    Irp->IoStatus.Status = Status;
                    Irp->IoStatus.Information = ((Status == STATUS_SUCCESS) ? SendReq->BufferArray[0].len : 0);
                    AfdClearCancelRoutine(Irp);
                    UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);
                    UnlockRequest(Irp, IoGetCurrentIrpStackLocation(Irp));
                    IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
                }
            }

            ExFreePoolWithTag(TargetAddress, TAG_AFD_TDI_CONNECTION_INFORMATION);

            SocketStateUnlock(FCB);

            return STATUS_PENDING;
        }
        else
        {
            UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);
            return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
        }
    }

    if (FCB->PollState & AFD_EVENT_CLOSE)
    {
        AFD_DbgPrint(MIN_TRACE,("Connection reset by remote peer\n"));

        /* This is an unexpected remote disconnect */
        return UnlockAndMaybeComplete(FCB, FCB->PollStatus[FD_CLOSE_BIT], Irp, 0);
    }

    if (FCB->PollState & AFD_EVENT_ABORT)
    {
        AFD_DbgPrint(MIN_TRACE,("Connection aborted\n"));

        /* This is an abortive socket closure on our side */
        return UnlockAndMaybeComplete(FCB, FCB->PollStatus[FD_CLOSE_BIT], Irp, 0);
    }

    if (FCB->SendClosed)
    {
        AFD_DbgPrint(MIN_TRACE,("No more sends\n"));

        /* This is a graceful send closure */
        return UnlockAndMaybeComplete(FCB, STATUS_FILE_CLOSED, Irp, 0);
    }

    if( !(SendReq = LockRequest( Irp, IrpSp, FALSE, &LockMode )) )
        return UnlockAndMaybeComplete
            ( FCB, STATUS_NO_MEMORY, Irp, 0 );

    SendReq->BufferArray = LockBuffers( SendReq->BufferArray,
                                        SendReq->BufferCount,
                                        NULL, NULL,
                                        FALSE, FALSE, LockMode );

    if( !SendReq->BufferArray ) {
        return UnlockAndMaybeComplete( FCB, STATUS_ACCESS_VIOLATION,
                                       Irp, 0 );
    }

    AFD_DbgPrint(MID_TRACE,("Socket state %u\n", FCB->SharedData.State));

    if( FCB->SharedData.State != SOCKET_STATE_CONNECTED ) {
        AFD_DbgPrint(MID_TRACE,("Socket not connected\n"));
        UnlockBuffers( SendReq->BufferArray, SendReq->BufferCount, FALSE );
        return UnlockAndMaybeComplete( FCB, STATUS_INVALID_CONNECTION, Irp, 0 );
    }

    AFD_DbgPrint(MID_TRACE,("FCB->Send.BytesUsed = %u\n",
                            FCB->Send.BytesUsed));

    SpaceAvail = FCB->Send.Size - FCB->Send.BytesUsed;

    AFD_DbgPrint(MID_TRACE,("We can accept %u bytes\n",
                            SpaceAvail));

    /* Count the total transfer size */
    SendLength = AfdSendLength(SendReq);

    /* Data copied ahead of a waiting request would go out of order */
    Waiter = AfdSendQueueHasWaiter(FCB);
    Blocking = !((SendReq->AfdFlags & AFD_IMMEDIATE) || (FCB->NonBlocking));

    /* Blocking requests queue all their data, however large */
    Irp->Tail.Overlay.DriverContext[2] = Blocking ? (PVOID)1 : NULL;
    Irp->IoStatus.Information = 0;

    /* Make sure we've got the space */
    if (Waiter || SendLength > SpaceAvail)
    {
        /* Blocking sockets have to wait here */
        if (Blocking && (Waiter || SendLength <= FCB->Send.Size || SpaceAvail == 0))
        {
            FCB->PollState &= ~AFD_EVENT_SEND;
            SetSendBytesInWindow(Irp, 0);
            return LeaveIrpUntilLater(FCB, Irp, FUNCTION_SEND);
        }

        /* Check if we can send anything */
        if (Waiter || SpaceAvail == 0)
        {
            FCB->PollState &= ~AFD_EVENT_SEND;

            /* Non-overlapped sockets will fail if we can send nothing */
            if (!(SendReq->AfdFlags & AFD_OVERLAPPED))
            {
                UnlockBuffers( SendReq->BufferArray, SendReq->BufferCount, FALSE );
                return UnlockAndMaybeComplete( FCB, STATUS_CANT_WAIT, Irp, 0 );
            }
            else
            {
                /* Overlapped sockets just pend */
                SetSendBytesInWindow(Irp, 0);
                return LeaveIrpUntilLater(FCB, Irp, FUNCTION_SEND);
            }
        }
    }

    TotalBytesCopied = AfdCopySendData(FCB, SendReq, 0);
    SpaceAvail = FCB->Send.Size - FCB->Send.BytesUsed;

    Irp->IoStatus.Information = TotalBytesCopied;

    if (TotalBytesCopied == 0 && SendLength != 0)
    {
        UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);
        return UnlockAndMaybeComplete(FCB, STATUS_INSUFFICIENT_RESOURCES, Irp, 0);
    }

    if( TotalBytesCopied == 0 ) {
        AFD_DbgPrint(MID_TRACE,("Empty send\n"));
        UnlockBuffers( SendReq->BufferArray, SendReq->BufferCount, FALSE );
        return UnlockAndMaybeComplete
        ( FCB, STATUS_SUCCESS, Irp, TotalBytesCopied );
    }

    if (SpaceAvail)
    {
        FCB->PollState |= AFD_EVENT_SEND;
        FCB->PollStatus[FD_WRITE_BIT] = STATUS_SUCCESS;
        PollReeval( FCB->DeviceExt, FCB->FileObject );
    }
    else
    {
        FCB->PollState &= ~AFD_EVENT_SEND;
    }

    SetSendBytesInWindow(Irp, TotalBytesCopied);

    /* A request cancelled at once still leaves its data in the window */
    QueueUserModeIrp(FCB, Irp, FUNCTION_SEND);
    AfdStartSend(FCB);

    SocketStateUnlock(FCB);

    return STATUS_PENDING;
}

NTSTATUS NTAPI
AfdPacketSocketWriteData(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                         PIO_STACK_LOCATION IrpSp) {
    NTSTATUS Status = STATUS_SUCCESS;
    PTDI_CONNECTION_INFORMATION TargetAddress;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PAFD_SEND_INFO_UDP SendReq;
    KPROCESSOR_MODE LockMode;

    UNREFERENCED_PARAMETER(DeviceObject);

    AFD_DbgPrint(MID_TRACE,("Called on %p\n", FCB));

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    FCB->EventSelectDisabled &= ~AFD_EVENT_SEND;

    /* Check that the socket is bound */
    if( FCB->SharedData.State != SOCKET_STATE_BOUND &&
        FCB->SharedData.State != SOCKET_STATE_CREATED)
    {
        AFD_DbgPrint(MIN_TRACE,("Invalid socket state\n"));
        return UnlockAndMaybeComplete(FCB, STATUS_INVALID_PARAMETER, Irp, 0);
    }

    if (FCB->SendClosed)
    {
        AFD_DbgPrint(MIN_TRACE,("No more sends\n"));
        return UnlockAndMaybeComplete(FCB, STATUS_FILE_CLOSED, Irp, 0);
    }

    if( !(SendReq = LockRequest( Irp, IrpSp, FALSE, &LockMode )) )
        return UnlockAndMaybeComplete(FCB, STATUS_NO_MEMORY, Irp, 0);

    if (FCB->SharedData.State == SOCKET_STATE_CREATED)
    {
        if (FCB->LocalAddress)
        {
            ExFreePoolWithTag(FCB->LocalAddress, TAG_AFD_TRANSPORT_ADDRESS);
        }

        FCB->LocalAddress =
        TaBuildNullTransportAddress( ((PTRANSPORT_ADDRESS)SendReq->TdiConnection.RemoteAddress)->
                                      Address[0].AddressType );

        if( FCB->LocalAddress ) {
            Status = WarmSocketForBind( FCB, AFD_SHARE_WILDCARD );

            if( NT_SUCCESS(Status) )
                FCB->SharedData.State = SOCKET_STATE_BOUND;
            else
                return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
        } else
            return UnlockAndMaybeComplete
            ( FCB, STATUS_NO_MEMORY, Irp, 0 );
    }

    SendReq->BufferArray = LockBuffers( SendReq->BufferArray,
                                        SendReq->BufferCount,
                                        NULL, NULL,
                                        FALSE, FALSE, LockMode );

    if( !SendReq->BufferArray )
        return UnlockAndMaybeComplete( FCB, STATUS_ACCESS_VIOLATION,
                                       Irp, 0 );

    AFD_DbgPrint
        (MID_TRACE,("RemoteAddress #%d Type %u\n",
                    ((PTRANSPORT_ADDRESS)SendReq->TdiConnection.RemoteAddress)->
                    TAAddressCount,
                    ((PTRANSPORT_ADDRESS)SendReq->TdiConnection.RemoteAddress)->
                    Address[0].AddressType));

    Status = TdiBuildConnectionInfo( &TargetAddress,
                            ((PTRANSPORT_ADDRESS)SendReq->TdiConnection.RemoteAddress) );

    /* Check the size of the Address given ... */

    if( NT_SUCCESS(Status) ) {
        FCB->PollState &= ~AFD_EVENT_SEND;

        Status = QueueUserModeIrp(FCB, Irp, FUNCTION_SEND);
        if (Status == STATUS_PENDING)
        {
            if (SendReq->BufferCount > 1)
            {
                AFD_DbgPrint(MIN_TRACE,("WARN: More than one buffer %ld\n", SendReq->BufferCount));
            }
            Status = TdiSendDatagram(&FCB->SendIrp.InFlightRequest,
                                     FCB->AddressFile.Object,
                                     SendReq->BufferArray[0].buf,
                                     SendReq->BufferArray[0].len,
                                     TargetAddress,
                                     PacketSocketSendComplete,
                                     FCB);
            if (Status != STATUS_PENDING)
            {
                NT_VERIFY(RemoveHeadList(&FCB->PendingIrpList[FUNCTION_SEND]) == &Irp->Tail.Overlay.ListEntry);
                Irp->IoStatus.Status = Status;
                Irp->IoStatus.Information = ((Status == STATUS_SUCCESS) ? SendReq->BufferArray[0].len : 0);
                AfdClearCancelRoutine(Irp);
                UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);
                UnlockRequest(Irp, IoGetCurrentIrpStackLocation(Irp));
                IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
            }
        }

        ExFreePoolWithTag(TargetAddress, TAG_AFD_TDI_CONNECTION_INFORMATION);

        SocketStateUnlock(FCB);

        return STATUS_PENDING;
    }
    else
    {
        UnlockBuffers(SendReq->BufferArray, SendReq->BufferCount, FALSE);
        return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
    }
}
