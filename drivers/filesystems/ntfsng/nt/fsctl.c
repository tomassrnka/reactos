/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IRP_MJ_FILE_SYSTEM_CONTROL: mount, verify, volume FSCTLs
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsng.h"
#include "../shim/include/ngos.h"
#include <ntstrsafe.h>

static NTSTATUS NgDeviceIoctl(PDEVICE_OBJECT Device, ULONG Code, PVOID Out, ULONG OutLength)
{
    IO_STATUS_BLOCK Iosb;
    KEVENT Event;
    NTSTATUS Status;
    PIRP Irp;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(Code, Device, NULL, 0, Out, OutLength, FALSE, &Event, &Iosb);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    IoGetNextIrpStackLocation(Irp)->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    Status = IoCallDriver(Device, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    return Status;
}

NTSYSAPI NTSTATUS NTAPI ExRaiseHardError(NTSTATUS ErrorStatus, ULONG NumberOfParameters, ULONG UnicodeStringParameterMask,
                                          PULONG_PTR Parameters, ULONG ValidResponseOptions, PULONG Response);

#define NG_HARDERROR_OVERRIDE_ERRORMODE 0x10000000
#define NG_OPTION_OK 1
#define NG_RESPONSE_RETURN_TO_CALLER 0
#define NG_MB_ICONERROR 0x10

typedef struct _NG_DAMAGE_NOTE
{
    UNICODE_STRING Text, Caption;
    WCHAR Buffer[512];
} NG_DAMAGE_NOTE, *PNG_DAMAGE_NOTE;

/*
 * Tells the user that a volume was mounted read-only because it is damaged: a message box through
 * the hard error port once the session's error port exists (until then ExRaiseHardError returns
 * ResponseReturnToCaller without showing anything), for at most about ten minutes after the mount.
 */
static VOID NTAPI NgDamageNotifyThread(PVOID Context)
{
    PNG_DAMAGE_NOTE Note = Context;
    ULONG_PTR Params[3] = { (ULONG_PTR)&Note->Text, (ULONG_PTR)&Note->Caption, NG_MB_ICONERROR };
    ULONG Response = NG_RESPONSE_RETURN_TO_CALLER, Try;
    LARGE_INTEGER Delay;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    Delay.QuadPart = -10 * 1000 * 1000 * 5LL;
    for (Try = 0; Try < 120; Try++)
    {
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        Status = ExRaiseHardError(STATUS_SERVICE_NOTIFICATION | NG_HARDERROR_OVERRIDE_ERRORMODE, 3, 3, Params,
                                  NG_OPTION_OK, &Response);
        if (NT_SUCCESS(Status) && Response != NG_RESPONSE_RETURN_TO_CALLER)
            break;
    }
    DPRINT1("ntfsng: damage notice %s (status %lx, response %lu, %lu tries)\n",
            Response != NG_RESPONSE_RETURN_TO_CALLER ? "shown" : "NOT shown", Status, Response, Try + 1);
    ExFreePoolWithTag(Note, TAG_NTFSNG);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID NgReportDamage(PNG_VCB Vcb, const char *Why)
{
    PNG_DAMAGE_NOTE Note;
    HANDLE Thread;
    ULONG i;

    for (i = 0; i < 3; i++)
        DPRINT1("ntfsng: ********** VOLUME %08lx IS DAMAGED: mounted READ-ONLY, nothing will be written to it **********\n",
                Vcb->Vpb->SerialNumber);
    DPRINT1("ntfsng: damage: %s\n", Why ? Why : "?");
    Note = ExAllocatePoolWithTag(PagedPool, sizeof(*Note), TAG_NTFSNG);
    if (!Note)
        return;
    RtlZeroMemory(Note, sizeof(*Note));
    RtlInitUnicodeString(&Note->Caption, L"NTFS volume damaged");
    Note->Text.Buffer = Note->Buffer;
    Note->Text.MaximumLength = sizeof(Note->Buffer) - 2 * sizeof(WCHAR);
    RtlStringCbPrintfW(Note->Buffer, Note->Text.MaximumLength,
                       L"The NTFS volume with serial number %04lX-%04lX is damaged (%hs).\n\n"
                       L"It was mounted read-only to protect it: changes made to it in this session, "
                       L"including registry changes on the system volume, are not saved. "
                       L"Check and repair the volume with a disk checker on another system.",
                       Vcb->Vpb->SerialNumber >> 16, Vcb->Vpb->SerialNumber & 0xffff, Why ? Why : "unknown");
    Note->Text.Length = (USHORT)(wcslen(Note->Buffer) * sizeof(WCHAR));
    if (NT_SUCCESS(PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, NgDamageNotifyThread, Note)))
        ZwClose(Thread);
    else
        ExFreePoolWithTag(Note, TAG_NTFSNG);
}

static NTSTATUS NgMountVolume(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PDEVICE_OBJECT Target = Stack->Parameters.MountVolume.DeviceObject;
    PVPB Vpb = Stack->Parameters.MountVolume.Vpb;
    DISK_GEOMETRY Geometry;
    PARTITION_INFORMATION PartInfo;
    PDEVICE_OBJECT Vdo = NULL;
    PNG_VCB Vcb;
    PUCHAR Boot;
    ULONG SectorSize = 512, i;
    ULONGLONG Size = 0;
    NTSTATUS Status;
    const char *WhyRo = NULL;
    int Err;

    if (DeviceObject != NgGlobal.ControlDevice)
        return STATUS_INVALID_DEVICE_REQUEST;

    if (NT_SUCCESS(NgDeviceIoctl(Target, IOCTL_DISK_GET_DRIVE_GEOMETRY, &Geometry, sizeof(Geometry))) &&
        Geometry.BytesPerSector >= 512 && Geometry.BytesPerSector <= 4096)
    {
        SectorSize = Geometry.BytesPerSector;
    }
    if (NT_SUCCESS(NgDeviceIoctl(Target, IOCTL_DISK_GET_PARTITION_INFO, &PartInfo, sizeof(PartInfo))))
        Size = PartInfo.PartitionLength.QuadPart;

    Boot = ExAllocatePoolWithTag(NonPagedPool, 4096, TAG_NTFSNG);
    if (!Boot)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (ngos_dev_read(Target, 0, Boot, SectorSize) ||
        RtlCompareMemory(Boot + 3, "NTFS    ", 8) != 8 ||
        Boot[510] != 0x55 || Boot[511] != 0xAA)
    {
        ExFreePoolWithTag(Boot, TAG_NTFSNG);
        return STATUS_UNRECOGNIZED_VOLUME;
    }
    if (!Size)
    {
        /* No partition information (superfloppy): trust the boot sector, plus its backup copy. */
        Size = (*(ULONGLONG UNALIGNED *)(Boot + 0x28) + 1) * *(USHORT UNALIGNED *)(Boot + 0x0b);
    }
    ExFreePoolWithTag(Boot, TAG_NTFSNG);

    /* The page index is 32 bits, so a byte offset at or above 16 TiB (2^32 pages of 4 KiB) wraps.
     * Refuse such a volume rather than alias its block-device and file I/O onto low offsets. */
    if (Size >= (1ULL << 44))
    {
        DPRINT1("ntfsng: volume of %I64u bytes is 16 TiB or larger, not mounting (32-bit page index)\n", Size);
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    Status = IoCreateDevice(NgGlobal.DriverObject, sizeof(NG_VCB), NULL, FILE_DEVICE_DISK_FILE_SYSTEM,
                            0, FALSE, &Vdo);
    if (!NT_SUCCESS(Status))
        return Status;
    Vcb = Vdo->DeviceExtension;
    RtlZeroMemory(Vcb, sizeof(*Vcb));
    Vcb->NodeType = NG_NODE_VCB;
    Vcb->VolumeDevice = Vdo;
    Vcb->StorageDevice = Target;
    Vcb->Vpb = Vpb;
    Vcb->SectorSize = SectorSize;
    ExInitializeResourceLite(&Vcb->CoreLock);
    ExInitializeResourceLite(&Vcb->CreateGate);
    ExInitializeFastMutex(&Vcb->FcbListLock);
    InitializeListHead(&Vcb->FcbList);
    InitializeListHead(&Vcb->DirNotifyList);
    FsRtlNotifyInitializeSync(&Vcb->NotifySync);
    FsRtlInitializeTunnelCache(&Vcb->Tunnel);
    Vdo->StackSize = Target->StackSize + 1;
    Vdo->AlignmentRequirement = Target->AlignmentRequirement;
    Vdo->SectorSize = (USHORT)SectorSize;

    NgAcquireCore(Vcb);
    Err = ngc_mount(Target, Size, SectorSize, !NgGlobal.ForceReadOnly, &Vcb->Core, &WhyRo);
    if (!Err)
        ngc_volinfo(Vcb->Core, &Vcb->Info);
    NgReleaseCore(Vcb);
    if (Err)
    {
        DPRINT1("ntfsng: fs/ntfs refused the volume (%d), leaving it to other file systems\n", Err);
        ExDeleteResourceLite(&Vcb->CoreLock);
        ExDeleteResourceLite(&Vcb->CreateGate);
        IoDeleteDevice(Vdo);
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    Vpb->DeviceObject = Vdo;
    Vpb->SerialNumber = (ULONG)Vcb->Info.serial;
    Vpb->VolumeLabelLength = (USHORT)(min(Vcb->Info.label_len, MAXIMUM_VOLUME_LABEL_LENGTH / sizeof(WCHAR)) * sizeof(WCHAR));
    for (i = 0; i < Vpb->VolumeLabelLength / sizeof(WCHAR); i++)
        Vpb->VolumeLabel[i] = Vcb->Info.label[i];
    Vcb->ReadOnly = Vcb->Info.read_only ? TRUE : FALSE;
    Vcb->Damaged = Vcb->Info.damaged ? TRUE : FALSE;
    if (!Vcb->ReadOnly && !NT_SUCCESS(NgStartFlusher(Vcb)))
    {
        DPRINT1("ntfsng: no flusher thread, mounting read-only\n");
        Vcb->ReadOnly = TRUE;
    }
    ExAcquireFastMutex(&NgGlobal.VcbListLock);
    InsertTailList(&NgGlobal.VcbList, &Vcb->GlobalLinks);
    ExReleaseFastMutex(&NgGlobal.VcbListLock);
    Vdo->Flags &= ~DO_DEVICE_INITIALIZING;

    DPRINT1("ntfsng: mounted NTFS %u.%u size %I64u, sector %lu, cluster %u, %I64u clusters (%I64u free), serial %08lx, %s%s%s\n",
            Vcb->Info.major, Vcb->Info.minor, Size, SectorSize, Vcb->Info.cluster_size,
            Vcb->Info.total_clusters, Vcb->Info.free_clusters, Vpb->SerialNumber,
            Vcb->ReadOnly ? "READ-ONLY" : "read-write", WhyRo ? ": " : "", WhyRo ? WhyRo : "");
    if (Vcb->Damaged)
        NgReportDamage(Vcb, WhyRo);
    return STATUS_SUCCESS;
}

/*
 * FSCTL_LOCK_VOLUME through a volume handle: after a flush, granted only while that handle is the
 * only open one on the volume; then every other open is refused until FSCTL_UNLOCK_VOLUME or the
 * holder's cleanup.  Cached files without handles do not count.
 */
static NTSTATUS NgLockVolume(PNG_VCB Vcb, PFILE_OBJECT FileObject)
{
    PNG_FCB Fcb = FileObject ? FileObject->FsContext : NULL;
    PLIST_ENTRY Entry;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Fcb || !Fcb->IsVolume)
        return STATUS_INVALID_PARAMETER;
    /* No create runs while the lock is decided, the volume flushed for it, or a dismount runs:
     * a running create finishes first (see NgCreate). */
    ExAcquireResourceExclusiveLite(&Vcb->CreateGate, TRUE);
    if (Vcb->Dismounted || Vcb->LockedBy || !FileObject->FsContext2)
    {
        Status = Vcb->Dismounted ? STATUS_VOLUME_DISMOUNTED : STATUS_ACCESS_DENIED;
        ExReleaseResourceLite(&Vcb->CreateGate);
        return Status;
    }
    NgFlushVolume(Vcb);
    ExAcquireFastMutex(&Vcb->FcbListLock);
    /* Checked again after the flush, in the hold that publishes the lock: also refused for a handle
     * whose cleanup ran meanwhile (nothing would ever unlock it). */
    if (Vcb->Dismounted || Vcb->LockedBy || ((PNG_CCB)FileObject->FsContext2)->CleanedUp)
        Status = STATUS_ACCESS_DENIED;
    for (Entry = Vcb->FcbList.Flink; NT_SUCCESS(Status) && Entry != &Vcb->FcbList; Entry = Entry->Flink)
    {
        PNG_FCB F = CONTAINING_RECORD(Entry, NG_FCB, VcbLinks);
        if (F != Fcb && F->OpenHandles)
        {
            Status = STATUS_ACCESS_DENIED;
            break;
        }
    }
    if (NT_SUCCESS(Status) && Fcb->OpenHandles != 1)
        Status = STATUS_ACCESS_DENIED;
    if (NT_SUCCESS(Status))
    {
        /* Owner and VPB flag in one hold: an unlock or cleanup in between cannot leave either behind. */
        KIRQL Irql;
        Vcb->LockedBy = FileObject;
        IoAcquireVpbSpinLock(&Irql);
        Vcb->Vpb->Flags |= VPB_LOCKED;
        IoReleaseVpbSpinLock(Irql);
    }
    ExReleaseFastMutex(&Vcb->FcbListLock);
    ExReleaseResourceLite(&Vcb->CreateGate);
    return Status;
}

/*
 * Unlocks the volume if FileObject holds the lock, checked under FcbListLock, the lock the volume lock
 * is published under (a later holder's lock is not dropped); with Cleanup the handle is also marked
 * cleaned up, so a lock request still running on it is refused.  Not under the create gate: a create
 * holding it only reads the lock at its start, and a dismount, which takes the gate exclusive, waits
 * for running creates.  FALSE if FileObject did not hold the lock.
 */
BOOLEAN NgUnlockVolume(PNG_VCB Vcb, PFILE_OBJECT FileObject, BOOLEAN Cleanup)
{
    PNG_CCB Ccb = FileObject->FsContext2;
    BOOLEAN Held;
    ExAcquireFastMutex(&Vcb->FcbListLock);
    if (Cleanup && Ccb)
        Ccb->CleanedUp = TRUE;
    Held = Vcb->LockedBy == FileObject;
    if (Held)
    {
        KIRQL Irql;
        Vcb->LockedBy = NULL;
        IoAcquireVpbSpinLock(&Irql);
        Vcb->Vpb->Flags &= ~VPB_LOCKED;
        IoReleaseVpbSpinLock(Irql);
    }
    ExReleaseFastMutex(&Vcb->FcbListLock);
    return Held;
}

/* The I/O manager frees a VPB that lost its last reference with this tag (TAG_VPB). */
#define NG_TAG_VPB ' BPV'

/*
 * FSCTL_DISMOUNT_VOLUME through the handle that holds the volume lock (a busy volume is not
 * dismounted by force).  The volume is flushed unless the lock holder already wrote the disk
 * directly, the core is unmounted, and the storage device gets a new VPB, so the next open mounts
 * the volume again; the file objects of this mount, the dismounting handle among them, keep the
 * old one.  That handle still reads and writes the disk directly; every other request on this
 * mount fails with STATUS_VOLUME_DISMOUNTED.  Not for a volume with a paging file.
 */
static NTSTATUS NgDismountVolumeGated(PNG_VCB Vcb, PFILE_OBJECT FileObject)
{
    PNG_FCB Fcb = FileObject ? FileObject->FsContext : NULL;
    PNG_CCB Ccb = FileObject ? FileObject->FsContext2 : NULL;
    PNG_FCB *List = NULL;
    ULONG Count = 0, Capacity, i;
    PLIST_ENTRY Entry;
    BOOLEAN Discard;
    PVPB NewVpb;
    KIRQL Irql;

    if (!Fcb || !Fcb->IsVolume || !Ccb)
        return STATUS_INVALID_PARAMETER;
    if (!Ccb->ManageVolume)
        return STATUS_INVALID_PARAMETER;
    if (Vcb->LockedBy != FileObject)
        return STATUS_ACCESS_DENIED;
    NewVpb = ExAllocatePoolWithTag(NonPagedPool, sizeof(VPB), NG_TAG_VPB);
    if (!NewVpb)
        return STATUS_INSUFFICIENT_RESOURCES;
    ExAcquireFastMutex(&Vcb->FcbListLock);
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink)
    {
        PNG_FCB F = CONTAINING_RECORD(Entry, NG_FCB, VcbLinks);
        if (F->IsPagingFile)
        {
            ExReleaseFastMutex(&Vcb->FcbListLock);
            ExFreePoolWithTag(NewVpb, NG_TAG_VPB);
            return STATUS_ACCESS_DENIED;
        }
        Count++;
    }
    ExReleaseFastMutex(&Vcb->FcbListLock);

    Discard = Vcb->RawWritten;
    if (!Discard && !Vcb->ReadOnly)
        NgFlushVolume(Vcb);

    /* Cached files lose their views; later paging I/O on them fails.  Every FCB is taken (the array is
     * sized again if the list grew, which the create gate now prevents). */
    for (Capacity = Count + 16;; Capacity = Count + 16)
    {
        List = ExAllocatePoolWithTag(PagedPool, Capacity * sizeof(PNG_FCB), TAG_NTFSNG);
        if (!List)
        {
            /* Without the snapshot the teardown would leave core inodes behind: the volume stays mounted. */
            ExFreePoolWithTag(NewVpb, NG_TAG_VPB);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Count = 0;
        ExAcquireFastMutex(&Vcb->FcbListLock);
        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink)
            Count++;
        if (Count <= Capacity)
            break;
        ExReleaseFastMutex(&Vcb->FcbListLock);
        ExFreePoolWithTag(List, TAG_NTFSNG);
        List = NULL;
    }
    Count = 0;
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink)
    {
        PNG_FCB F = CONTAINING_RECORD(Entry, NG_FCB, VcbLinks);
        F->Header.IsFastIoPossible = FastIoIsNotPossible;
        InterlockedIncrement(&F->RefCount);
        List[Count++] = F;
    }
    ExReleaseFastMutex(&Vcb->FcbListLock);
    for (i = 0; i < Count; i++)
    {
        if (List[i]->SectionObjectPointers.DataSectionObject || List[i]->SectionObjectPointers.SharedCacheMap)
            CcPurgeCacheSection(&List[i]->SectionObjectPointers, NULL, 0, FALSE);
        if (List[i]->SectionObjectPointers.ImageSectionObject)
            MmFlushImageSection(&List[i]->SectionObjectPointers, MmFlushForDelete);
    }
    Vcb->Dismounted = TRUE;

    ExAcquireFastMutex(&NgGlobal.VcbListLock);
    RemoveEntryList(&Vcb->GlobalLinks);
    ExReleaseFastMutex(&NgGlobal.VcbListLock);
    if (Vcb->Flusher)
    {
        KeSetEvent(&Vcb->FlusherStop, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(Vcb->Flusher, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Vcb->Flusher);
        Vcb->Flusher = NULL;
    }

    NgAcquireCore(Vcb);
    for (i = 0; i < Count; i++)
    {
        List[i]->Deleted = TRUE;
        if (List[i]->Node)
        {
            ngc_put(List[i]->Node);
            List[i]->Node = NULL;
        }
    }
    if (!Discard && !Vcb->ReadOnly)
        ngc_sync(Vcb->Core);
    ngc_umount(Vcb->Core, Discard);
    Vcb->Core = NULL;
    NgReleaseCore(Vcb);
    for (i = 0; i < Count; i++)
        NgDereferenceFcb(List[i]);
    if (List)
        ExFreePoolWithTag(List, TAG_NTFSNG);

    RtlZeroMemory(NewVpb, sizeof(VPB));
    NewVpb->Type = IO_TYPE_VPB;
    NewVpb->Size = sizeof(VPB);
    IoAcquireVpbSpinLock(&Irql);
    NewVpb->RealDevice = Vcb->Vpb->RealDevice;
    NewVpb->Flags = Vcb->Vpb->Flags & VPB_REMOVE_PENDING;
    if (Vcb->Vpb->RealDevice->Vpb == Vcb->Vpb)
    {
        Vcb->Vpb->RealDevice->Vpb = NewVpb;
        NewVpb = NULL;
    }
    IoReleaseVpbSpinLock(Irql);
    if (NewVpb)
        ExFreePoolWithTag(NewVpb, NG_TAG_VPB);
    DPRINT1("ntfsng: volume %08lx dismounted%s\n", Vcb->Vpb->SerialNumber,
            Discard ? " (written directly by the lock holder: mounted state dropped)" : "");
    return STATUS_SUCCESS;
}

/*
 * The dismount runs with the create gate held exclusive: no create, cleanup or shutdown flush is
 * inside the core it frees.  The dismount notification goes out before the gate is taken
 * (its callbacks may open files on the volume from another thread).
 */
static NTSTATUS NgDismountVolume(PNG_VCB Vcb, PFILE_OBJECT FileObject)
{
    PNG_FCB Fcb = FileObject ? FileObject->FsContext : NULL;
    PNG_CCB Ccb = FileObject ? FileObject->FsContext2 : NULL;
    NTSTATUS Status;
    if (!Fcb || !Fcb->IsVolume || !Ccb || !Ccb->ManageVolume)
        return STATUS_INVALID_PARAMETER;
    if (Vcb->LockedBy != FileObject)
        return STATUS_ACCESS_DENIED;
    FsRtlNotifyVolumeEvent(FileObject, FSRTL_VOLUME_DISMOUNT);
    ExAcquireResourceExclusiveLite(&Vcb->CreateGate, TRUE);
    Status = Vcb->Dismounted ? STATUS_VOLUME_DISMOUNTED : NgDismountVolumeGated(Vcb, FileObject);
    ExReleaseResourceLite(&Vcb->CreateGate);
    if (!NT_SUCCESS(Status) && Status != STATUS_VOLUME_DISMOUNTED)
        FsRtlNotifyVolumeEvent(FileObject, FSRTL_VOLUME_DISMOUNT_FAILED);
    return Status;
}

/* FSCTL_GET/SET/DELETE_REPARSE_POINT on a file or directory (not a stream, not the volume). */
static NTSTATUS NgReparseFsctl(PNG_VCB Vcb, PNG_FCB Fcb, PNG_CCB Ccb, PIRP Irp, PIO_STACK_LOCATION Stack, ULONG Code)
{
    PREPARSE_DATA_BUFFER Rp = Irp->AssociatedIrp.SystemBuffer;
    ULONG In = Stack->Parameters.FileSystemControl.InputBufferLength;
    ULONG Out = Stack->Parameters.FileSystemControl.OutputBufferLength;
    ULONG Header;
    NTSTATUS Status = STATUS_SUCCESS;
    void *Data = NULL;
    unsigned int Len = 0;
    int Err;

    if (!Fcb || Fcb->IsVolume || Fcb->Stream.Length || !Fcb->HasNode)
        return STATUS_INVALID_PARAMETER;
    if (Code != FSCTL_GET_REPARSE_POINT && Ccb && !(Ccb->Granted & (FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES)))
        return STATUS_ACCESS_DENIED;
    /* The root and the $Extend metadata files never become reparse points. */
    if (Code == FSCTL_SET_REPARSE_POINT && (Fcb->IsRoot || !Ccb || Ccb->ParentMftNo == 11))
        return STATUS_ACCESS_DENIED;
    if (Code != FSCTL_GET_REPARSE_POINT && Vcb->ReadOnly)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (Code != FSCTL_GET_REPARSE_POINT)
    {
        /* The buffer is a REPARSE_DATA_BUFFER, or the GUID form for third-party tags. */
        if (!Rp || In < REPARSE_DATA_BUFFER_HEADER_SIZE || In > MAXIMUM_REPARSE_DATA_BUFFER_SIZE)
            return STATUS_IO_REPARSE_DATA_INVALID;
        if (Rp->ReparseTag == IO_REPARSE_TAG_RESERVED_ZERO || Rp->ReparseTag == IO_REPARSE_TAG_RESERVED_ONE)
            return STATUS_IO_REPARSE_TAG_INVALID;
        Header = IsReparseTagMicrosoft(Rp->ReparseTag) ? REPARSE_DATA_BUFFER_HEADER_SIZE : REPARSE_GUID_DATA_BUFFER_HEADER_SIZE;
        if (In < Header || Rp->ReparseDataLength + Header != In)
            return STATUS_IO_REPARSE_DATA_INVALID;
        if (Code == FSCTL_DELETE_REPARSE_POINT && Rp->ReparseDataLength)
            return STATUS_IO_REPARSE_DATA_INVALID;
        /* Tags the core would turn into special files, or read data through (WOF), are refused. */
        if (Code == FSCTL_SET_REPARSE_POINT &&
            (Rp->ReparseTag == 0x80000017 /* WOF */ || Rp->ReparseTag == 0x80000023 /* AF_UNIX */ ||
             Rp->ReparseTag == 0x80000024 || Rp->ReparseTag == 0x80000025 || Rp->ReparseTag == 0x80000026 ||
             Rp->ReparseTag == 0xA000001D /* LX_* */))
            return STATUS_IO_REPARSE_TAG_INVALID;
        if (Code == FSCTL_SET_REPARSE_POINT && Rp->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT)
        {
            ULONG Path = In - FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer);
            if (In < FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer) ||
                (ULONG)Rp->MountPointReparseBuffer.SubstituteNameOffset + Rp->MountPointReparseBuffer.SubstituteNameLength > Path ||
                (ULONG)Rp->MountPointReparseBuffer.PrintNameOffset + Rp->MountPointReparseBuffer.PrintNameLength > Path ||
                ((Rp->MountPointReparseBuffer.SubstituteNameOffset | Rp->MountPointReparseBuffer.SubstituteNameLength |
                  Rp->MountPointReparseBuffer.PrintNameOffset | Rp->MountPointReparseBuffer.PrintNameLength) & 1) ||
                !Rp->MountPointReparseBuffer.SubstituteNameLength)
                return STATUS_IO_REPARSE_DATA_INVALID;
            if (!Fcb->IsDirectory)
                return STATUS_NOT_A_DIRECTORY;
        }
    }

    ExAcquireResourceExclusiveLite(Fcb->Header.Resource, TRUE);
    NgAcquireCore(Vcb);
    Err = NgEnsureNode(Fcb);
    if (!Err && Code == FSCTL_GET_REPARSE_POINT)
    {
        Err = ngc_get_reparse(Fcb->Node, &Data, &Len);
        if (Err == -NGC_ENODATA)
            Status = STATUS_NOT_A_REPARSE_POINT;
        Err = Status == STATUS_SUCCESS ? Err : 0;
    }
    else if (!Err && !IsReparseTagMicrosoft(Rp->ReparseTag) && !ngc_get_reparse(Fcb->Node, &Data, &Len) &&
             ((PREPARSE_GUID_DATA_BUFFER)Data)->ReparseTag == Rp->ReparseTag &&
             (Len < REPARSE_GUID_DATA_BUFFER_HEADER_SIZE ||
              !IsEqualGUID(&((PREPARSE_GUID_DATA_BUFFER)Data)->ReparseGuid, &((PREPARSE_GUID_DATA_BUFFER)Rp)->ReparseGuid)))
    {
        /* A third-party tag is also identified by its GUID. */
        Status = STATUS_REPARSE_ATTRIBUTE_CONFLICT;
    }
    else if (!Err && Code == FSCTL_SET_REPARSE_POINT)
    {
        /* A directory becomes a reparse point only while it is empty. */
        if (Fcb->IsDirectory && ngc_dir_empty(Vcb->Core, Fcb->Node) != 1)
            Status = STATUS_DIRECTORY_NOT_EMPTY;
        else
            Err = ngc_set_reparse(Vcb->Core, Fcb->Node, Rp, In);
    }
    else if (!Err)
    {
        Err = ngc_delete_reparse(Vcb->Core, Fcb->Node, Rp->ReparseTag);
        if (Err == -NGC_ENODATA)
        {
            Status = STATUS_NOT_A_REPARSE_POINT;
            Err = 0;
        }
    }
    if (Code != FSCTL_GET_REPARSE_POINT && Data)
    {
        ngc_free(Data);
        Data = NULL;
    }
    if (Err == -NGC_EXDEV)
        Status = STATUS_IO_REPARSE_TAG_MISMATCH;
    else if (Err == -NGC_EINVAL || Err == -NGC_EFBIG)
        Status = STATUS_IO_REPARSE_DATA_INVALID;
    else if (Err)
        Status = NgErrnoToStatus(Err);
    if (Code != FSCTL_GET_REPARSE_POINT)
    {
        /* Also after a failure: a failed replace may have removed the old reparse point. */
        ngc_stat(Fcb->Node, &Fcb->Stat);
        NgAfterChange(Vcb);
    }
    NgReleaseCore(Vcb);
    ExReleaseResourceLite(Fcb->Header.Resource);

    if (Code == FSCTL_GET_REPARSE_POINT && NT_SUCCESS(Status))
    {
        PVOID Buf = Irp->AssociatedIrp.SystemBuffer;
        if (!Buf || Out < REPARSE_DATA_BUFFER_HEADER_SIZE)
            Status = STATUS_BUFFER_TOO_SMALL;
        else if (Out < Len)
        {
            RtlCopyMemory(Buf, Data, Out);
            Irp->IoStatus.Information = Out;
            Status = STATUS_BUFFER_OVERFLOW;
        }
        else
        {
            RtlCopyMemory(Buf, Data, Len);
            Irp->IoStatus.Information = Len;
        }
    }
    if (Data)
        ngc_free(Data);
    if (Code != FSCTL_GET_REPARSE_POINT && NT_SUCCESS(Status) && Ccb)
        NgNotify(Vcb, &Ccb->Path, FILE_NOTIFY_CHANGE_ATTRIBUTES, FILE_ACTION_MODIFIED);
    return Status;
}

static NTSTATUS NgUserFsRequest(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    PFILE_OBJECT FileObject = Stack->FileObject;
    PNG_FCB Fcb = FileObject ? FileObject->FsContext : NULL;
    switch (Stack->Parameters.FileSystemControl.FsControlCode)
    {
        case FSCTL_IS_VOLUME_MOUNTED:
            return STATUS_SUCCESS;
        case FSCTL_SET_COMPRESSION:
        {
            /* Turning compression off on an uncompressed stream is a no-op (the registry asks). */
            PUSHORT Format = Irp->AssociatedIrp.SystemBuffer;
            if (Vcb->ReadOnly)
                return STATUS_MEDIA_WRITE_PROTECTED;
            if (!Format || Stack->Parameters.FileSystemControl.InputBufferLength < sizeof(USHORT))
                return STATUS_INVALID_PARAMETER;
            if (*Format == COMPRESSION_FORMAT_NONE && Fcb && !(Fcb->Stat.flags & NGC_ATTR_COMPRESSED))
                return STATUS_SUCCESS;
            return STATUS_NOT_SUPPORTED;
        }
        case FSCTL_GET_COMPRESSION:
        {
            PUSHORT Format = Irp->AssociatedIrp.SystemBuffer;
            if (!Format || Stack->Parameters.FileSystemControl.OutputBufferLength < sizeof(USHORT))
                return STATUS_BUFFER_TOO_SMALL;
            *Format = (Fcb && (Fcb->Stat.flags & NGC_ATTR_COMPRESSED)) ? COMPRESSION_FORMAT_LZNT1 : COMPRESSION_FORMAT_NONE;
            Irp->IoStatus.Information = sizeof(USHORT);
            return STATUS_SUCCESS;
        }
        case FSCTL_MARK_AS_SYSTEM_HIVE:
            /* Advisory: the driver never dismounts a volume with open hives anyway. */
            return STATUS_SUCCESS;
        case FSCTL_IS_VOLUME_DIRTY:
        {
            PULONG Out = Irp->AssociatedIrp.SystemBuffer;
            if (!Out || Stack->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG))
                return STATUS_INVALID_PARAMETER;
            NgAcquireCore(Vcb);
            ngc_volinfo(Vcb->Core, &Vcb->Info);
            NgReleaseCore(Vcb);
            *Out = Vcb->Info.dirty ? VOLUME_IS_DIRTY : 0;
            Irp->IoStatus.Information = sizeof(ULONG);
            return STATUS_SUCCESS;
        }
        case FSCTL_LOCK_VOLUME:
            if (Fcb && Fcb->IsVolume && !((PNG_CCB)FileObject->FsContext2)->ManageVolume)
                return STATUS_INVALID_PARAMETER;
            return NgLockVolume(Vcb, FileObject);
        case FSCTL_UNLOCK_VOLUME:
            if (Fcb && Fcb->IsVolume && !((PNG_CCB)FileObject->FsContext2)->ManageVolume)
                return STATUS_INVALID_PARAMETER;
            if (!Fcb || !Fcb->IsVolume || !NgUnlockVolume(Vcb, FileObject, FALSE))
                return STATUS_NOT_LOCKED;
            return STATUS_SUCCESS;
        case FSCTL_REQUEST_OPLOCK_LEVEL_1:
        case FSCTL_REQUEST_OPLOCK_LEVEL_2:
        case FSCTL_REQUEST_BATCH_OPLOCK:
        case FSCTL_REQUEST_FILTER_OPLOCK:
            /* Oplocks are never granted; callers fall back to uncached sharing. */
            return STATUS_OPLOCK_NOT_GRANTED;
        case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
        case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
        case FSCTL_OPLOCK_BREAK_NOTIFY:
        case FSCTL_OPLOCK_BREAK_ACK_NO_2:
            return STATUS_INVALID_OPLOCK_PROTOCOL;
        case FSCTL_NG_LOCK_STATS:
        {
            /* Diagnostic: a snapshot of the CoreLock statistics (benchmark tools diff two). */
            ULONG Out = Stack->Parameters.FileSystemControl.OutputBufferLength;
            if (!Irp->AssociatedIrp.SystemBuffer || Out < sizeof(NG_LOCK_STATS))
                return STATUS_BUFFER_TOO_SMALL;
            NgAcquireCore(Vcb);
            Vcb->LockStats.Version = 1;
            Vcb->LockStats.Categories = NG_LOCK_CATEGORIES;
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &Vcb->LockStats, sizeof(NG_LOCK_STATS));
            NgReleaseCore(Vcb);
            Irp->IoStatus.Information = sizeof(NG_LOCK_STATS);
            return STATUS_SUCCESS;
        }
        case FSCTL_NG_PROFILE:
        {
            /* Diagnostic: a snapshot of the time per stage (benchmark tools diff two). */
            ULONG Out = Stack->Parameters.FileSystemControl.OutputBufferLength;
            if (!Irp->AssociatedIrp.SystemBuffer || Out < sizeof(NG_PROFILE))
                return STATUS_BUFFER_TOO_SMALL;
            NgProfile.Version = 1;
            NgProfile.Stages = NG_PROFILE_STAGES;
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &NgProfile, sizeof(NG_PROFILE));
            Irp->IoStatus.Information = sizeof(NG_PROFILE);
            return STATUS_SUCCESS;
        }
        case FSCTL_GET_REPARSE_POINT:
        case FSCTL_SET_REPARSE_POINT:
        case FSCTL_DELETE_REPARSE_POINT:
            return NgReparseFsctl(Vcb, Fcb, FileObject ? FileObject->FsContext2 : NULL, Irp, Stack,
                                  Stack->Parameters.FileSystemControl.FsControlCode);
        case FSCTL_DISMOUNT_VOLUME:
            return NgDismountVolume(Vcb, FileObject);
        default:
            return STATUS_INVALID_DEVICE_REQUEST;
    }
}

NTSTATUS NgFileSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    switch (Stack->MinorFunction)
    {
        case IRP_MN_MOUNT_VOLUME:
            return NgMountVolume(DeviceObject, Irp);
        case IRP_MN_VERIFY_VOLUME:
            /* Fixed media only: the volume never changes underneath us. */
            Stack->Parameters.VerifyVolume.Vpb->RealDevice->Flags &= ~DO_VERIFY_VOLUME;
            return STATUS_SUCCESS;
        case IRP_MN_USER_FS_REQUEST:
        case IRP_MN_KERNEL_CALL:
            if (DeviceObject == NgGlobal.ControlDevice)
                return STATUS_INVALID_DEVICE_REQUEST;
            return NgUserFsRequest(DeviceObject, Irp);
        default:
            return STATUS_INVALID_DEVICE_REQUEST;
    }
}
