/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver entry, dispatch, FCB lifetime
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
        case NGC_ENOSPC: return STATUS_DISK_FULL;
        case NGC_EEXIST: return STATUS_OBJECT_NAME_COLLISION;
        case NGC_ENOTEMPTY: return STATUS_DIRECTORY_NOT_EMPTY;
        case NGC_EACCES:
        case NGC_EPERM: return STATUS_ACCESS_DENIED;
        case NGC_EFBIG: return STATUS_DISK_FULL;
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
    Fcb->Header.Resource = &Fcb->MainResource;
    Fcb->Header.PagingIoResource = &Fcb->PagingIoResource;
    ExInitializeResourceLite(&Fcb->MainResource);
    ExInitializeResourceLite(&Fcb->PagingIoResource);
    FsRtlInitializeFileLock(&Fcb->FileLock, NULL, NULL);
    KeInitializeSpinLock(&Fcb->RunLock);
    Fcb->Header.IsFastIoPossible = FastIoIsQuestionable;
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
    if (Fcb->Deleted)
        return -NGC_ENOENT;
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
    if (Fcb->SectionObjectPointers.SharedCacheMap && Fcb->Header.FileSize.QuadPart != (LONGLONG)Fcb->Stat.size)
        DPRINT1("ntfsng: BUG: cached %I64x header size %I64d, core size %I64u\n", Fcb->MftNo,
                Fcb->Header.FileSize.QuadPart, Fcb->Stat.size);
    Fcb->MftNo = Fcb->Stat.mft_ref & 0xffffffffffffULL;
    Fcb->Header.FileSize.QuadPart = Fcb->Stat.size;
    Fcb->Header.ValidDataLength.QuadPart = Fcb->Stat.size;
    /* Cc needs AllocationSize >= FileSize; compressed and sparse streams report less on disk. */
    Alloc = (Fcb->Stat.size + PAGE_SIZE - 1) & ~(LONGLONG)(PAGE_SIZE - 1);
    if ((LONGLONG)Fcb->Stat.alloc > Alloc)
        Alloc = Fcb->Stat.alloc;
    Fcb->Header.AllocationSize.QuadPart = Alloc;
    if (Alloc > Fcb->CachedEnd)
        Fcb->CachedEnd = Alloc;
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
    FsRtlUninitializeFileLock(&Fcb->FileLock);
    if (Fcb->Runs)
        ExFreePoolWithTag(Fcb->Runs, TAG_NTFSNG);
    if (Fcb->DelPath.Buffer)
        ExFreePoolWithTag(Fcb->DelPath.Buffer, TAG_NTFSNG);
    ExDeleteResourceLite(&Fcb->MainResource);
    ExDeleteResourceLite(&Fcb->PagingIoResource);
    InterlockedDecrement(&NgGlobal.FcbLive);
    ExFreePoolWithTag(Fcb, TAG_NTFSNG);
}

/* EA, volume label and security writes are not implemented. */
static NTSTATUS NgRefuseWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/*
 * Security descriptors are not written yet.  Mm sets a DACL on a new paging file and gives up
 * the paging file if that fails, so for a paging file the request succeeds without effect.
 */
static NTSTATUS NgSetSecurity(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    PNG_FCB Fcb = FileObject ? FileObject->FsContext : NULL;
    if (Fcb && Fcb->IsPagingFile)
        return STATUS_SUCCESS;
    return NgRefuseWrite(DeviceObject, Irp);
}

static NTSTATUS NgRefuseWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    UNREFERENCED_PARAMETER(Irp);
    return Vcb->ReadOnly ? STATUS_MEDIA_WRITE_PROTECTED : STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS NgUnsupported(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    return STATUS_INVALID_DEVICE_REQUEST;
}

/* TRUE for an open of the volume itself (\\.\C:), whose requests go to the storage stack. */
static BOOLEAN NgIsVolumeOpen(PIRP Irp)
{
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    PNG_FCB Fcb = FileObject ? FileObject->FsContext : NULL;
    return Fcb && Fcb->IsVolume;
}

/*
 * Volume handles pass storage IOCTLs (geometry, partition info, mount manager queries)
 * through.  Files and directories refuse device controls: kernel32 takes a
 * directory whose handle answers IOCTL_MOUNTDEV_QUERY_DEVICE_NAME for a volume root.
 */
static NTSTATUS NgPassToStorage(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    if (!NgIsVolumeOpen(Irp))
    {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Vcb->StorageDevice, Irp);
}

/* Fast I/O: cached reads go straight to Cc (FsRtlCopyRead); everything else takes the IRP path. */
static BOOLEAN NTAPI NgFastIoCheckIfPossible(PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length,
                                             BOOLEAN Wait, ULONG LockKey, BOOLEAN CheckForReadOperation,
                                             PIO_STATUS_BLOCK IoStatus, PDEVICE_OBJECT DeviceObject)
{
    PNG_FCB Fcb = FileObject ? FileObject->FsContext : NULL;
    LARGE_INTEGER Len;
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);
    if (!CheckForReadOperation || !Fcb || Fcb->IsVolume || Fcb->IsDirectory)
        return FALSE;
    Len.QuadPart = Length;
    return FsRtlFastCheckLockForRead(&Fcb->FileLock, FileOffset, &Len, LockKey, FileObject, PsGetCurrentProcess());
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
        if (Major == IRP_MJ_SHUTDOWN)
            Status = NgShutdown(DeviceObject, Irp);
        else
            Status = (Major == IRP_MJ_CREATE || Major == IRP_MJ_CLEANUP || Major == IRP_MJ_CLOSE)
                     ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_REQUEST;
        if (Major == IRP_MJ_CREATE)
            Irp->IoStatus.Information = FILE_OPENED;
    }
    else if (Major == IRP_MJ_DEVICE_CONTROL && DeviceObject != NgGlobal.ControlDevice)
    {
        /* Passed down: the storage stack completes it. */
        Status = NgPassToStorage(DeviceObject, Irp);
        goto out;
    }
    else if (Major == IRP_MJ_LOCK_CONTROL)
    {
        /* FsRtl completes lock IRPs itself. */
        Status = NgLockControl(DeviceObject, Irp);
        goto out;
    }
    else
    {
        Status = NgHandlers[Major](DeviceObject, Irp);
    }
    if (Status != STATUS_PENDING)
    {
        NgDiagLogRequest(DeviceObject, Irp, Status);
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

/* Reads a REG_DWORD from the service key; 0 if it is absent. */
static ULONG NgReadDword(PUNICODE_STRING KeyPath, PCWSTR Name)
{
    OBJECT_ATTRIBUTES Oa;
    UNICODE_STRING ValueName;
    UCHAR Buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    HANDLE Key;
    ULONG Length, Value = 0;
    NTSTATUS Status;

    InitializeObjectAttributes(&Oa, KeyPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenKey(&Key, KEY_READ, &Oa);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ntfsng: cannot open %wZ (0x%08lx)\n", KeyPath, Status);
        return 0;
    }
    RtlInitUnicodeString(&ValueName, Name);
    Status = ZwQueryValueKey(Key, &ValueName, KeyValuePartialInformation, Info, sizeof(Buffer), &Length);
    if (NT_SUCCESS(Status) && Info->Type == REG_DWORD && Info->DataLength == sizeof(ULONG))
        Value = *(PULONG)Info->Data;
    ZwClose(Key);
    return Value;
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING Name;
    NTSTATUS Status;
    ULONG i;
    int Err;

    DPRINT1("ntfsng: NTFS on the Linux fs/ntfs core (v7.3-rc6), loading\n");
    ExInitializeFastMutex(&NgGlobal.VcbListLock);
    InitializeListHead(&NgGlobal.VcbList);
    NgGlobal.PermissiveOpen = NgReadDword(RegistryPath, L"PermissiveOpen");
    NgGlobal.ForceReadOnly = NgReadDword(RegistryPath, L"ReadOnly");
    DPRINT1("ntfsng: ReadOnly=%lu\n", NgGlobal.ForceReadOnly);
    i = NgReadDword(RegistryPath, L"JournalFault");
    if (i)
    {
        /* Test only: stop the machine inside one commit between JournalFault and 2*JournalFault. */
        LARGE_INTEGER Now;
        ULONG Target;
        KeQuerySystemTime(&Now);
        Target = i + (ULONG)((ULONGLONG)Now.QuadPart / 10000 % i);
        ngc_set_journal_fault(Target);
        DPRINT1("ntfsng: JournalFault: the system stops in the middle of commit %lu\n", Target);
    }
    DPRINT1("ntfsng: service key %wZ, PermissiveOpen=%lu%s\n", RegistryPath, NgGlobal.PermissiveOpen,
            NgGlobal.PermissiveOpen ? " (opens with write access are granted, modifications are still refused)" : "");

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
    NgHandlers[IRP_MJ_WRITE] = NgWrite;
    NgHandlers[IRP_MJ_QUERY_INFORMATION] = NgQueryInformation;
    NgHandlers[IRP_MJ_SET_INFORMATION] = NgSetInformation;
    NgHandlers[IRP_MJ_SET_EA] = NgRefuseWrite;
    NgHandlers[IRP_MJ_SET_VOLUME_INFORMATION] = NgRefuseWrite;
    NgHandlers[IRP_MJ_SET_SECURITY] = NgSetSecurity;
    NgHandlers[IRP_MJ_FLUSH_BUFFERS] = NgFlushBuffers;
    NgHandlers[IRP_MJ_SHUTDOWN] = NgShutdown;
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
    NgGlobal.CacheCallbacks.AcquireForReadAhead = NgAcquireForReadAhead;
    NgGlobal.CacheCallbacks.ReleaseFromReadAhead = NgReleaseFromReadAhead;

    NgGlobal.ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    IoRegisterFileSystem(NgGlobal.ControlDevice);
    DPRINT1("ntfsng: registered file system\n");
    return STATUS_SUCCESS;
}
