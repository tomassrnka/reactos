/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IRP_MJ_FILE_SYSTEM_CONTROL: mount, verify, volume FSCTLs
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsng.h"
#include "../shim/include/ngos.h"

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
        IoDeleteDevice(Vdo);
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    Vpb->DeviceObject = Vdo;
    Vpb->SerialNumber = (ULONG)Vcb->Info.serial;
    Vpb->VolumeLabelLength = (USHORT)(min(Vcb->Info.label_len, MAXIMUM_VOLUME_LABEL_LENGTH / sizeof(WCHAR)) * sizeof(WCHAR));
    for (i = 0; i < Vpb->VolumeLabelLength / sizeof(WCHAR); i++)
        Vpb->VolumeLabel[i] = Vcb->Info.label[i];
    Vcb->ReadOnly = Vcb->Info.read_only ? TRUE : FALSE;
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
    if (Vcb->LockedBy)
        return STATUS_ACCESS_DENIED;
    NgFlushVolume(Vcb);
    ExAcquireFastMutex(&Vcb->FcbListLock);
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink)
    {
        PNG_FCB F = CONTAINING_RECORD(Entry, NG_FCB, VcbLinks);
        if (F != Fcb && F->OpenHandles)
        {
            Status = STATUS_ACCESS_DENIED;
            break;
        }
    }
    if (NT_SUCCESS(Status) && Fcb->OpenHandles > 1)
        Status = STATUS_ACCESS_DENIED;
    if (NT_SUCCESS(Status))
        Vcb->LockedBy = FileObject;
    ExReleaseFastMutex(&Vcb->FcbListLock);
    if (NT_SUCCESS(Status))
    {
        KIRQL Irql;
        IoAcquireVpbSpinLock(&Irql);
        Vcb->Vpb->Flags |= VPB_LOCKED;
        IoReleaseVpbSpinLock(Irql);
    }
    return Status;
}

VOID NgUnlockVolume(PNG_VCB Vcb)
{
    KIRQL Irql;
    Vcb->LockedBy = NULL;
    IoAcquireVpbSpinLock(&Irql);
    Vcb->Vpb->Flags &= ~VPB_LOCKED;
    IoReleaseVpbSpinLock(Irql);
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
            return NgLockVolume(Vcb, FileObject);
        case FSCTL_UNLOCK_VOLUME:
            if (!Fcb || !Fcb->IsVolume || Vcb->LockedBy != FileObject)
                return STATUS_NOT_LOCKED;
            NgUnlockVolume(Vcb);
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
        case FSCTL_DISMOUNT_VOLUME:
            /* Dismount is not implemented: volumes stay mounted until shutdown. */
            return STATUS_ACCESS_DENIED;
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
