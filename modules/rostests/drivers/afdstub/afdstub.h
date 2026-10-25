/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Control interface of the AFD stub datagram transport
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#pragma once

/* AFD opens this device as the transport of a datagram socket (the transport
 * name of the endpoint). A handle opened without extended attributes is the
 * control channel of the test. */
#define AFDSTUB_DEVICE_NAME     L"\\Device\\AfdStubUdp"

#define IOCTL_AFDSTUB_SET       CTL_CODE(FILE_DEVICE_NETWORK, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AFDSTUB_QUERY     CTL_CODE(FILE_DEVICE_NETWORK, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_AFDSTUB_COMPLETE  CTL_CODE(FILE_DEVICE_NETWORK, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define AFDSTUB_FAIL            2
#define AFDSTUB_ALL             0xFFFFFFFF
#define AFDSTUB_MAX_REPORTED    16

typedef struct _AFDSTUB_SET
{
    /* 0: complete a datagram send inside the call, and return
     * STATUS_PENDING like tcpip does. 1: keep it in flight until IOCTL_AFDSTUB_COMPLETE.
     * AFDSTUB_FAIL: complete it inside the call with a failure. */
    ULONG HoldSends;
    /* 1: IoCancelIrp on a held datagram completes it with STATUS_CANCELLED.
     * 0: the cancel is only recorded (a transport that cannot take it back). */
    ULONG HonorCancel;
    /* 1: closing the address file completes its held datagrams. 0: they stay
     * until IOCTL_AFDSTUB_COMPLETE, i.e. they may complete after the socket
     * is gone. */
    ULONG CompleteOnCleanup;
} AFDSTUB_SET, *PAFDSTUB_SET;

typedef struct _AFDSTUB_HELD
{
    ULONG Sequence;     /* number of the send since the last set */
    ULONG Length;
    ULONG FirstByte;
    ULONG CancelRequested;
} AFDSTUB_HELD, *PAFDSTUB_HELD;

typedef struct _AFDSTUB_STATE
{
    ULONG SendsSeen;
    ULONG SendsInline;
    ULONG Held;                 /* datagram sends in flight now */
    ULONG CleanupWhileHeld;     /* address file cleanups that found sends in flight */
    ULONG CancelledByCancel;    /* completed by the cancel routine */
    ULONG CompletedByTest;
    ULONG AddressFilesOpen;
    ULONG Reported;
    AFDSTUB_HELD Entry[AFDSTUB_MAX_REPORTED];   /* in arrival order */
} AFDSTUB_STATE, *PAFDSTUB_STATE;

typedef struct _AFDSTUB_COMPLETE
{
    ULONG Index;                /* among the held sends in arrival order, or AFDSTUB_ALL */
    LONG Status;
} AFDSTUB_COMPLETE, *PAFDSTUB_COMPLETE;
