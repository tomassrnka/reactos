/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NDIS User I/O driver
 * FILE:        ndisuio.h
 * PURPOSE:     NDISUIO definitions
 */

#ifndef __NDISUIO_H
#define __NDISUIO_H

#include <ndis.h>
#include <nuiouser.h>

extern PDEVICE_OBJECT GlobalDeviceObject;
extern NDIS_HANDLE GlobalProtocolHandle;
extern LIST_ENTRY GlobalAdapterList;
extern KSPIN_LOCK GlobalAdapterListLock;

/* One NDIS request and its own completion. NDIS completes a pending request
   through NduRequestComplete with that request, so the waiter is found from
   it: several requests can be pending on one adapter at a time. */
typedef struct _NDISUIO_REQUEST
{
    NDIS_REQUEST Request;
    KEVENT Event;
    NDIS_STATUS Status;
} NDISUIO_REQUEST, *PNDISUIO_REQUEST;

/* One send and its own completion, as for a request: NDIS completes a pending
   send through NduSendComplete with its packet, which points to this (in the
   packet's ProtocolReserved). Several writes can be pending on one adapter, and
   each waits for its own packet, which the miniport holds until then. */
typedef struct _NDISUIO_SEND
{
    KEVENT Event;
    NDIS_STATUS Status;
} NDISUIO_SEND, *PNDISUIO_SEND;

/* A received packet whose data NdisTransferData copies, maybe later
   (NduTransferDataComplete): the buffer that gets the header and data, in the
   packet's ProtocolReserved. The transfer holds a reference on the adapter. */
typedef struct _NDISUIO_TRANSFER
{
    PVOID PacketBuffer;
    ULONG HeaderSize;
} NDISUIO_TRANSFER, *PNDISUIO_TRANSFER;

C_ASSERT(sizeof(PNDISUIO_SEND) <= PROTOCOL_RESERVED_SIZE_IN_PACKET);
C_ASSERT(sizeof(NDISUIO_TRANSFER) <= PROTOCOL_RESERVED_SIZE_IN_PACKET);

#define NDISUIO_PACKET_SEND(Packet)     (*(PNDISUIO_SEND *)&(Packet)->ProtocolReserved[0])
#define NDISUIO_PACKET_TRANSFER(Packet) ((PNDISUIO_TRANSFER)&(Packet)->ProtocolReserved[0])

typedef struct _NDISUIO_ADAPTER_CONTEXT
{
    /* Asynchronous completion of the open and the close (one at a time: the bind and
       the unbind). Sends, requests and transfers have their own. */
    NDIS_STATUS AsyncStatus;
    KEVENT AsyncEvent;

    /* NDIS binding information */
    NDIS_HANDLE BindingHandle;

    /* Reference count information */
    ULONG OpenCount;
    LIST_ENTRY OpenEntryList;

    /* The binding's own reference, and one per read, write or IOCTL using the
       adapter (ReferenceFileAdapter): the unbind frees the context once the
       last one is gone */
    volatile LONG RefCount;
    KEVENT RefsGoneEvent;

    /* The adapter is being unbound: no new open or call, and the reads that
       wait for a packet end (UnbindingEvent is set) */
    BOOLEAN Unbinding;
    KEVENT UnbindingEvent;

    /* NDIS pools */
    NDIS_HANDLE PacketPoolHandle;
    NDIS_HANDLE BufferPoolHandle;

    /* Receive packet list */
    LIST_ENTRY PacketList;
    KEVENT PacketReadEvent;

    /* Mac options */
    ULONG MacOptions;

    /* Device name */
    UNICODE_STRING DeviceName;

    /* Global list entry */
    LIST_ENTRY ListEntry;

    /* Spin lock */
    KSPIN_LOCK Spinlock;
} NDISUIO_ADAPTER_CONTEXT, *PNDISUIO_ADAPTER_CONTEXT;

typedef struct _NDISUIO_OPEN_ENTRY
{
    /* File object */
    PFILE_OBJECT FileObject;

    /* Tracks how this adapter was opened (write-only or read-write) */
    BOOLEAN WriteOnly;

    /* List entry */
    LIST_ENTRY ListEntry;
} NDISUIO_OPEN_ENTRY, *PNDISUIO_OPEN_ENTRY;

typedef struct _NDISUIO_PACKET_ENTRY
{
    /* Length of data at the end of the struct */
    ULONG PacketLength;

    /* Entry on the packet list */
    LIST_ENTRY ListEntry;

    /* Packet data */
    UCHAR PacketData[1];
} NDISUIO_PACKET_ENTRY, *PNDISUIO_PACKET_ENTRY;

/* NDIS version info */
#define NDIS_MAJOR_VERSION 5
#define NDIS_MINOR_VERSION 0

/* createclose.c */
NTSTATUS
NTAPI
NduDispatchCreate(PDEVICE_OBJECT DeviceObject,
                  PIRP Irp);

NTSTATUS
NTAPI
NduDispatchClose(PDEVICE_OBJECT DeviceObject,
                 PIRP Irp);

/* ioctl.c */
NTSTATUS
NTAPI
NduDispatchDeviceControl(PDEVICE_OBJECT DeviceObject,
                         PIRP Irp);

/* misc.c */
NDIS_STATUS
AllocateAndChainBuffer(PNDISUIO_ADAPTER_CONTEXT AdapterContext,
                       PNDIS_PACKET Packet,
                       PVOID Buffer,
                       ULONG BufferSize,
                       BOOLEAN Front);

PNDIS_PACKET
CreatePacketFromPoolBuffer(PNDISUIO_ADAPTER_CONTEXT AdapterContext,
                           PVOID Buffer,
                           ULONG BufferSize);

VOID
CleanupAndFreePacket(PNDIS_PACKET Packet,
                     BOOLEAN FreePool);

PNDISUIO_ADAPTER_CONTEXT
FindAdapterContextByName(PNDIS_STRING DeviceName);

PNDISUIO_ADAPTER_CONTEXT
ReferenceFileAdapter(PFILE_OBJECT FileObject,
                     PBOOLEAN WriteOnly);

BOOLEAN
ReferenceAdapterContext(PNDISUIO_ADAPTER_CONTEXT AdapterContext);

VOID
DereferenceAdapterContext(PNDISUIO_ADAPTER_CONTEXT AdapterContext);

/* protocol.c */
VOID
NTAPI
NduOpenAdapterComplete(NDIS_HANDLE ProtocolBindingContext,
                       NDIS_STATUS Status,
                       NDIS_STATUS OpenStatus);

VOID
NTAPI
NduCloseAdapterComplete(NDIS_HANDLE ProtocolBindingContext,
                        NDIS_STATUS Status);

NDIS_STATUS
NTAPI
NduNetPnPEvent(NDIS_HANDLE ProtocolBindingContext,
               PNET_PNP_EVENT NetPnPEvent);

VOID
NTAPI
NduSendComplete(NDIS_HANDLE ProtocolBindingContext,
                PNDIS_PACKET Packet,
                NDIS_STATUS Status);

VOID
NTAPI
NduTransferDataComplete(NDIS_HANDLE ProtocolBindingContext,
                        PNDIS_PACKET Packet,
                        NDIS_STATUS Status,
                        UINT BytesTransferred);

VOID
NTAPI
NduResetComplete(NDIS_HANDLE ProtocolBindingContext,
                 NDIS_STATUS Status);

VOID
NduInitRequest(PNDISUIO_REQUEST Request);

NDIS_STATUS
NduIssueRequest(NDIS_HANDLE BindingHandle,
                PNDISUIO_REQUEST Request);

VOID
NTAPI
NduRequestComplete(NDIS_HANDLE ProtocolBindingContext,
                   PNDIS_REQUEST NdisRequest,
                   NDIS_STATUS Status);

NDIS_STATUS
NTAPI
NduReceive(NDIS_HANDLE ProtocolBindingContext,
           NDIS_HANDLE MacReceiveContext,
           PVOID HeaderBuffer,
           UINT HeaderBufferSize,
           PVOID LookAheadBuffer,
           UINT LookaheadBufferSize,
           UINT PacketSize);

VOID
NTAPI
NduReceiveComplete(NDIS_HANDLE ProtocolBindingContext);

VOID
NTAPI
NduStatus(NDIS_HANDLE ProtocolBindingContext,
          NDIS_STATUS GeneralStatus,
          PVOID StatusBuffer,
          UINT StatusBufferSize);

VOID
NTAPI
NduStatusComplete(NDIS_HANDLE ProtocolBindingContext);

VOID
NTAPI
NduBindAdapter(PNDIS_STATUS Status,
               NDIS_HANDLE BindContext,
               PNDIS_STRING DeviceName,
               PVOID SystemSpecific1,
               PVOID SystemSpecific2);

VOID
NTAPI
NduUnbindAdapter(PNDIS_STATUS Status,
                 NDIS_HANDLE ProtocolBindingContext,
                 NDIS_HANDLE UnbindContext);

/* readwrite.c */
NTSTATUS
NTAPI
NduDispatchRead(PDEVICE_OBJECT DeviceObject,
                PIRP Irp);

NTSTATUS
NTAPI
NduDispatchWrite(PDEVICE_OBJECT DeviceObject,
                 PIRP Irp);

#endif /* __NDISUIO_H */
