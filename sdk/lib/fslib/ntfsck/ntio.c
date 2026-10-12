/*
 * PROJECT:     ReactOS NTFS checker (ntfsprogs-plus port)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NT native volume I/O for the checker: works in a native process (autochk)
 *              as in a Win32 one (untfs.dll under fmifs.dll)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsck_ros.h"
#include "ntio.h"

/* Largest single read or write: well below the MDL size limit of a transfer. */
#define NTIO_MAX_TRANSFER (1024 * 1024)

struct ntio {
    HANDLE Handle;
    BOOLEAN Writable;
    BOOLEAN Locked;
    ULONG SectorSize;
    LONGLONG Size;
    PUCHAR Bounce;          /* NTIO_MAX_TRANSFER bytes, sector aligned */
};

struct ntio_policy NtioPolicy;
struct ntio_state NtioState;

/* Devices still open, so that the end of a check can close what an error path left open. */
#define NTIO_MAX_OPEN 4
static struct ntio *OpenIo[NTIO_MAX_OPEN];

static BOOLEAN TrackOpen(struct ntio *io)
{
    int i;
    for (i = 0; i < NTIO_MAX_OPEN; i++)
    {
        if (!OpenIo[i])
        {
            OpenIo[i] = io;
            return TRUE;
        }
    }
    return FALSE;
}

static void UntrackOpen(struct ntio *io)
{
    int i;
    for (i = 0; i < NTIO_MAX_OPEN; i++)
    {
        if (OpenIo[i] == io)
            OpenIo[i] = NULL;
    }
}

static int StatusToErrno(NTSTATUS Status)
{
    switch (Status)
    {
        case STATUS_ACCESS_DENIED:
        case STATUS_PRIVILEGE_NOT_HELD:
            return EACCES;
        case STATUS_SHARING_VIOLATION:
        case STATUS_DEVICE_BUSY:
            return EBUSY;
        case STATUS_OBJECT_NAME_NOT_FOUND:
        case STATUS_OBJECT_PATH_NOT_FOUND:
        case STATUS_NO_SUCH_DEVICE:
            return ENOENT;
        case STATUS_NO_MEDIA_IN_DEVICE:
        case STATUS_DEVICE_NOT_READY:
            return ENXIO;
        case STATUS_MEDIA_WRITE_PROTECTED:
            return EROFS;
        case STATUS_INSUFFICIENT_RESOURCES:
        case STATUS_NO_MEMORY:
            return ENOMEM;
        case STATUS_INVALID_PARAMETER:
            return EINVAL;
        default:
            return EIO;
    }
}

static NTSTATUS QueryGeometry(struct ntio *io)
{
    IO_STATUS_BLOCK Iosb;
    DISK_GEOMETRY Geometry;
    GET_LENGTH_INFORMATION Length;
    PARTITION_INFORMATION_EX PartInfo;
    NTSTATUS Status;

    io->SectorSize = 512;
    Status = NtDeviceIoControlFile(io->Handle, NULL, NULL, NULL, &Iosb,
                                   IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
                                   &Geometry, sizeof(Geometry));
    if (NT_SUCCESS(Status) && Geometry.BytesPerSector >= 512 &&
        Geometry.BytesPerSector <= 4096 &&
        !(Geometry.BytesPerSector & (Geometry.BytesPerSector - 1)))
    {
        io->SectorSize = Geometry.BytesPerSector;
    }

    Status = NtDeviceIoControlFile(io->Handle, NULL, NULL, NULL, &Iosb,
                                   IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                                   &Length, sizeof(Length));
    if (NT_SUCCESS(Status))
    {
        io->Size = Length.Length.QuadPart;
        return STATUS_SUCCESS;
    }
    Status = NtDeviceIoControlFile(io->Handle, NULL, NULL, NULL, &Iosb,
                                   IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0,
                                   &PartInfo, sizeof(PartInfo));
    if (NT_SUCCESS(Status))
        io->Size = PartInfo.PartitionLength.QuadPart;
    return Status;
}

/*
 * A check of a mounted volume reads the disk under the file system. Before it does, the file
 * system writes back what it holds in memory (ntfsng: commits its journal and writes the
 * transaction in place), through a separate handle: the checker's own handle is read-only.
 */
static NTSTATUS FlushVolume(POBJECT_ATTRIBUTES Attributes)
{
    IO_STATUS_BLOCK Iosb;
    HANDLE Handle;
    NTSTATUS Status;

    Status = NtOpenFile(&Handle, SYNCHRONIZE | FILE_READ_DATA | FILE_WRITE_DATA, Attributes, &Iosb,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = NtFlushBuffersFile(Handle, &Iosb);
    NtClose(Handle);
    return Status;
}

int ntio_open(const char *name, int writable, struct ntio **out)
{
    struct ntio *io;
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;
    ANSI_STRING AnsiName;
    NTSTATUS Status;

    *out = NULL;
    io = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*io));
    if (!io)
    {
        errno = ENOMEM;
        return -1;
    }
    io->Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, NTIO_MAX_TRANSFER);
    if (!io->Bounce)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, io);
        errno = ENOMEM;
        return -1;
    }
    RtlInitAnsiString(&AnsiName, name);
    Status = RtlAnsiStringToUnicodeString(&Name, &AnsiName, TRUE);
    if (!NT_SUCCESS(Status))
        goto fail;
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NtioPolicy.flush_first && !NtioState.opened)
        NtioState.flush_status = FlushVolume(&Attributes);
    Status = NtOpenFile(&io->Handle,
                        SYNCHRONIZE | FILE_READ_DATA | (writable ? FILE_WRITE_DATA : 0),
                        &Attributes, &Iosb, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    RtlFreeUnicodeString(&Name);
    if (!NT_SUCCESS(Status))
        goto fail;
    io->Writable = writable ? TRUE : FALSE;

    if (NtioPolicy.lock_mode != NTIO_LOCK_NONE)
    {
        Status = NtFsControlFile(io->Handle, NULL, NULL, NULL, &Iosb,
                                 FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0);
        NtioState.lock_status = Status;
        if (NT_SUCCESS(Status))
        {
            io->Locked = TRUE;
        }
        else if (NtioPolicy.lock_mode == NTIO_LOCK_REQUIRED)
        {
            NtClose(io->Handle);
            Status = STATUS_DEVICE_BUSY;
            goto fail;
        }
    }
    NtioState.locked = io->Locked;

    /* The backup boot sector lies past the end that the file system reports for the volume. */
    NtFsControlFile(io->Handle, NULL, NULL, NULL, &Iosb,
                    FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0);

    Status = QueryGeometry(io);
    if (!NT_SUCCESS(Status) || io->Size <= 0)
    {
        if (io->Locked)
            NtFsControlFile(io->Handle, NULL, NULL, NULL, &Iosb,
                            FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0);
        NtClose(io->Handle);
        if (NT_SUCCESS(Status))
            Status = STATUS_UNRECOGNIZED_MEDIA;
        goto fail;
    }
    if (!TrackOpen(io))
    {
        if (io->Locked)
            NtFsControlFile(io->Handle, NULL, NULL, NULL, &Iosb,
                            FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0);
        NtClose(io->Handle);
        Status = STATUS_TOO_MANY_OPENED_FILES;
        goto fail;
    }
    NtioState.opened++;
    NtioState.size = io->Size;
    NtioState.sector_size = (int)io->SectorSize;
    *out = io;
    return 0;

fail:
    NtioState.open_status = Status;
    RtlFreeHeap(RtlGetProcessHeap(), 0, io->Bounce);
    RtlFreeHeap(RtlGetProcessHeap(), 0, io);
    errno = StatusToErrno(Status);
    return -1;
}

int ntio_close(struct ntio *io)
{
    IO_STATUS_BLOCK Iosb;

    if (io->Locked)
    {
        /* After writes the file system must read the volume afresh: Windows chkdsk /F does the same. */
        if (io->Writable && NtioPolicy.dismount_on_close)
            NtFsControlFile(io->Handle, NULL, NULL, NULL, &Iosb,
                            FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0);
        NtFsControlFile(io->Handle, NULL, NULL, NULL, &Iosb,
                        FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0);
    }
    NtClose(io->Handle);
    UntrackOpen(io);
    RtlFreeHeap(RtlGetProcessHeap(), 0, io->Bounce);
    RtlFreeHeap(RtlGetProcessHeap(), 0, io);
    return 0;
}

int ntio_close_all(void)
{
    int i, n = 0;
    for (i = 0; i < NTIO_MAX_OPEN; i++)
    {
        if (OpenIo[i])
        {
            ntio_close(OpenIo[i]);
            n++;
        }
    }
    return n;
}

static NTSTATUS RawRead(struct ntio *io, PVOID Buffer, ULONG Length, LONGLONG Offset, PULONG Done)
{
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER Where;
    NTSTATUS Status;

    Where.QuadPart = Offset;
    Status = NtReadFile(io->Handle, NULL, NULL, NULL, &Iosb, Buffer, Length, &Where, NULL);
    *Done = NT_SUCCESS(Status) ? (ULONG)Iosb.Information : 0;
    return Status;
}

static NTSTATUS RawWrite(struct ntio *io, PVOID Buffer, ULONG Length, LONGLONG Offset)
{
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER Where;
    NTSTATUS Status;

    Where.QuadPart = Offset;
    Status = NtWriteFile(io->Handle, NULL, NULL, NULL, &Iosb, Buffer, Length, &Where, NULL);
    if (NT_SUCCESS(Status) && Iosb.Information != Length)
        Status = STATUS_UNEXPECTED_IO_ERROR;
    return Status;
}

/*
 * Volume reads must start and end on sector boundaries; libntfs reads at any byte offset.
 * Unaligned parts go through the bounce buffer.
 */
long long ntio_pread(struct ntio *io, void *buf, long long count, long long offset)
{
    PUCHAR Out = buf;
    long long Total = 0;
    ULONG Mask = io->SectorSize - 1;

    if (count < 0 || offset < 0)
    {
        errno = EINVAL;
        return -1;
    }
    if (offset >= io->Size)
        return 0;
    if (count > io->Size - offset)
        count = io->Size - offset;

    while (count > 0)
    {
        LONGLONG Start = offset & ~(LONGLONG)Mask;
        ULONG Skip = (ULONG)(offset - Start);
        ULONG Want, Done;
        NTSTATUS Status;

        if (!Skip && !(count & Mask) && !((ULONG_PTR)Out & 7))
        {
            Want = (ULONG)min(count, (long long)NTIO_MAX_TRANSFER);
            Status = RawRead(io, Out, Want, offset, &Done);
            if (!NT_SUCCESS(Status))
                goto error;
            NtioState.bytes_read += Done;
            Total += Done;
            if (Done < Want)
                break;
            Out += Done;
            offset += Done;
            count -= Done;
            continue;
        }

        Want = (ULONG)min((long long)(Skip + count + Mask) & ~(long long)Mask,
                          (long long)NTIO_MAX_TRANSFER);
        if (Start + Want > io->Size)
            Want = (ULONG)((io->Size - Start + Mask) & ~(LONGLONG)Mask);
        Status = RawRead(io, io->Bounce, Want, Start, &Done);
        if (!NT_SUCCESS(Status))
            goto error;
        NtioState.bytes_read += Done;
        if (Done <= Skip)
            break;
        Done -= Skip;
        if (Done > count)
            Done = (ULONG)count;
        memcpy(Out, io->Bounce + Skip, Done);
        Total += Done;
        Out += Done;
        offset += Done;
        count -= Done;
        if (Skip + Done < Want && count)
            break;
        continue;

error:
        if (Total)
            break;
        errno = StatusToErrno(Status);
        return -1;
    }
    return Total;
}

long long ntio_pwrite(struct ntio *io, const void *buf, long long count, long long offset)
{
    const UCHAR *In = buf;
    long long Total = 0;
    ULONG Mask = io->SectorSize - 1;
    NTSTATUS Status;

    if (!io->Writable)
    {
        NtioState.writes_refused++;
        errno = EROFS;
        return -1;
    }
    if (count < 0 || offset < 0 || offset > io->Size || count > io->Size - offset)
    {
        errno = EINVAL;
        return -1;
    }
    while (count > 0)
    {
        LONGLONG Start = offset & ~(LONGLONG)Mask;
        ULONG Skip = (ULONG)(offset - Start);
        ULONG Want, Done, Part;

        if (!Skip && !(count & Mask))
        {
            Want = (ULONG)min(count, (long long)NTIO_MAX_TRANSFER);
            memcpy(io->Bounce, In, Want);
            Status = RawWrite(io, io->Bounce, Want, offset);
            if (!NT_SUCCESS(Status))
                goto error;
            Part = Want;
        }
        else
        {
            /* Read, modify, write the sectors around the unaligned part. */
            Want = (ULONG)min((long long)(Skip + count + Mask) & ~(long long)Mask,
                              (long long)NTIO_MAX_TRANSFER);
            Status = RawRead(io, io->Bounce, Want, Start, &Done);
            if (!NT_SUCCESS(Status) || Done != Want)
            {
                if (NT_SUCCESS(Status))
                    Status = STATUS_UNEXPECTED_IO_ERROR;
                goto error;
            }
            Part = (ULONG)min((long long)(Want - Skip), count);
            memcpy(io->Bounce + Skip, In, Part);
            Status = RawWrite(io, io->Bounce, Want, Start);
            if (!NT_SUCCESS(Status))
                goto error;
        }
        Total += Part;
        In += Part;
        offset += Part;
        count -= Part;
        continue;

error:
        if (Total)
            break;
        errno = StatusToErrno(Status);
        return -1;
    }
    return Total;
}

int ntio_sync(struct ntio *io)
{
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    if (!io->Writable)
        return 0;
    Status = NtFlushBuffersFile(io->Handle, &Iosb);
    if (!NT_SUCCESS(Status))
    {
        errno = StatusToErrno(Status);
        return -1;
    }
    return 0;
}

long long ntio_size(struct ntio *io)
{
    return io->Size;
}

int ntio_sector_size(struct ntio *io)
{
    return (int)io->SectorSize;
}
