/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IRP_MJ_WRITE (cached, non-cached, paging), size changes, flush, shutdown,
 *              Cc callbacks and the per-volume metadata flusher
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsng.h"

/* ngos_nt.c */
int ngos_dev_read(void *dev, unsigned long long off, void *buf, unsigned int len);
int ngos_dev_write(void *dev, unsigned long long off, void *buf, unsigned int len);

/* Cc maps files in views of this size (VACB_MAPPING_GRANULARITY). */
#define NG_VACB_SIZE (256 * 1024)

/*
 * Lock order: FCB MainResource -> FCB PagingIoResource -> Vcb->CoreLock -> core mutexes.
 * CoreLock is never held across a Cc or Mm call or while touching unlocked user memory, and
 * paging I/O takes only CoreLock, so Cc's lazy writer and Mm's writers cannot deadlock with a
 * user thread that holds FCB resources while it waits inside Cc.
 */

BOOLEAN NTAPI NgAcquireForLazyWrite(PVOID Context, BOOLEAN Wait)
{
    PNG_FCB Fcb = Context;
    if (!ExAcquireResourceSharedLite(Fcb->Header.PagingIoResource, Wait))
        return FALSE;
    ASSERT(IoGetTopLevelIrp() == NULL);
    IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    return TRUE;
}

VOID NTAPI NgReleaseFromLazyWrite(PVOID Context)
{
    PNG_FCB Fcb = Context;
    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP)
        IoSetTopLevelIrp(NULL);
    ExReleaseResourceLite(Fcb->Header.PagingIoResource);
}

BOOLEAN NTAPI NgAcquireForReadAhead(PVOID Context, BOOLEAN Wait)
{
    PNG_FCB Fcb = Context;
    if (!ExAcquireResourceSharedLite(Fcb->Header.Resource, Wait))
        return FALSE;
    ASSERT(IoGetTopLevelIrp() == NULL);
    IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    return TRUE;
}

VOID NTAPI NgReleaseFromReadAhead(PVOID Context)
{
    PNG_FCB Fcb = Context;
    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP)
        IoSetTopLevelIrp(NULL);
    ExReleaseResourceLite(Fcb->Header.Resource);
}

/* Under CoreLock, after a change: once the system is shutting down, nothing stays unsynced. */
VOID NgAfterChange(PNG_VCB Vcb)
{
    if (Vcb->WriteThrough)
    {
        int Err = ngc_sync(Vcb->Core);
        Vcb->Syncs++;
        if (Err)
            DPRINT1("ntfsng: write-through sync failed %d\n", Err);
    }
}

/* Header sizes from the core inode (caller holds CoreLock and the node). */
static VOID NgSizesFromCore(PNG_FCB Fcb)
{
    LONGLONG Alloc;
    ngc_stat(Fcb->Node, &Fcb->Stat);
    Alloc = (Fcb->Stat.size + PAGE_SIZE - 1) & ~(LONGLONG)(PAGE_SIZE - 1);
    if ((LONGLONG)Fcb->Stat.alloc > Alloc)
        Alloc = Fcb->Stat.alloc;
    Fcb->Header.AllocationSize.QuadPart = Alloc;
    if (Alloc > Fcb->CachedEnd)
        Fcb->CachedEnd = Alloc;
    Fcb->Header.FileSize.QuadPart = Fcb->Stat.size;
    Fcb->Header.ValidDataLength.QuadPart = Fcb->Stat.size;
}

/* Last write and change time to now plus the archive bit, once per modification burst. */
VOID NgApplyModified(PNG_FCB Fcb)
{
    static const long long Now[4] = { 0, 0, -1, -1 };
    static const long long ChangeOnly[4] = { 0, 0, 0, -1 };
    PNG_VCB Vcb = Fcb->Vcb;
    if (!Fcb->Modified || Vcb->ReadOnly || !Fcb->HasNode || Fcb->Deleted)
        return;
    Fcb->Modified = FALSE;
    NgAcquireCore(Vcb);
    if (!NgEnsureNode(Fcb))
    {
        ngc_set_info(Vcb->Core, Fcb->Node, Fcb->UserSetWriteTime ? ChangeOnly : Now,
                     FILE_ATTRIBUTE_ARCHIVE, Fcb->IsDirectory ? 0 : FILE_ATTRIBUTE_ARCHIVE);
        NgAfterChange(Vcb);
        if (Fcb->OpenHandles == 0)
            NgParkNode(Fcb);
    }
    NgReleaseCore(Vcb);
}

/*
 * Whole-stream flushes and purges always name an explicit range.  Given no range (or length 0),
 * ReactOS MmFlushSegment and MmPurgeSegment end at the page table that was created last, which
 * is not the highest one once a file has shrunk and grown: pages above it are neither flushed
 * nor purged (stale data came back after a later grow).  The range ends past every size the
 * stream has had, and goes in chunks because the length is a ULONG.
 */
#define NG_RANGE_CHUNK 0x40000000
static LONGLONG NgCachedLimit(PNG_FCB Fcb)
{
    LONGLONG End = max(Fcb->CachedEnd, Fcb->Header.AllocationSize.QuadPart);
    return ((End + NG_VACB_SIZE - 1) & ~(LONGLONG)(NG_VACB_SIZE - 1)) + NG_VACB_SIZE;
}

VOID NgFlushStream(PNG_FCB Fcb, PIO_STATUS_BLOCK Iosb)
{
    LARGE_INTEGER Li;
    LONGLONG End;

    Iosb->Status = STATUS_SUCCESS;
    Iosb->Information = 0;
    if (Fcb->SectionObjectPointers.SharedCacheMap)
    {
        /* Cc walks its views up to FileSize and hands Mm explicit ranges itself. */
        CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, Iosb);
        return;
    }
    if (!Fcb->SectionObjectPointers.DataSectionObject)
        return;
    End = NgCachedLimit(Fcb);
    for (Li.QuadPart = 0; Li.QuadPart < End; Li.QuadPart += NG_RANGE_CHUNK)
    {
        IO_STATUS_BLOCK One;
        One.Status = STATUS_SUCCESS;
        CcFlushCache(&Fcb->SectionObjectPointers, &Li, (ULONG)min(End - Li.QuadPart, (LONGLONG)NG_RANGE_CHUNK), &One);
        if (!NT_SUCCESS(One.Status) && NT_SUCCESS(Iosb->Status))
            Iosb->Status = One.Status;
    }
}

/* Drops every cached page of the stream from Start on; FALSE if something was in use. */
BOOLEAN NgPurgeFrom(PNG_FCB Fcb, LONGLONG Start)
{
    LARGE_INTEGER Li;
    LONGLONG End = NgCachedLimit(Fcb);
    BOOLEAN Ok;

    /* Cc's views first (its view walk handles an open end correctly), then Mm's pages by range. */
    Li.QuadPart = Start;
    Ok = CcPurgeCacheSection(&Fcb->SectionObjectPointers, &Li, 0, FALSE);
    for (; Ok && Li.QuadPart < End; Li.QuadPart += NG_RANGE_CHUNK)
        Ok = CcPurgeCacheSection(&Fcb->SectionObjectPointers, &Li,
                                 (ULONG)min(End - Li.QuadPart, (LONGLONG)NG_RANGE_CHUNK), FALSE);
    return Ok;
}

/*
 * EOF change.  Caller holds MainResource exclusive.  Core first, then the header, then Cc.
 * A shrink flushes Cc first so the core's on-disk view of the surviving bytes is current.
 */
NTSTATUS NgSetFileSize(PNG_FCB Fcb, PFILE_OBJECT FileObject, LONGLONG NewSize)
{
    PNG_VCB Vcb = Fcb->Vcb;
    LONGLONG OldSize = Fcb->Header.FileSize.QuadPart;
    LARGE_INTEGER Li, Li2;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status = STATUS_SUCCESS;
    int Err;

    if (NewSize < 0)
        return STATUS_INVALID_PARAMETER;
    if (NewSize == OldSize)
        return STATUS_SUCCESS;
    if (NewSize < OldSize)
    {
        Li.QuadPart = NewSize;
        if (!MmCanFileBeTruncated(&Fcb->SectionObjectPointers, &Li))
            return STATUS_USER_MAPPED_FILE;
        NgFlushStream(Fcb, &Iosb);
    }
    ExAcquireResourceExclusiveLite(Fcb->Header.PagingIoResource, TRUE);
    NgAcquireCore(Vcb);
    Err = NgEnsureNode(Fcb);
    if (!Err)
        Err = ngc_set_size(Vcb->Core, Fcb->Node, (unsigned long long)NewSize);
    if (Fcb->Node)
        NgSizesFromCore(Fcb);
    if (!Err)
        NgAfterChange(Vcb);
    if (!Err && Fcb->IsPagingFile && !NT_SUCCESS(NgPagingFileMap(Fcb)))
        Err = -NGC_ENOMEM;
    NgReleaseCore(Vcb);
    ExReleaseResourceLite(Fcb->Header.PagingIoResource);
    if (Err)
    {
        DPRINT1("ntfsng: set size of %I64x to %I64d failed %d\n", Fcb->MftNo, NewSize, Err);
        Status = Err == -NGC_ENOSPC ? STATUS_DISK_FULL : NgErrnoToStatus(Err);
    }
    /*
     * Keep Cc from showing bytes from before a truncation once the file grows again.  ReactOS's
     * CcPurgeCacheSection skips a 256 KiB view that starts before the purge offset, and
     * CcSetFileSizes purges only past the allocation, so pages past EOF can survive a shrink.
     */
    _SEH2_TRY
    {
        if (FileObject && Fcb->SectionObjectPointers.SharedCacheMap)
        {
            if (NewSize > OldSize && NT_SUCCESS(Status))
            {
                /* CcZeroData skips the work when the range straddles the cache map's valid data
                 * length, so that is held at the old size while zeroing the exposed range. */
                CC_FILE_SIZES Sizes;
                Sizes.AllocationSize = Fcb->Header.AllocationSize;
                Sizes.FileSize = Fcb->Header.FileSize;
                Sizes.ValidDataLength.QuadPart = OldSize;
                CcSetFileSizes(FileObject, &Sizes);
                Li.QuadPart = OldSize;
                Li2.QuadPart = NewSize;
                CcZeroData(FileObject, &Li, &Li2, TRUE);
            }
            CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize);
        }
        else if (NewSize > OldSize && Fcb->SectionObjectPointers.DataSectionObject)
        {
            /* No cache map: drop whatever Mm still holds from the old EOF's view on (flushed first:
             * the pages below the old EOF may be dirty). */
            Li.QuadPart = OldSize & ~(LONGLONG)(NG_VACB_SIZE - 1);
            CcFlushCache(&Fcb->SectionObjectPointers, &Li, (ULONG)(OldSize - Li.QuadPart) + PAGE_SIZE, &Iosb);
            if (!NgPurgeFrom(Fcb, Li.QuadPart))
                DPRINT1("ntfsng: purge of %I64x from %I64d after a grow failed\n", Fcb->MftNo, Li.QuadPart);
        }
        if (NewSize < OldSize && NT_SUCCESS(Status) &&
            (Fcb->SectionObjectPointers.SharedCacheMap || Fcb->SectionObjectPointers.DataSectionObject))
        {
            /* Everything was flushed above, so purging from the view boundary loses nothing. */
            Li.QuadPart = NewSize & ~(LONGLONG)(NG_VACB_SIZE - 1);
            if (!NgPurgeFrom(Fcb, Li.QuadPart))
                DPRINT1("ntfsng: purge of %I64x from %I64d after a shrink failed\n", Fcb->MftNo, Li.QuadPart);
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (NT_SUCCESS(Status))
    {
        Fcb->Modified = TRUE;
        if (Fcb->LogicalVdl > NewSize)
            Fcb->LogicalVdl = NewSize;
    }
    return Status;
}

/* Paging write from the lazy writer or Mm: no FCB resources here, only CoreLock. */
static NTSTATUS NgPagingWrite(PNG_VCB Vcb, PNG_FCB Fcb, PIRP Irp, LONGLONG Offset, ULONG Length)
{
    LONGLONG FileSize = Fcb->Header.FileSize.QuadPart;
    PVOID Buffer;
    ULONG Clipped;
    long Done = 0;

    Irp->IoStatus.Information = Length;
    if (Fcb->Deleted || Offset >= FileSize)
        return STATUS_SUCCESS;
    Clipped = (ULONG)min((LONGLONG)Length, FileSize - Offset);
    Buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, HighPagePriority);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    NgAcquireCore(Vcb);
    /* Deleted under CoreLock: a page writer that raced with the last cleanup's delete drops its data. */
    if (Fcb->Deleted)
    {
        NgReleaseCore(Vcb);
        return STATUS_SUCCESS;
    }
    Done = NgEnsureNode(Fcb);
    if (!Done)
        Done = ngc_write(Vcb->Core, Fcb->Node, Offset, Clipped, Buffer);
    if (Done >= 0)
        NgAfterChange(Vcb);
    if (Fcb->OpenHandles == 0)
        NgParkNode(Fcb);
    NgReleaseCore(Vcb);
    if (Done < 0)
    {
        DPRINT1("ntfsng: paging write of %I64x at %I64d len %lu failed %ld (size %I64d, handles %ld, deleted %u)\n",
                Fcb->MftNo, Offset, Clipped, Done, Fcb->Header.FileSize.QuadPart, Fcb->OpenHandles, Fcb->Deleted);
        Irp->IoStatus.Information = 0;
        return Done == -NGC_EROFS ? STATUS_MEDIA_WRITE_PROTECTED : STATUS_UNEXPECTED_IO_ERROR;
    }
    return STATUS_SUCCESS;
}

/* Writes a locked buffer straight to the storage device, through a sector-aligned pool buffer. */
static NTSTATUS NgWriteDevice(PNG_VCB Vcb, LONGLONG Offset, PUCHAR Buffer, ULONG Length)
{
    PUCHAR Bounce = ExAllocatePoolWithTag(NonPagedPool, 64 * 1024, TAG_NTFSNG);
    ULONG Done = 0;

    if (!Bounce)
        return STATUS_INSUFFICIENT_RESOURCES;
    while (Done < Length)
    {
        ULONG n = min(Length - Done, 64 * 1024);
        RtlCopyMemory(Bounce, Buffer + Done, n);
        if (ngos_dev_write(Vcb->StorageDevice, (unsigned long long)Offset + Done, Bounce, n))
            break;
        Done += n;
    }
    ExFreePoolWithTag(Bounce, TAG_NTFSNG);
    return Done == Length ? STATUS_SUCCESS : STATUS_UNEXPECTED_IO_ERROR;
}

/*
 * Writes through a volume handle.  The handle that holds the volume lock writes anywhere, as on
 * Windows (formatters lock the volume and write it); from then on the mounted state no longer
 * describes the disk, so nothing of it is written back when the volume is dismounted.  Without
 * the lock only the boot code ($Boot, the first 8 KiB) may change under the mounted volume: setup
 * installs the boot sector that way, through the core's device path so the journal overlay and
 * the cached boot sector see it.
 */
#define NG_BOOT_REGION 8192
static NTSTATUS NgWriteVolume(PNG_VCB Vcb, PIRP Irp, LONGLONG Offset, ULONG Length)
{
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    BOOLEAN Holder = Vcb->LockedBy == FileObject || Vcb->Dismounted;
    BOOLEAN Raw = Vcb->Dismounted;
    PMDL Mdl = NULL;
    PVOID Buffer;
    NTSTATUS Status = STATUS_SUCCESS;
    int Err;

    if (Vcb->ReadOnly && !Holder)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (Offset < 0 || ((ULONG)Offset | Length) & (Vcb->SectorSize - 1))
        return STATUS_INVALID_PARAMETER;
    if (Offset + Length > NG_BOOT_REGION)
    {
        if (!Holder)
        {
            DPRINT1("ntfsng: volume write at %I64d len %lu refused (outside the boot code, volume not locked)\n", Offset, Length);
            return STATUS_ACCESS_DENIED;
        }
        Raw = TRUE;
    }
    if (Offset == 0 && Length < 512)
        return STATUS_INVALID_PARAMETER;
    if (Irp->MdlAddress)
    {
        Buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
    }
    else
    {
        Mdl = IoAllocateMdl(Irp->UserBuffer, Length, FALSE, FALSE, NULL);
        if (!Mdl)
            return STATUS_INSUFFICIENT_RESOURCES;
        _SEH2_TRY
        {
            MmProbeAndLockPages(Mdl, Irp->RequestorMode, IoReadAccess);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        if (!NT_SUCCESS(Status))
        {
            IoFreeMdl(Mdl);
            return Status;
        }
        Buffer = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
    }
    if (!Buffer)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    if (Offset == 0 && !Raw)
    {
        /* Only the boot code may change under the mounted volume: the BPB and the signature stay. */
        PUCHAR Old = ExAllocatePoolWithTag(NonPagedPool, 512, TAG_NTFSNG);
        BOOLEAN Same;
        if (!Old)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto out;
        }
        Same = !ngos_dev_read(Vcb->StorageDevice, 0, Old, 512) &&
               RtlCompareMemory(Old + 3, (PUCHAR)Buffer + 3, 0x54 - 3) == 0x54 - 3 &&
               RtlCompareMemory(Old + 510, (PUCHAR)Buffer + 510, 2) == 2;
        ExFreePoolWithTag(Old, TAG_NTFSNG);
        if (!Same && !Holder)
        {
            DPRINT1("ntfsng: volume write at 0 refused: it changes the BPB\n");
            Status = STATUS_ACCESS_DENIED;
            goto out;
        }
        Raw = !Same;
    }
    if (Raw || Vcb->ReadOnly)
    {
        if (!Vcb->RawWritten && !Vcb->Dismounted)
            DPRINT1("ntfsng: the lock holder writes the volume directly (at %I64d len %lu): the mounted state is dropped at dismount\n",
                    Offset, Length);
        Vcb->RawWritten = TRUE;
        Status = NgWriteDevice(Vcb, Offset, Buffer, Length);
        if (NT_SUCCESS(Status))
            Irp->IoStatus.Information = Length;
        goto out;
    }
    NgAcquireCore(Vcb);
    Err = ngc_raw_write(Vcb->Core, (unsigned long long)Offset, Buffer, Length);
    if (!Err)
        Err = ngc_sync(Vcb->Core);
    NgReleaseCore(Vcb);
    DPRINT1("ntfsng: boot code written through the volume handle at %I64d len %lu: %d\n", Offset, Length, Err);
    if (Err)
        Status = NgErrnoToStatus(Err);
    else
        Irp->IoStatus.Information = Length;
out:
    if (Mdl)
    {
        MmUnlockPages(Mdl);
        IoFreeMdl(Mdl);
    }
    return Status;
}

NTSTATUS NgWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    PNG_FCB Fcb = FileObject->FsContext;
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    LARGE_INTEGER Offset = Stack->Parameters.Write.ByteOffset;
    ULONG Length = Stack->Parameters.Write.Length;
    BOOLEAN Paging = (Irp->Flags & IRP_PAGING_IO) != 0;
    BOOLEAN NonCached = (Irp->Flags & IRP_NOCACHE) != 0;
    BOOLEAN WriteThrough = (FileObject->Flags & FO_WRITE_THROUGH) || (Stack->Flags & SL_WRITE_THROUGH);
    BOOLEAN Append = (Offset.LowPart == FILE_WRITE_TO_END_OF_FILE && Offset.HighPart == -1);
    PMDL LockedMdl = NULL;
    PVOID Buffer;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status = STATUS_SUCCESS;
    LONGLONG End, OldSize;
    long Done;
    int Err;

    if (Stack->MinorFunction & IRP_MN_MDL)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Fcb && Fcb->IsVolume && Length)
    {
        if (Offset.LowPart == FILE_USE_FILE_POINTER_POSITION && Offset.HighPart == -1)
            Offset = FileObject->CurrentByteOffset;
        Status = NgWriteVolume(Vcb, Irp, Offset.QuadPart, Length);
        if (NT_SUCCESS(Status) && (FileObject->Flags & FO_SYNCHRONOUS_IO))
            FileObject->CurrentByteOffset.QuadPart = Offset.QuadPart + Irp->IoStatus.Information;
        return Status;
    }
    if (!Fcb || Fcb->IsVolume || Fcb->IsDirectory || !Fcb->HasNode)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Vcb->ReadOnly)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (Length == 0)
        return STATUS_SUCCESS;
    if (Paging && Fcb->IsPagingFile)
        return NgPagingFileIo(Vcb, Fcb, Irp, TRUE, Offset.QuadPart, Length);
    if (Fcb->Stat.flags & NGC_ATTR_NOWRITE)
        return STATUS_ACCESS_DENIED;
    if (Paging)
        return NgPagingWrite(Vcb, Fcb, Irp, Offset.QuadPart, Length);

    if (Offset.LowPart == FILE_USE_FILE_POINTER_POSITION && Offset.HighPart == -1)
    {
        /* Only a synchronous handle has a file position. */
        if (!(FileObject->Flags & FO_SYNCHRONOUS_IO))
            return STATUS_INVALID_PARAMETER;
        Offset = FileObject->CurrentByteOffset;
    }
    if (Offset.QuadPart < 0 && !Append)
        return STATUS_INVALID_PARAMETER;
    /* A handle with FILE_APPEND_DATA but not FILE_WRITE_DATA writes at the end of the file. */
    if (FileObject->FsContext2 && ((PNG_CCB)FileObject->FsContext2)->AppendOnly)
        Append = TRUE;
    if (!FsRtlCheckLockForWriteAccess(&Fcb->FileLock, Irp))
        return STATUS_FILE_LOCK_CONFLICT;
    if (NonCached && !Append && ((Offset.LowPart | Length) & (Vcb->SectorSize - 1)))
        return STATUS_INVALID_PARAMETER;

    /* The user buffer: locked before any core call so no page fault can happen under CoreLock. */
    if (Irp->MdlAddress)
    {
        Buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
        if (!Buffer)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    else if (NonCached)
    {
        LockedMdl = IoAllocateMdl(Irp->UserBuffer, Length, FALSE, FALSE, NULL);
        if (!LockedMdl)
            return STATUS_INSUFFICIENT_RESOURCES;
        _SEH2_TRY
        {
            MmProbeAndLockPages(LockedMdl, Irp->RequestorMode, IoReadAccess);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        if (!NT_SUCCESS(Status))
        {
            IoFreeMdl(LockedMdl);
            return Status;
        }
        Buffer = MmGetSystemAddressForMdlSafe(LockedMdl, NormalPagePriority);
        if (!Buffer)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto out_mdl;
        }
    }
    else
    {
        /* Checked before the file grows: a bad buffer must not leave a longer file behind. */
        Buffer = Irp->UserBuffer;
        if (!Buffer)
            return STATUS_INVALID_USER_BUFFER;
        if (Irp->RequestorMode != KernelMode)
        {
            _SEH2_TRY
            {
                ProbeForRead(Buffer, Length, 1);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
            }
            _SEH2_END;
            if (!NT_SUCCESS(Status))
                return Status;
        }
    }

    ExAcquireResourceExclusiveLite(Fcb->Header.Resource, TRUE);
    if (Append)
        Offset.QuadPart = Fcb->Header.FileSize.QuadPart;
    End = Offset.QuadPart + Length;
    OldSize = Fcb->Header.FileSize.QuadPart;
    if (End > Fcb->Header.FileSize.QuadPart)
    {
        Status = NgSetFileSize(Fcb, FileObject, End);
        if (!NT_SUCCESS(Status))
            goto out_unlock;
    }

    /*
     * A non-cached write must not leave older bytes in Cc or in a mapped view, and a paging read
     * can bring a purged range back at any time.  While a data section exists the write goes
     * through the cache and straight on to disk.  Without one, the main resource held exclusive
     * keeps a section from being created and cached reads out until the write is on disk.
     */
    if (NonCached && Fcb->SectionObjectPointers.DataSectionObject)
    {
        NonCached = FALSE;
        WriteThrough = TRUE;
        Vcb->NonCachedViaCache++;
    }
    if (!NonCached)
    {
        _SEH2_TRY
        {
            if (!FileObject->PrivateCacheMap)
            {
                CcInitializeCacheMap(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize, FALSE,
                                     &NgGlobal.CacheCallbacks, Fcb);
            }
            if (!CcCopyWrite(FileObject, &Offset, Length, TRUE, Buffer))
                Status = STATUS_UNSUCCESSFUL;
            else if (WriteThrough)
            {
                CcFlushCache(FileObject->SectionObjectPointer, &Offset, Length, &Iosb);
                Status = Iosb.Status;
            }
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        if (!NT_SUCCESS(Status) && End > OldSize && OldSize < Fcb->Header.FileSize.QuadPart)
            NgSetFileSize(Fcb, FileObject, OldSize);
    }
    else
    {
        NgAcquireCore(Vcb);
        Err = NgEnsureNode(Fcb);
        Done = Err ? Err : ngc_write(Vcb->Core, Fcb->Node, Offset.QuadPart, Length, Buffer);
        if (Done >= 0)
            NgAfterChange(Vcb);
        NgReleaseCore(Vcb);
        if (Done < 0)
        {
            DPRINT1("ntfsng: write of %I64x at %I64d len %lu failed %ld\n", Fcb->MftNo, Offset.QuadPart, Length, Done);
            Status = Done == -NGC_EROFS ? STATUS_MEDIA_WRITE_PROTECTED :
                     Done == -NGC_ENOSPC ? STATUS_DISK_FULL : STATUS_UNEXPECTED_IO_ERROR;
        }
    }
    if (NT_SUCCESS(Status))
    {
        Irp->IoStatus.Information = Length;
        Fcb->Modified = TRUE;
        if (End > Fcb->LogicalVdl)
            Fcb->LogicalVdl = End;
        if (FileObject->Flags & FO_SYNCHRONOUS_IO)
            FileObject->CurrentByteOffset.QuadPart = Offset.QuadPart + Length;
        FileObject->Flags |= FO_FILE_MODIFIED;
    }
out_unlock:
    ExReleaseResourceLite(Fcb->Header.Resource);
out_mdl:
    if (LockedMdl)
    {
        MmUnlockPages(LockedMdl);
        IoFreeMdl(LockedMdl);
    }
    return Status;
}

/* Flushes every cached stream of the volume through Cc, then the core metadata. */
VOID NgFlushVolume(PNG_VCB Vcb)
{
    PLIST_ENTRY Entry;
    PNG_FCB *List;
    ULONG Count = 0, Cap = 0, i;
    IO_STATUS_BLOCK Iosb;
    int Err;

    if (Vcb->ReadOnly)
        return;
    ExAcquireFastMutex(&Vcb->FcbListLock);
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink)
        Cap++;
    List = Cap ? ExAllocatePoolWithTag(NonPagedPool, Cap * sizeof(PNG_FCB), TAG_NTFSNG) : NULL;
    if (List)
    {
        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList && Count < Cap; Entry = Entry->Flink)
        {
            PNG_FCB Fcb = CONTAINING_RECORD(Entry, NG_FCB, VcbLinks);
            InterlockedIncrement(&Fcb->RefCount);
            List[Count++] = Fcb;
        }
    }
    ExReleaseFastMutex(&Vcb->FcbListLock);
    for (i = 0; i < Count; i++)
    {
        PNG_FCB Fcb = List[i];
        if (Fcb->SectionObjectPointers.DataSectionObject)
        {
            ExAcquireResourceSharedLite(Fcb->Header.Resource, TRUE);
            NgFlushStream(Fcb, &Iosb);
            ExReleaseResourceLite(Fcb->Header.Resource);
        }
        NgApplyModified(Fcb);
        NgDereferenceFcb(Fcb);
    }
    if (List)
        ExFreePoolWithTag(List, TAG_NTFSNG);
    NgAcquireCore(Vcb);
    Err = ngc_sync(Vcb->Core);
    Vcb->Syncs++;
    NgReleaseCore(Vcb);
    if (Err)
        DPRINT1("ntfsng: volume sync failed %d\n", Err);
}

NTSTATUS NgFlushBuffers(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    PNG_FCB Fcb = FileObject ? FileObject->FsContext : NULL;
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    IO_STATUS_BLOCK Iosb;
    int Err;

    if (!Fcb || Vcb->ReadOnly)
        return STATUS_SUCCESS;
    if (Fcb->IsVolume)
    {
        NgFlushVolume(Vcb);
        return STATUS_SUCCESS;
    }
    Iosb.Status = STATUS_SUCCESS;
    if (Fcb->SectionObjectPointers.DataSectionObject)
    {
        ExAcquireResourceExclusiveLite(Fcb->Header.Resource, TRUE);
        NgFlushStream(Fcb, &Iosb);
        ExReleaseResourceLite(Fcb->Header.Resource);
    }
    NgApplyModified(Fcb);
    NgAcquireCore(Vcb);
    Err = ngc_commit_now(Vcb->Core);
    Vcb->Syncs++;
    NgReleaseCore(Vcb);
    if (Err)
        return STATUS_UNEXPECTED_IO_ERROR;
    return Iosb.Status;
}

/*
 * ReactOS sends IRP_MJ_SHUTDOWN to file system control devices in IoShutdownSystem(1), after
 * the registry and Cc have flushed.  The lazy writer may still run afterwards, so every later
 * change is synced immediately (WriteThrough) and the volume ends clean.
 */
NTSTATUS NgShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PLIST_ENTRY Entry;
    PNG_VCB Vcbs[16];
    ULONG Count = 0, i;
    unsigned long Writes, Syncs, Dirties;
    unsigned long long Bytes;
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    /* A VCB is never freed after its mount, so the pointers stay valid outside the list lock; a
     * dismount is waited for (create gate) and its volume skipped. */
    ExAcquireFastMutex(&NgGlobal.VcbListLock);
    for (Entry = NgGlobal.VcbList.Flink; Entry != &NgGlobal.VcbList && Count < RTL_NUMBER_OF(Vcbs); Entry = Entry->Flink)
        Vcbs[Count++] = CONTAINING_RECORD(Entry, NG_VCB, GlobalLinks);
    ExReleaseFastMutex(&NgGlobal.VcbListLock);
    for (i = 0; i < Count; i++)
    {
        PNG_VCB Vcb = Vcbs[i];
        if (Vcb->ReadOnly)
            continue;
        ExAcquireResourceSharedLite(&Vcb->CreateGate, TRUE);
        if (Vcb->Dismounted || Vcb->RawWritten)
        {
            /* Gone, or the lock holder wrote the disk directly: mounted state is not written over it. */
            ExReleaseResourceLite(&Vcb->CreateGate);
            continue;
        }
        NgFlushVolume(Vcb);
        Vcb->WriteThrough = TRUE;
        NgAcquireCore(Vcb);
        ngc_sync(Vcb->Core);
        ngc_volinfo(Vcb->Core, &Vcb->Info);
        ngc_jnl_report(Vcb->Core);
        NgReleaseCore(Vcb);
        DPRINT1("ntfsng: shutdown: volume %08lx flushed, %s, %lu syncs, %lu non-cached writes via Cc, paging file %ld reads %ld writes\n",
                Vcb->Vpb->SerialNumber, Vcb->Info.dirty ? "STILL DIRTY" : "clean", Vcb->Syncs, Vcb->NonCachedViaCache,
                Vcb->PagingFileReads, Vcb->PagingFileWrites);
        NgPrintLockStats(Vcb);
        ExReleaseResourceLite(&Vcb->CreateGate);
    }
    ngc_write_stats(&Writes, &Bytes, &Syncs, &Dirties);
    DPRINT1("ntfsng: shutdown: %lu device writes, %I64u bytes, %lu core syncs, %lu folio dirties, stack max %lu (IRP_MJ 0x%x), core at device %lu\n",
            Writes, Bytes, Syncs, Dirties, NgGlobal.MaxStackUsed, NgGlobal.MaxStackMajor, NgGlobal.MaxStackAtIo);
    return STATUS_SUCCESS;
}

/* Linux's flusher stand-in: writes back dirty core metadata every NG_FLUSH_PERIOD_MS. */
static VOID NTAPI NgFlusherThread(PVOID Context)
{
    PNG_VCB Vcb = Context;
    LARGE_INTEGER Period;
    Period.QuadPart = -10000LL * NG_FLUSH_PERIOD_MS;
    for (;;)
    {
        if (KeWaitForSingleObject(&Vcb->FlusherStop, Executive, KernelMode, FALSE, &Period) == STATUS_SUCCESS)
            break;
        NgAcquireCore(Vcb);
        if (ngc_dirty(Vcb->Core))
        {
            /* Busy: commit and keep VOLUME_IS_DIRTY.  Quiet for a whole period: also clear the flag. */
            int Err = ngc_changed(Vcb->Core) ? ngc_commit_now(Vcb->Core) : ngc_sync(Vcb->Core);
            Vcb->Syncs++;
            if (Err)
                DPRINT1("ntfsng: background sync failed %d\n", Err);
        }
        NgReleaseCore(Vcb);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS NgStartFlusher(PNG_VCB Vcb)
{
    HANDLE Thread;
    NTSTATUS Status;
    KeInitializeEvent(&Vcb->FlusherStop, NotificationEvent, FALSE);
    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, NgFlusherThread, Vcb);
    if (!NT_SUCCESS(Status))
        return Status;
    ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&Vcb->Flusher, NULL);
    ZwClose(Thread);
    return STATUS_SUCCESS;
}
