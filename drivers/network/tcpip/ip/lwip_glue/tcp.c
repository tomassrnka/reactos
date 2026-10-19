#include <debug.h>
#include <lwip/tcpip.h>

#include "lwip_glue.h"

/* tcp_process_refused_data hands over data held while a connection waited to be accepted */
#include <lwip/priv/tcp_priv.h>

static const char * const tcp_state_str[] = {
  "CLOSED",
  "LISTEN",
  "SYN_SENT",
  "SYN_RCVD",
  "ESTABLISHED",
  "FIN_WAIT_1",
  "FIN_WAIT_2",
  "CLOSE_WAIT",
  "CLOSING",
  "LAST_ACK",
  "TIME_WAIT"
};

/* The way that lwIP does multi-threading is really not ideal for our purposes but
 * we best go along with it unless we want another unstable TCP library. lwIP uses
 * a thread called the "tcpip thread" which is the only one allowed to call raw API
 * functions. Since this is the case, for each of our LibTCP* functions, we queue a request
 * for a callback to "tcpip thread" which calls our LibTCP*Callback functions. Yes, this is
 * a lot of unnecessary thread swapping and it could definitely be faster, but I don't want
 * to going messing around in lwIP because I have no desire to create another mess like oskittcp
 *
 * lwIP's core locking (LOCK_TCPIP_CORE) lets any thread call the raw API while it holds the core
 * lock, as the tcpip thread does while it runs these callbacks. Sends and received packets use it
 * to stay on their own threads; the other requests still go through the tcpip thread. */

extern KEVENT TerminationEvent;
extern NPAGED_LOOKASIDE_LIST MessageLookasideList;
extern NPAGED_LOOKASIDE_LIST QueueEntryLookasideList;

/* Required for ERR_T to NTSTATUS translation in receive error handling */
NTSTATUS TCPTranslateError(const err_t err);

void
LibTCPDumpPcb(PVOID SocketContext)
{
    struct tcp_pcb *pcb = (struct tcp_pcb*)SocketContext;
    unsigned int addr = lwip_ntohl(pcb->remote_ip.addr);

    DbgPrint("\tState: %s\n", tcp_state_str[pcb->state]);
    DbgPrint("\tRemote: (%d.%d.%d.%d, %d)\n",
    (addr >> 24) & 0xFF,
    (addr >> 16) & 0xFF,
    (addr >> 8) & 0xFF,
    addr & 0xFF,
    pcb->remote_port);
}

static
void
LibTCPEmptyQueue(PCONNECTION_ENDPOINT Connection)
{
    PLIST_ENTRY Entry;
    PQUEUE_ENTRY qp = NULL;

    ReferenceObject(Connection);

    while (!IsListEmpty(&Connection->PacketQueue))
    {
        Entry = RemoveHeadList(&Connection->PacketQueue);
        qp = CONTAINING_RECORD(Entry, QUEUE_ENTRY, ListEntry);

        /* We're in the tcpip thread here so this is safe */
        pbuf_free(qp->p);

        ExFreeToNPagedLookasideList(&QueueEntryLookasideList, qp);
    }

    DereferenceObject(Connection);
}

void LibTCPEnqueuePacket(PCONNECTION_ENDPOINT Connection, struct pbuf *p)
{
    PQUEUE_ENTRY qp;

    qp = (PQUEUE_ENTRY)ExAllocateFromNPagedLookasideList(&QueueEntryLookasideList);
    qp->p = p;
    qp->Offset = 0;

    LockObject(Connection);
    InsertTailList(&Connection->PacketQueue, &qp->ListEntry);
    UnlockObject(Connection);
}

PQUEUE_ENTRY LibTCPDequeuePacket(PCONNECTION_ENDPOINT Connection)
{
    PLIST_ENTRY Entry;
    PQUEUE_ENTRY qp = NULL;

    if (IsListEmpty(&Connection->PacketQueue)) return NULL;

    Entry = RemoveHeadList(&Connection->PacketQueue);

    qp = CONTAINING_RECORD(Entry, QUEUE_ENTRY, ListEntry);

    return qp;
}

NTSTATUS LibTCPGetDataFromConnectionQueue(PCONNECTION_ENDPOINT Connection, PUCHAR RecvBuffer, UINT RecvLen, UINT *Received)
{
    PQUEUE_ENTRY qp;
    struct pbuf* p;
    NTSTATUS Status;
    UINT ReadLength, PayloadLength, Offset, Copied;

    (*Received) = 0;

    LockObject(Connection);

    if (!IsListEmpty(&Connection->PacketQueue))
    {
        while ((qp = LibTCPDequeuePacket(Connection)) != NULL)
        {
            p = qp->p;

            /* Calculate the payload length first */
            PayloadLength = p->tot_len;
            PayloadLength -= qp->Offset;
            Offset = qp->Offset;

            /* Check if we're reading the whole buffer */
            ReadLength = MIN(PayloadLength, RecvLen);
            ASSERT(ReadLength != 0);
            if (ReadLength != PayloadLength)
            {
                /* Save this one for later */
                qp->Offset += ReadLength;
                InsertHeadList(&Connection->PacketQueue, &qp->ListEntry);
                qp = NULL;
            }

            Copied = pbuf_copy_partial(p, RecvBuffer, ReadLength, Offset);
            ASSERT(Copied == ReadLength);

            /* Update trackers */
            RecvLen -= ReadLength;
            RecvBuffer += ReadLength;
            (*Received) += ReadLength;

            if (qp != NULL)
            {
                /* lwIP's memory functions may be called from any thread (SYS_LIGHTWEIGHT_PROT);
                 * the segment is ours since the receive callback took it */
                pbuf_free(qp->p);

                ExFreeToNPagedLookasideList(&QueueEntryLookasideList, qp);
            }
            else
            {
                /* If we get here, it means we've filled the buffer */
                ASSERT(RecvLen == 0);
            }

            ASSERT((*Received) != 0);
            Status = STATUS_SUCCESS;

            if (!RecvLen)
                break;
        }
    }
    else
    {
        if (Connection->ReceiveShutdown)
            Status = Connection->ReceiveShutdownStatus;
        else
            Status = STATUS_PENDING;
    }

    UnlockObject(Connection);

    return Status;
}

static
BOOLEAN
WaitForEventSafely(PRKEVENT Event)
{
    PVOID WaitObjects[] = {Event, &TerminationEvent};

    if (KeWaitForMultipleObjects(2,
                                 WaitObjects,
                                 WaitAny,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL,
                                 NULL) == STATUS_WAIT_0)
    {
        /* Signalled by the caller's event */
        return TRUE;
    }
    else /* if KeWaitForMultipleObjects() == STATUS_WAIT_1 */
    {
        /* Signalled by our termination event */
        return FALSE;
    }
}

static
err_t
InternalSendEventHandler(void *arg, PTCP_PCB pcb, const u16_t space)
{
    /* Make sure the socket didn't get closed */
    if (!arg) return ERR_OK;

    TCPSendEventHandler(arg, space);

    return ERR_OK;
}

static
err_t
InternalRecvEventHandler(void *arg, PTCP_PCB pcb, struct pbuf *p, const err_t err)
{
    PCONNECTION_ENDPOINT Connection = arg;

    /* Make sure the socket didn't get closed */
    if (!arg)
    {
        if (p)
            pbuf_free(p);

        return ERR_OK;
    }

    if (p)
    {
        /* A reader may consume and free the segment as soon as it is queued */
        u16_t Length = p->tot_len;

        LibTCPEnqueuePacket(Connection, p);

        tcp_recved(pcb, Length);

        TCPRecvEventHandler(arg);
    }
    else if (err == ERR_OK)
    {
        /* Complete pending reads with 0 bytes to indicate a graceful closure,
         * but note that send is still possible in this state so we don't close the
         * whole socket here (by calling tcp_close()) as that would violate TCP specs
         */
        Connection->ReceiveShutdown = TRUE;
        Connection->ReceiveShutdownStatus = STATUS_SUCCESS;

        /* If we already did a send shutdown, we're in TIME_WAIT so we can't use this PCB anymore */
        if (Connection->SendShutdown)
        {
            Connection->SocketContext = NULL;
            tcp_arg(pcb, NULL);
        }

        /* Indicate the graceful close event */
        TCPRecvEventHandler(arg);

        /* If the PCB is gone, clean up the connection */
        if (Connection->SendShutdown)
        {
            TCPFinEventHandler(Connection, ERR_CLSD);
        }
    }

    return ERR_OK;
}

/* An established connection that no listen request has taken yet */
typedef struct _PENDING_ACCEPT
{
    LIST_ENTRY ListEntry;
    PCONNECTION_ENDPOINT Listener;
    PTCP_PCB Pcb;
    BOOLEAN PeerClosed;
} PENDING_ACCEPT, *PPENDING_ACCEPT;

/* Bounds the connections a listener keeps waiting, like the lwIP backlog */
#define MAX_PENDING_ACCEPTS 255

static
err_t
InternalPendingRecvEventHandler(void *arg, PTCP_PCB pcb, struct pbuf *p, const err_t err)
{
    PPENDING_ACCEPT Pending = arg;

    /* Refusing the data makes lwIP keep it until the connection is accepted */
    if (p)
        return ERR_MEM;

    if (Pending && err == ERR_OK)
        Pending->PeerClosed = TRUE;

    return ERR_OK;
}

static
void
InternalPendingErrorEventHandler(void *arg, const err_t err)
{
    PPENDING_ACCEPT Pending = arg;
    PCONNECTION_ENDPOINT Listener;

    /* lwIP already freed the PCB */
    if (!Pending)
        return;

    Listener = Pending->Listener;
    RemoveEntryList(&Pending->ListEntry);
    Listener->PendingAcceptCount--;
    ExFreePoolWithTag(Pending, LWIP_ACCEPT_TAG);
    DereferenceObject(Listener);
}

static void LibTCPClaimPendingAcceptCallback(void *arg);

static
VOID
LibTCPPostClaim(PCONNECTION_ENDPOINT Listener, u8_t Block)
{
    struct lwip_callback_msg *msg;

    msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    if (!msg)
        return;

    ReferenceObject(Listener);
    msg->Input.Socket.Arg = Listener;
    if (tcpip_callback_with_block(LibTCPClaimPendingAcceptCallback, msg, Block) != ERR_OK)
    {
        DereferenceObject(Listener);
        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);
    }
}

static
BOOLEAN
LibTCPQueuePendingAccept(PCONNECTION_ENDPOINT Listener, PTCP_PCB pcb)
{
    PPENDING_ACCEPT Pending;

    if (Listener->PendingAcceptCount >= MAX_PENDING_ACCEPTS)
        return FALSE;

    Pending = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Pending), LWIP_ACCEPT_TAG);
    if (!Pending)
        return FALSE;

    /* Each pending connection keeps its listener */
    ReferenceObject(Listener);
    Pending->Listener = Listener;
    Pending->Pcb = pcb;
    Pending->PeerClosed = FALSE;
    InsertTailList(&Listener->PendingAccepts, &Pending->ListEntry);
    Listener->PendingAcceptCount++;

    tcp_arg(pcb, Pending);
    tcp_recv(pcb, InternalPendingRecvEventHandler);
    tcp_err(pcb, InternalPendingErrorEventHandler);
    tcp_sent(pcb, NULL);

    return TRUE;
}

/* This function MUST return an error value that is not ERR_ABRT or ERR_OK if the connection
 * is not accepted to avoid leaking the new PCB */
static
err_t
InternalAcceptEventHandler(void *arg, PTCP_PCB newpcb, const err_t err)
{
    /* Make sure the socket didn't get closed */
    if (!arg)
        return ERR_CLSD;

    /* lwIP reports a failed PCB allocation through this callback too */
    if (!newpcb || err != ERR_OK)
        return ERR_VAL;

    /* lwIP gave the new PCB the listener's argument; only LibTCPAccept may set it */
    tcp_arg(newpcb, NULL);

    /* Older connections are served first; the lwIP thread must not wait on its own queue */
    if (!IsListEmpty(&((PCONNECTION_ENDPOINT)arg)->PendingAccepts))
    {
        if (!LibTCPQueuePendingAccept(arg, newpcb))
            return ERR_CLSD;
        LibTCPPostClaim(arg, 0);
        return ERR_OK;
    }

    TCPAcceptEventHandler(arg, newpcb);

    /* Set in LibTCPAccept (called from TCPAcceptEventHandler) */
    if (newpcb->callback_arg)
        return ERR_OK;

    /* No listen request was queued, keep the connection until the next one comes */
    if (LibTCPQueuePendingAccept(arg, newpcb))
        return ERR_OK;

    return ERR_CLSD;
}

static
void
LibTCPClaimPendingAcceptCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;
    PCONNECTION_ENDPOINT Listener = msg->Input.Socket.Arg;
    PPENDING_ACCEPT Pending;
    PTCP_PCB pcb;
    BOOLEAN PeerClosed;

    /* A closed listener has already aborted its pending connections */
    while (Listener->SocketContext && !IsListEmpty(&Listener->PendingAccepts))
    {
        Pending = CONTAINING_RECORD(Listener->PendingAccepts.Flink, PENDING_ACCEPT, ListEntry);
        pcb = Pending->Pcb;

        tcp_arg(pcb, NULL);
        TCPAcceptEventHandler(Listener, pcb);
        if (!pcb->callback_arg)
        {
            /* No listen request took it, keep it waiting */
            tcp_arg(pcb, Pending);
            break;
        }

        RemoveEntryList(&Pending->ListEntry);
        Listener->PendingAcceptCount--;
        PeerClosed = Pending->PeerClosed;
        ExFreePoolWithTag(Pending, LWIP_ACCEPT_TAG);
        DereferenceObject(Listener);

        /* Hand over what arrived while the connection waited */
        if (pcb->refused_data)
            tcp_process_refused_data(pcb);
        else if (PeerClosed)
            InternalRecvEventHandler(pcb->callback_arg, pcb, NULL, ERR_OK);
    }

    DereferenceObject(Listener);
    ExFreeToNPagedLookasideList(&MessageLookasideList, msg);
}

VOID
LibTCPClaimPendingAccept(PCONNECTION_ENDPOINT Listener)
{
    /* Not waited for: the caller may hold locks the accept path takes */
    LibTCPPostClaim(Listener, 1);
}

/* Runs on the lwIP thread when the listener closes */
static
void
LibTCPAbortPendingAccepts(PCONNECTION_ENDPOINT Listener)
{
    PPENDING_ACCEPT Pending;

    while (!IsListEmpty(&Listener->PendingAccepts))
    {
        Pending = CONTAINING_RECORD(RemoveHeadList(&Listener->PendingAccepts), PENDING_ACCEPT, ListEntry);
        tcp_arg(Pending->Pcb, NULL);
        tcp_abort(Pending->Pcb);
        ExFreePoolWithTag(Pending, LWIP_ACCEPT_TAG);
        DereferenceObject(Listener);
    }
    Listener->PendingAcceptCount = 0;
}

static
err_t
InternalConnectEventHandler(void *arg, PTCP_PCB pcb, const err_t err)
{
    /* Make sure the socket didn't get closed */
    if (!arg)
        return ERR_OK;

    TCPConnectEventHandler(arg, err);

    return ERR_OK;
}

static
void
InternalErrorEventHandler(void *arg, const err_t err)
{
    PCONNECTION_ENDPOINT Connection = arg;

    /* Make sure the socket didn't get closed */
    if (!arg || Connection->SocketContext == NULL) return;

    /* The PCB is dead now */
    Connection->SocketContext = NULL;

    /* Give them one shot to receive the remaining data */
    Connection->ReceiveShutdown = TRUE;
    Connection->ReceiveShutdownStatus = TCPTranslateError(err);
    TCPRecvEventHandler(Connection);

    /* Terminate the connection */
    TCPFinEventHandler(Connection, err);
}

static
void
LibTCPSocketCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;

    ASSERT(msg);

    msg->Output.Socket.NewPcb = tcp_new();

    if (msg->Output.Socket.NewPcb)
    {
        tcp_arg(msg->Output.Socket.NewPcb, msg->Input.Socket.Arg);
        tcp_err(msg->Output.Socket.NewPcb, InternalErrorEventHandler);
    }

    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

struct tcp_pcb *
LibTCPSocket(void *arg)
{
    struct lwip_callback_msg *msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    struct tcp_pcb *ret;

    if (msg)
    {
        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);
        msg->Input.Socket.Arg = arg;

        tcpip_callback_with_block(LibTCPSocketCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
            ret = msg->Output.Socket.NewPcb;
        else
            ret = NULL;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);

        return ret;
    }

    return NULL;
}

static
void
LibTCPFreeSocketCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;

    ASSERT(msg);

    /* Calling tcp_close will free it */
    tcp_close(msg->Input.FreeSocket.pcb);

    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

void LibTCPFreeSocket(PTCP_PCB pcb)
{
    struct lwip_callback_msg msg;

    KeInitializeEvent(&msg.Event, NotificationEvent, FALSE);
    msg.Input.FreeSocket.pcb = pcb;

    tcpip_callback_with_block(LibTCPFreeSocketCallback, &msg, 1);

    WaitForEventSafely(&msg.Event);
}


static
void
LibTCPBindCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;
    PTCP_PCB pcb = msg->Input.Bind.Connection->SocketContext;

    ASSERT(msg);

    if (!msg->Input.Bind.Connection->SocketContext)
    {
        msg->Output.Bind.Error = ERR_CLSD;
        goto done;
    }

    /* We're guaranteed that the local address is valid to bind at this point */
    pcb->so_options |= SOF_REUSEADDR;

    msg->Output.Bind.Error = tcp_bind(pcb,
                                      msg->Input.Bind.IpAddress,
                                      lwip_ntohs(msg->Input.Bind.Port));

done:
    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

err_t
LibTCPBind(PCONNECTION_ENDPOINT Connection, ip4_addr_t *const ipaddr, const u16_t port)
{
    struct lwip_callback_msg *msg;
    err_t ret;

    msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    if (msg)
    {
        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);
        msg->Input.Bind.Connection = Connection;
        msg->Input.Bind.IpAddress = ipaddr;
        msg->Input.Bind.Port = port;

        tcpip_callback_with_block(LibTCPBindCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
            ret = msg->Output.Bind.Error;
        else
            ret = ERR_CLSD;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);

        return ret;
    }

    return ERR_MEM;
}

static
void
LibTCPListenCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;

    ASSERT(msg);

    if (!msg->Input.Listen.Connection->SocketContext)
    {
        msg->Output.Listen.NewPcb = NULL;
        goto done;
    }

    msg->Output.Listen.NewPcb = tcp_listen_with_backlog((PTCP_PCB)msg->Input.Listen.Connection->SocketContext, msg->Input.Listen.Backlog);

    if (msg->Output.Listen.NewPcb)
    {
        tcp_accept(msg->Output.Listen.NewPcb, InternalAcceptEventHandler);
    }

done:
    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

PTCP_PCB
LibTCPListen(PCONNECTION_ENDPOINT Connection, UINT Backlog)
{
    struct lwip_callback_msg *msg;
    PTCP_PCB ret;

    msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    if (msg)
    {
        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);
        msg->Input.Listen.Connection = Connection;
        /* lwIP keeps the backlog in a u8_t, a plain cast turned 1024 into 0 */
        msg->Input.Listen.Backlog = (u8_t)min(Backlog, 0xFF);

        tcpip_callback_with_block(LibTCPListenCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
            ret = msg->Output.Listen.NewPcb;
        else
            ret = NULL;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);

        return ret;
    }

    return NULL;
}

/* Called with the core lock held */
static
err_t
LibTCPSendLocked(PCONNECTION_ENDPOINT Connection, void *Data, u16_t DataLength, PTDI_BUCKET Bucket, u32_t *Information)
{
    PTCP_PCB pcb = Connection->SocketContext;
    ULONG SendLength;
    UCHAR SendFlags;
    err_t Error;

    ASSERT(sys_tcpip_core_locked());

    if (!pcb || Connection->SendShutdown)
        return ERR_CLSD;

    SendFlags = TCP_WRITE_FLAG_COPY;
    SendLength = DataLength;
    if (tcp_sndbuf(pcb) == 0)
    {
        /* No buffer space so return pending */
        Error = ERR_INPROGRESS;
    }
    else
    {
        if (tcp_sndbuf(pcb) < SendLength)
        {
            /* We've got some room so let's send what we can */
            SendLength = tcp_sndbuf(pcb);

            /* Don't set the push flag */
            SendFlags |= TCP_WRITE_FLAG_MORE;
        }

        Error = tcp_write(pcb, Data, SendLength, SendFlags);
        if (Error == ERR_OK)
        {
            /* Queued successfully so try to send it */
            tcp_output(pcb);
            *Information = SendLength;
        }
        else if (Error == ERR_MEM)
        {
            /* The queue is too long */
            Error = ERR_INPROGRESS;
        }
    }

    /* Sent events are delivered under the core lock, so the request is in the queue
     * before the next one can look for it */
    if (Error == ERR_INPROGRESS && Bucket)
    {
        LockObject(Connection);
        InsertTailList(&Connection->SendRequest, &Bucket->Entry);
        UnlockObject(Connection);
    }

    return Error;
}

static
void
LibTCPSendCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;

    ASSERT(msg);

    msg->Output.Send.Error = LibTCPSendLocked(msg->Input.Send.Connection,
                                              msg->Input.Send.Data,
                                              msg->Input.Send.DataLength,
                                              msg->Input.Send.Bucket,
                                              &msg->Output.Send.Information);

    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

/* Stack a sender must have left to run lwIP's output path on its own thread */
#define LIBTCP_SEND_INLINE_STACK (KERNEL_STACK_SIZE / 2)

/* Bucket, if not NULL, is queued as a waiting send request if the data cannot be sent now */
err_t
LibTCPSend(PCONNECTION_ENDPOINT Connection, void *const dataptr, const u16_t len, ULONG *sent, const int safe, PTDI_BUCKET Bucket)
{
    err_t ret;
    u32_t Information = 0;
    struct lwip_callback_msg *msg;

    if (safe)
    {
        /* From a sent event: the core lock is already held */
        ret = LibTCPSendLocked(Connection, dataptr, len, Bucket, &Information);
    }
    else if (IoGetRemainingStackSize() >= LIBTCP_SEND_INLINE_STACK)
    {
        /* Send on the caller's thread rather than switching to the tcpip thread and back */
        LOCK_TCPIP_CORE();
        ret = LibTCPSendLocked(Connection, dataptr, len, Bucket, &Information);
        UNLOCK_TCPIP_CORE();
    }
    else
    {
        msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
        if (!msg)
            return ERR_MEM;

        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);
        msg->Input.Send.Connection = Connection;
        msg->Input.Send.Data = dataptr;
        msg->Input.Send.DataLength = len;
        msg->Input.Send.Bucket = Bucket;

        tcpip_callback_with_block(LibTCPSendCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
        {
            ret = msg->Output.Send.Error;
            Information = msg->Output.Send.Information;
        }
        else
            ret = ERR_CLSD;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);
    }

    if (ret == ERR_OK)
        *sent = Information;
    else
        *sent = 0;

    return ret;
}

static
void
LibTCPConnectCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;
    err_t Error;

    ASSERT(arg);

    if (!msg->Input.Connect.Connection->SocketContext)
    {
        msg->Output.Connect.Error = ERR_CLSD;
        goto done;
    }

    tcp_recv((PTCP_PCB)msg->Input.Connect.Connection->SocketContext, InternalRecvEventHandler);
    tcp_sent((PTCP_PCB)msg->Input.Connect.Connection->SocketContext, InternalSendEventHandler);

    Error = tcp_connect((PTCP_PCB)msg->Input.Connect.Connection->SocketContext,
                        msg->Input.Connect.IpAddress, lwip_ntohs(msg->Input.Connect.Port),
                        InternalConnectEventHandler);

    msg->Output.Connect.Error = Error == ERR_OK ? ERR_INPROGRESS : Error;

done:
    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

err_t
LibTCPConnect(PCONNECTION_ENDPOINT Connection, ip_addr_t *const ipaddr, const u16_t port)
{
    struct lwip_callback_msg *msg;
    err_t ret;

    msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    if (msg)
    {
        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);
        msg->Input.Connect.Connection = Connection;
        msg->Input.Connect.IpAddress = ipaddr;
        msg->Input.Connect.Port = port;

        tcpip_callback_with_block(LibTCPConnectCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
        {
            ret = msg->Output.Connect.Error;
        }
        else
            ret = ERR_CLSD;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);

        return ret;
    }

    return ERR_MEM;
}

static
void
LibTCPShutdownCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;
    PTCP_PCB pcb = msg->Input.Shutdown.Connection->SocketContext;

    if (!msg->Input.Shutdown.Connection->SocketContext)
    {
        msg->Output.Shutdown.Error = ERR_CLSD;
        goto done;
    }

    /* LwIP makes the (questionable) assumption that SHUTDOWN_RDWR is equivalent to tcp_close().
     * This assumption holds even if the shutdown calls are done separately (even through multiple
     * WinSock shutdown() calls). This assumption means that lwIP has the right to deallocate our
     * PCB without telling us if we shutdown TX and RX. To avoid these problems, we'll clear the
     * socket context if we have called shutdown for TX and RX.
     */
    if (msg->Input.Shutdown.shut_rx != msg->Input.Shutdown.shut_tx) {
        if (msg->Input.Shutdown.shut_rx) {
            msg->Output.Shutdown.Error = tcp_shutdown(pcb, TRUE, FALSE);
        }
        if (msg->Input.Shutdown.shut_tx) {
            msg->Output.Shutdown.Error = tcp_shutdown(pcb, FALSE, TRUE);
        }
    }
    else if (msg->Input.Shutdown.shut_rx) {
        /* We received both RX and TX requests, which seems to mean closing connection from TDI.
         * So call tcp_close, otherwise we risk to be put in TCP_WAIT_* states, which makes further
         * attempts to close the socket to fail in this state.
         */
        msg->Output.Shutdown.Error = tcp_close(pcb);
    }
    else {
        /* This case shouldn't happen */
        DbgPrint("Requested socket shutdown(0, 0) !\n");
    }

    if (!msg->Output.Shutdown.Error)
    {
        if (msg->Input.Shutdown.shut_rx)
        {
            msg->Input.Shutdown.Connection->ReceiveShutdown = TRUE;
            msg->Input.Shutdown.Connection->ReceiveShutdownStatus = STATUS_FILE_CLOSED;
        }

        if (msg->Input.Shutdown.shut_tx)
            msg->Input.Shutdown.Connection->SendShutdown = TRUE;

        if (msg->Input.Shutdown.Connection->ReceiveShutdown &&
            msg->Input.Shutdown.Connection->SendShutdown)
        {
            /* The PCB is not ours anymore */
            msg->Input.Shutdown.Connection->SocketContext = NULL;
            tcp_arg(pcb, NULL);
            TCPFinEventHandler(msg->Input.Shutdown.Connection, ERR_CLSD);
        }
    }

done:
    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

err_t
LibTCPShutdown(PCONNECTION_ENDPOINT Connection, const int shut_rx, const int shut_tx)
{
    struct lwip_callback_msg *msg;
    err_t ret;

    msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    if (msg)
    {
        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);

        msg->Input.Shutdown.Connection = Connection;
        msg->Input.Shutdown.shut_rx = shut_rx;
        msg->Input.Shutdown.shut_tx = shut_tx;

        tcpip_callback_with_block(LibTCPShutdownCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
            ret = msg->Output.Shutdown.Error;
        else
            ret = ERR_CLSD;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);

        return ret;
    }

    return ERR_MEM;
}

static
void
LibTCPCloseCallback(void *arg)
{
    struct lwip_callback_msg *msg = arg;
    PTCP_PCB pcb = msg->Input.Close.Connection->SocketContext;

    /* Empty the queue even if we're already "closed" */
    LibTCPEmptyQueue(msg->Input.Close.Connection);
    LibTCPAbortPendingAccepts(msg->Input.Close.Connection);

    /* Check if we've already been closed */
    if (msg->Input.Close.Connection->Closing)
    {
        msg->Output.Close.Error = ERR_OK;
        goto done;
    }

    /* Enter "closing" mode if we're doing a normal close */
    if (msg->Input.Close.Callback)
        msg->Input.Close.Connection->Closing = TRUE;

    /* Check if the PCB was already "closed" but the client doesn't know it yet */
    if (!msg->Input.Close.Connection->SocketContext)
    {
        msg->Output.Close.Error = ERR_OK;
        goto done;
    }

    /* Clear the PCB pointer and stop callbacks */
    msg->Input.Close.Connection->SocketContext = NULL;
    tcp_arg(pcb, NULL);

    /* This may generate additional callbacks but we don't care,
     * because they're too inconsistent to rely on */
    msg->Output.Close.Error = tcp_close(pcb);

    if (msg->Output.Close.Error)
    {
        /* Restore the PCB pointer */
        msg->Input.Close.Connection->SocketContext = pcb;
        msg->Input.Close.Connection->Closing = FALSE;
    }
    else if (msg->Input.Close.Callback)
    {
        TCPFinEventHandler(msg->Input.Close.Connection, ERR_CLSD);
    }

done:
    KeSetEvent(&msg->Event, IO_NO_INCREMENT, FALSE);
}

err_t
LibTCPClose(PCONNECTION_ENDPOINT Connection, const int safe, const int callback)
{
    err_t ret;
    struct lwip_callback_msg *msg;

    msg = ExAllocateFromNPagedLookasideList(&MessageLookasideList);
    if (msg)
    {
        KeInitializeEvent(&msg->Event, NotificationEvent, FALSE);

        msg->Input.Close.Connection = Connection;
        msg->Input.Close.Callback = callback;

        if (safe)
            LibTCPCloseCallback(msg);
        else
            tcpip_callback_with_block(LibTCPCloseCallback, msg, 1);

        if (WaitForEventSafely(&msg->Event))
            ret = msg->Output.Close.Error;
        else
            ret = ERR_CLSD;

        ExFreeToNPagedLookasideList(&MessageLookasideList, msg);

        return ret;
    }

    return ERR_MEM;
}

void
LibTCPAccept(PTCP_PCB pcb, struct tcp_pcb *listen_pcb, void *arg)
{
    ASSERT(arg);

    tcp_arg(pcb, NULL);
    tcp_recv(pcb, InternalRecvEventHandler);
    tcp_sent(pcb, InternalSendEventHandler);
    tcp_err(pcb, InternalErrorEventHandler);
    tcp_arg(pcb, arg);

    tcp_accepted(listen_pcb);
}

err_t
LibTCPGetHostName(PTCP_PCB pcb, ip_addr_t *const ipaddr, u16_t *const port)
{
    if (!pcb)
        return ERR_CLSD;

    *ipaddr = pcb->local_ip;
    *port = pcb->local_port;

    return ERR_OK;
}

err_t
LibTCPGetPeerName(PTCP_PCB pcb, ip_addr_t * const ipaddr, u16_t * const port)
{
    if (!pcb)
        return ERR_CLSD;

    *ipaddr = pcb->remote_ip;
    *port = pcb->remote_port;

    return ERR_OK;
}

void
LibTCPSetNoDelay(
    PTCP_PCB pcb,
    BOOLEAN Set)
{
    if (Set)
        pcb->flags |= TF_NODELAY;
    else
        pcb->flags &= ~TF_NODELAY;
}

void
LibTCPSetKeepAlive(
    PTCP_PCB pcb,
    BOOLEAN Set)
{
    if (Set)
        pcb->so_options |= SOF_KEEPALIVE;
    else
        pcb->so_options &= ~SOF_KEEPALIVE;
}

void
LibTcpSetKeepAliveValues(
    PTCP_PCB pcb,
    u32_t KeepAliveTime,
    u32_t KeepAliveInterval
)
{
    pcb->keep_idle = KeepAliveTime;
    pcb->keep_intvl = KeepAliveInterval;
    pcb->keep_cnt = 10;
}

void
LibTCPGetSocketStatus(
    PTCP_PCB pcb,
    PULONG State)
{
    /* Translate state from enum tcp_state -> MIB_TCP_STATE */
    *State = pcb->state + 1;
}
