/*
 * PROJECT:     ReactOS VFAT filesystem library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Growing a FAT volume offline (the FATs too), on a locked and dismounted volume
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "vfatlib.h"
#include "fatgrow.h"
#include <ntddstor.h>

#define NDEBUG
#include <debug.h>

#ifndef FSCTL_ALLOW_EXTENDED_DASD_IO
#define FSCTL_ALLOW_EXTENDED_DASD_IO CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 32, METHOD_NEITHER, FILE_ANY_ACCESS)
#endif

typedef struct _EXTEND_IO
{
    HANDLE Handle;
    ULONG SectorSize;
    PUCHAR Bounce;      /* one sector, for requests that do not cover whole sectors */
} EXTEND_IO, *PEXTEND_IO;

static
int
ExtendTransfer(
    PEXTEND_IO Io,
    BOOLEAN Write,
    ULONGLONG Offset,
    PUCHAR Buffer,
    ULONG Length)
{
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER Position;
    NTSTATUS Status;

    while (Length)
    {
        ULONG Head = (ULONG)(Offset % Io->SectorSize);
        ULONG Chunk;

        if (Head || Length < Io->SectorSize)
        {
            /* A partial sector goes through the bounce buffer (read, change, write). */
            Chunk = min(Length, Io->SectorSize - Head);
            Position.QuadPart = (LONGLONG)(Offset - Head);
            Status = NtReadFile(Io->Handle, NULL, NULL, NULL, &Iosb, Io->Bounce, Io->SectorSize, &Position, NULL);
            if (!NT_SUCCESS(Status))
                return 1;
            if (Write)
            {
                RtlCopyMemory(Io->Bounce + Head, Buffer, Chunk);
                Status = NtWriteFile(Io->Handle, NULL, NULL, NULL, &Iosb, Io->Bounce, Io->SectorSize, &Position, NULL);
                if (!NT_SUCCESS(Status))
                    return 1;
            }
            else
            {
                RtlCopyMemory(Buffer, Io->Bounce + Head, Chunk);
            }
        }
        else
        {
            Chunk = Length - Length % Io->SectorSize;
            Position.QuadPart = (LONGLONG)Offset;
            if (Write)
                Status = NtWriteFile(Io->Handle, NULL, NULL, NULL, &Iosb, Buffer, Chunk, &Position, NULL);
            else
                Status = NtReadFile(Io->Handle, NULL, NULL, NULL, &Iosb, Buffer, Chunk, &Position, NULL);
            if (!NT_SUCCESS(Status) || Iosb.Information != Chunk)
                return 1;
        }
        Offset += Chunk;
        Buffer += Chunk;
        Length -= Chunk;
    }
    return 0;
}

static int ExtendRead(void *Ctx, unsigned long long Offset, void *Buffer, unsigned int Length)
{
    return ExtendTransfer(Ctx, FALSE, Offset, Buffer, Length);
}

static int ExtendWrite(void *Ctx, unsigned long long Offset, const void *Buffer, unsigned int Length)
{
    return ExtendTransfer(Ctx, TRUE, Offset, (PUCHAR)Buffer, Length);
}

static void *ExtendAlloc(void *Ctx, unsigned long long Size)
{
    UNREFERENCED_PARAMETER(Ctx);
    if (Size > MAXULONG_PTR)
        return NULL;
    return RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)Size);
}

static void ExtendFree(void *Ctx, void *Pointer)
{
    UNREFERENCED_PARAMETER(Ctx);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Pointer);
}

/*
 * Flushes the write cache of the disk under the volume Volume (dismounted by
 * now, so the file system no longer passes a flush on): what fatgrow wrote is
 * on the medium before the volume is given back.  A disk without a write
 * cache to flush (STATUS_INVALID_DEVICE_REQUEST) counts as flushed.
 */
static
NTSTATUS
FlushDisk(
    _In_ HANDLE Volume)
{
    STORAGE_DEVICE_NUMBER Number;
    WCHAR Buffer[64];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK Iosb;
    HANDLE Disk;
    NTSTATUS Status;

    Status = NtDeviceIoControlFile(Volume, NULL, NULL, NULL, &Iosb, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                   NULL, 0, &Number, sizeof(Number));
    if (!NT_SUCCESS(Status))
        return Status;
    _snwprintf(Buffer, ARRAYSIZE(Buffer) - 1, L"\\Device\\Harddisk%lu\\Partition0", Number.DeviceNumber);
    Buffer[ARRAYSIZE(Buffer) - 1] = UNICODE_NULL;
    RtlInitUnicodeString(&Name, Buffer);
    InitializeObjectAttributes(&ObjectAttributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Disk, FILE_GENERIC_WRITE | SYNCHRONIZE, &ObjectAttributes, &Iosb,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtFlushBuffersFile(Disk, &Iosb);
    NtClose(Disk);
    return (Status == STATUS_INVALID_DEVICE_REQUEST) ? STATUS_SUCCESS : Status;
}

/*
 * Grows the FAT volume DriveRoot (an NT device path) to NewSectors sectors,
 * growing its FATs when they cannot describe the new clusters (see fatgrow.h).
 * The partition must already hold NewSectors sectors.  The volume is locked
 * for the whole operation (STATUS_ACCESS_DENIED while files are open on it)
 * and dismounted, so the next access mounts the grown volume.  NOT CRASH-SAFE:
 * a power cut or a write error after the directories are renumbered and
 * before the boot sector is written leaves a volume in neither numbering.
 */
NTSTATUS
NTAPI
VfatExtend(
    _In_ PUNICODE_STRING DriveRoot,
    _In_ ULONGLONG NewSectors)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK Iosb;
    DISK_GEOMETRY Geometry;
    fatgrow_io FgIo;
    fatgrow_result Result;
    EXTEND_IO Io;
    NTSTATUS Status;
    int Err;

    RtlZeroMemory(&Io, sizeof(Io));
    InitializeObjectAttributes(&ObjectAttributes, DriveRoot, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Io.Handle,
                        FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE,
                        &ObjectAttributes,
                        &Iosb,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtDeviceIoControlFile(Io.Handle, NULL, NULL, NULL, &Iosb, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                   NULL, 0, &Geometry, sizeof(Geometry));
    Io.SectorSize = (NT_SUCCESS(Status) && Geometry.BytesPerSector >= 512) ? Geometry.BytesPerSector : 512;
    Io.Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, Io.SectorSize);
    if (Io.Bounce == NULL)
    {
        NtClose(Io.Handle);
        return STATUS_NO_MEMORY;
    }

    /* Nothing else may use the volume; then the file system lets go of it. */
    Status = NtFsControlFile(Io.Handle, NULL, NULL, NULL, &Iosb, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0);
    if (!NT_SUCCESS(Status))
        goto Close;
    NtFsControlFile(Io.Handle, NULL, NULL, NULL, &Iosb, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0);
    Status = NtFsControlFile(Io.Handle, NULL, NULL, NULL, &Iosb, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0);
    if (!NT_SUCCESS(Status))
        goto Unlock;

    FgIo.ctx = &Io;
    FgIo.read = ExtendRead;
    FgIo.write = ExtendWrite;
    FgIo.alloc = ExtendAlloc;
    FgIo.free = ExtendFree;
    Err = fatgrow(&FgIo, NewSectors, &Result);
    DPRINT1("VfatExtend(%wZ, %I64u): %d, FAT %u -> %u sectors, clusters %u -> %u, %u moved\n", DriveRoot, NewSectors,
            Err, Result.old_fat_sectors, Result.new_fat_sectors, Result.old_clusters, Result.new_clusters, Result.moved_clusters);
    switch (Err)
    {
        case FG_OK: Status = STATUS_SUCCESS; break;
        case FG_ENOMEM: Status = STATUS_NO_MEMORY; break;
        case FG_EINVAL: Status = STATUS_INVALID_PARAMETER; break;
        case FG_EBADFS: Status = STATUS_FILE_CORRUPT_ERROR; break;
        case FG_ENOSPC: Status = STATUS_DISK_FULL; break;
        case FG_ETYPE: Status = STATUS_NOT_SUPPORTED; break;
        default: Status = STATUS_IO_DEVICE_ERROR; break;
    }
    if (Err == FG_OK)
    {
        Status = FlushDisk(Io.Handle);
        if (!NT_SUCCESS(Status))
            DPRINT1("VfatExtend: the disk flush failed: 0x%08lx\n", Status);
    }

Unlock:
    NtFsControlFile(Io.Handle, NULL, NULL, NULL, &Iosb, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0);
Close:
    RtlFreeHeap(RtlGetProcessHeap(), 0, Io.Bounce);
    NtClose(Io.Handle);
    return Status;
}
