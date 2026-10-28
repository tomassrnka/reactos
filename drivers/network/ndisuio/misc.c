/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NDIS User I/O driver
 * FILE:        misc.c
 * PURPOSE:     Helper functions
 * PROGRAMMERS: Cameron Gutman (cameron.gutman@reactos.org)
 */

#include "ndisuio.h"

#define NDEBUG
#include <debug.h>

NDIS_STATUS
AllocateAndChainBuffer(PNDISUIO_ADAPTER_CONTEXT AdapterContext,
                       PNDIS_PACKET Packet,
                       PVOID Buffer,
                       ULONG BufferSize,
                       BOOLEAN Front)
{
    NDIS_STATUS Status;
    PNDIS_BUFFER NdisBuffer;

    /* Allocate the NDIS buffer mapping the pool */
    NdisAllocateBuffer(&Status,
                       &NdisBuffer,
                       AdapterContext->BufferPoolHandle,
                       Buffer,
                       BufferSize);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("No free buffer descriptors\n");
        return Status;
    }

    if (Front)
    {
        /* Chain the buffer to front */
        NdisChainBufferAtFront(Packet, NdisBuffer);
    }
    else
    {
        /* Chain the buffer to back */
        NdisChainBufferAtBack(Packet, NdisBuffer);
    }

    /* Return success */
    return NDIS_STATUS_SUCCESS;
}

PNDIS_PACKET
CreatePacketFromPoolBuffer(PNDISUIO_ADAPTER_CONTEXT AdapterContext,
                           PVOID Buffer,
                           ULONG BufferSize)
{
    PNDIS_PACKET Packet;
    NDIS_STATUS Status;

    /* Allocate a packet descriptor */
    NdisAllocatePacket(&Status,
                       &Packet,
                       AdapterContext->PacketPoolHandle);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DPRINT1("No free packet descriptors\n");
        return NULL;
    }

    /* Use the helper to chain the buffer */
    Status = AllocateAndChainBuffer(AdapterContext, Packet,
                                    Buffer, BufferSize, TRUE);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NdisFreePacket(Packet);
        return NULL;
    }

    /* Return the packet */
    return Packet;
}

VOID
CleanupAndFreePacket(PNDIS_PACKET Packet, BOOLEAN FreePool)
{
    PNDIS_BUFFER Buffer;
    PVOID Data;
    ULONG Length;

    /* Free each buffer and its backing pool memory */
    while (TRUE)
    {
        /* Unchain each buffer */
        NdisUnchainBufferAtFront(Packet, &Buffer);
        if (!Buffer)
            break;

        /* Get the backing memory */
        NdisQueryBuffer(Buffer, &Data, &Length);

        /* Free the buffer */
        NdisFreeBuffer(Buffer);

        if (FreePool)
        {
            /* Free the backing memory */
            ExFreePool(Data);
        }
    }

    /* Free the packet descriptor */
    NdisFreePacket(Packet);
}

/* Called with GlobalAdapterListLock held: the context stays valid only while it is held */
PNDISUIO_ADAPTER_CONTEXT
FindAdapterContextByName(PNDIS_STRING DeviceName)
{
    PLIST_ENTRY CurrentEntry;
    PNDISUIO_ADAPTER_CONTEXT AdapterContext;

    CurrentEntry = GlobalAdapterList.Flink;
    while (CurrentEntry != &GlobalAdapterList)
    {
        AdapterContext = CONTAINING_RECORD(CurrentEntry, NDISUIO_ADAPTER_CONTEXT, ListEntry);

        /* Check if the device name matches */
        if (RtlEqualUnicodeString(&AdapterContext->DeviceName, DeviceName, TRUE))
            return AdapterContext;

        CurrentEntry = CurrentEntry->Flink;
    }

    return NULL;
}

/* The adapter a handle is bound to, referenced for one call (DereferenceAdapterContext), or NULL when
   it has none or the adapter is being unbound. GlobalAdapterListLock orders this with the unbind, which
   detaches and frees the handles'\''' open entries under it: the open entry must not be touched after the
   lock is dropped, so its WriteOnly flag is captured here instead of returning the entry. */
PNDISUIO_ADAPTER_CONTEXT
ReferenceFileAdapter(PFILE_OBJECT FileObject,
                     PBOOLEAN WriteOnly)
{
    PNDISUIO_ADAPTER_CONTEXT AdapterContext;
    PNDISUIO_OPEN_ENTRY OpenEntry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&GlobalAdapterListLock, &OldIrql);
    AdapterContext = FileObject->FsContext;
    if (AdapterContext != NULL && !AdapterContext->Unbinding)
    {
        InterlockedIncrement(&AdapterContext->RefCount);
        if (WriteOnly != NULL)
        {
            OpenEntry = FileObject->FsContext2;
            *WriteOnly = OpenEntry->WriteOnly;
        }
    }
    else
    {
        AdapterContext = NULL;
    }
    KeReleaseSpinLock(&GlobalAdapterListLock, OldIrql);

    return AdapterContext;
}

VOID
DereferenceAdapterContext(PNDISUIO_ADAPTER_CONTEXT AdapterContext)
{
    /* The last one lets the unbind free the context */
    if (InterlockedDecrement(&AdapterContext->RefCount) == 0)
        KeSetEvent(&AdapterContext->RefsGoneEvent, IO_NO_INCREMENT, FALSE);
}
