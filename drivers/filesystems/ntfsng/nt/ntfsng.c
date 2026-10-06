/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver entry, dispatch, FCB lifetime, read-only refusals
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsng.h"

NG_GLOBAL NgGlobal;

#define NG_STACK_FILL 0x4e474e47UL

NTSTATUS NgErrnoToStatus(int Err)
{
    switch (-Err)
    {
        case 0: return STATUS_SUCCESS;
        case NGC_ENOENT: return STATUS_OBJECT_NAME_NOT_FOUND;
        case NGC_ENOMEM: return STATUS_INSUFFICIENT_RESOURCES;
        case NGC_ENOTDIR: return STATUS_NOT_A_DIRECTORY;
        case NGC_EISDIR: return STATUS_FILE_IS_A_DIRECTORY;
        case NGC_EROFS: return STATUS_MEDIA_WRITE_PROTECTED;
        case NGC_ENAMETOOLONG: return STATUS_OBJECT_NAME_INVALID;
        case NGC_EOPNOTSUPP: return STATUS_NOT_SUPPORTED;
        case NGC_EINVAL:
        case NGC_EUCLEAN: return STATUS_FILE_CORRUPT_ERROR;
        default: return STATUS_UNEXPECTED_IO_ERROR;
    }
}

VOID NgAcquireCore(PNG_VCB Vcb)
{
    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->CoreLock, TRUE);
}

VOID NgReleaseCore(PNG_VCB Vcb)
{
    ExReleaseResourceLite(&Vcb->CoreLock);
    KeLeaveCriticalRegion();
}

/*
 * Stack measurement.  On entry to every dispatch routine the unused part of
 * the thread's kernel stack is filled with a pattern; on exit the lowest
 * overwritten word gives the deepest use by this request, including the
 * storage stack below us and any interrupt that nested on this stack.
 */
static ULONG_PTR NgStackFill(VOID)
{
    ULONG_PTR Low, High, Here = (ULONG_PTR)&Low, P;
    IoGetStackLimits(&Low, &High);
    if (Here <= Low || Here > High)
        return 0;
    for (P = Low + 256; P + 512 < Here; P += sizeof(ULONG))
        *(volatile ULONG *)P = NG_STACK_FILL;
    return Low;
}

static VOID NgStackScan(ULONG_PTR Low, UCHAR Major)
{
    ULONG_PTR High, L2, P;
    ULONG Used;
    if (!Low)
        return;
    IoGetStackLimits(&L2, &High);
    if (L2 != Low)
        return;
    for (P = Low + 256; P < High; P += sizeof(ULONG))
        if (*(volatile ULONG *)P != NG_STACK_FILL)
            break;
    Used = (ULONG)(High - P);
    if (Used > NgGlobal.MaxStackUsed)
    {
        NgGlobal.MaxStackUsed = Used;
        NgGlobal.MaxStackMajor = Major;
        DPRINT1("ntfsng: stack: new max %lu bytes of %lu (IRP_MJ 0x%x)\n",
                Used, (ULONG)(High - Low), Major);
    }
}

VOID NgStackSample(VOID)
{
    ULONG_PTR Low, High, Here = (ULONG_PTR)&Low;
    IoGetStackLimits(&Low, &High);
    if (Here > Low && Here <= High && (ULONG)(High - Here) > NgGlobal.MaxStackAtIo)
    {
        NgGlobal.MaxStackAtIo = (ULONG)(High - Here);
        DPRINT1("ntfsng: stack: core at device read %lu bytes deep\n", NgGlobal.MaxStackAtIo);
    }
}

PNG_FCB NgAllocateFcb(PNG_VCB Vcb)
{
    PNG_FCB Fcb = ExAllocatePoolWithTag(NonPagedPool, sizeof(NG_FCB), TAG_NTFSNG);
    if (!Fcb)
        return NULL;
    RtlZeroMemory(Fcb, sizeof(*Fcb));
    Fcb->Header.NodeTypeCode = NG_NODE_FCB;
    Fcb->Header.NodeByteSize = sizeof(NG_FCB);
    Fcb->Header.IsFastIoPossible = FastIoIsPossible;
    Fcb->Header.Resource = &Fcb->MainResource;
    Fcb->Header.PagingIoResource = &Fcb->PagingIoResource;
    ExInitializeResourceLite(&Fcb->MainResource);
    ExInitializeResourceLite(&Fcb->PagingIoResource);
    Fcb->Vcb = Vcb;
    Fcb->RefCount = 1;
    InterlockedIncrement(&NgGlobal.FcbLive);
    return Fcb;
}

/*
 * Mm and Cc keep file objects (and so FCBs) of cached files alive long after
 * the last handle is closed.  An FCB without open handles therefore parks its
 * core inode and re-acquires it by MFT number on the next paging read.
 * Both run under the core lock.
 */
int NgEnsureNode(PNG_FCB Fcb)
{
    ngc_node *Base, *Stream;
    int Err;
    if (Fcb->Node || !Fcb->HasNode)
        return Fcb->Node ? 0 : -NGC_EINVAL;
    Err = ngc_iget(Fcb->Vcb->Core, Fcb->MftNo, &Base);
    if (Err)
        return Err;
    if (Fcb->Stream.Length)
    {
        Err = ngc_open_stream(Fcb->Vcb->Core, Base, Fcb->Stream.Buffer, Fcb->Stream.Length / sizeof(WCHAR), &Stream);
        ngc_put(Base);
        if (Err)
            return Err;
        Base = Stream;
    }
    Fcb->Node = Base;
    return 0;
}

VOID NgParkNode(PNG_FCB Fcb)
{
    if (Fcb->Node && !Fcb->IsRoot)
    {
        ngc_put(Fcb->Node);
        Fcb->Node = NULL;
    }
}

/* Fills Fcb->Stat and the Cc file sizes from the core inode. */
VOID NgFillStat(PNG_FCB Fcb)
{
    LONGLONG Alloc;
    if (!Fcb->HasNode)
        return;
    NgAcquireCore(Fcb->Vcb);
    if (NgEnsureNode(Fcb))
    {
        NgReleaseCore(Fcb->Vcb);
        return;
    }
    ngc_stat(Fcb->Node, &Fcb->Stat);
    NgReleaseCore(Fcb->Vcb);
    Fcb->MftNo = Fcb->Stat.mft_ref & 0xffffffffffffULL;
    Fcb->Header.FileSize.QuadPart = Fcb->Stat.size;
    Fcb->Header.ValidDataLength.QuadPart = Fcb->Stat.size;
    /* Cc needs AllocationSize >= FileSize; compressed and sparse streams report less on disk. */
    Alloc = (Fcb->Stat.size + PAGE_SIZE - 1) & ~(LONGLONG)(PAGE_SIZE - 1);
    if ((LONGLONG)Fcb->Stat.alloc > Alloc)
        Alloc = Fcb->Stat.alloc;
    Fcb->Header.AllocationSize.QuadPart = Alloc;
}

VOID NgDereferenceFcb(PNG_FCB Fcb)
{
    PNG_VCB Vcb = Fcb->Vcb;
    BOOLEAN Free;

    ExAcquireFastMutex(&Vcb->FcbListLock);
    Free = (InterlockedDecrement(&Fcb->RefCount) == 0);
    if (Free && Fcb->VcbLinks.Flink)
        RemoveEntryList(&Fcb->VcbLinks);
    if (Free && Vcb->VolumeFcb == Fcb)
        Vcb->VolumeFcb = NULL;
    ExReleaseFastMutex(&Vcb->FcbListLock);
    if (!Free)
        return;
    if (Fcb->Node)
    {
        NgAcquireCore(Vcb);
        ngc_put(Fcb->Node);
        NgReleaseCore(Vcb);
    }
    ExDeleteResourceLite(&Fcb->MainResource);
    ExDeleteResourceLite(&Fcb->PagingIoResource);
    InterlockedDecrement(&NgGlobal.FcbLive);
    ExFreePoolWithTag(Fcb, TAG_NTFSNG);
}

/* Cc callbacks: reads only, the core lock is taken inside the paging read itself. */
static BOOLEAN NTAPI NgAcquireForLazyWrite(PVOID Context, BOOLEAN Wait)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Wait);
    return TRUE;
}

static VOID NTAPI NgReleaseFromLazyWrite(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static NTSTATUS NgRefuseWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    return STATUS_MEDIA_WRITE_PROTECTED;
}

static NTSTATUS NgFlush(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    return STATUS_SUCCESS;
}

static NTSTATUS NgUnsupported(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS NgDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    NTSTATUS Status;
    /* Volume handles pass storage IOCTLs (geometry, partition info) through. */
    IoSkipCurrentIrpStackLocation(Irp);
    Status = IoCallDriver(Vcb->StorageDevice, Irp);
    return Status;
}

/* Fast I/O: cached reads go straight to Cc (FsRtlCopyRead); everything else takes the IRP path. */
static BOOLEAN NTAPI NgFastIoCheckIfPossible(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length,
                                             BOOLEAN Wait, ULONG LockKey, BOOLEAN CheckForReadOperation,
                                             PIO_STATUS_BLOCK IoStatus, PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(LockKey);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);
    return CheckForReadOperation;
}

static BOOLEAN NTAPI NgFastIoWrite(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length,
                                   BOOLEAN Wait, ULONG LockKey, PVOID Buffer, PIO_STATUS_BLOCK IoStatus,
                                   PDEVICE_OBJECT DeviceObject)
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(LockKey);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);
    return FALSE;
}

static FAST_IO_DISPATCH NgFastIoDispatch;

typedef NTSTATUS (*NG_HANDLER)(PDEVICE_OBJECT, PIRP);
static NG_HANDLER NgHandlers[IRP_MJ_MAXIMUM_FUNCTION + 1];

/*
 * The single dispatch entry: enters the file system, measures stack use and
 * completes the IRP with the handler's status (handlers never pend).
 */
static NTSTATUS NTAPI NgDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    UCHAR Major = Stack->MajorFunction;
    BOOLEAN TopLevel = FALSE;
    ULONG_PTR Low;
    NTSTATUS Status;

    Low = NgStackFill();
    FsRtlEnterFileSystem();
    if (!IoGetTopLevelIrp())
    {
        IoSetTopLevelIrp(Irp);
        TopLevel = TRUE;
    }
    Irp->IoStatus.Information = 0;
    if (DeviceObject == NgGlobal.ControlDevice && Major != IRP_MJ_FILE_SYSTEM_CONTROL)
    {
        /* The control device only accepts opens/closes and mount requests. */
        Status = (Major == IRP_MJ_CREATE || Major == IRP_MJ_CLEANUP || Major == IRP_MJ_CLOSE)
                 ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_REQUEST;
        if (Major == IRP_MJ_CREATE)
            Irp->IoStatus.Information = FILE_OPENED;
    }
    else if (Major == IRP_MJ_DEVICE_CONTROL && DeviceObject != NgGlobal.ControlDevice)
    {
        /* Passed down: the storage stack completes it. */
        Status = NgDeviceControl(DeviceObject, Irp);
        goto out;
    }
    else
    {
        Status = NgHandlers[Major](DeviceObject, Irp);
    }
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, NT_SUCCESS(Status) ? IO_DISK_INCREMENT : IO_NO_INCREMENT);
    }
out:
    if (TopLevel)
        IoSetTopLevelIrp(NULL);
    FsRtlExitFileSystem();
    NgStackScan(Low, Major);
    return Status;
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING Name;
    NTSTATUS Status;
    ULONG i;
    int Err;

    UNREFERENCED_PARAMETER(RegistryPath);
    DPRINT1("ntfsng: read-only NTFS on the Linux fs/ntfs core (v7.3-rc6), loading\n");

    Err = ngc_init();
    if (Err)
    {
        DPRINT1("ntfsng: core init failed %d\n", Err);
        return STATUS_UNSUCCESSFUL;
    }

    RtlInitUnicodeString(&Name, L"\\NtfsNg");
    Status = IoCreateDevice(DriverObject, 0, &Name, FILE_DEVICE_DISK_FILE_SYSTEM, 0, FALSE,
                            &NgGlobal.ControlDevice);
    if (!NT_SUCCESS(Status))
        return Status;
    NgGlobal.DriverObject = DriverObject;

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
    {
        NgHandlers[i] = NgUnsupported;
        DriverObject->MajorFunction[i] = NgDispatch;
    }
    NgHandlers[IRP_MJ_CREATE] = NgCreate;
    NgHandlers[IRP_MJ_CLEANUP] = NgCleanup;
    NgHandlers[IRP_MJ_CLOSE] = NgClose;
    NgHandlers[IRP_MJ_READ] = NgRead;
    NgHandlers[IRP_MJ_WRITE] = NgRefuseWrite;
    NgHandlers[IRP_MJ_QUERY_INFORMATION] = NgQueryInformation;
    NgHandlers[IRP_MJ_SET_INFORMATION] = NgSetInformation;
    NgHandlers[IRP_MJ_SET_EA] = NgRefuseWrite;
    NgHandlers[IRP_MJ_SET_VOLUME_INFORMATION] = NgRefuseWrite;
    NgHandlers[IRP_MJ_SET_SECURITY] = NgRefuseWrite;
    NgHandlers[IRP_MJ_FLUSH_BUFFERS] = NgFlush;
    NgHandlers[IRP_MJ_QUERY_VOLUME_INFORMATION] = NgQueryVolumeInformation;
    NgHandlers[IRP_MJ_DIRECTORY_CONTROL] = NgDirectoryControl;
    NgHandlers[IRP_MJ_FILE_SYSTEM_CONTROL] = NgFileSystemControl;
    DriverObject->DriverUnload = NULL;

    NgFastIoDispatch.SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    NgFastIoDispatch.FastIoCheckIfPossible = NgFastIoCheckIfPossible;
    NgFastIoDispatch.FastIoRead = FsRtlCopyRead;
    NgFastIoDispatch.FastIoWrite = NgFastIoWrite;
    DriverObject->FastIoDispatch = &NgFastIoDispatch;

    NgGlobal.CacheCallbacks.AcquireForLazyWrite = NgAcquireForLazyWrite;
    NgGlobal.CacheCallbacks.ReleaseFromLazyWrite = NgReleaseFromLazyWrite;
    NgGlobal.CacheCallbacks.AcquireForReadAhead = NgAcquireForLazyWrite;
    NgGlobal.CacheCallbacks.ReleaseFromReadAhead = NgReleaseFromLazyWrite;

    NgGlobal.ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    IoRegisterFileSystem(NgGlobal.ControlDevice);
    DPRINT1("ntfsng: registered file system\n");
    return STATUS_SUCCESS;
}
