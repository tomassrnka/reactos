/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test driver for a lazy write that races with the deletion of another cache map
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define IOCTL_RUN_TEST 1

typedef struct _TEST_FCB
{
    FSRTL_ADVANCED_FCB_HEADER Header;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    FAST_MUTEX HeaderMutex;
} TEST_FCB, *PTEST_FCB;

enum
{
    PHASE_IDLE,
    PHASE_ARMED,
    PHASE_DELETING,
    PHASE_DONE
};

static KMT_IRP_HANDLER TestIrpHandler;
static KMT_MESSAGE_HANDLER TestMessageHandler;

static PTEST_FCB TestFcb[2];
static PFILE_OBJECT TestFileObject[2];
static volatile LONG TestPhase = PHASE_IDLE;
static volatile LONG TestRevived;
static KEVENT FirstAcquireEvent;
static KEVENT DeleteFlushEvent;
static KEVENT FirstFailedEvent;

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    *DeviceName = L"CcLazyWriteDelete";
    *Flags = TESTENTRY_NO_EXCLUSIVE_DEVICE |
             TESTENTRY_BUFFERED_IO_DEVICE |
             TESTENTRY_NO_READONLY_DEVICE;

    KmtRegisterIrpHandler(IRP_MJ_READ, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_WRITE, NULL, TestIrpHandler);
    KmtRegisterMessageHandler(0, NULL, TestMessageHandler);

    return STATUS_SUCCESS;
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
}

static
VOID
WaitSeconds(
    _In_ PKEVENT Event,
    _In_ LONG Seconds)
{
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = Seconds * -10000000LL;
    if (Event)
        KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &Timeout);
    else
        KeDelayExecutionThread(KernelMode, FALSE, &Timeout);
}

/*
 * Cc calls this without its list lock. On the first call for file 0, the cache map of file 1,
 * whose dirty view follows file 0's in the dirty list, is deleted meanwhile; the acquire then
 * fails, as for a file whose lock is busy. Cc must not go on to lazy-write file 1.
 */
static
BOOLEAN
NTAPI
AcquireForLazyWrite(
    _In_ PVOID Context,
    _In_ BOOLEAN Wait)
{
    if (TestPhase == PHASE_IDLE)
        return FALSE;

    if (Context == TestFcb[0] &&
        InterlockedCompareExchange(&TestPhase, PHASE_DELETING, PHASE_ARMED) == PHASE_ARMED)
    {
        KeSetEvent(&FirstAcquireEvent, IO_NO_INCREMENT, FALSE);
        WaitSeconds(&DeleteFlushEvent, 10);
        KeSetEvent(&FirstFailedEvent, IO_NO_INCREMENT, FALSE);
        return FALSE;
    }

    /* Until its deletion writes the view, file 1 is still listed and another walker may ask for it */
    if (Context == TestFcb[1] && TestPhase == PHASE_DELETING)
    {
        if (KeReadStateEvent(&DeleteFlushEvent))
        {
            DPRINT1("CcLazyWriteDelete: lazy write of a cache map that is being deleted\n");
            InterlockedExchange(&TestRevived, 1);
        }
        return FALSE;
    }

    return TRUE;
}

static
VOID
NTAPI
ReleaseFromLazyWrite(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static
BOOLEAN
NTAPI
AcquireForReadAhead(
    _In_ PVOID Context,
    _In_ BOOLEAN Wait)
{
    return TRUE;
}

static
VOID
NTAPI
ReleaseFromReadAhead(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static CACHE_MANAGER_CALLBACKS Callbacks = {
    AcquireForLazyWrite,
    ReleaseFromLazyWrite,
    AcquireForReadAhead,
    ReleaseFromReadAhead,
};

static
BOOLEAN
SetupFile(
    _In_ ULONG Index,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    PTEST_FCB Fcb;
    PFILE_OBJECT FileObject;

    FileObject = IoCreateStreamFileObject(NULL, DeviceObject);
    if (skip(FileObject != NULL, "Failed to allocate FO\n"))
        return FALSE;

    Fcb = ExAllocatePoolWithTag(NonPagedPool, sizeof(TEST_FCB), 'FwlK');
    if (skip(Fcb != NULL, "Failed to allocate FCB\n"))
    {
        ObDereferenceObject(FileObject);
        return FALSE;
    }

    RtlZeroMemory(Fcb, sizeof(TEST_FCB));
    ExInitializeFastMutex(&Fcb->HeaderMutex);
    FsRtlSetupAdvancedHeader(&Fcb->Header, &Fcb->HeaderMutex);
    Fcb->Header.AllocationSize.QuadPart = VACB_MAPPING_GRANULARITY;
    Fcb->Header.FileSize.QuadPart = VACB_MAPPING_GRANULARITY;
    Fcb->Header.ValidDataLength.QuadPart = VACB_MAPPING_GRANULARITY;
    FileObject->FsContext = Fcb;
    FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;

    TestFcb[Index] = Fcb;
    TestFileObject[Index] = FileObject;

    KmtStartSeh();
    CcInitializeCacheMap(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize, TRUE, &Callbacks, Fcb);
    KmtEndSeh(STATUS_SUCCESS);

    return !skip(CcIsFileCached(FileObject) == TRUE, "CcInitializeCacheMap failed\n");
}

static
BOOLEAN
DirtyFile(
    _In_ ULONG Index)
{
    LARGE_INTEGER Offset;
    PVOID Bcb;
    PULONG Buffer;
    BOOLEAN Ret = FALSE;

    Offset.QuadPart = 0;
    KmtStartSeh();
    Ret = CcPinRead(TestFileObject[Index], &Offset, PAGE_SIZE, PIN_WAIT, &Bcb, (PVOID *)&Buffer);
    KmtEndSeh(STATUS_SUCCESS);
    if (skip(Ret == TRUE, "CcPinRead failed\n"))
        return FALSE;

    Buffer[0] = 0xDADADADA;
    CcSetDirtyPinnedData(Bcb, NULL);
    CcUnpinData(Bcb);
    return TRUE;
}

static
VOID
UncacheFile(
    _In_ ULONG Index)
{
    CACHE_UNINITIALIZE_EVENT UninitEvent;

    if (!TestFileObject[Index])
        return;

    if (CcIsFileCached(TestFileObject[Index]))
    {
        KeInitializeEvent(&UninitEvent.Event, NotificationEvent, FALSE);
        CcUninitializeCacheMap(TestFileObject[Index], NULL, &UninitEvent);
        KeWaitForSingleObject(&UninitEvent.Event, Executive, KernelMode, FALSE, NULL);
    }
}

static
VOID
FreeFile(
    _In_ ULONG Index)
{
    if (TestFileObject[Index])
    {
        /* Drop the pages Mm still holds, so that its section lets the file object go */
        CcPurgeCacheSection(TestFileObject[Index]->SectionObjectPointer, NULL, 0, FALSE);
        TestFileObject[Index]->FsContext = NULL;
        TestFileObject[Index]->SectionObjectPointer = NULL;
        ObDereferenceObject(TestFileObject[Index]);
        TestFileObject[Index] = NULL;
    }
    if (TestFcb[Index])
    {
        ExFreePoolWithTag(TestFcb[Index], 'FwlK');
        TestFcb[Index] = NULL;
    }
}

static
VOID
RunTest(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    KeInitializeEvent(&FirstAcquireEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&DeleteFlushEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&FirstFailedEvent, NotificationEvent, FALSE);
    TestRevived = 0;
    TestPhase = PHASE_IDLE;

    /*
     * Until armed, every lazy write fails, so both views stay dirty, file 0's ahead of file 1's.
     * A view dirtied elsewhere in the system between the two can hide the race (a pass, never a
     * failure): Cc offers no way to check that the two are adjacent in its dirty list.
     */
    if (SetupFile(0, DeviceObject) && SetupFile(1, DeviceObject) &&
        DirtyFile(0) && DirtyFile(1))
    {
        InterlockedExchange(&TestPhase, PHASE_ARMED);

        Timeout.QuadPart = -30 * 10000000LL;
        Status = KeWaitForSingleObject(&FirstAcquireEvent, Executive, KernelMode, FALSE, &Timeout);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (!skip(Status == STATUS_SUCCESS, "No lazy write of file 0\n"))
        {
            /* Delete file 1's cache map; its dirty view is written while the acquire for file 0 is pending */
            UncacheFile(1);
            ok(KeReadStateEvent(&DeleteFlushEvent) != 0, "The deleted cache map's dirty view was not written\n");
            ok(KeReadStateEvent(&FirstFailedEvent) != 0, "The acquire for file 0 did not return\n");
            ok_eq_long(TestRevived, 0L);
        }
    }

    InterlockedExchange(&TestPhase, PHASE_DONE);
    UncacheFile(1);
    UncacheFile(0);
    FreeFile(1);
    FreeFile(0);
    TestPhase = PHASE_IDLE;
}

static
NTSTATUS
TestMessageHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _In_opt_ PVOID Buffer,
    _In_ SIZE_T InLength,
    _Inout_ PSIZE_T OutLength)
{
    NTSTATUS Status = STATUS_SUCCESS;

    switch (ControlCode)
    {
        case IOCTL_RUN_TEST:
            FsRtlEnterFileSystem();
            RunTest(DeviceObject);
            FsRtlExitFileSystem();
            break;

        default:
            Status = STATUS_NOT_IMPLEMENTED;
            break;
    }

    return Status;
}

static
NTSTATUS
TestIrpHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStack)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PVOID Buffer;

    PAGED_CODE();

    ASSERT(IoStack->MajorFunction == IRP_MJ_READ ||
           IoStack->MajorFunction == IRP_MJ_WRITE);

    FsRtlEnterFileSystem();

    if (IoStack->MajorFunction == IRP_MJ_READ)
    {
        Buffer = Irp->MdlAddress ? MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority) : NULL;
        ok(Buffer != NULL, "No buffer for a paging read\n");
        if (Buffer)
        {
            RtlZeroMemory(Buffer, IoStack->Parameters.Read.Length);
            Irp->IoStatus.Information = IoStack->Parameters.Read.Length;
        }
        else
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Information = 0;
        }
    }
    else
    {
        if (IoStack->FileObject == TestFileObject[1] && TestPhase == PHASE_DELETING &&
            !KeReadStateEvent(&DeleteFlushEvent))
        {
            /* Let the failed acquire for file 0 return, and give Cc time to go on with its list */
            KeSetEvent(&DeleteFlushEvent, IO_NO_INCREMENT, FALSE);
            WaitSeconds(&FirstFailedEvent, 10);
            WaitSeconds(NULL, 2);
        }
        Irp->IoStatus.Information = IoStack->Parameters.Write.Length;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    FsRtlExitFileSystem();

    return Status;
}
