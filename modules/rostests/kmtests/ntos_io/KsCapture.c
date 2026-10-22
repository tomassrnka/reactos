/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Defensive contract tests for the KS user-buffer capture paths
 *              (T54: methods.c / property.c / event.c).
 *
 * These are DEFENSIVE contract tests, not exploits. They verify that the KS
 * property/method/event dispatch helpers reject hostile (unmapped) user
 * buffers cleanly - returning an NTSTATUS error instead of bugchecking - and
 * that well-formed calls still dispatch. Before the T54 fix, the unmapped-
 * buffer method/property cases dereference user memory with no probe/SEH and
 * bugcheck the machine (fail-before); after the fix they return an error
 * (pass-after).
 */

#include <kmt_test.h>
#include <ks.h>

/* An address inside the NULL-dereference guard region of the caller process:
 * always within user range (so ProbeForRead's range check passes) but never
 * mapped (so the subsequent capture copy faults). */
#define BAD_USER_PTR  ((PVOID)(ULONG_PTR)0x1000)

static const GUID TestSetGuid =
    {0x11223344, 0x5566, 0x7788, {0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00}};

static BOOLEAN TestMethodCalled;

static
NTSTATUS
NTAPI
TestMethodHandler(
    _In_ PIRP Irp,
    _In_ PKSIDENTIFIER Request,
    _Inout_ PVOID Data)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Data);
    TestMethodCalled = TRUE;
    return STATUS_SUCCESS;
}

static const KSMETHOD_ITEM TestMethodItems[] =
{
    { 0, { TestMethodHandler }, sizeof(KSMETHOD), 0, NULL, 0 }
};

static const KSMETHOD_SET TestMethodSet[] =
{
    { &TestSetGuid, 1, TestMethodItems, 0, NULL }
};

static const KSEVENT_ITEM TestEventItems[] =
{
    { 0, 0, 0, NULL, NULL, NULL }
};

static KSEVENT_SET TestEventSet[] =
{
    { &TestSetGuid, 1, TestEventItems }
};

static
PIRP
BuildKsIrp(
    _In_ ULONG IoControlCode,
    _In_ KPROCESSOR_MODE Mode,
    _In_opt_ PVOID Type3Input,
    _In_ ULONG InputLength,
    _In_opt_ PVOID UserBuffer,
    _In_ ULONG OutputLength)
{
    PIRP Irp;
    PIO_STACK_LOCATION IoStack;

    Irp = IoAllocateIrp(1, FALSE);
    if (Irp == NULL)
        return NULL;

    IoSetNextIrpStackLocation(Irp);
    IoStack = IoGetCurrentIrpStackLocation(Irp);
    IoStack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    IoStack->MinorFunction = 0;
    IoStack->FileObject = NULL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
    IoStack->Parameters.DeviceIoControl.InputBufferLength = InputLength;
    IoStack->Parameters.DeviceIoControl.OutputBufferLength = OutputLength;
    IoStack->Parameters.DeviceIoControl.Type3InputBuffer = Type3Input;

    Irp->UserBuffer = UserBuffer;
    Irp->RequestorMode = Mode;
    Irp->AssociatedIrp.SystemBuffer = NULL;
    Irp->Flags = 0;
    Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
    Irp->IoStatus.Information = 0;
    return Irp;
}

START_TEST(KsCapture)
{
    PIRP Irp;
    NTSTATUS Status;
    KSMETHOD Method;
    KSEVENT Event;
    KSEVENTDATA EventData;
    LIST_ENTRY EventList;
    KSPIN_LOCK EventLock;

    ok_irql(PASSIVE_LEVEL);

    /* -----------------------------------------------------------------
     * T1: KsMethodHandler, user mode, UNMAPPED Type3 input buffer.
     * Pre-fix: no probe/capture -> dereferences user memory -> bugcheck.
     * Post-fix: probe + SEH capture -> clean error return.
     * --------------------------------------------------------------- */
    Irp = BuildKsIrp(IOCTL_KS_METHOD, UserMode, BAD_USER_PTR, sizeof(KSMETHOD), NULL, 0);
    if (!skip(Irp != NULL, "IoAllocateIrp failed\n"))
    {
        Status = KsMethodHandler(Irp, 1, TestMethodSet);
        trace("T1 method/unmapped: Status=0x%08lx\n", Status);
        ok(!NT_SUCCESS(Status), "T1: expected failure for unmapped method buffer, got 0x%08lx\n", Status);
        ok(Status == STATUS_ACCESS_VIOLATION, "T1: expected STATUS_ACCESS_VIOLATION, got 0x%08lx\n", Status);
        IoFreeIrp(Irp);
    }

    /* -----------------------------------------------------------------
     * T2: KsPropertyHandler, user mode, UNMAPPED Type3 input buffer.
     * Pre-fix: ProbeForRead/RtlCopyMemory not under SEH -> bugcheck.
     * Post-fix: probe + SEH capture -> clean error return.
     * --------------------------------------------------------------- */
    Irp = BuildKsIrp(IOCTL_KS_PROPERTY, UserMode, BAD_USER_PTR, sizeof(KSPROPERTY), NULL, 0);
    if (!skip(Irp != NULL, "IoAllocateIrp failed\n"))
    {
        Status = KsPropertyHandler(Irp, 1, NULL);
        trace("T2 property/unmapped: Status=0x%08lx\n", Status);
        ok(!NT_SUCCESS(Status), "T2: expected failure for unmapped property buffer, got 0x%08lx\n", Status);
        ok(Status == STATUS_ACCESS_VIOLATION, "T2: expected STATUS_ACCESS_VIOLATION, got 0x%08lx\n", Status);
        IoFreeIrp(Irp);
    }

    /* -----------------------------------------------------------------
     * T3: KsMethodHandler, kernel mode, VALID matching method.
     * Functional positive: dispatch still works, handler is invoked.
     * --------------------------------------------------------------- */
    RtlZeroMemory(&Method, sizeof(Method));
    Method.Set = TestSetGuid;
    Method.Id = 0;
    Method.Flags = 0;
    TestMethodCalled = FALSE;
    Irp = BuildKsIrp(IOCTL_KS_METHOD, KernelMode, &Method, sizeof(KSMETHOD), NULL, 0);
    if (!skip(Irp != NULL, "IoAllocateIrp failed\n"))
    {
        Status = KsMethodHandler(Irp, 1, TestMethodSet);
        trace("T3 method/valid: Status=0x%08lx Called=%d\n", Status, TestMethodCalled);
        ok(Status == STATUS_SUCCESS, "T3: expected STATUS_SUCCESS, got 0x%08lx\n", Status);
        ok(TestMethodCalled == TRUE, "T3: method handler was not invoked\n");
        IoFreeIrp(Irp);
    }

    /* -----------------------------------------------------------------
     * T4: KsPropertyHandler, undersized input buffer.
     * Functional positive: too-small input rejected cleanly before any
     * probe/allocation.
     * --------------------------------------------------------------- */
    Irp = BuildKsIrp(IOCTL_KS_PROPERTY, KernelMode, &Method, sizeof(KSPROPERTY) - 1, NULL, 0);
    if (!skip(Irp != NULL, "IoAllocateIrp failed\n"))
    {
        Status = KsPropertyHandler(Irp, 1, NULL);
        trace("T4 property/undersized: Status=0x%08lx\n", Status);
        ok(Status == STATUS_INVALID_BUFFER_SIZE, "T4: expected STATUS_INVALID_BUFFER_SIZE, got 0x%08lx\n", Status);
        IoFreeIrp(Irp);
    }

    /* -----------------------------------------------------------------
     * T5: KsEnableEvent, kernel mode, VALID matching event set, invalid
     * NotificationType. Exercises the type-validation decision taken from
     * the (captured) event data; must reject cleanly with no list change.
     * --------------------------------------------------------------- */
    InitializeListHead(&EventList);
    KeInitializeSpinLock(&EventLock);
    RtlZeroMemory(&Event, sizeof(Event));
    Event.Set = TestSetGuid;
    Event.Id = 0;
    Event.Flags = KSEVENT_TYPE_ENABLE;
    RtlZeroMemory(&EventData, sizeof(EventData));
    EventData.NotificationType = 0xDEADBEEF;
    Irp = BuildKsIrp(IOCTL_KS_ENABLE_EVENT, KernelMode, &Event, sizeof(KSEVENT), &EventData, sizeof(KSEVENTDATA));
    if (!skip(Irp != NULL, "IoAllocateIrp failed\n"))
    {
        Status = KsEnableEvent(Irp, 1, TestEventSet, &EventList, KSEVENTS_SPINLOCK, &EventLock);
        trace("T5 event/badtype: Status=0x%08lx\n", Status);
        ok(Status == STATUS_INVALID_PARAMETER, "T5: expected STATUS_INVALID_PARAMETER, got 0x%08lx\n", Status);
        ok(IsListEmpty(&EventList), "T5: event list must remain empty after rejection\n");
        IoFreeIrp(Irp);
    }

    /* -----------------------------------------------------------------
     * T6: KsEnableEvent, user mode, UNMAPPED Type3 (KSEVENT) buffer.
     * Defensive sanity: a hostile user buffer is rejected without a
     * bugcheck. (Note: the KsEnableEvent prologue already probed the
     * KSEVENT under SEH before T54, so this is not a before/after
     * differentiator - the exploitable event.c bug is a NotificationType
     * double-fetch race, which is deliberately NOT weaponized here.)
     * --------------------------------------------------------------- */
    Irp = BuildKsIrp(IOCTL_KS_ENABLE_EVENT, UserMode, BAD_USER_PTR, sizeof(KSEVENT), BAD_USER_PTR, sizeof(KSEVENTDATA));
    if (!skip(Irp != NULL, "IoAllocateIrp failed\n"))
    {
        Status = KsEnableEvent(Irp, 1, TestEventSet, &EventList, KSEVENTS_SPINLOCK, &EventLock);
        trace("T6 event/unmapped: Status=0x%08lx\n", Status);
        ok(!NT_SUCCESS(Status), "T6: expected failure for unmapped event buffer, got 0x%08lx\n", Status);
        ok(IsListEmpty(&EventList), "T6: event list must remain empty after rejection\n");
        IoFreeIrp(Irp);
    }
}
