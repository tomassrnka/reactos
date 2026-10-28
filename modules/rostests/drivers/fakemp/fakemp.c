/*
 * PROJECT:     ReactOS test suite
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Fake NDIS 5.1 miniport that pends, never completes, completes late and resets on command,
 *              to exercise NDIS's request serialization, abort on reset and on halt/remove.
 *              It has no hardware: a root-enumerated device. It is built serialized and
 *              deserialized (FAKEMP_DESERIALIZED), and a third time serialized with its link up
 *              (FAKEMP_LINK_UP), for tests that need tcpip to configure the adapter.
 */

#include <ndis.h>

#include "fakemp.h"

#if defined(FAKEMP_LINK_UP)
#define FAKEMP_NAME         L"FakeMpT"
#define FAKEMP_LAST_BYTE    0x54
#define FAKEMP_ATTRIBUTES   0
#define FAKEMP_DESCRIPTION  "FakeMp link up"
#elif defined(FAKEMP_DESERIALIZED)
#define FAKEMP_NAME         L"FakeMpD"
#define FAKEMP_LAST_BYTE    0x44
#define FAKEMP_ATTRIBUTES   NDIS_ATTRIBUTE_DESERIALIZE
#define FAKEMP_DESCRIPTION  "FakeMp deserialized"
#else
#define FAKEMP_NAME         L"FakeMpS"
#define FAKEMP_LAST_BYTE    0x53
#define FAKEMP_ATTRIBUTES   0
#define FAKEMP_DESCRIPTION  "FakeMp serialized"
#endif

#define FAKEMP_SPIN_LIMIT_MS    20000

/* One adapter per driver binary: the state outlives the adapter, so that a test can
   read it after the halt (the control device keeps the driver loaded) */
static struct
{
    NDIS_HANDLE Adapter;
    NDIS_HANDLE Wrapper;
    KSPIN_LOCK Lock;
    FAKEMP_CONFIG Cfg;
    FAKEMP_STATS St;
    BOOLEAN Halted;
    BOOLEAN Held;
    BOOLEAN HeldIsSet;
    BOOLEAN ResetPending;
    BOOLEAN AddressingReset;
    BOOLEAN HangFlag;
    volatile BOOLEAN ReleaseSpin;
    volatile BOOLEAN ResetRelease;  /* ... because a reset started */
    BOOLEAN TimerInitialized;
    NDIS_MINIPORT_TIMER Timer;
    ULONG PacketFilter;
    ULONG Lookahead;
    PDEVICE_OBJECT ControlDevice;
    NDIS_HANDLE ControlHandle;
    /* Held test sends, oldest first, with the checksum of their data when they came */
    struct
    {
        PNDIS_PACKET Packet;
        ULONG Sum;
    } HeldSends[FAKEMP_MAX_HELD_SENDS];
    ULONG HeldSendCount;
    NDIS_MINIPORT_TIMER SendTimer;
    /* The frame indicated last (FAKEMP_IOCTL_INDICATE), which the transfers copy from */
    UCHAR RxFrame[FAKEMP_RX_FRAME_SIZE];
    ULONG TransferMode;
    struct
    {
        PNDIS_PACKET Packet;
        UINT ByteOffset;
        UINT BytesToTransfer;
        BOOLEAN Never;      /* FAKEMP_XFER_NEVER: the halt forgets it */
    } HeldXfers[FAKEMP_MAX_HELD_XFERS];
    ULONG HeldXferCount;
    volatile LONG FailFilter;       /* sets of the packet filter still to fail */
} G;

static const UCHAR AdapterAddress[6] = { 0x02, 0xFA, 0xCE, 0x4D, 0x50, FAKEMP_LAST_BYTE };

static const NDIS_OID SupportedOids[] =
{
    OID_GEN_SUPPORTED_LIST,
    OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_LOOKAHEAD,
    OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_DRIVER_VERSION,
    OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_PROTOCOL_OPTIONS,
    OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS,
    OID_GEN_MAXIMUM_SEND_PACKETS,
    OID_GEN_XMIT_OK,
    OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR,
    OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER,
    OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_RCV_ERROR_ALIGNMENT,
    OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS,
};

static
VOID
Lock(PKIRQL OldIrql)
{
    KeAcquireSpinLock(&G.Lock, OldIrql);
}

static
VOID
Unlock(KIRQL OldIrql)
{
    KeReleaseSpinLock(&G.Lock, OldIrql);
}

/* A handler is entered: count it, and note a call into the halted miniport */
static
VOID
EnterHandler(BOOLEAN IsRequest)
{
    KIRQL OldIrql;

    Lock(&OldIrql);
    if (G.Halted)
    {
        G.St.CallsAfterHalt++;
        DbgPrint("FAKEMP: handler called after halt\n");
    }
    if (IsRequest)
    {
        G.St.Requests++;
        G.St.InHandler++;
        if (G.St.InHandler > G.St.MaxInHandler)
            G.St.MaxInHandler = G.St.InHandler;
        G.St.Outstanding++;
        if (G.St.Outstanding > G.St.MaxOutstanding)
            G.St.MaxOutstanding = G.St.Outstanding;
    }
    Unlock(OldIrql);
}

static
VOID
LeaveRequestHandler(NDIS_STATUS Status)
{
    KIRQL OldIrql;

    Lock(&OldIrql);
    G.St.InHandler--;
    if (Status != NDIS_STATUS_PENDING)
        G.St.Outstanding--;
    Unlock(OldIrql);
}

/* Completes the pended request, if there is one. NDIS has to cope when it is gone already: aborted by a reset or halt */
static
BOOLEAN
CompleteHeld(NDIS_STATUS Status)
{
    KIRQL OldIrql;
    BOOLEAN WasHeld, IsSet;
    NDIS_HANDLE Adapter;

    Lock(&OldIrql);
    WasHeld = G.Held;
    IsSet = G.HeldIsSet;
    Adapter = G.Adapter;
    if (WasHeld)
    {
        G.Held = FALSE;
        G.St.Held = 0;
        G.St.Outstanding--;
        G.St.Completions++;
    }
    Unlock(OldIrql);

    if (!WasHeld || !Adapter)
        return FALSE;

    if (IsSet)
    {
        NdisMSetInformationComplete(Adapter, Status);
    }
    else
    {
        NdisMQueryInformationComplete(Adapter, Status);
    }
    return TRUE;
}

static
VOID
NTAPI
TimerFunction(
    PVOID SystemSpecific1,
    PVOID FunctionContext,
    PVOID SystemSpecific2,
    PVOID SystemSpecific3)
{
    CompleteHeld(NDIS_STATUS_SUCCESS);
}

/* Waits (at the caller's IRQL) for *Flag, at most Milliseconds of real time; with no flag, for the whole time */
static
VOID
SpinWait(volatile BOOLEAN *Flag, ULONG Milliseconds)
{
    ULONGLONG End = KeQueryInterruptTime() + (ULONGLONG)Milliseconds * 10000;

    while ((!Flag || !*Flag) && KeQueryInterruptTime() < End)
        KeStallExecutionProcessor(50);
}

/* The request for a private OID, as configured */
static
NDIS_STATUS
TestRequest(
    BOOLEAN IsSet,
    PVOID Buffer,
    ULONG Length,
    PULONG BytesDone,
    PULONG BytesNeeded)
{
    ULONG Mode = IsSet ? G.Cfg.SetMode : G.Cfg.QueryMode;
    KIRQL OldIrql;

    if (Length < sizeof(ULONG))
    {
        *BytesNeeded = sizeof(ULONG);
        return NDIS_STATUS_INVALID_LENGTH;
    }
    /* The buffer belongs to the caller only until this returns: a request that is aborted
       is gone when the miniport completes it later, so nothing is written afterwards */
    if (!IsSet)
        *(PULONG)Buffer = 0xFA4E0001;
    *BytesDone = sizeof(ULONG);

    if (Mode == FAKEMP_REQ_IMMEDIATE)
        return NDIS_STATUS_SUCCESS;

    Lock(&OldIrql);
    G.HeldIsSet = IsSet;
    Unlock(OldIrql);

    if (Mode == FAKEMP_REQ_SPIN)
    {
        Lock(&OldIrql);
        G.ReleaseSpin = FALSE;
        G.ResetRelease = FALSE;
        G.St.Spinning = 1;
        Unlock(OldIrql);

        /* At DISPATCH_LEVEL, as NDIS calls a serialized miniport: a reset or the test, on another CPU, ends it */
        {
            ULONGLONG Begin = KeQueryInterruptTime();

            SpinWait(&G.ReleaseSpin, FAKEMP_SPIN_LIMIT_MS);
            DbgPrint("FAKEMP: spin ended after %lu ms, release %u, reset %u\n",
                     (ULONG)((KeQueryInterruptTime() - Begin) / 10000), G.ReleaseSpin, G.ResetRelease);
        }

        /* A reset ended the wait: the handler stays DelayMs longer, so that the reset (and the abort at its
           end) happens with the request still in the handler */
        if (G.ResetRelease)
            SpinWait(NULL, G.Cfg.DelayMs);

        Lock(&OldIrql);
        G.St.Spinning = 0;
        Unlock(OldIrql);
    }

    Lock(&OldIrql);
    G.Held = TRUE;
    G.St.Held = 1;
    Unlock(OldIrql);

    if (Mode == FAKEMP_REQ_PEND_DELAY)
        NdisMSetTimer(&G.Timer, G.Cfg.DelayMs ? G.Cfg.DelayMs : 1);

    return NDIS_STATUS_PENDING;
}

static
NDIS_STATUS
StandardQuery(
    NDIS_OID Oid,
    PVOID Buffer,
    ULONG Length,
    PULONG BytesWritten,
    PULONG BytesNeeded)
{
    static const CHAR Description[] = FAKEMP_DESCRIPTION;
    PVOID Source = NULL;
    ULONG Size = sizeof(ULONG);
    ULONG Value = 0;

    switch (Oid)
    {
        case OID_GEN_SUPPORTED_LIST:
            Source = (PVOID)SupportedOids;
            Size = sizeof(SupportedOids);
            break;
        case OID_GEN_HARDWARE_STATUS:
            Value = NdisHardwareStatusReady;
            break;
        case OID_GEN_MEDIA_SUPPORTED:
        case OID_GEN_MEDIA_IN_USE:
            Value = NdisMedium802_3;
            break;
        case OID_GEN_MAXIMUM_LOOKAHEAD:
        case OID_GEN_MAXIMUM_FRAME_SIZE:
        case OID_GEN_TRANSMIT_BLOCK_SIZE:
        case OID_GEN_RECEIVE_BLOCK_SIZE:
            Value = 1500;
            break;
        case OID_GEN_MAXIMUM_TOTAL_SIZE:
            Value = 1514;
            break;
        case OID_GEN_LINK_SPEED:
            Value = 1000000; /* 100 Mbit/s in 100 bps units */
            break;
        case OID_GEN_TRANSMIT_BUFFER_SPACE:
        case OID_GEN_RECEIVE_BUFFER_SPACE:
            Value = 16384;
            break;
        case OID_GEN_VENDOR_ID:
            Value = 0x00FFFFFF;
            break;
        case OID_GEN_VENDOR_DESCRIPTION:
            Source = (PVOID)Description;
            Size = sizeof(Description);
            break;
        case OID_GEN_CURRENT_PACKET_FILTER:
            Value = G.PacketFilter;
            break;
        case OID_GEN_CURRENT_LOOKAHEAD:
            Value = G.Lookahead;
            break;
        case OID_GEN_DRIVER_VERSION:
            Value = 0x0501;
            Size = sizeof(USHORT);
            break;
        case OID_GEN_MAC_OPTIONS:
            /* Transfers may pend (FAKEMP_XFER_HOLD) */
            Value = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_NO_LOOPBACK;
            break;
        case OID_GEN_MEDIA_CONNECT_STATUS:
#ifdef FAKEMP_LINK_UP
            /* tcpip configures the adapter, and sends through it */
            Value = NdisMediaStateConnected;
#else
            /* No link: nothing is sent, and tcpip does not configure the adapter */
            Value = NdisMediaStateDisconnected;
#endif
            break;
        case OID_GEN_MAXIMUM_SEND_PACKETS:
            Value = 1;
            break;
        case OID_802_3_PERMANENT_ADDRESS:
        case OID_802_3_CURRENT_ADDRESS:
            Source = (PVOID)AdapterAddress;
            Size = sizeof(AdapterAddress);
            break;
        case OID_802_3_MULTICAST_LIST:
            *BytesWritten = 0;
            return NDIS_STATUS_SUCCESS;
        case OID_802_3_MAXIMUM_LIST_SIZE:
            Value = 32;
            break;
        case OID_GEN_PROTOCOL_OPTIONS:
        case OID_GEN_XMIT_OK:
        case OID_GEN_RCV_OK:
        case OID_GEN_XMIT_ERROR:
        case OID_GEN_RCV_ERROR:
        case OID_GEN_RCV_NO_BUFFER:
        case OID_802_3_RCV_ERROR_ALIGNMENT:
        case OID_802_3_XMIT_ONE_COLLISION:
        case OID_802_3_XMIT_MORE_COLLISIONS:
            Value = 0;
            break;
        default:
            return NDIS_STATUS_NOT_SUPPORTED;
    }

    if (Length < Size)
    {
        *BytesNeeded = Size;
        return NDIS_STATUS_BUFFER_TOO_SHORT;
    }

    if (Source)
        RtlCopyMemory(Buffer, Source, Size);
    else
        RtlCopyMemory(Buffer, &Value, Size);
    *BytesWritten = Size;
    return NDIS_STATUS_SUCCESS;
}

static
NDIS_STATUS
NTAPI
MpQueryInformation(
    NDIS_HANDLE MiniportAdapterContext,
    NDIS_OID Oid,
    PVOID InformationBuffer,
    ULONG InformationBufferLength,
    PULONG BytesWritten,
    PULONG BytesNeeded)
{
    NDIS_STATUS Status;

    EnterHandler(TRUE);
    *BytesWritten = 0;
    *BytesNeeded = 0;

    if (Oid == FAKEMP_OID_TEST_QUERY)
        Status = TestRequest(FALSE, InformationBuffer, InformationBufferLength, BytesWritten, BytesNeeded);
    else
        Status = StandardQuery(Oid, InformationBuffer, InformationBufferLength, BytesWritten, BytesNeeded);

    LeaveRequestHandler(Status);
    return Status;
}

static
NDIS_STATUS
NTAPI
MpSetInformation(
    NDIS_HANDLE MiniportAdapterContext,
    NDIS_OID Oid,
    PVOID InformationBuffer,
    ULONG InformationBufferLength,
    PULONG BytesRead,
    PULONG BytesNeeded)
{
    NDIS_STATUS Status;

    EnterHandler(TRUE);
    *BytesRead = 0;
    *BytesNeeded = 0;

    switch (Oid)
    {
        case FAKEMP_OID_TEST_SET:
            Status = TestRequest(TRUE, InformationBuffer, InformationBufferLength, BytesRead, BytesNeeded);
            break;
        case OID_GEN_CURRENT_PACKET_FILTER:
            if (InformationBufferLength < sizeof(ULONG))
            {
                *BytesNeeded = sizeof(ULONG);
                Status = NDIS_STATUS_INVALID_LENGTH;
                break;
            }
            if (G.FailFilter > 0)
            {
                InterlockedDecrement(&G.FailFilter);
                G.St.FilterFailures++;
                Status = NDIS_STATUS_FAILURE;
                break;
            }
            G.PacketFilter = *(PULONG)InformationBuffer;
            *BytesRead = sizeof(ULONG);
            Status = NDIS_STATUS_SUCCESS;
            break;
        case OID_GEN_CURRENT_LOOKAHEAD:
            if (InformationBufferLength < sizeof(ULONG))
            {
                *BytesNeeded = sizeof(ULONG);
                Status = NDIS_STATUS_INVALID_LENGTH;
                break;
            }
            G.Lookahead = *(PULONG)InformationBuffer;
            *BytesRead = sizeof(ULONG);
            Status = NDIS_STATUS_SUCCESS;
            break;
        case OID_802_3_MULTICAST_LIST:
        case OID_GEN_PROTOCOL_OPTIONS:
            *BytesRead = InformationBufferLength;
            Status = NDIS_STATUS_SUCCESS;
            break;
        default:
            Status = NDIS_STATUS_NOT_SUPPORTED;
            break;
    }

    LeaveRequestHandler(Status);
    return Status;
}

static
NDIS_STATUS
NTAPI
MpReset(
    PBOOLEAN AddressingReset,
    NDIS_HANDLE MiniportAdapterContext)
{
    KIRQL OldIrql;
    NDIS_STATUS Status;

    EnterHandler(FALSE);

    Lock(&OldIrql);
    G.St.Resets++;
    G.ResetRelease = TRUE;
    G.ReleaseSpin = TRUE; /* a handler spinning on another CPU is in the handler as the reset runs */
    G.AddressingReset = !!G.Cfg.AddressingReset;
    *AddressingReset = G.AddressingReset;
    DbgPrint("FAKEMP: reset, addressing reset %u, mode %lu\n", G.AddressingReset, G.Cfg.ResetMode);
    if (G.Cfg.ResetMode == FAKEMP_RESET_SYNC)
    {
        G.St.ResetsCompleted++;
        Status = NDIS_STATUS_SUCCESS;
    }
    else
    {
        G.ResetPending = TRUE;
        G.St.ResetPending = 1;
        Status = NDIS_STATUS_PENDING;
    }
    Unlock(OldIrql);

    return Status;
}

static
BOOLEAN
NTAPI
MpCheckForHang(
    NDIS_HANDLE MiniportAdapterContext)
{
    KIRQL OldIrql;
    BOOLEAN Hung;

    EnterHandler(FALSE);

    Lock(&OldIrql);
    G.St.HangChecks++;
    G.St.HangCpu = KeGetCurrentProcessorNumber();
    Hung = G.HangFlag;
    G.HangFlag = FALSE;
    Unlock(OldIrql);

    return Hung;
}

/* A checksum of the frame's data, and whether it is a test frame. Reading the data is what a NIC does with
   a send it holds (DMA): a buffer its sender freed is caught here, by special pool or by the checksum */
static
ULONG
PacketSum(PNDIS_PACKET Packet, PBOOLEAN IsTest)
{
    PNDIS_BUFFER Buffer;
    PUCHAR Data;
    UINT Length, i, Offset = 0;
    ULONG Sum = 0;
    UCHAR Type[2] = { 0, 0 };

    NdisQueryPacket(Packet, NULL, NULL, &Buffer, NULL);
    while (Buffer)
    {
        NdisQueryBuffer(Buffer, (PVOID *)&Data, &Length);
        for (i = 0; i < Length; i++, Offset++)
        {
            Sum = Sum * 31 + Data[i];
            if (Offset == 12 || Offset == 13)
                Type[Offset - 12] = Data[i];
        }
        NdisGetNextBuffer(Buffer, &Buffer);
    }

    if (IsTest)
        *IsTest = (((Type[0] << 8) | Type[1]) == FAKEMP_TEST_ETHERTYPE);
    return Sum + Offset;
}

/* Completes held sends (FAKEMP_COMPLETE_*), returns how many */
static
ULONG
CompleteSends(ULONG Which, NDIS_STATUS Status, BOOLEAN AtHalt)
{
    PNDIS_PACKET Packet;
    NDIS_HANDLE Adapter;
    ULONG Sum, Done = 0, i;
    KIRQL OldIrql;

    for (;;)
    {
        Lock(&OldIrql);
        Adapter = G.Adapter;
        if (!G.HeldSendCount || !Adapter)
        {
            Unlock(OldIrql);
            break;
        }
        if (Which == FAKEMP_COMPLETE_OLDEST)
        {
            Packet = G.HeldSends[0].Packet;
            Sum = G.HeldSends[0].Sum;
            for (i = 1; i < G.HeldSendCount; i++)
                G.HeldSends[i - 1] = G.HeldSends[i];
        }
        else
        {
            Packet = G.HeldSends[G.HeldSendCount - 1].Packet;
            Sum = G.HeldSends[G.HeldSendCount - 1].Sum;
        }
        G.HeldSendCount--;
        G.St.SendsHeld = G.HeldSendCount;
        G.St.SendsCompleted++;
        if (AtHalt)
            G.St.SendsCompletedAtHalt++;
        Unlock(OldIrql);

        /* The data is still the sender's until this completes */
        if (PacketSum(Packet, NULL) != Sum)
        {
            Lock(&OldIrql);
            G.St.SendDataBad++;
            Unlock(OldIrql);
            DbgPrint("FAKEMP: the data of held send %p changed before its completion\n", Packet);
        }

        NdisMSendComplete(Adapter, Packet, Status);
        Done++;
        if (Which != FAKEMP_COMPLETE_ALL)
            break;
    }
    return Done;
}

static
VOID
NTAPI
SendTimerFunction(
    PVOID SystemSpecific1,
    PVOID FunctionContext,
    PVOID SystemSpecific2,
    PVOID SystemSpecific3)
{
    /* Newest first: out of the order they were sent in */
    while (CompleteSends(FAKEMP_COMPLETE_NEWEST, NDIS_STATUS_SUCCESS, FALSE))
        ;
}

static
NDIS_STATUS
NTAPI
MpSend(
    NDIS_HANDLE MiniportAdapterContext,
    PNDIS_PACKET Packet,
    UINT Flags)
{
    BOOLEAN IsTest;
    ULONG Sum, Mode;
    KIRQL OldIrql;

    EnterHandler(FALSE);

    Sum = PacketSum(Packet, &IsTest);
    if (!IsTest)
        return NDIS_STATUS_SUCCESS;

    Lock(&OldIrql);
    G.St.Sends++;
    Mode = G.Cfg.SendMode;
    if (Mode == FAKEMP_SEND_IMMEDIATE || G.HeldSendCount == FAKEMP_MAX_HELD_SENDS)
    {
        Unlock(OldIrql);
        return NDIS_STATUS_SUCCESS;
    }
    G.HeldSends[G.HeldSendCount].Packet = Packet;
    G.HeldSends[G.HeldSendCount].Sum = Sum;
    G.HeldSendCount++;
    G.St.SendsHeld = G.HeldSendCount;
    if (G.St.SendsHeld > G.St.MaxSendsHeld)
        G.St.MaxSendsHeld = G.St.SendsHeld;
    Unlock(OldIrql);

    if (Mode == FAKEMP_SEND_DELAY)
        NdisMSetTimer(&G.SendTimer, G.Cfg.SendDelayMs ? G.Cfg.SendDelayMs : 1);

    return NDIS_STATUS_PENDING;
}

/* Copies the indicated frame's data from ByteOffset (after the header) into the packet */
static
UINT
CopyToPacket(PNDIS_PACKET Packet, UINT ByteOffset, UINT BytesToTransfer)
{
    PNDIS_BUFFER Buffer;
    PUCHAR Data;
    UINT Length, Copy, Done = 0;
    UINT Available = FAKEMP_RX_FRAME_SIZE - 14;

    if (ByteOffset >= Available)
        return 0;
    if (BytesToTransfer > Available - ByteOffset)
        BytesToTransfer = Available - ByteOffset;

    NdisQueryPacket(Packet, NULL, NULL, &Buffer, NULL);
    while (Buffer && Done < BytesToTransfer)
    {
        NdisQueryBuffer(Buffer, (PVOID *)&Data, &Length);
        Copy = min(Length, BytesToTransfer - Done);
        RtlCopyMemory(Data, &G.RxFrame[14 + ByteOffset + Done], Copy);
        Done += Copy;
        NdisGetNextBuffer(Buffer, &Buffer);
    }
    return Done;
}

static
NDIS_STATUS
NTAPI
MpTransferData(
    PNDIS_PACKET Packet,
    PUINT BytesTransferred,
    NDIS_HANDLE MiniportAdapterContext,
    NDIS_HANDLE MiniportReceiveContext,
    UINT ByteOffset,
    UINT BytesToTransfer)
{
    KIRQL OldIrql;

    EnterHandler(FALSE);

    Lock(&OldIrql);
    G.St.Transfers++;
    if ((G.TransferMode == FAKEMP_XFER_HOLD || G.TransferMode == FAKEMP_XFER_NEVER) && G.HeldXferCount < FAKEMP_MAX_HELD_XFERS)
    {
        G.HeldXfers[G.HeldXferCount].Never = (G.TransferMode == FAKEMP_XFER_NEVER);
        if (G.TransferMode == FAKEMP_XFER_NEVER)
            G.St.TransfersNever++;
        G.HeldXfers[G.HeldXferCount].Packet = Packet;
        G.HeldXfers[G.HeldXferCount].ByteOffset = ByteOffset;
        G.HeldXfers[G.HeldXferCount].BytesToTransfer = BytesToTransfer;
        G.HeldXferCount++;
        G.St.TransfersHeld = G.HeldXferCount;
        Unlock(OldIrql);
        return NDIS_STATUS_PENDING;
    }
    Unlock(OldIrql);

    *BytesTransferred = CopyToPacket(Packet, ByteOffset, BytesToTransfer);
    return NDIS_STATUS_SUCCESS;
}

/* The halt: transfers pended in FAKEMP_XFER_NEVER mode are forgotten, never completed (NDIS must fail them) */
static
VOID
ForgetNeverTransfers(VOID)
{
    KIRQL OldIrql;
    ULONG i, Kept = 0;

    Lock(&OldIrql);
    for (i = 0; i < G.HeldXferCount; i++)
    {
        if (!G.HeldXfers[i].Never)
            G.HeldXfers[Kept++] = G.HeldXfers[i];
    }
    G.HeldXferCount = Kept;
    G.St.TransfersHeld = Kept;
    Unlock(OldIrql);
}

/* Completes the held transfers: copied, or failed in the halt. Returns how many */
static
ULONG
CompleteTransfers(BOOLEAN AtHalt)
{
    PNDIS_PACKET Packet;
    NDIS_HANDLE Adapter;
    UINT ByteOffset, BytesToTransfer, Done;
    ULONG Count = 0;
    KIRQL OldIrql;

    for (;;)
    {
        Lock(&OldIrql);
        Adapter = G.Adapter;
        if (!G.HeldXferCount || !Adapter)
        {
            Unlock(OldIrql);
            break;
        }
        G.HeldXferCount--;
        Packet = G.HeldXfers[G.HeldXferCount].Packet;
        ByteOffset = G.HeldXfers[G.HeldXferCount].ByteOffset;
        BytesToTransfer = G.HeldXfers[G.HeldXferCount].BytesToTransfer;
        G.St.TransfersHeld = G.HeldXferCount;
        G.St.TransfersCompleted++;
        if (AtHalt)
            G.St.TransfersCompletedAtHalt++;
        Unlock(OldIrql);

        Done = AtHalt ? 0 : CopyToPacket(Packet, ByteOffset, BytesToTransfer);
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
        NdisMTransferDataComplete(Adapter, Packet, AtHalt ? NDIS_STATUS_FAILURE : NDIS_STATUS_SUCCESS, Done);
        KeLowerIrql(OldIrql);
        Count++;
    }
    return Count;
}

/* A received frame, as the interrupt DPC indicates it */
static
NTSTATUS
Indicate(PFAKEMP_INDICATE Ind)
{
    NDIS_HANDLE Adapter;
    ULONG i, Lookahead;
    KIRQL OldIrql;

    Lock(&OldIrql);
    Adapter = G.Adapter;
    if (Adapter)
    {
        G.St.Indications++;
        G.TransferMode = Ind->TransferMode;
        RtlCopyMemory(G.RxFrame, AdapterAddress, 6);
        G.RxFrame[6] = 0x02;
        G.RxFrame[7] = 0x00;
        G.RxFrame[8] = 0x5E;
        G.RxFrame[9] = 0x10;
        G.RxFrame[10] = 0x00;
        G.RxFrame[11] = 0x02;
        G.RxFrame[12] = FAKEMP_TEST_ETHERTYPE >> 8;
        G.RxFrame[13] = FAKEMP_TEST_ETHERTYPE & 0xFF;
        for (i = 14; i < FAKEMP_RX_FRAME_SIZE; i++)
            G.RxFrame[i] = (UCHAR)(Ind->Seed + i);
    }
    Unlock(OldIrql);
    if (!Adapter)
        return STATUS_DEVICE_NOT_READY;

    Lookahead = min(Ind->Lookahead, FAKEMP_RX_FRAME_SIZE - 14);
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    NdisMEthIndicateReceive(Adapter, (NDIS_HANDLE)&G, (PCHAR)G.RxFrame, 14, &G.RxFrame[14], Lookahead, FAKEMP_RX_FRAME_SIZE - 14);
    NdisMEthIndicateReceiveComplete(Adapter);
    KeLowerIrql(OldIrql);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
MpHalt(
    NDIS_HANDLE MiniportAdapterContext)
{
    KIRQL OldIrql;
    BOOLEAN Cancelled;

    Lock(&OldIrql);
    G.Halted = TRUE;
    G.St.Halted = 1;
    G.St.Halts++;
    Unlock(OldIrql);

    /* A halted miniport completes nothing: the timer goes, and a request it still held is forgotten */
    NdisMCancelTimer(&G.Timer, &Cancelled);

    /* ... but the sends it holds are completed now (some NICs complete a send only here) */
    NdisMCancelTimer(&G.SendTimer, &Cancelled);
    CompleteSends(FAKEMP_COMPLETE_ALL, NDIS_STATUS_FAILURE, TRUE);
    ForgetNeverTransfers();
    CompleteTransfers(TRUE);

    Lock(&OldIrql);
    G.Held = FALSE;
    G.St.Held = 0;
    G.ResetPending = FALSE;
    G.St.ResetPending = 0;
    G.ReleaseSpin = TRUE;
    G.Adapter = NULL;
    Unlock(OldIrql);
}

static
NDIS_STATUS
NTAPI
MpInitialize(
    PNDIS_STATUS OpenErrorStatus,
    PUINT SelectedMediumIndex,
    PNDIS_MEDIUM MediumArray,
    UINT MediumArraySize,
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_HANDLE WrapperConfigurationContext)
{
    KIRQL OldIrql;
    UINT i;

    for (i = 0; i < MediumArraySize; i++)
    {
        if (MediumArray[i] == NdisMedium802_3)
            break;
    }
    if (i == MediumArraySize)
        return NDIS_STATUS_UNSUPPORTED_MEDIA;
    *SelectedMediumIndex = i;

    /* Check for a hang every second: a test that wants a reset waits at most that */
    NdisMSetAttributesEx(MiniportAdapterHandle,
                         &G,
                         1,
                         FAKEMP_ATTRIBUTES | NDIS_ATTRIBUTE_IGNORE_PACKET_TIMEOUT,
                         NdisInterfaceInternal);

    NdisMInitializeTimer(&G.Timer, MiniportAdapterHandle, TimerFunction, NULL);
    NdisMInitializeTimer(&G.SendTimer, MiniportAdapterHandle, SendTimerFunction, NULL);
    G.TimerInitialized = TRUE;

    Lock(&OldIrql);
    G.Adapter = MiniportAdapterHandle;
    G.Halted = FALSE;
    G.St.Halted = 0;
    G.St.Initializes++;
    G.Held = FALSE;
    G.St.Held = 0;
    G.ResetPending = FALSE;
    G.St.ResetPending = 0;
    G.HangFlag = FALSE;
    G.HeldSendCount = 0;
    G.St.SendsHeld = 0;
    G.HeldXferCount = 0;
    G.St.TransfersHeld = 0;
    G.PacketFilter = 0;
    G.Lookahead = 1500;
    Unlock(OldIrql);

    return NDIS_STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
ControlCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
ControlIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG InLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG OutLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Information = 0;
    NDIS_HANDLE Adapter;
    BOOLEAN Finish;
    KIRQL OldIrql;

    switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
    {
        case FAKEMP_IOCTL_SET_CONFIG:
            if (InLength < sizeof(FAKEMP_CONFIG))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            Lock(&OldIrql);
            G.Cfg = *(PFAKEMP_CONFIG)Buffer;
            Unlock(OldIrql);
            break;

        case FAKEMP_IOCTL_GET_STATS:
            if (OutLength < sizeof(FAKEMP_STATS))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            Lock(&OldIrql);
            *(PFAKEMP_STATS)Buffer = G.St;
            Unlock(OldIrql);
            Information = sizeof(FAKEMP_STATS);
            break;

        case FAKEMP_IOCTL_RESET_STATS:
            Lock(&OldIrql);
            G.St.Requests = 0;
            G.St.MaxInHandler = G.St.InHandler;
            G.St.MaxOutstanding = G.St.Outstanding;
            G.St.Completions = 0;
            G.St.Resets = 0;
            G.St.ResetsCompleted = 0;
            G.St.CallsAfterHalt = 0;
            G.St.HangChecks = 0;
            G.St.Sends = 0;
            G.St.MaxSendsHeld = G.St.SendsHeld;
            G.St.SendsCompleted = 0;
            G.St.SendsCompletedAtHalt = 0;
            G.St.SendDataBad = 0;
            G.St.Indications = 0;
            G.St.Transfers = 0;
            G.St.TransfersCompleted = 0;
            G.St.TransfersCompletedAtHalt = 0;
            G.St.TransfersNever = 0;
            G.St.FilterFailures = 0;
            Unlock(OldIrql);
            break;

        case FAKEMP_IOCTL_TRIGGER_HANG:
            Lock(&OldIrql);
            G.HangFlag = TRUE;
            Unlock(OldIrql);
            break;

        case FAKEMP_IOCTL_COMPLETE_HELD:
            if (!CompleteHeld(NDIS_STATUS_SUCCESS))
                Status = STATUS_NOT_FOUND;
            break;

        case FAKEMP_IOCTL_FINISH_RESET:
            Lock(&OldIrql);
            Adapter = G.Adapter;
            Finish = G.ResetPending;
            G.ResetPending = FALSE;
            G.St.ResetPending = 0;
            if (Finish)
                G.St.ResetsCompleted++;
            Unlock(OldIrql);
            if (Adapter && Finish)
            {
                NdisMResetComplete(Adapter, NDIS_STATUS_SUCCESS, G.AddressingReset);
            }
            else
            {
                Status = STATUS_NOT_FOUND;
            }
            break;

        case FAKEMP_IOCTL_RELEASE_SPIN:
            G.ReleaseSpin = TRUE;
            break;

        case FAKEMP_IOCTL_INDICATE:
            if (InLength < sizeof(FAKEMP_INDICATE))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            Status = Indicate((PFAKEMP_INDICATE)Buffer);
            break;

        case FAKEMP_IOCTL_FAIL_FILTER:
            if (InLength < sizeof(ULONG))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            InterlockedExchange(&G.FailFilter, (LONG)*(PULONG)Buffer);
            break;

        case FAKEMP_IOCTL_COMPLETE_TRANSFERS:
            if (!CompleteTransfers(FALSE))
                Status = STATUS_NOT_FOUND;
            break;

        case FAKEMP_IOCTL_COMPLETE_SEND:
        {
            ULONG Done;

            if (InLength < sizeof(ULONG))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            Done = CompleteSends(*(PULONG)Buffer, NDIS_STATUS_SUCCESS, FALSE);
            if (OutLength >= sizeof(ULONG))
            {
                *(PULONG)Buffer = Done;
                Information = sizeof(ULONG);
            }
            if (!Done)
                Status = STATUS_NOT_FOUND;
            break;
        }

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
VOID
NTAPI
MpUnload(PDRIVER_OBJECT DriverObject)
{
    if (G.ControlHandle)
        NdisMDeregisterDevice(G.ControlHandle);
    G.ControlHandle = NULL;
}

NTSTATUS
NTAPI
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath)
{
    NDIS_MINIPORT_CHARACTERISTICS Characteristics;
    PDRIVER_DISPATCH MajorFunctions[IRP_MJ_MAXIMUM_FUNCTION + 1];
    static UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(L"\\Device\\" FAKEMP_NAME);
    static UNICODE_STRING LinkName = RTL_CONSTANT_STRING(L"\\DosDevices\\" FAKEMP_NAME);
    NDIS_STATUS Status;
    UINT i;

    KeInitializeSpinLock(&G.Lock);

    NdisMInitializeWrapper(&G.Wrapper, DriverObject, RegistryPath, NULL);

    RtlZeroMemory(&Characteristics, sizeof(Characteristics));
    Characteristics.MajorNdisVersion = 5;
    Characteristics.MinorNdisVersion = 1;
    Characteristics.InitializeHandler = MpInitialize;
    Characteristics.HaltHandler = MpHalt;
    Characteristics.QueryInformationHandler = MpQueryInformation;
    Characteristics.SetInformationHandler = MpSetInformation;
    Characteristics.ResetHandler = MpReset;
    Characteristics.CheckForHangHandler = MpCheckForHang;
    Characteristics.SendHandler = MpSend;
    Characteristics.TransferDataHandler = MpTransferData;

    Status = NdisMRegisterMiniport(G.Wrapper, &Characteristics, sizeof(Characteristics));
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NdisTerminateWrapper(G.Wrapper, NULL);
        return Status;
    }

    /* The test's control device, which stays while the adapter comes and goes */
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        MajorFunctions[i] = NULL;
    MajorFunctions[IRP_MJ_CREATE] = ControlCreateClose;
    MajorFunctions[IRP_MJ_CLOSE] = ControlCreateClose;
    MajorFunctions[IRP_MJ_CLEANUP] = ControlCreateClose;
    MajorFunctions[IRP_MJ_DEVICE_CONTROL] = ControlIoctl;

    Status = NdisMRegisterDevice(G.Wrapper, &DeviceName, &LinkName, MajorFunctions,
                                 &G.ControlDevice, &G.ControlHandle);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        DbgPrint("FAKEMP: no control device (0x%x)\n", Status);
    }

    NdisMRegisterUnloadHandler(G.Wrapper, MpUnload);
    return STATUS_SUCCESS;
}
