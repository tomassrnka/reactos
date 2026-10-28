/*
 * PROJECT:     ReactOS test suite
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Fake NDIS 5.1 miniport, control interface shared with the user-mode test
 */

#ifndef _FAKEMP_H
#define _FAKEMP_H

/* Private OIDs: the fake miniport behaves as configured for these, and answers the
   OIDs NDIS and the protocols need at once. */
#define FAKEMP_OID_TEST_QUERY   0xFF000100
#define FAKEMP_OID_TEST_SET     0xFF000101

/* How the miniport handles a request for a private OID */
#define FAKEMP_REQ_IMMEDIATE    0   /* Completes in the handler */
#define FAKEMP_REQ_PEND_DELAY   1   /* NDIS_STATUS_PENDING, completes from a timer after DelayMs */
#define FAKEMP_REQ_PEND_NEVER   2   /* NDIS_STATUS_PENDING, completes only on FAKEMP_IOCTL_COMPLETE_HELD */
#define FAKEMP_REQ_SPIN         3   /* Stays in the handler (at DISPATCH_LEVEL) until a reset or
                                       FAKEMP_IOCTL_RELEASE_SPIN, then returns PENDING as PEND_NEVER.
                                       After a reset it stays DelayMs longer. */

/* How the miniport handles a reset */
#define FAKEMP_RESET_SYNC       0   /* Completes in the handler */
#define FAKEMP_RESET_PEND       1   /* NDIS_STATUS_PENDING until FAKEMP_IOCTL_FINISH_RESET */

/* How the miniport handles a send of a FAKEMP_TEST_ETHERTYPE frame (the others complete in the handler) */
#define FAKEMP_SEND_IMMEDIATE   0   /* Completes in the handler */
#define FAKEMP_SEND_HOLD        1   /* NDIS_STATUS_PENDING, completes on FAKEMP_IOCTL_COMPLETE_SEND or in the halt */
#define FAKEMP_SEND_DELAY       2   /* NDIS_STATUS_PENDING, a timer completes the held sends, newest first, SendDelayMs
                                       after the last one came */

#define FAKEMP_TEST_ETHERTYPE   0x88B5  /* IEEE local experimental */
#define FAKEMP_MAX_HELD_SENDS   32

/* FAKEMP_IOCTL_COMPLETE_SEND input: which held send completes */
#define FAKEMP_COMPLETE_OLDEST  0
#define FAKEMP_COMPLETE_NEWEST  1
#define FAKEMP_COMPLETE_ALL     2

/* FAKEMP_IOCTL_INDICATE: a FAKEMP_TEST_ETHERTYPE frame of FAKEMP_RX_FRAME_SIZE bytes to the adapter's address,
   data byte i (from the start of the frame) = Seed + i. With Lookahead less than the data, the protocols copy
   the rest with NdisTransferData, which the miniport does as TransferMode says. */
#define FAKEMP_RX_FRAME_SIZE    128
#define FAKEMP_XFER_SYNC        0   /* Copies in the handler */
#define FAKEMP_XFER_HOLD        1   /* NDIS_STATUS_PENDING, copies and completes on FAKEMP_IOCTL_COMPLETE_TRANSFERS or in the halt (fails) */
#define FAKEMP_XFER_NEVER       2   /* NDIS_STATUS_PENDING, held like HOLD for FAKEMP_IOCTL_COMPLETE_TRANSFERS, but the halt does NOT
                                       complete it: it is forgotten there, as a miniport that never completes a transfer */
#define FAKEMP_MAX_HELD_XFERS   8

typedef struct _FAKEMP_INDICATE
{
    ULONG Seed;
    ULONG Lookahead;            /* Data bytes after the header in the lookahead buffer */
    ULONG TransferMode;         /* FAKEMP_XFER_* */
} FAKEMP_INDICATE, *PFAKEMP_INDICATE;

typedef struct _FAKEMP_CONFIG
{
    ULONG QueryMode;            /* FAKEMP_REQ_* for FAKEMP_OID_TEST_QUERY */
    ULONG SetMode;              /* FAKEMP_REQ_* for FAKEMP_OID_TEST_SET */
    ULONG DelayMs;              /* FAKEMP_REQ_PEND_DELAY */
    ULONG ResetMode;            /* FAKEMP_RESET_* */
    ULONG AddressingReset;      /* *AddressingReset the reset handler returns */
    ULONG SendMode;             /* FAKEMP_SEND_* */
    ULONG SendDelayMs;          /* FAKEMP_SEND_DELAY */
} FAKEMP_CONFIG, *PFAKEMP_CONFIG;

typedef struct _FAKEMP_STATS
{
    ULONG Initializes;
    ULONG Halts;
    ULONG Halted;               /* Currently halted */
    ULONG CallsAfterHalt;       /* Handlers entered while halted */
    ULONG Requests;             /* Query/set handler entries */
    ULONG InHandler;            /* Query/set handlers running now */
    ULONG MaxInHandler;         /* ... the most at once */
    ULONG Outstanding;          /* Requests given to the miniport and not completed by it */
    ULONG MaxOutstanding;
    ULONG Held;                 /* A request is pended */
    ULONG Spinning;             /* A handler is in FAKEMP_REQ_SPIN */
    ULONG Completions;          /* Requests the miniport completed after pending them */
    ULONG Resets;
    ULONG ResetPending;         /* The reset handler returned PENDING and is not finished */
    ULONG ResetsCompleted;
    ULONG HangChecks;
    ULONG HangCpu;              /* The processor the last CheckForHang ran on: NDIS's timer DPC (and the reset) run there */
    ULONG Sends;                /* Test frames given to the send handler */
    ULONG SendsHeld;            /* ... pended and not completed yet */
    ULONG MaxSendsHeld;
    ULONG SendsCompleted;       /* ... completed after pending them */
    ULONG SendsCompletedAtHalt; /* ... of those, by the halt */
    ULONG SendDataBad;          /* A held send's data changed before the miniport completed it: its sender let go of it */
    ULONG Indications;          /* FAKEMP_IOCTL_INDICATE */
    ULONG Transfers;            /* Transfer handler entries */
    ULONG TransfersHeld;        /* ... pended and not completed yet */
    ULONG TransfersCompleted;   /* ... completed after pending them */
    ULONG TransfersCompletedAtHalt;
    ULONG FilterFailures;       /* OID_GEN_CURRENT_PACKET_FILTER sets failed (FAKEMP_IOCTL_FAIL_FILTER) */
    ULONG TransfersNever;       /* Transfers pended in FAKEMP_XFER_NEVER mode (forgotten at the halt) */
} FAKEMP_STATS, *PFAKEMP_STATS;

#define FAKEMP_IOCTL_SET_CONFIG     CTL_CODE(FILE_DEVICE_NETWORK, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS) /* in: FAKEMP_CONFIG */
#define FAKEMP_IOCTL_GET_STATS      CTL_CODE(FILE_DEVICE_NETWORK, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS) /* out: FAKEMP_STATS */
#define FAKEMP_IOCTL_RESET_STATS    CTL_CODE(FILE_DEVICE_NETWORK, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS) /* clears the counters, not the state */
#define FAKEMP_IOCTL_TRIGGER_HANG   CTL_CODE(FILE_DEVICE_NETWORK, 0x903, METHOD_BUFFERED, FILE_ANY_ACCESS) /* next CheckForHang returns TRUE: NDIS resets */
#define FAKEMP_IOCTL_COMPLETE_HELD  CTL_CODE(FILE_DEVICE_NETWORK, 0x904, METHOD_BUFFERED, FILE_ANY_ACCESS) /* completes the pended request now (NDIS_STATUS_SUCCESS) */
#define FAKEMP_IOCTL_FINISH_RESET   CTL_CODE(FILE_DEVICE_NETWORK, 0x905, METHOD_BUFFERED, FILE_ANY_ACCESS) /* NdisMResetComplete for a pended reset */
#define FAKEMP_IOCTL_RELEASE_SPIN   CTL_CODE(FILE_DEVICE_NETWORK, 0x906, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FAKEMP_IOCTL_INDICATE       CTL_CODE(FILE_DEVICE_NETWORK, 0x908, METHOD_BUFFERED, FILE_ANY_ACCESS) /* in: FAKEMP_INDICATE */
#define FAKEMP_IOCTL_COMPLETE_TRANSFERS CTL_CODE(FILE_DEVICE_NETWORK, 0x909, METHOD_BUFFERED, FILE_ANY_ACCESS) /* completes the held transfers */
#define FAKEMP_IOCTL_FAIL_FILTER    CTL_CODE(FILE_DEVICE_NETWORK, 0x90A, METHOD_BUFFERED, FILE_ANY_ACCESS) /* in: ULONG n: the next n sets of the packet filter fail (kept across a halt and re-add: makes a protocol's bind fail after its media indication) */
#define FAKEMP_IOCTL_COMPLETE_SEND  CTL_CODE(FILE_DEVICE_NETWORK, 0x907, METHOD_BUFFERED, FILE_ANY_ACCESS) /* in: ULONG FAKEMP_COMPLETE_*; out (optional): ULONG sends completed */


#endif
