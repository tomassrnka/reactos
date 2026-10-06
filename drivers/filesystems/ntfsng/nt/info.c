/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IRP_MJ_QUERY/SET_INFORMATION, IRP_MJ_QUERY_VOLUME_INFORMATION
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsng.h"

ULONG NgFileAttributes(PNG_FCB Fcb, const struct ngc_stat *St)
{
    ULONG A = St->file_attributes & ~(FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_DIRECTORY);
    UNREFERENCED_PARAMETER(Fcb);
    if (St->is_dir)
        A |= FILE_ATTRIBUTE_DIRECTORY;
    if (St->flags & NGC_ATTR_SPARSE)
        A |= FILE_ATTRIBUTE_SPARSE_FILE;
    if (St->flags & NGC_ATTR_COMPRESSED)
        A |= FILE_ATTRIBUTE_COMPRESSED;
    return A ? A : FILE_ATTRIBUTE_NORMAL;
}

static VOID NgBasic(PNG_FCB Fcb, PFILE_BASIC_INFORMATION B)
{
    B->CreationTime.QuadPart = Fcb->Stat.crtime;
    B->LastAccessTime.QuadPart = Fcb->Stat.atime;
    B->LastWriteTime.QuadPart = Fcb->Stat.mtime;
    B->ChangeTime.QuadPart = Fcb->Stat.ctime;
    B->FileAttributes = NgFileAttributes(Fcb, &Fcb->Stat);
}

static VOID NgStandard(PNG_FCB Fcb, PFILE_STANDARD_INFORMATION S)
{
    S->AllocationSize.QuadPart = Fcb->IsDirectory ? 0 : Fcb->Stat.alloc;
    S->EndOfFile.QuadPart = Fcb->IsDirectory ? 0 : Fcb->Stat.size;
    S->NumberOfLinks = Fcb->Stat.nlink ? Fcb->Stat.nlink : 1;
    S->DeletePending = FALSE;
    S->Directory = Fcb->IsDirectory;
}

/* Copies the open name; returns STATUS_BUFFER_OVERFLOW with a truncated name if it does not fit. */
static NTSTATUS NgName(PNG_CCB Ccb, PFILE_NAME_INFORMATION N, ULONG Room, PULONG Used)
{
    ULONG Fixed = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName);
    ULONG Len = Ccb->Path.Length ? Ccb->Path.Length : sizeof(WCHAR);
    ULONG Copy;
    if (Room < Fixed)
        return STATUS_BUFFER_TOO_SMALL;
    N->FileNameLength = Len;
    Copy = min(Len, Room - Fixed);
    if (Ccb->Path.Length)
        RtlCopyMemory(N->FileName, Ccb->Path.Buffer, Copy);
    else if (Copy)
        N->FileName[0] = L'\\';
    *Used = Fixed + Copy;
    return Copy < Len ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

typedef struct _NG_STREAMS
{
    PUCHAR Buffer;
    ULONG Room;
    ULONG Used;
    ULONG Last;
    ULONG Count;
    NTSTATUS Status;
} NG_STREAMS;

static int NgStreamFill(void *Context, const unsigned short *Name, unsigned int Len,
                        unsigned long long Size, unsigned long long Alloc)
{
    NG_STREAMS *S = Context;
    static const WCHAR Suffix[] = L":$DATA";
    ULONG NameBytes = (1 + Len) * sizeof(WCHAR) + sizeof(Suffix) - sizeof(WCHAR);
    ULONG Offset = S->Count ? ALIGN_UP_BY(S->Used, 8) : 0;
    ULONG Need = FIELD_OFFSET(FILE_STREAM_INFORMATION, StreamName) + NameBytes;
    PFILE_STREAM_INFORMATION P;

    if (Offset + Need > S->Room)
    {
        S->Status = STATUS_BUFFER_OVERFLOW;
        return 1;
    }
    P = (PVOID)(S->Buffer + Offset);
    P->NextEntryOffset = 0;
    P->StreamNameLength = NameBytes;
    P->StreamSize.QuadPart = Size;
    P->StreamAllocationSize.QuadPart = Alloc;
    P->StreamName[0] = L':';
    RtlCopyMemory(P->StreamName + 1, Name, Len * sizeof(WCHAR));
    RtlCopyMemory(P->StreamName + 1 + Len, Suffix, sizeof(Suffix) - sizeof(WCHAR));
    if (S->Count)
        ((PFILE_STREAM_INFORMATION)(S->Buffer + S->Last))->NextEntryOffset = Offset - S->Last;
    S->Last = Offset;
    S->Used = Offset + Need;
    S->Count++;
    return 0;
}

NTSTATUS NgQueryInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    PNG_FCB Fcb = FileObject->FsContext;
    PNG_CCB Ccb = FileObject->FsContext2;
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    ULONG Length = Stack->Parameters.QueryFile.Length, Used = 0;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Fcb || !Ccb)
        return STATUS_INVALID_PARAMETER;
    if (Fcb->IsVolume)
        return STATUS_INVALID_PARAMETER;
    NgFillStat(Fcb);

#define NEED(t) do { if (Length < sizeof(t)) return STATUS_BUFFER_TOO_SMALL; Used = sizeof(t); } while (0)
    switch (Stack->Parameters.QueryFile.FileInformationClass)
    {
        case FileBasicInformation:
            NEED(FILE_BASIC_INFORMATION);
            NgBasic(Fcb, Buffer);
            break;
        case FileStandardInformation:
            NEED(FILE_STANDARD_INFORMATION);
            NgStandard(Fcb, Buffer);
            break;
        case FileInternalInformation:
            NEED(FILE_INTERNAL_INFORMATION);
            ((PFILE_INTERNAL_INFORMATION)Buffer)->IndexNumber.QuadPart = Fcb->Stat.mft_ref;
            break;
        case FileEaInformation:
            NEED(FILE_EA_INFORMATION);
            ((PFILE_EA_INFORMATION)Buffer)->EaSize = 0;
            break;
        case FilePositionInformation:
            NEED(FILE_POSITION_INFORMATION);
            ((PFILE_POSITION_INFORMATION)Buffer)->CurrentByteOffset = FileObject->CurrentByteOffset;
            break;
        case FileNameInformation:
            Status = NgName(Ccb, Buffer, Length, &Used);
            break;
        case FileNetworkOpenInformation:
        {
            PFILE_NETWORK_OPEN_INFORMATION N = Buffer;
            FILE_BASIC_INFORMATION B;
            NEED(FILE_NETWORK_OPEN_INFORMATION);
            NgBasic(Fcb, &B);
            N->CreationTime = B.CreationTime;
            N->LastAccessTime = B.LastAccessTime;
            N->LastWriteTime = B.LastWriteTime;
            N->ChangeTime = B.ChangeTime;
            N->AllocationSize.QuadPart = Fcb->IsDirectory ? 0 : Fcb->Stat.alloc;
            N->EndOfFile.QuadPart = Fcb->IsDirectory ? 0 : Fcb->Stat.size;
            N->FileAttributes = B.FileAttributes;
            break;
        }
        case FileAttributeTagInformation:
            NEED(FILE_ATTRIBUTE_TAG_INFORMATION);
            ((PFILE_ATTRIBUTE_TAG_INFORMATION)Buffer)->FileAttributes = NgFileAttributes(Fcb, &Fcb->Stat);
            ((PFILE_ATTRIBUTE_TAG_INFORMATION)Buffer)->ReparseTag = 0;
            break;
        case FileAllInformation:
        {
            PFILE_ALL_INFORMATION A = Buffer;
            ULONG NameUsed = 0;
            if (Length < FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation.FileName))
                return STATUS_BUFFER_TOO_SMALL;
            NgBasic(Fcb, &A->BasicInformation);
            NgStandard(Fcb, &A->StandardInformation);
            A->InternalInformation.IndexNumber.QuadPart = Fcb->Stat.mft_ref;
            A->EaInformation.EaSize = 0;
            A->AccessInformation.AccessFlags = 0;
            A->PositionInformation.CurrentByteOffset = FileObject->CurrentByteOffset;
            A->ModeInformation.Mode = 0;
            A->AlignmentInformation.AlignmentRequirement = DeviceObject->AlignmentRequirement;
            Status = NgName(Ccb, &A->NameInformation, Length - FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation), &NameUsed);
            Used = FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation) + NameUsed;
            break;
        }
        case FileStreamInformation:
        {
            NG_STREAMS S = { Buffer, Length, 0, 0, 0, STATUS_SUCCESS };
            int Err;
            if (Fcb->Stream.Length)
                return STATUS_INVALID_PARAMETER;
            NgAcquireCore(Vcb);
            Err = NgEnsureNode(Fcb);
            if (!Err)
                Err = ngc_streams(Fcb->Node, NgStreamFill, &S);
            NgReleaseCore(Vcb);
            if (Err)
                return NgErrnoToStatus(Err);
            Status = S.Status;
            Used = S.Used;
            break;
        }
        default:
            return STATUS_INVALID_PARAMETER;
    }
#undef NEED
    Irp->IoStatus.Information = Used;
    return Status;
}

static NTSTATUS NgSetBasic(PNG_FCB Fcb, PFILE_BASIC_INFORMATION B)
{
    PNG_VCB Vcb = Fcb->Vcb;
    long long Times[4];
    unsigned int Attrs = 0, Mask = 0;
    int Err;

    /* 0 leaves a time alone; -1 (stop automatic updates for this handle) is treated the same. */
    Times[0] = B->CreationTime.QuadPart > 0 ? B->CreationTime.QuadPart : 0;
    Times[1] = B->LastAccessTime.QuadPart > 0 ? B->LastAccessTime.QuadPart : 0;
    Times[2] = B->LastWriteTime.QuadPart > 0 ? B->LastWriteTime.QuadPart : 0;
    Times[3] = B->ChangeTime.QuadPart > 0 ? B->ChangeTime.QuadPart : 0;
    if (B->LastWriteTime.QuadPart)
        Fcb->UserSetWriteTime = TRUE;
    if (B->FileAttributes)
    {
        if (Fcb->IsDirectory && (B->FileAttributes & FILE_ATTRIBUTE_TEMPORARY))
            return STATUS_INVALID_PARAMETER;
        Attrs = B->FileAttributes & ~(FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_DIRECTORY);
        Mask = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
               FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    }
    if (!Times[0] && !Times[1] && !Times[2] && !Times[3] && !Mask)
        return STATUS_SUCCESS;
    NgAcquireCore(Vcb);
    Err = NgEnsureNode(Fcb);
    if (!Err)
        Err = ngc_set_info(Vcb->Core, Fcb->Node, Times, Attrs, Mask);
    if (!Err)
    {
        ngc_stat(Fcb->Node, &Fcb->Stat);
        NgAfterChange(Vcb);
    }
    NgReleaseCore(Vcb);
    return Err ? NgErrnoToStatus(Err) : STATUS_SUCCESS;
}

NTSTATUS NgSetInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    PNG_FCB Fcb = FileObject->FsContext;
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    FILE_INFORMATION_CLASS Class = Stack->Parameters.SetFile.FileInformationClass;
    ULONG Length = Stack->Parameters.SetFile.Length;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS Status;

    /* Moving the file pointer is the only change that does not touch the volume. */
    if (Class == FilePositionInformation)
    {
        PFILE_POSITION_INFORMATION P = Buffer;
        if (Length < sizeof(*P))
            return STATUS_INVALID_PARAMETER;
        FileObject->CurrentByteOffset = P->CurrentByteOffset;
        return STATUS_SUCCESS;
    }
    if (Vcb->ReadOnly)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (!Fcb || Fcb->IsVolume || !Fcb->HasNode)
        return STATUS_INVALID_PARAMETER;

    switch (Class)
    {
        case FileBasicInformation:
            if (Length < sizeof(FILE_BASIC_INFORMATION))
                return STATUS_INVALID_PARAMETER;
            ExAcquireResourceExclusiveLite(Fcb->Header.Resource, TRUE);
            Status = NgSetBasic(Fcb, Buffer);
            ExReleaseResourceLite(Fcb->Header.Resource);
            return Status;
        case FileEndOfFileInformation:
        case FileAllocationInformation:
        {
            LONGLONG New = ((PLARGE_INTEGER)Buffer)->QuadPart;
            if (Length < sizeof(LARGE_INTEGER))
                return STATUS_INVALID_PARAMETER;
            if (Fcb->IsDirectory)
                return STATUS_INVALID_PARAMETER;
            ExAcquireResourceExclusiveLite(Fcb->Header.Resource, TRUE);
            if (Class == FileEndOfFileInformation && Stack->Parameters.SetFile.AdvanceOnly)
            {
                /* Cc advancing the on-disk EOF/VDL: the core's size is already current. */
                Status = STATUS_SUCCESS;
            }
            else if (Class == FileAllocationInformation && New >= Fcb->Header.FileSize.QuadPart)
            {
                /* Preallocation beyond EOF is not kept (allocation follows the file size). */
                Status = STATUS_SUCCESS;
            }
            else
            {
                Status = NgSetFileSize(Fcb, FileObject, New);
            }
            ExReleaseResourceLite(Fcb->Header.Resource);
            return Status;
        }
        case FileValidDataLengthInformation:
            /* Valid data length is kept equal to the file size; bytes past the on-disk VDL read as zeros. */
            return STATUS_SUCCESS;
        default:
            return STATUS_INVALID_PARAMETER;
    }
}

NTSTATUS NgQueryVolumeInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    ULONG Length = Stack->Parameters.QueryVolume.Length, Used = 0;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG SectorsPerCluster = Vcb->Info.cluster_size / Vcb->SectorSize;

    if (DeviceObject == NgGlobal.ControlDevice)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (!Vcb->ReadOnly)
    {
        NgAcquireCore(Vcb);
        ngc_volinfo(Vcb->Core, &Vcb->Info);
        NgReleaseCore(Vcb);
    }
    switch (Stack->Parameters.QueryVolume.FsInformationClass)
    {
        case FileFsVolumeInformation:
        {
            PFILE_FS_VOLUME_INFORMATION V = Buffer;
            ULONG Fixed = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel), Copy;
            if (Length < Fixed)
                return STATUS_BUFFER_TOO_SMALL;
            V->VolumeCreationTime.QuadPart = 0;
            V->VolumeSerialNumber = Vcb->Vpb->SerialNumber;
            V->SupportsObjects = FALSE;
            V->VolumeLabelLength = Vcb->Vpb->VolumeLabelLength;
            Copy = min(V->VolumeLabelLength, Length - Fixed);
            RtlCopyMemory(V->VolumeLabel, Vcb->Vpb->VolumeLabel, Copy);
            Used = Fixed + Copy;
            if (Copy < V->VolumeLabelLength)
                Status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        case FileFsSizeInformation:
        {
            PFILE_FS_SIZE_INFORMATION S = Buffer;
            if (Length < sizeof(*S))
                return STATUS_BUFFER_TOO_SMALL;
            S->TotalAllocationUnits.QuadPart = Vcb->Info.total_clusters;
            S->AvailableAllocationUnits.QuadPart = Vcb->Info.free_clusters;
            S->SectorsPerAllocationUnit = SectorsPerCluster;
            S->BytesPerSector = Vcb->SectorSize;
            Used = sizeof(*S);
            break;
        }
        case FileFsFullSizeInformation:
        {
            PFILE_FS_FULL_SIZE_INFORMATION S = Buffer;
            if (Length < sizeof(*S))
                return STATUS_BUFFER_TOO_SMALL;
            S->TotalAllocationUnits.QuadPart = Vcb->Info.total_clusters;
            S->CallerAvailableAllocationUnits.QuadPart = Vcb->Info.free_clusters;
            S->ActualAvailableAllocationUnits.QuadPart = Vcb->Info.free_clusters;
            S->SectorsPerAllocationUnit = SectorsPerCluster;
            S->BytesPerSector = Vcb->SectorSize;
            Used = sizeof(*S);
            break;
        }
        case FileFsDeviceInformation:
        {
            PFILE_FS_DEVICE_INFORMATION D = Buffer;
            if (Length < sizeof(*D))
                return STATUS_BUFFER_TOO_SMALL;
            D->DeviceType = FILE_DEVICE_DISK;
            D->Characteristics = Vcb->StorageDevice->Characteristics;
            Used = sizeof(*D);
            break;
        }
        case FileFsAttributeInformation:
        {
            PFILE_FS_ATTRIBUTE_INFORMATION A = Buffer;
            static const WCHAR Name[] = L"NTFS";
            ULONG Fixed = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName), Copy;
            if (Length < Fixed)
                return STATUS_BUFFER_TOO_SMALL;
            A->FileSystemAttributes = FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK |
                                      FILE_NAMED_STREAMS | FILE_SUPPORTS_SPARSE_FILES |
                                      FILE_FILE_COMPRESSION | (Vcb->ReadOnly ? FILE_READ_ONLY_VOLUME : 0);
            A->MaximumComponentNameLength = 255;
            A->FileSystemNameLength = sizeof(Name) - sizeof(WCHAR);
            Copy = min(A->FileSystemNameLength, Length - Fixed);
            RtlCopyMemory(A->FileSystemName, Name, Copy);
            Used = Fixed + Copy;
            if (Copy < A->FileSystemNameLength)
                Status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        default:
            return STATUS_INVALID_PARAMETER;
    }
    Irp->IoStatus.Information = Used;
    return Status;
}
