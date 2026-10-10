/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite registry flush failure test driver:
 *              a RAM disk whose flushes can be made to fail
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>
#include <ntdddisk.h>
#include <ntddstor.h>

#include "CmFlush.h"

#define NDEBUG
#include <debug.h>

/* One FAT16 volume that starts at sector 0 */
#define DISK_SECTORS        16384
#define SECTOR_SIZE         512
#define DISK_SIZE           (DISK_SECTORS * SECTOR_SIZE)
#define SECTORS_PER_CLUSTER 2
#define RESERVED_SECTORS    1
#define FAT_COUNT           2
#define FAT_SECTORS         32
#define ROOT_ENTRIES        512

static KMT_IRP_HANDLER DiskReadWrite;
static KMT_IRP_HANDLER DiskFlush;
static KMT_IRP_HANDLER DiskDeviceControl;
static KMT_MESSAGE_HANDLER TestMessageHandler;

static PDEVICE_OBJECT DiskDevice;
static PUCHAR Image;
static FAST_MUTEX DiskLock;
static ULONG FlushCount;
static NTSTATUS FlushStatus;

static
VOID
PutUshort(
    _Out_writes_bytes_(2) PUCHAR Buffer,
    _In_ USHORT Value)
{
    Buffer[0] = (UCHAR)Value;
    Buffer[1] = (UCHAR)(Value >> 8);
}

static
VOID
FormatImage(VOID)
{
    PUCHAR Boot = Image;
    PUCHAR Fat;
    ULONG i;

    RtlZeroMemory(Image, DISK_SIZE);

    Boot[0] = 0xEB;
    Boot[1] = 0x3C;
    Boot[2] = 0x90;
    RtlCopyMemory(Boot + 3, "MSWIN4.1", 8);
    PutUshort(Boot + 11, SECTOR_SIZE);
    Boot[13] = SECTORS_PER_CLUSTER;
    PutUshort(Boot + 14, RESERVED_SECTORS);
    Boot[16] = FAT_COUNT;
    PutUshort(Boot + 17, ROOT_ENTRIES);
    PutUshort(Boot + 19, DISK_SECTORS);
    Boot[21] = 0xF8;
    PutUshort(Boot + 22, FAT_SECTORS);
    PutUshort(Boot + 24, 32);
    PutUshort(Boot + 26, 2);
    Boot[36] = 0x80;
    Boot[38] = 0x29;
    Boot[39] = 0x4D;
    Boot[40] = 0x43;
    Boot[41] = 0x4D;
    Boot[42] = 0x4B;
    RtlCopyMemory(Boot + 43, "KMTCMFLUSH ", 11);
    RtlCopyMemory(Boot + 54, "FAT16   ", 8);
    Boot[510] = 0x55;
    Boot[511] = 0xAA;

    for (i = 0; i < FAT_COUNT; i++)
    {
        Fat = Image + (RESERVED_SECTORS + i * FAT_SECTORS) * SECTOR_SIZE;
        PutUshort(Fat, 0xFFF8);
        PutUshort(Fat + 2, 0xFFFF);
    }
}

static
NTSTATUS
DiskReadWrite(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStack)
{
    NTSTATUS Status = STATUS_SUCCESS;
    LONGLONG Offset;
    ULONG Length;
    PUCHAR Buffer;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (IoStack->MajorFunction == IRP_MJ_READ)
    {
        Offset = IoStack->Parameters.Read.ByteOffset.QuadPart;
        Length = IoStack->Parameters.Read.Length;
    }
    else
    {
        Offset = IoStack->Parameters.Write.ByteOffset.QuadPart;
        Length = IoStack->Parameters.Write.Length;
    }

    Irp->IoStatus.Information = 0;
    if (Offset < 0 || Offset % SECTOR_SIZE != 0 || Length % SECTOR_SIZE != 0 ||
        Offset > DISK_SIZE || Length > DISK_SIZE - Offset)
    {
        Status = STATUS_INVALID_PARAMETER;
    }
    else if (Length != 0)
    {
        Buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
        if (Buffer == NULL)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            ExAcquireFastMutex(&DiskLock);
            if (IoStack->MajorFunction == IRP_MJ_READ)
                RtlCopyMemory(Buffer, Image + Offset, Length);
            else
                RtlCopyMemory(Image + Offset, Buffer, Length);
            ExReleaseFastMutex(&DiskLock);
            Irp->IoStatus.Information = Length;
        }
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
NTSTATUS
DiskFlush(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStack)
{
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(IoStack);

    ExAcquireFastMutex(&DiskLock);
    FlushCount++;
    Status = FlushStatus;
    ExReleaseFastMutex(&DiskLock);

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
NTSTATUS
DiskDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStack)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG OutLength = IoStack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG_PTR Information = 0;

    UNREFERENCED_PARAMETER(DeviceObject);

    switch (IoStack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_DISK_GET_DRIVE_GEOMETRY:
        {
            PDISK_GEOMETRY Geometry = Buffer;

            if (OutLength < sizeof(*Geometry))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            Geometry->Cylinders.QuadPart = DISK_SECTORS / (32 * 2);
            Geometry->MediaType = FixedMedia;
            Geometry->TracksPerCylinder = 2;
            Geometry->SectorsPerTrack = 32;
            Geometry->BytesPerSector = SECTOR_SIZE;
            Information = sizeof(*Geometry);
            break;
        }

        case IOCTL_DISK_GET_PARTITION_INFO:
        {
            PPARTITION_INFORMATION Partition = Buffer;

            if (OutLength < sizeof(*Partition))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            RtlZeroMemory(Partition, sizeof(*Partition));
            Partition->PartitionLength.QuadPart = DISK_SIZE;
            Partition->PartitionNumber = 1;
            Partition->PartitionType = PARTITION_FAT_16;
            Partition->RecognizedPartition = TRUE;
            Information = sizeof(*Partition);
            break;
        }

        case IOCTL_DISK_GET_PARTITION_INFO_EX:
        {
            PPARTITION_INFORMATION_EX Partition = Buffer;

            if (OutLength < sizeof(*Partition))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            RtlZeroMemory(Partition, sizeof(*Partition));
            Partition->PartitionStyle = PARTITION_STYLE_MBR;
            Partition->PartitionLength.QuadPart = DISK_SIZE;
            Partition->PartitionNumber = 1;
            Partition->Mbr.PartitionType = PARTITION_FAT_16;
            Partition->Mbr.RecognizedPartition = TRUE;
            Information = sizeof(*Partition);
            break;
        }

        case IOCTL_DISK_GET_LENGTH_INFO:
        {
            PGET_LENGTH_INFORMATION LengthInfo = Buffer;

            if (OutLength < sizeof(*LengthInfo))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            LengthInfo->Length.QuadPart = DISK_SIZE;
            Information = sizeof(*LengthInfo);
            break;
        }

        case IOCTL_DISK_CHECK_VERIFY:
        case IOCTL_STORAGE_CHECK_VERIFY:
        case IOCTL_STORAGE_CHECK_VERIFY2:
        case IOCTL_DISK_IS_WRITABLE:
            break;

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
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
    UNREFERENCED_PARAMETER(DeviceObject);

    if (DiskDevice == NULL)
    {
        *OutLength = 0;
        return STATUS_DEVICE_NOT_READY;
    }

    switch (ControlCode)
    {
        case IOCTL_CMFLUSH_SET_FLUSH_STATUS:
            *OutLength = 0;
            if (Buffer == NULL || InLength < sizeof(ULONG))
                return STATUS_INVALID_PARAMETER;
            ExAcquireFastMutex(&DiskLock);
            FlushStatus = *(PULONG)Buffer;
            ExReleaseFastMutex(&DiskLock);
            return STATUS_SUCCESS;

        case IOCTL_CMFLUSH_GET_FLUSH_COUNT:
            if (Buffer == NULL || *OutLength < sizeof(ULONG))
            {
                *OutLength = 0;
                return STATUS_INVALID_PARAMETER;
            }
            ExAcquireFastMutex(&DiskLock);
            *(PULONG)Buffer = FlushCount;
            ExReleaseFastMutex(&DiskLock);
            *OutLength = sizeof(ULONG);
            return STATUS_SUCCESS;

        default:
            *OutLength = 0;
            return STATUS_INVALID_DEVICE_REQUEST;
    }
}

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    UNICODE_STRING DiskName = RTL_CONSTANT_STRING(CMFLUSH_DISK_NAME);
    PDEVICE_OBJECT Device;
    NTSTATUS Status;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(RegistryPath);

    *DeviceName = L"CmFlush";
    *Flags = TESTENTRY_NO_EXCLUSIVE_DEVICE;

    ExInitializeFastMutex(&DiskLock);
    FlushStatus = STATUS_SUCCESS;
    KmtRegisterMessageHandler(0, NULL, TestMessageHandler);

    Image = ExAllocatePoolWithTag(NonPagedPool, DISK_SIZE, 'FcmK');
    if (Image == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    FormatImage();

    Status = IoCreateDevice(DriverObject, 0, &DiskName, FILE_DEVICE_DISK, 0, FALSE, &Device);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Image, 'FcmK');
        Image = NULL;
        return Status;
    }

    Device->Flags |= DO_DIRECT_IO;
    Device->SectorSize = SECTOR_SIZE;

    KmtRegisterIrpHandler(IRP_MJ_READ, Device, DiskReadWrite);
    KmtRegisterIrpHandler(IRP_MJ_WRITE, Device, DiskReadWrite);
    KmtRegisterIrpHandler(IRP_MJ_FLUSH_BUFFERS, Device, DiskFlush);
    KmtRegisterIrpHandler(IRP_MJ_DEVICE_CONTROL, Device, DiskDeviceControl);

    Device->Flags &= ~DO_DEVICE_INITIALIZING;
    DiskDevice = Device;

    /*
     * No unload once the disk exists: the file system keeps the test volume
     * mounted on it, and its stream file objects can outlive a dismount.
     */
    *Flags |= TESTENTRY_NO_REGISTER_UNLOAD;
    return STATUS_SUCCESS;
}

/* Only reached when TestEntry failed: no disk was created */
VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);

    if (Image != NULL)
    {
        ExFreePoolWithTag(Image, 'FcmK');
        Image = NULL;
    }
}
