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

typedef struct _FAKEMP_CONFIG
{
    ULONG QueryMode;            /* FAKEMP_REQ_* for FAKEMP_OID_TEST_QUERY */
    ULONG SetMode;              /* FAKEMP_REQ_* for FAKEMP_OID_TEST_SET */
    ULONG DelayMs;              /* FAKEMP_REQ_PEND_DELAY */
    ULONG ResetMode;            /* FAKEMP_RESET_* */
    ULONG AddressingReset;      /* *AddressingReset the reset handler returns */
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
} FAKEMP_STATS, *PFAKEMP_STATS;

#define FAKEMP_IOCTL_SET_CONFIG     CTL_CODE(FILE_DEVICE_NETWORK, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS) /* in: FAKEMP_CONFIG */
#define FAKEMP_IOCTL_GET_STATS      CTL_CODE(FILE_DEVICE_NETWORK, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS) /* out: FAKEMP_STATS */
#define FAKEMP_IOCTL_RESET_STATS    CTL_CODE(FILE_DEVICE_NETWORK, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS) /* clears the counters, not the state */
#define FAKEMP_IOCTL_TRIGGER_HANG   CTL_CODE(FILE_DEVICE_NETWORK, 0x903, METHOD_BUFFERED, FILE_ANY_ACCESS) /* next CheckForHang returns TRUE: NDIS resets */
#define FAKEMP_IOCTL_COMPLETE_HELD  CTL_CODE(FILE_DEVICE_NETWORK, 0x904, METHOD_BUFFERED, FILE_ANY_ACCESS) /* completes the pended request now (NDIS_STATUS_SUCCESS) */
#define FAKEMP_IOCTL_FINISH_RESET   CTL_CODE(FILE_DEVICE_NETWORK, 0x905, METHOD_BUFFERED, FILE_ANY_ACCESS) /* NdisMResetComplete for a pended reset */
#define FAKEMP_IOCTL_RELEASE_SPIN   CTL_CODE(FILE_DEVICE_NETWORK, 0x906, METHOD_BUFFERED, FILE_ANY_ACCESS)


#endif
