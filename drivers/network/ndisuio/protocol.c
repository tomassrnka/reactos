/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NDIS User I/O driver
 * FILE:        protocol.c
 * PURPOSE:     Protocol stuff
 * PROGRAMMERS: Cameron Gutman (cameron.gutman@reactos.org)
 */

#include "ndisuio.h"

#define NDEBUG
#include <debug.h>

VOID
NTAPI
NduOpenAdapterComplete(NDIS_HANDLE ProtocolBindingContext,
                       NDIS_STATUS Status,
                       NDIS_STATUS OpenStatus)
{
    PNDISUIO_ADAPTER_CONTEXT AdapterContext = ProtocolBindingContext;

    DPRINT("Asynchronous adapter open completed\n");

    /* Store the final status and signal the event */
    AdapterContext->AsyncStatus = Status;
    KeSetEvent(&AdapterContext->AsyncEvent, IO_NO_INCREMENT, FALSE);
}

VOID
NTAPI
NduCloseAdapterComplete(NDIS_HANDLE ProtocolBindingContext,
                        NDIS_STATUS Status)
{
    PNDISUIO_ADAPTER_CONTEXT AdapterContext = ProtocolBindingContext;

    DPRINT("Asynchronous adapter close completed\n");

    /* Store the final status and signal the event */
    AdapterContext->AsyncStatus = Status;
    KeSetEvent(&AdapterContext->AsyncEvent, IO_NO_INCREMENT, FALSE);
}

NDIS_STATUS
NTAPI
NduNetPnPEvent(NDIS_HANDLE ProtocolBindingContext,
               PNET_PNP_EVENT NetPnPEvent)
{
    PNDIS_DEVICE_POWER_STATE PowerState;

    DPRINT("NetPnPEvent\n");

    switch (NetPnPEvent->NetEvent)
    {
        case NetEventQueryRemoveDevice:
            /* Nothing to do */
            DPRINT1("NetPnPEvent: QueryRemoveDevice\n");
            return NDIS_STATUS_SUCCESS;

        case NetEventSetPower:
            ASSERT(NetPnPEvent->BufferLength >= sizeof(*PowerState));

            PowerState = NetPnPEvent->Buffer;
            switch (*PowerState)
            {
                case NdisDeviceStateD0:
                    DPRINT1("NetPnPEvent: SetPower D0\n");
                    return NDIS_STATUS_SUCCESS;

                default:
                    DPRINT1("NetPnPEvent: SetPower state %d not supported\n", *PowerState);
                    return NDIS_STATUS_FAILURE;
            }

        case NetEventQueryPower:
            DPRINT1("NetPnPEvent: QueryPower\n");
            return NDIS_STATUS_SUCCESS;

        case NetEventCancelRemoveDevice:
            DPRINT1("NetPnPEvent: CancelRemoveDevice\n");
            return NDIS_STATUS_SUCCESS;

        case NetEventReconfigure:
            DPRINT1("NetPnPEvent: Reconfigure\n");
            return NDIS_STATUS_SUCCESS;

        case NetEventBindList:
            DPRINT1("NetPnPEvent: BindList\n");
            return NDIS_STATUS_SUCCESS;

        case NetEventBindsComplete:
            DPRINT("NetPnPEvent: BindsComplete\n");
            return NDIS_STATUS_SUCCESS;

        case NetEventPnPCapabilities:
            DPRINT1("NetPnPEvent: PnPCapabilities\n");
            return NDIS_STATUS_SUCCESS;

        default:
            DPRINT1("NetPnPEvent unimplemented for net event 0x%x\n", NetPnPEvent->NetEvent);
            return NDIS_STATUS_FAILURE;
    }
}

VOID
NTAPI
NduSendComplete(NDIS_HANDLE ProtocolBindingContext,
                PNDIS_PACKET Packet,
                NDIS_STATUS Status)
{
    PNDISUIO_ADAPTER_CONTEXT AdapterContext = ProtocolBindingContext;

    DPRINT("Asynchronous adapter send completed\n");

    /* Store the final status and signal the event */
    AdapterContext->AsyncStatus = Status;
    KeSetEvent(&AdapterContext->AsyncEvent, IO_NO_INCREMENT, FALSE);
}

VOID
NTAPI
NduTransferDataComplete(NDIS_HANDLE ProtocolBindingContext,
                        PNDIS_PACKET Packet,
                        NDIS_STATUS Status,
                        UINT BytesTransferred)
{
    PNDISUIO_ADAPTER_CONTEXT AdapterContext = ProtocolBindingContext;

    DPRINT("Asynchronous adapter transfer completed\n");

    /* Store the final status and signal the event */
    AdapterContext->AsyncStatus = Status;
    KeSetEvent(&AdapterContext->AsyncEvent, IO_NO_INCREMENT, FALSE);
}

VOID
NTAPI
NduResetComplete(NDIS_HANDLE ProtocolBindingContext,
                 NDIS_STATUS Status)
{
    PNDISUIO_ADAPTER_CONTEXT AdapterContext = ProtocolBindingContext;

    DPRINT("Asynchronous adapter reset completed\n");

    /* Store the final status and signal the event */
    AdapterContext->AsyncStatus = Status;
    KeSetEvent(&AdapterContext->AsyncEvent, IO_NO_INCREMENT, FALSE);
}

VOID
NTAPI
NduRequestComplete(NDIS_HANDLE ProtocolBindingContext,
                   PNDIS_REQUEST NdisRequest,
                   NDIS_STATUS Status)
{
    PNDISUIO_REQUEST Request = CONTAINING_RECORD(NdisRequest, NDISUIO_REQUEST, Request);

    UNREFERENCED_PARAMETER(ProtocolBindingContext);

    DPRINT("Asynchronous adapter request completed\n");

    /* Store the status of this request and wake the one waiting for it */
    Request->Status = Status;
    KeSetEvent(&Request->Event, IO_NO_INCREMENT, FALSE);
}

VOID
NduInitRequest(PNDISUIO_REQUEST Request)
{
    RtlZeroMemory(&Request->Request, sizeof(Request->Request));
    KeInitializeEvent(&Request->Event, NotificationEvent, FALSE);
    Request->Status = NDIS_STATUS_SUCCESS;
}

/* Sends the request and waits for it. The request and the buffers it points
   to belong to the caller's frame until this returns. */
NDIS_STATUS
NduIssueRequest(NDIS_HANDLE BindingHandle,
                PNDISUIO_REQUEST Request)
{
    NDIS_STATUS Status;

    NdisRequest(&Status, BindingHandle, &Request->Request);
    if (Status == NDIS_STATUS_PENDING)
    {
        KeWaitForSingleObject(&Request->Event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        Status = Request->Status;
    }

    return Status;
}

NDIS_STATUS
NTAPI
NduReceive(NDIS_HANDLE ProtocolBindingContext,
           NDIS_HANDLE MacReceiveContext,
           PVOID HeaderBuffer,
           UINT HeaderBufferSize,
           PVOID LookAheadBuffer,
           UINT LookaheadBufferSize,
           UINT PacketSize)
{
    PNDISUIO_ADAPTER_CONTEXT AdapterContext = ProtocolBindingContext;
    PNDISUIO_PACKET_ENTRY PacketEntry;
    PVOID PacketBuffer;
    PNDIS_PACKET Packet;
    NDIS_STATUS Status;
    UINT BytesTransferred;

    DPRINT("Received a %d byte packet\n", PacketSize);

    /* Discard if nobody is waiting for it */
    if (AdapterContext->OpenCount == 0)
        return NDIS_STATUS_NOT_ACCEPTED;

    /* Allocate a buffer to hold the packet data and header */
    PacketBuffer = ExAllocatePool(NonPagedPool, PacketSize + HeaderBufferSize);
    if (!PacketBuffer)
        return NDIS_STATUS_NOT_ACCEPTED;

    /* Allocate the packet descriptor and buffer */
    Packet = CreatePacketFromPoolBuffer(AdapterContext,
                                        (PUCHAR)PacketBuffer + HeaderBufferSize,
                                        PacketSize);
    if (!Packet)
    {
        ExFreePool(PacketBuffer);
        return NDIS_STATUS_NOT_ACCEPTED;
    }

    /* Transfer the packet data into our data buffer */
    if (LookaheadBufferSize == PacketSize)
    {
        NdisCopyLookaheadData((PVOID)((PUCHAR)PacketBuffer + HeaderBufferSize),
                              LookAheadBuffer,
                              PacketSize,
                              AdapterContext->MacOptions);
        BytesTransferred = PacketSize;
    }
    else
    {
        NdisTransferData(&Status,
                         AdapterContext->BindingHandle,
                         MacReceiveContext,
                         0,
                         PacketSize,
                         Packet,
                         &BytesTransferred);
        if (Status == NDIS_STATUS_PENDING)
        {
            KeWaitForSingleObject(&AdapterContext->AsyncEvent,
                                  Executive,
                                  KernelMode,
                                  FALSE,
                                  NULL);
            Status = AdapterContext->AsyncStatus;
        }
        if (Status != NDIS_STATUS_SUCCESS)
        {
            DPRINT1("Failed to transfer data with status 0x%x\n", Status);
            CleanupAndFreePacket(Packet, FALSE);
            ExFreePool(PacketBuffer);
            return NDIS_STATUS_NOT_ACCEPTED;
        }
    }

    /* Copy the header data */
    RtlCopyMemory(PacketBuffer, HeaderBuffer, HeaderBufferSize);

    /* Free the packet descriptor and buffers
       but not the pool because we still need it */
    CleanupAndFreePacket(Packet, FALSE);

    /* Allocate a packet entry from pool */
    PacketEntry = ExAllocatePool(NonPagedPool, sizeof(NDISUIO_PACKET_ENTRY) + BytesTransferred + HeaderBufferSize - 1);
    if (!PacketEntry)
    {
        ExFreePool(PacketBuffer);
        return NDIS_STATUS_RESOURCES;
    }

    /* Initialize the packet entry and copy in packet data */
    PacketEntry->PacketLength = BytesTransferred + HeaderBufferSize;
    RtlCopyMemory(PacketEntry->PacketData, PacketBuffer, PacketEntry->PacketLength);

    /* Free the old buffer */
    ExFreePool(PacketBuffer);

    /* Insert the packet on the adapter's packet list */
    ExInterlockedInsertTailList(&AdapterContext->PacketList,
                                &PacketEntry->ListEntry,
                                &AdapterContext->Spinlock);

    /* Signal the read event */
    KeSetEvent(&AdapterContext->PacketReadEvent,
               IO_NETWORK_INCREMENT,
               FALSE);

    return NDIS_STATUS_SUCCESS;
}

VOID
NTAPI
NduReceiveComplete(NDIS_HANDLE ProtocolBindingContext)
{
    /* No op */
}

VOID
NTAPI
NduStatus(NDIS_HANDLE ProtocolBindingContext,
          NDIS_STATUS GeneralStatus,
          PVOID StatusBuffer,
          UINT StatusBufferSize)
{
    /* FIXME: Implement status tracking */
}

VOID
NTAPI
NduStatusComplete(NDIS_HANDLE ProtocolBindingContext)
{
    /* FIXME: Implement status tracking */
}

/*
 * Called by NDIS when the adapter goes away (it has failed the adapter's requests and sends by
 * then, so nothing here waits for the miniport). The handles bound to the adapter are detached:
 * they take no call any more, and a read waiting for a packet ends. The binding is closed once
 * no read, write or IOCTL uses it, and everything the binding had is freed.
 */
static
NDIS_STATUS
UnbindAdapterByContext(PNDISUIO_ADAPTER_CONTEXT AdapterContext)
{
    KIRQL OldIrql;
    PLIST_ENTRY CurrentEntry;
    PNDISUIO_OPEN_ENTRY OpenEntry;
    PNDISUIO_PACKET_ENTRY PacketEntry;
    NDIS_STATUS Status;

    DPRINT("Unbinding adapter %wZ\n", &AdapterContext->DeviceName);

    /* Nothing finds the adapter or uses it through a handle from here on */
    KeAcquireSpinLock(&GlobalAdapterListLock, &OldIrql);
    RemoveEntryList(&AdapterContext->ListEntry);
    AdapterContext->Unbinding = TRUE;

    KeAcquireSpinLockAtDpcLevel(&AdapterContext->Spinlock);
    while (!IsListEmpty(&AdapterContext->OpenEntryList))
    {
        CurrentEntry = RemoveHeadList(&AdapterContext->OpenEntryList);
        OpenEntry = CONTAINING_RECORD(CurrentEntry, NDISUIO_OPEN_ENTRY, ListEntry);

        /* Make sure the entry is sane */
        ASSERT(OpenEntry->FileObject);
        ASSERT(AdapterContext == OpenEntry->FileObject->FsContext);
        ASSERT(OpenEntry == OpenEntry->FileObject->FsContext2);

        /* The handle is not bound to an adapter any more (see NduDispatchClose) */
        OpenEntry->FileObject->FsContext = NULL;
        OpenEntry->FileObject->FsContext2 = NULL;
        AdapterContext->OpenCount--;

        ExFreePool(OpenEntry);
    }

    /* If this fails, we have a refcount mismatch somewhere */
    ASSERT(AdapterContext->OpenCount == 0);
    KeReleaseSpinLockFromDpcLevel(&AdapterContext->Spinlock);
    KeReleaseSpinLock(&GlobalAdapterListLock, OldIrql);

    /* End the reads waiting for a packet, then wait for the calls that use the adapter */
    KeSetEvent(&AdapterContext->UnbindingEvent, IO_NO_INCREMENT, FALSE);
    DereferenceAdapterContext(AdapterContext);
    KeWaitForSingleObject(&AdapterContext->RefsGoneEvent,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);

    /* Send the close request: no send or request of ours is in NDIS any more */
    NdisCloseAdapter(&Status,
                     AdapterContext->BindingHandle);

    /* Wait for a pending close */
    if (Status == NDIS_STATUS_PENDING)
    {
        KeWaitForSingleObject(&AdapterContext->AsyncEvent,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        Status = AdapterContext->AsyncStatus;
    }

    /* NDIS indicates nothing to the closed binding: free the packets nobody read */
    while (!IsListEmpty(&AdapterContext->PacketList))
    {
        CurrentEntry = RemoveHeadList(&AdapterContext->PacketList);
        PacketEntry = CONTAINING_RECORD(CurrentEntry, NDISUIO_PACKET_ENTRY, ListEntry);
        ExFreePool(PacketEntry);
    }

    NdisFreePacketPool(AdapterContext->PacketPoolHandle);
    NdisFreeBufferPool(AdapterContext->BufferPoolHandle);
    RtlFreeUnicodeString(&AdapterContext->DeviceName);

    /* Free the context */
    ExFreePool(AdapterContext);

    return Status;
}

static
NDIS_STATUS
BindAdapterByName(PNDIS_STRING DeviceName)
{
    NDIS_STATUS OpenErrorStatus;
    PNDISUIO_ADAPTER_CONTEXT AdapterContext;
    NDIS_MEDIUM SupportedMedia[1] = {NdisMedium802_3};
    UINT SelectedMedium;
    NDIS_STATUS Status;
    NDISUIO_REQUEST Request;
    ULONG NameLength;

    /* Allocate the adapter context */
    AdapterContext = ExAllocatePool(NonPagedPool, sizeof(*AdapterContext));
    if (!AdapterContext)
    {
        return NDIS_STATUS_RESOURCES;
    }

    /* Set up the adapter context */
    RtlZeroMemory(AdapterContext, sizeof(*AdapterContext));
    KeInitializeEvent(&AdapterContext->AsyncEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&AdapterContext->PacketReadEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&AdapterContext->RefsGoneEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&AdapterContext->UnbindingEvent, NotificationEvent, FALSE);
    KeInitializeSpinLock(&AdapterContext->Spinlock);
    InitializeListHead(&AdapterContext->PacketList);
    InitializeListHead(&AdapterContext->OpenEntryList);
    AdapterContext->OpenCount = 0;
    AdapterContext->RefCount = 1;

    /* Store the name without a terminator, so an open matches
     * whether or not the caller passes one */
    NameLength = DeviceName->Length;
    if (NameLength >= sizeof(WCHAR) &&
        DeviceName->Buffer[NameLength / sizeof(WCHAR) - 1] == UNICODE_NULL)
    {
        NameLength -= sizeof(WCHAR);
    }

    AdapterContext->DeviceName.Length =
    AdapterContext->DeviceName.MaximumLength = (USHORT)NameLength;
    AdapterContext->DeviceName.Buffer = ExAllocatePool(NonPagedPool, NameLength);
    if (!AdapterContext->DeviceName.Buffer)
    {
        ExFreePool(AdapterContext);
        return NDIS_STATUS_RESOURCES;
    }

    /* Copy the device name into the adapter context */
    RtlCopyMemory(AdapterContext->DeviceName.Buffer, DeviceName->Buffer, NameLength);

    DPRINT("Binding adapter %wZ\n", &AdapterContext->DeviceName);

    /* Create the buffer pool */
    NdisAllocateBufferPool(&Status,
                           &AdapterContext->BufferPoolHandle,
                           50);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("Failed to allocate buffer pool with status 0x%x\n", Status);
        RtlFreeUnicodeString(&AdapterContext->DeviceName);
        ExFreePool(AdapterContext);
        return Status;
    }

    /* Create the packet pool */
    NdisAllocatePacketPool(&Status,
                           &AdapterContext->PacketPoolHandle,
                           25,
                           PROTOCOL_RESERVED_SIZE_IN_PACKET);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("Failed to allocate packet pool with status 0x%x\n", Status);
        NdisFreeBufferPool(AdapterContext->BufferPoolHandle);
        RtlFreeUnicodeString(&AdapterContext->DeviceName);
        ExFreePool(AdapterContext);
        return Status;
    }

    /* Send the open request */
    NdisOpenAdapter(&Status,
                    &OpenErrorStatus,
                    &AdapterContext->BindingHandle,
                    &SelectedMedium,
                    SupportedMedia,
                    1,
                    GlobalProtocolHandle,
                    AdapterContext,
                    DeviceName,
                    0,
                    NULL);

    /* Wait for a pending open */
    if (Status == NDIS_STATUS_PENDING)
    {
        KeWaitForSingleObject(&AdapterContext->AsyncEvent,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        Status = AdapterContext->AsyncStatus;
    }

    /* Check the final status */
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("Failed to open adapter for bind with status 0x%x\n", Status);
        NdisFreePacketPool(AdapterContext->PacketPoolHandle);
        NdisFreeBufferPool(AdapterContext->BufferPoolHandle);
        RtlFreeUnicodeString(&AdapterContext->DeviceName);
        ExFreePool(AdapterContext);
        return Status;
    }

    /* Get the MAC options */
    NduInitRequest(&Request);
    Request.Request.RequestType = NdisRequestQueryInformation;
    Request.Request.DATA.QUERY_INFORMATION.Oid = OID_GEN_MAC_OPTIONS;
    Request.Request.DATA.QUERY_INFORMATION.InformationBuffer = &AdapterContext->MacOptions;
    Request.Request.DATA.QUERY_INFORMATION.InformationBufferLength = sizeof(ULONG);
    Status = NduIssueRequest(AdapterContext->BindingHandle, &Request);

    /* Check the final status */
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_STATUS CloseStatus;

        DPRINT1("Failed to get MAC options with status 0x%x\n", Status);

        NdisCloseAdapter(&CloseStatus,
                         AdapterContext->BindingHandle);
        if (CloseStatus == NDIS_STATUS_PENDING)
        {
            KeWaitForSingleObject(&AdapterContext->AsyncEvent,
                                  Executive,
                                  KernelMode,
                                  FALSE,
                                  NULL);
        }

        NdisFreePacketPool(AdapterContext->PacketPoolHandle);
        NdisFreeBufferPool(AdapterContext->BufferPoolHandle);
        RtlFreeUnicodeString(&AdapterContext->DeviceName);
        ExFreePool(AdapterContext);
        return Status;
    }

    /* Add the adapter context to the global list */
    ExInterlockedInsertTailList(&GlobalAdapterList,
                                &AdapterContext->ListEntry,
                                &GlobalAdapterListLock);

    return STATUS_SUCCESS;
}

VOID
NTAPI
NduBindAdapter(PNDIS_STATUS Status,
               NDIS_HANDLE BindContext,
               PNDIS_STRING DeviceName,
               PVOID SystemSpecific1,
               PVOID SystemSpecific2)
{
    /* Use our helper function to create a context for this adapter */
    *Status = BindAdapterByName(DeviceName);
}

VOID
NTAPI
NduUnbindAdapter(PNDIS_STATUS Status,
                 NDIS_HANDLE ProtocolBindingContext,
                 NDIS_HANDLE UnbindContext)
{
    /* This is forced unbind. UnbindAdapterByContext() will take care of
     * invalidating file handles pointer to this adapter for us */
    *Status = UnbindAdapterByContext(ProtocolBindingContext);
}
