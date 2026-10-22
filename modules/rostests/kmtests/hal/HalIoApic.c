/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test I/O APIC access while interrupts are connected and arrive
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define PHASE_SECONDS           5

/* ISA IRQs that must have no device and no driver on any processor, as in the QEMU PC
   virtual machine this hidden test is meant for */
static const ULONG CandidateIrqs[] = { 5, 10, 3 };

typedef struct _TEST_IRQ
{
    ULONG Irq;
    ULONG Vector;
    KIRQL Irql;
    KAFFINITY Affinity;
} TEST_IRQ, *PTEST_IRQ;

static TEST_IRQ QueryIrq, ConnectIrq;
static volatile LONG StopConnect, StopLocal, StopQuery;
static volatile LONG Connects, ConnectFailures;
static volatile LONG Queries, QueryMismatches;
static volatile LONG LocalQueries, LocalMismatches;

static KSERVICE_ROUTINE IdleIsr;

static
BOOLEAN
NTAPI
IdleIsr(
    _In_ PKINTERRUPT Interrupt,
    _In_ PVOID ServiceContext)
{
    return FALSE;
}

static
NTSTATUS
ConnectTestIrq(
    _In_ PTEST_IRQ TestIrq,
    _Out_ PKINTERRUPT *Interrupt)
{
    /* Every connection goes to processor 0 */
    return IoConnectInterrupt(Interrupt, IdleIsr, NULL, NULL,
                              TestIrq->Vector, TestIrq->Irql, TestIrq->Irql,
                              Latched, FALSE, 1, FALSE);
}

static
BOOLEAN
PickIrq(
    _Out_ PTEST_IRQ TestIrq,
    _In_ ULONG SkipIrq)
{
    PKINTERRUPT Interrupt;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(CandidateIrqs); i++)
    {
        if (CandidateIrqs[i] == SkipIrq)
            continue;

        TestIrq->Irq = CandidateIrqs[i];
        TestIrq->Vector = HalGetInterruptVector(Isa, 0, TestIrq->Irq, TestIrq->Irq,
                                                &TestIrq->Irql, &TestIrq->Affinity);
        if (TestIrq->Vector == 0)
            continue;

        /* Skip an IRQ that a driver has connected on processor 0 (this does not prove it free) */
        if (!NT_SUCCESS(ConnectTestIrq(TestIrq, &Interrupt)))
            continue;
        IoDisconnectInterrupt(Interrupt);
        return TRUE;
    }
    return FALSE;
}

static
VOID
ResetCounters(VOID)
{
    Connects = ConnectFailures = 0;
    Queries = QueryMismatches = 0;
    LocalQueries = LocalMismatches = 0;
}

static KSTART_ROUTINE ConnectThread;
static
VOID
NTAPI
ConnectThread(
    _In_ PVOID Context)
{
    PKINTERRUPT Interrupt;

    /* KeConnectInterrupt programs the I/O APIC at SYNCH_LEVEL on processor 0 */
    while (!StopConnect)
    {
        if (NT_SUCCESS(ConnectTestIrq(&ConnectIrq, &Interrupt)))
            IoDisconnectInterrupt(Interrupt);
        else
            InterlockedIncrement(&ConnectFailures);
        InterlockedIncrement(&Connects);
    }
}

#ifdef _M_IX86
static KSTART_ROUTINE LocalQueryThread;
static
VOID
NTAPI
LocalQueryThread(
    _In_ PVOID Context)
{
    ULONG Vector, i;
    KIRQL OldIrql, Irql;
    KAFFINITY Affinity;

    KeSetSystemAffinityThread(1);

    /*
     * With the lazy IRQL, the HAL defers the first clock interrupt that arrives while this
     * processor is at HIGH_LEVEL; without the I/O APIC lock, often in the middle of a query.
     * HIGH_LEVEL is not a supported IRQL for HalGetInterruptVector: it only invites this.
     */
    while (!StopLocal)
    {
        KeRaiseIrql(HIGH_LEVEL, &OldIrql);
        for (i = 0; i < 64; i++)
        {
            Vector = HalGetInterruptVector(Isa, 0, ConnectIrq.Irq, ConnectIrq.Irq,
                                           &Irql, &Affinity);
            if (Vector != ConnectIrq.Vector)
                InterlockedIncrement(&LocalMismatches);
            InterlockedIncrement(&LocalQueries);
        }
        KeLowerIrql(OldIrql);
    }

    KeRevertToUserAffinityThread();
}
#endif /* _M_IX86 */

static KSTART_ROUTINE QueryThread;
static
VOID
NTAPI
QueryThread(
    _In_ PVOID Context)
{
    ULONG Vector;
    KIRQL Irql;
    KAFFINITY Affinity;

    KeSetSystemAffinityThread((KAFFINITY)1 << 1);

    /* HalGetInterruptVector reads the redirection entry of an allocated IRQ */
    while (!StopQuery)
    {
        Vector = HalGetInterruptVector(Isa, 0, QueryIrq.Irq, QueryIrq.Irq, &Irql, &Affinity);
        if (Vector != QueryIrq.Vector)
            InterlockedIncrement(&QueryMismatches);
        InterlockedIncrement(&Queries);
    }

    KeRevertToUserAffinityThread();
}

static
VOID
WaitSeconds(
    _In_ ULONG Seconds)
{
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = -(LONGLONG)Seconds * 10 * 1000 * 1000;
    KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
}

#ifdef _M_IX86
static
VOID
TestClockDeferral(VOID)
{
    PKTHREAD Worker;

    /* Phase 1: the clock interrupt arrives while processor 0 reads the I/O APIC. The lazy IRQL
       defers it, so this needs one processor. It fails only while an I/O APIC access can be
       interrupted and the deferral reads the I/O APIC. */
    ExSetTimerResolution(10 * 1000, TRUE);
    ResetCounters();
    StopLocal = 0;
    Worker = KmtStartThread(LocalQueryThread, NULL);
    WaitSeconds(PHASE_SECONDS);
    StopLocal = 1;
    KmtFinishThread(Worker, NULL);
    ExSetTimerResolution(0, FALSE);
    DbgPrint("HalIoApic phase 1: %ld queries at HIGH_LEVEL, %ld wrong\n",
             LocalQueries, LocalMismatches);
    ok(LocalQueries > 0, "No queries\n");
    ok(LocalMismatches == 0, "%ld of %ld vector queries returned another vector\n",
       LocalMismatches, LocalQueries);
}
#endif /* _M_IX86 */

static
VOID
TestConcurrentAccess(VOID)
{
    PKTHREAD Connect, Query;

    /* Phase 2: processor 1 reads the I/O APIC while processor 0 programs it */
    ResetCounters();
    StopConnect = StopQuery = 0;
    Connect = KmtStartThread(ConnectThread, NULL);
    Query = KmtStartThread(QueryThread, NULL);
    WaitSeconds(PHASE_SECONDS);
    StopQuery = 1;
    KmtFinishThread(Query, NULL);
    StopConnect = 1;
    KmtFinishThread(Connect, NULL);
    DbgPrint("HalIoApic phase 2: %ld connects, %ld failed, %ld queries, %ld wrong\n",
             Connects, ConnectFailures, Queries, QueryMismatches);
    ok(Connects > 0, "No connects\n");
    ok(Queries > 0, "No queries\n");
    ok(ConnectFailures == 0, "%ld of %ld connects failed\n", ConnectFailures, Connects);
    ok(QueryMismatches == 0, "%ld of %ld vector queries returned another vector\n",
       QueryMismatches, Queries);
}

START_TEST(HalIoApic)
{
    INT CpuInfo[4];

    /* It borrows ISA IRQs that only a virtual machine is known to leave unused */
    __cpuid(CpuInfo, 1);
    if (skip(((ULONG)CpuInfo[2] & 0x80000000UL) != 0, "The test runs only under a hypervisor\n"))
        return;

    if (skip(PickIrq(&QueryIrq, 0) && PickIrq(&ConnectIrq, QueryIrq.Irq), "No free ISA IRQ\n"))
        return;
    trace("Query IRQ %lu vector 0x%lx IRQL %u, connect IRQ %lu vector 0x%lx IRQL %u\n",
          QueryIrq.Irq, QueryIrq.Vector, QueryIrq.Irql,
          ConnectIrq.Irq, ConnectIrq.Vector, ConnectIrq.Irql);

#ifdef _M_IX86
    /* Only the ReactOS ACPI HALs let HalGetInterruptVector run at HIGH_LEVEL. This excludes
       other systems; it does not detect the ReactOS legacy HALs, which the test does not target. */
    if (!skip(is_reactos(), "Phase 1 runs only on ReactOS\n"))
        TestClockDeferral();
#endif
    if (skip(KeNumberProcessors >= 2, "Phase 2 needs two processors\n"))
        return;
    TestConcurrentAccess();
}
