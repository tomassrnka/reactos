/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test driver for CcZeroData on cached files
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define TEST_FILE_SIZE  0x80000
#define DISK_BYTE       0x5A

typedef struct _TEST_FCB
{
    FSRTL_ADVANCED_FCB_HEADER Header;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    FAST_MUTEX HeaderMutex;
} TEST_FCB, *PTEST_FCB;

static KMT_IRP_HANDLER TestIrpHandler;
static FAST_IO_DISPATCH TestFastIoDispatch;

/* Paging I/O seen by the "disk": one bit per page */
static ULONG PagesRead[TEST_FILE_SIZE / PAGE_SIZE / 32];
static ULONG PagesWrittenZero[TEST_FILE_SIZE / PAGE_SIZE / 32];
static ULONG PagesWrittenData[TEST_FILE_SIZE / PAGE_SIZE / 32];

static
BOOLEAN
NTAPI
FastIoRead(
    _In_ PFILE_OBJECT FileObject,
    _In_ PLARGE_INTEGER FileOffset,
    _In_ ULONG Length,
    _In_ BOOLEAN Wait,
    _In_ ULONG LockKey,
    _Out_ PVOID Buffer,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    IoStatus->Status = STATUS_NOT_SUPPORTED;
    return FALSE;
}

static
BOOLEAN
NTAPI
FastIoWrite(
    _In_ PFILE_OBJECT FileObject,
    _In_ PLARGE_INTEGER FileOffset,
    _In_ ULONG Length,
    _In_ BOOLEAN Wait,
    _In_ ULONG LockKey,
    _Out_ PVOID Buffer,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    IoStatus->Status = STATUS_NOT_SUPPORTED;
    return FALSE;
}

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(RegistryPath);

    *DeviceName = L"CcZeroData";
    *Flags = TESTENTRY_NO_EXCLUSIVE_DEVICE |
             TESTENTRY_BUFFERED_IO_DEVICE |
             TESTENTRY_NO_READONLY_DEVICE;

    KmtRegisterIrpHandler(IRP_MJ_CLEANUP, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_CREATE, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_READ, NULL, TestIrpHandler);
    KmtRegisterIrpHandler(IRP_MJ_WRITE, NULL, TestIrpHandler);

    TestFastIoDispatch.FastIoRead = FastIoRead;
    TestFastIoDispatch.FastIoWrite = FastIoWrite;
    DriverObject->FastIoDispatch = &TestFastIoDispatch;

    return STATUS_SUCCESS;
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
}

static BOOLEAN NTAPI AcquireForLazyWrite(_In_ PVOID Context, _In_ BOOLEAN Wait) { return TRUE; }
static VOID NTAPI ReleaseFromLazyWrite(_In_ PVOID Context) { }
static BOOLEAN NTAPI AcquireForReadAhead(_In_ PVOID Context, _In_ BOOLEAN Wait) { return TRUE; }
static VOID NTAPI ReleaseFromReadAhead(_In_ PVOID Context) { }

static CACHE_MANAGER_CALLBACKS Callbacks = {
    AcquireForLazyWrite,
    ReleaseFromLazyWrite,
    AcquireForReadAhead,
    ReleaseFromReadAhead,
};

static
VOID
MarkPages(PULONG Bits, LONGLONG Offset, ULONG Length)
{
    LONGLONG Page;

    for (Page = Offset / PAGE_SIZE; Page < (Offset + Length + PAGE_SIZE - 1) / PAGE_SIZE && Page < TEST_FILE_SIZE / PAGE_SIZE; Page++)
        Bits[Page / 32] |= 1UL << (Page % 32);
}

static
BOOLEAN
PageMarked(PULONG Bits, LONGLONG Offset)
{
    LONGLONG Page = Offset / PAGE_SIZE;

    return (Bits[Page / 32] & (1UL << (Page % 32))) != 0;
}

/* Bytes of the cached range that read as Value */
static
ULONG
CountCached(PFILE_OBJECT FileObject, LONGLONG Offset, ULONG Length, UCHAR Value)
{
    PUCHAR Buffer;
    LARGE_INTEGER Start;
    IO_STATUS_BLOCK IoStatus;
    ULONG i, Count = 0;

    Buffer = ExAllocatePoolWithTag(NonPagedPool, Length, 'DZcC');
    if (!Buffer)
        return MAXULONG;
    Start.QuadPart = Offset;
    _SEH2_TRY
    {
        if (CcCopyRead(FileObject, &Start, Length, TRUE, Buffer, &IoStatus))
        {
            for (i = 0; i < Length; i++)
                Count += Buffer[i] == Value;
        }
        else
        {
            Count = MAXULONG;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Count = MAXULONG;
    }
    _SEH2_END;
    ExFreePoolWithTag(Buffer, 'DZcC');
    return Count;
}

static
BOOLEAN
ZeroData(PFILE_OBJECT FileObject, LONGLONG Start, LONGLONG End)
{
    LARGE_INTEGER S, E;
    BOOLEAN Ret = FALSE;

    S.QuadPart = Start;
    E.QuadPart = End;
    _SEH2_TRY
    {
        Ret = CcZeroData(FileObject, &S, &E, TRUE);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        ok(FALSE, "CcZeroData raised %lx\n", _SEH2_GetExceptionCode());
    }
    _SEH2_END;
    return Ret;
}

/*
 * Measured on Windows Server 2008 R2 and Windows 10 (x64): CcZeroData leaves pages that are in the cache as they are
 * (data from the disk or written through the cache), and a page it only partly covers keeps the disk's data. Whole
 * pages that are not cached read zero afterwards on Windows (it does not read them); ReactOS reads such pages when
 * they lie below the cache map's valid data length and keeps what the file system returned, so the test accepts
 * either result for them, but never a mix. Pages past the valid data length read zero on both.
 */
static
VOID
Test_CcZeroData(PFILE_OBJECT FileObject)
{
    PTEST_FCB Fcb = FileObject->FsContext;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    UCHAR Data[0x100];
    ULONG Zeros, Disk;

    RtlZeroMemory(PagesRead, sizeof(PagesRead));
    RtlZeroMemory(PagesWrittenZero, sizeof(PagesWrittenZero));
    RtlZeroMemory(PagesWrittenData, sizeof(PagesWrittenData));

    /* 1. Whole pages not in the cache, below the valid data length */
    ok_bool_true(ZeroData(FileObject, 0x10000, 0x12000), "CcZeroData");
    trace("not cached: page 0x10000 read from disk during CcZeroData: %d\n", PageMarked(PagesRead, 0x10000));
    Zeros = CountCached(FileObject, 0x10000, 0x2000, 0);
    Disk = CountCached(FileObject, 0x10000, 0x2000, DISK_BYTE);
    ok(Zeros == 0x2000 || Disk == 0x2000, "not cached: %lu zero and %lu disk bytes of 0x2000\n", Zeros, Disk);

    /* 2. Pages in the cache, as the disk returned them */
    Disk = CountCached(FileObject, 0x20000, 0x2000, DISK_BYTE);
    ok_eq_ulong(Disk, 0x2000);
    ok_bool_true(ZeroData(FileObject, 0x20000, 0x22000), "CcZeroData");
    Disk = CountCached(FileObject, 0x20000, 0x2000, DISK_BYTE);
    ok_eq_ulong(Disk, 0x2000);

    /* 3. Pages in the cache with data written through the cache */
    RtlFillMemory(Data, sizeof(Data), 0xC3);
    Offset.QuadPart = 0x30000;
    ok_bool_true(CcCopyWrite(FileObject, &Offset, sizeof(Data), TRUE, Data), "CcCopyWrite");
    ok_bool_true(ZeroData(FileObject, 0x30000, 0x31000), "CcZeroData");
    Disk = CountCached(FileObject, 0x30000, sizeof(Data), 0xC3);
    ok_eq_ulong(Disk, sizeof(Data));

    /* 4. Part of a page that is not cached */
    ok_bool_true(ZeroData(FileObject, 0x38200, 0x39000), "CcZeroData");
    Disk = CountCached(FileObject, 0x38000, 0x1000, DISK_BYTE);
    ok_eq_ulong(Disk, 0x1000);

    CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &IoStatus);
    trace("written as zero: 0x10000 %d 0x20000 %d 0x30000 %d; with data: 0x20000 %d 0x30000 %d 0x38000 %d\n",
          PageMarked(PagesWrittenZero, 0x10000), PageMarked(PagesWrittenZero, 0x20000), PageMarked(PagesWrittenZero, 0x30000),
          PageMarked(PagesWrittenData, 0x20000), PageMarked(PagesWrittenData, 0x30000), PageMarked(PagesWrittenData, 0x38000));

    /* 5. Whole pages not in the cache, past the valid data length: zero, nothing read */
    Fcb->Header.ValidDataLength.QuadPart = 0x50000;
    CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
    RtlZeroMemory(PagesRead, sizeof(PagesRead));
    ok_bool_true(ZeroData(FileObject, 0x60000, 0x62000), "CcZeroData");
    ok(!PageMarked(PagesRead, 0x60000) && !PageMarked(PagesRead, 0x61000), "past VDL: pages were read from disk\n");
    Zeros = CountCached(FileObject, 0x60000, 0x2000, 0);
    ok_eq_ulong(Zeros, 0x2000);
}

static
PVOID
MapIrpBuffer(_In_ PIRP Irp, _In_ ULONG Length)
{
    UNREFERENCED_PARAMETER(Length);
    if (!Irp->MdlAddress)
        return NULL;
    return MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
}

static
NTSTATUS
TestIrpHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStack)
{
    LARGE_INTEGER Zero = RTL_CONSTANT_LARGE_INTEGER(0LL);
    NTSTATUS Status = STATUS_NOT_SUPPORTED;
    PTEST_FCB Fcb;
    CACHE_UNINITIALIZE_EVENT CacheUninitEvent;

    PAGED_CODE();

    Irp->IoStatus.Information = 0;

    if (IoStack->MajorFunction == IRP_MJ_CREATE)
    {
        Fcb = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Fcb), 'FzcC');
        if (!Fcb)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            RtlZeroMemory(Fcb, sizeof(*Fcb));
            ExInitializeFastMutex(&Fcb->HeaderMutex);
            FsRtlSetupAdvancedHeader(&Fcb->Header, &Fcb->HeaderMutex);
            Fcb->Header.AllocationSize.QuadPart = TEST_FILE_SIZE;
            Fcb->Header.FileSize.QuadPart = TEST_FILE_SIZE;
            Fcb->Header.ValidDataLength.QuadPart = TEST_FILE_SIZE;
            Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
            IoStack->FileObject->FsContext = Fcb;
            IoStack->FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;
            CcInitializeCacheMap(IoStack->FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize, FALSE, &Callbacks, NULL);
            Irp->IoStatus.Information = FILE_OPENED;
            Status = STATUS_SUCCESS;
        }
    }
    else if (IoStack->MajorFunction == IRP_MJ_READ && FlagOn(Irp->Flags, IRP_PAGING_IO))
    {
        ULONG Length = IoStack->Parameters.Read.Length;
        PVOID Buffer = MapIrpBuffer(Irp, Length);

        MarkPages(PagesRead, IoStack->Parameters.Read.ByteOffset.QuadPart, Length);
        if (Buffer)
            RtlFillMemory(Buffer, Length, DISK_BYTE);
        Irp->IoStatus.Information = Length;
        Status = Buffer ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    }
    else if (IoStack->MajorFunction == IRP_MJ_WRITE && FlagOn(Irp->Flags, IRP_PAGING_IO))
    {
        ULONG Length = IoStack->Parameters.Write.Length, i, Page;
        LONGLONG Offset = IoStack->Parameters.Write.ByteOffset.QuadPart;
        PUCHAR Buffer = MapIrpBuffer(Irp, Length);

        for (Page = 0; Buffer && Page < Length; Page += PAGE_SIZE)
        {
            BOOLEAN AllZero = TRUE;

            for (i = 0; i < PAGE_SIZE && Page + i < Length; i++)
                AllZero &= Buffer[Page + i] == 0;
            MarkPages(AllZero ? PagesWrittenZero : PagesWrittenData, Offset + Page, 1);
        }
        Irp->IoStatus.Information = Length;
        Status = STATUS_SUCCESS;
    }
    else if (IoStack->MajorFunction == IRP_MJ_WRITE)
    {
        Test_CcZeroData(IoStack->FileObject);
        Irp->IoStatus.Information = IoStack->Parameters.Write.Length;
        Status = STATUS_SUCCESS;
    }
    else if (IoStack->MajorFunction == IRP_MJ_CLEANUP)
    {
        KeInitializeEvent(&CacheUninitEvent.Event, NotificationEvent, FALSE);
        CcUninitializeCacheMap(IoStack->FileObject, &Zero, &CacheUninitEvent);
        KeWaitForSingleObject(&CacheUninitEvent.Event, Executive, KernelMode, FALSE, NULL);
        Fcb = IoStack->FileObject->FsContext;
        ExFreePoolWithTag(Fcb, 'FzcC');
        IoStack->FileObject->FsContext = NULL;
        Status = STATUS_SUCCESS;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}
