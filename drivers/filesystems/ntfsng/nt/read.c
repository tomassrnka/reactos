/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IRP_MJ_READ: cached (CcCopyRead), non-cached and paging reads
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsng.h"

NTSTATUS NgRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    PNG_FCB Fcb = FileObject->FsContext;
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    LARGE_INTEGER Offset = Stack->Parameters.Read.ByteOffset;
    ULONG Length = Stack->Parameters.Read.Length;
    BOOLEAN Paging = (Irp->Flags & IRP_PAGING_IO) != 0;
    BOOLEAN NonCached = (Irp->Flags & IRP_NOCACHE) != 0;
    LONGLONG FileSize;
    PVOID Buffer;
    NTSTATUS Status;
    long Done;

    if (Stack->MinorFunction & IRP_MN_MDL)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (!Fcb || Fcb->IsVolume || Fcb->IsDirectory || !Fcb->HasNode)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (Offset.LowPart == FILE_USE_FILE_POINTER_POSITION && Offset.HighPart == -1)
        Offset = FileObject->CurrentByteOffset;
    if (Length == 0)
        return STATUS_SUCCESS;

    FileSize = Fcb->Header.FileSize.QuadPart;
    if (Offset.QuadPart >= FileSize)
        return STATUS_END_OF_FILE;
    if (!Paging && !FsRtlCheckLockForReadAccess(&Fcb->FileLock, Irp))
        return STATUS_FILE_LOCK_CONFLICT;
    if (!Paging && Offset.QuadPart + Length > FileSize)
        Length = (ULONG)(FileSize - Offset.QuadPart);

    if (Irp->MdlAddress)
    {
        Buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
        if (!Buffer)
            return STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        Buffer = Irp->UserBuffer;
    }

    if (Paging || NonCached)
    {
        /* Through the shim page cache; bytes past EOF in the last page come back zeroed. */
        NgAcquireCore(Vcb);
        Done = NgEnsureNode(Fcb);
        if (!Done)
            Done = ngc_read(Fcb->Node, Offset.QuadPart, Length, Buffer, 1);
        else if (Fcb->Deleted)
        {
            /* A deleted file's pages can still be faulted in by a mapping: zeros. */
            RtlZeroMemory(Buffer, Length);
            Done = Length;
        }
        if (Paging && Fcb->OpenHandles == 0)
            NgParkNode(Fcb);
        NgReleaseCore(Vcb);
        if (Done < 0)
        {
            DPRINT1("ntfsng: read of %I64x at %I64d len %lu failed %ld\n", Fcb->MftNo, Offset.QuadPart, Length, Done);
            return NgErrnoToStatus((int)Done);
        }
        Irp->IoStatus.Information = Length;
        Status = STATUS_SUCCESS;
    }
    else
    {
        _SEH2_TRY
        {
            if (!FileObject->PrivateCacheMap)
            {
                CcInitializeCacheMap(FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize, FALSE,
                                     &NgGlobal.CacheCallbacks, Fcb);
            }
            if (CcCopyRead(FileObject, &Offset, Length, TRUE, Buffer, &Irp->IoStatus))
                Status = Irp->IoStatus.Status;
            else
                Status = STATUS_UNSUCCESSFUL;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
            Irp->IoStatus.Information = 0;
        }
        _SEH2_END;
    }

    if (NT_SUCCESS(Status) && !Paging && (FileObject->Flags & FO_SYNCHRONOUS_IO))
        FileObject->CurrentByteOffset.QuadPart = Offset.QuadPart + Irp->IoStatus.Information;
    return Status;
}
