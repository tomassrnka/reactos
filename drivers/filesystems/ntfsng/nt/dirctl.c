/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IRP_MJ_DIRECTORY_CONTROL: query directory, change notification
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsng.h"

VOID NgFreeDirSnapshot(PNG_CCB Ccb)
{
    ULONG i;
    for (i = 0; i < Ccb->EntryCount; i++)
        ExFreePoolWithTag(Ccb->Entries[i], TAG_NTFSNG);
    if (Ccb->Entries)
        ExFreePoolWithTag(Ccb->Entries, TAG_NTFSNG);
    Ccb->Entries = NULL;
    Ccb->EntryCount = Ccb->EntryCapacity = 0;
    Ccb->Enumerated = FALSE;
}

typedef struct _NG_SNAP
{
    PNG_CCB Ccb;
    BOOLEAN IsRoot;
    BOOLEAN Failed;
} NG_SNAP;

static int NgSnapFill(void *Context, const unsigned short *Name, unsigned int Len,
                      unsigned long long MftNo, unsigned int Type)
{
    NG_SNAP *Snap = Context;
    PNG_CCB Ccb = Snap->Ccb;
    BOOLEAN IsDot = (Len == 1 && Name[0] == L'.') || (Len == 2 && Name[0] == L'.' && Name[1] == L'.');
    PNG_DIRENT Entry;
    UNREFERENCED_PARAMETER(Type);

    /* The root of an NT volume has no "." and ".." entries. */
    if (IsDot && Snap->IsRoot)
        return 0;
    if (Ccb->EntryCount == Ccb->EntryCapacity)
    {
        ULONG Cap = Ccb->EntryCapacity ? Ccb->EntryCapacity * 2 : 64;
        PNG_DIRENT *New = ExAllocatePoolWithTag(PagedPool, Cap * sizeof(PNG_DIRENT), TAG_NTFSNG);
        if (!New)
        {
            Snap->Failed = TRUE;
            return 1;
        }
        if (Ccb->Entries)
        {
            RtlCopyMemory(New, Ccb->Entries, Ccb->EntryCount * sizeof(PNG_DIRENT));
            ExFreePoolWithTag(Ccb->Entries, TAG_NTFSNG);
        }
        Ccb->Entries = New;
        Ccb->EntryCapacity = Cap;
    }
    Entry = ExAllocatePoolWithTag(PagedPool, FIELD_OFFSET(NG_DIRENT, Name) + Len * sizeof(WCHAR), TAG_NTFSNG);
    if (!Entry)
    {
        Snap->Failed = TRUE;
        return 1;
    }
    Entry->MftNo = MftNo;
    Entry->NameLength = (USHORT)(Len * sizeof(WCHAR));
    Entry->IsDot = IsDot;
    RtlCopyMemory(Entry->Name, Name, Len * sizeof(WCHAR));
    Ccb->Entries[Ccb->EntryCount++] = Entry;
    return 0;
}

/* Writes one entry; returns its unaligned size, or 0 if it does not fit in Room. */
static ULONG NgFillEntry(FILE_INFORMATION_CLASS Class, PUCHAR Out, ULONG Room, PNG_DIRENT E,
                         const struct ngc_stat *St, ULONG Index)
{
    ULONG Attributes = NgFileAttributes(NULL, St);
    LONGLONG Eof = St->is_dir ? 0 : St->size, Alloc = St->is_dir ? 0 : St->alloc;
    ULONG Need;

#define NG_COMMON(p) \
    (p)->NextEntryOffset = 0; (p)->FileIndex = Index; \
    (p)->CreationTime.QuadPart = St->crtime; (p)->LastAccessTime.QuadPart = St->atime; \
    (p)->LastWriteTime.QuadPart = St->mtime; (p)->ChangeTime.QuadPart = St->ctime; \
    (p)->EndOfFile.QuadPart = Eof; (p)->AllocationSize.QuadPart = Alloc; \
    (p)->FileAttributes = Attributes; (p)->FileNameLength = E->NameLength; \
    RtlCopyMemory((p)->FileName, E->Name, E->NameLength)

    switch (Class)
    {
        case FileDirectoryInformation:
        {
            PFILE_DIRECTORY_INFORMATION P = (PVOID)Out;
            Need = FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName) + E->NameLength;
            if (Need > Room) return 0;
            NG_COMMON(P);
            return Need;
        }
        case FileFullDirectoryInformation:
        {
            PFILE_FULL_DIR_INFORMATION P = (PVOID)Out;
            Need = FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName) + E->NameLength;
            if (Need > Room) return 0;
            NG_COMMON(P);
            P->EaSize = 0;
            return Need;
        }
        case FileIdFullDirectoryInformation:
        {
            PFILE_ID_FULL_DIR_INFORMATION P = (PVOID)Out;
            Need = FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileName) + E->NameLength;
            if (Need > Room) return 0;
            NG_COMMON(P);
            P->EaSize = 0;
            P->FileId.QuadPart = St->mft_ref;
            return Need;
        }
        case FileBothDirectoryInformation:
        {
            PFILE_BOTH_DIR_INFORMATION P = (PVOID)Out;
            Need = FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName) + E->NameLength;
            if (Need > Room) return 0;
            NG_COMMON(P);
            P->EaSize = 0;
            P->ShortNameLength = 0;
            RtlZeroMemory(P->ShortName, sizeof(P->ShortName));
            return Need;
        }
        case FileIdBothDirectoryInformation:
        {
            PFILE_ID_BOTH_DIR_INFORMATION P = (PVOID)Out;
            Need = FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName) + E->NameLength;
            if (Need > Room) return 0;
            NG_COMMON(P);
            P->EaSize = 0;
            P->ShortNameLength = 0;
            RtlZeroMemory(P->ShortName, sizeof(P->ShortName));
            P->FileId.QuadPart = St->mft_ref;
            return Need;
        }
        case FileNamesInformation:
        {
            PFILE_NAMES_INFORMATION P = (PVOID)Out;
            Need = FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName) + E->NameLength;
            if (Need > Room) return 0;
            P->NextEntryOffset = 0;
            P->FileIndex = Index;
            P->FileNameLength = E->NameLength;
            RtlCopyMemory(P->FileName, E->Name, E->NameLength);
            return Need;
        }
        default:
            return 0;
    }
#undef NG_COMMON
}

NTSTATUS NgDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    PNG_FCB Fcb = FileObject->FsContext;
    PNG_CCB Ccb = FileObject->FsContext2;
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    FILE_INFORMATION_CLASS Class;
    PUNICODE_STRING Pattern;
    ULONG Length, Used = 0, LastOffset = 0, Written = 0;
    BOOLEAN Restart, Single;
    PUCHAR Buffer;
    NTSTATUS Status = STATUS_SUCCESS;
    int Err;

    if (Stack->MinorFunction == IRP_MN_NOTIFY_CHANGE_DIRECTORY)
    {
        if (!Fcb || !Ccb || !Fcb->IsDirectory)
            return STATUS_INVALID_PARAMETER;
        /* FsRtl keeps the IRP and completes it when a reported change matches the filter. */
        FsRtlNotifyFullChangeDirectory(Vcb->NotifySync, &Vcb->DirNotifyList, Ccb, (PSTRING)&Ccb->Path,
                                       (Stack->Flags & SL_WATCH_TREE) != 0, FALSE,
                                       Stack->Parameters.NotifyDirectory.CompletionFilter, Irp, NULL, NULL);
        return STATUS_PENDING;
    }
    if (Stack->MinorFunction != IRP_MN_QUERY_DIRECTORY)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (!Fcb || !Ccb || !Fcb->IsDirectory)
        return STATUS_INVALID_PARAMETER;

    Class = Stack->Parameters.QueryDirectory.FileInformationClass;
    Length = Stack->Parameters.QueryDirectory.Length;
    Pattern = Stack->Parameters.QueryDirectory.FileName;
    Restart = (Stack->Flags & SL_RESTART_SCAN) != 0;
    Single = (Stack->Flags & SL_RETURN_SINGLE_ENTRY) != 0;
    switch (Class)
    {
        case FileDirectoryInformation: case FileFullDirectoryInformation:
        case FileIdFullDirectoryInformation: case FileBothDirectoryInformation:
        case FileIdBothDirectoryInformation: case FileNamesInformation:
            break;
        default:
            return STATUS_INVALID_INFO_CLASS;
    }

    if (Pattern && Pattern->Length && (!Ccb->Pattern.Buffer || Restart))
    {
        if (Ccb->Pattern.Buffer)
            ExFreePoolWithTag(Ccb->Pattern.Buffer, TAG_NTFSNG);
        Ccb->Pattern.Buffer = ExAllocatePoolWithTag(PagedPool, Pattern->Length, TAG_NTFSNG);
        if (!Ccb->Pattern.Buffer)
            return STATUS_INSUFFICIENT_RESOURCES;
        Ccb->Pattern.MaximumLength = Pattern->Length;
        RtlUpcaseUnicodeString(&Ccb->Pattern, Pattern, FALSE);
        Ccb->PatternIsStar = (Pattern->Length == sizeof(WCHAR) && Pattern->Buffer[0] == L'*');
    }
    else if (!Ccb->Pattern.Buffer)
    {
        Ccb->PatternIsStar = TRUE;
    }

    if (!Ccb->Enumerated || Restart)
    {
        NG_SNAP Snap;
        NgFreeDirSnapshot(Ccb);
        Snap.Ccb = Ccb;
        Snap.IsRoot = Fcb->IsRoot;
        Snap.Failed = FALSE;
        NgAcquireCore(Vcb);
        Err = NgEnsureNode(Fcb);
        if (!Err)
            Err = ngc_readdir(Vcb->Core, Fcb->Node, NgSnapFill, &Snap);
        NgReleaseCore(Vcb);
        if (Err || Snap.Failed)
        {
            NgFreeDirSnapshot(Ccb);
            return Err ? NgErrnoToStatus(Err) : STATUS_INSUFFICIENT_RESOURCES;
        }
        Ccb->Enumerated = TRUE;
        Ccb->NextIndex = 0;
        Ccb->AnyReturned = FALSE;
    }
    if (Stack->Flags & SL_INDEX_SPECIFIED)
        Ccb->NextIndex = Stack->Parameters.QueryDirectory.FileIndex;

    if (Irp->MdlAddress)
        Buffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
    else
        Buffer = Irp->UserBuffer;
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    while (Ccb->NextIndex < Ccb->EntryCount)
    {
        PNG_DIRENT E = Ccb->Entries[Ccb->NextIndex];
        UNICODE_STRING Name;
        struct ngc_stat St;
        ngc_node *Node;
        ULONG Offset, Size;

        Name.Buffer = E->Name;
        Name.Length = Name.MaximumLength = E->NameLength;
        if (!Ccb->PatternIsStar && !FsRtlIsNameInExpression(&Ccb->Pattern, &Name, TRUE, NULL))
        {
            Ccb->NextIndex++;
            continue;
        }
        if (E->IsDot)
        {
            St = Fcb->Stat;
        }
        else
        {
            NgAcquireCore(Vcb);
            Err = ngc_iget(Vcb->Core, E->MftNo, &Node);
            if (!Err)
            {
                ngc_stat(Node, &St);
                ngc_put(Node);
            }
            NgReleaseCore(Vcb);
            if (Err)
            {
                DPRINT1("ntfsng: directory entry %wZ: inode %I64u unreadable (%d), skipped\n", &Name, E->MftNo, Err);
                Ccb->NextIndex++;
                continue;
            }
        }
        Offset = Written ? ALIGN_UP_BY(Used, 8) : 0;
        Size = Offset < Length ? NgFillEntry(Class, Buffer + Offset, Length - Offset, E, &St, Ccb->NextIndex) : 0;
        if (!Size)
        {
            if (!Written)
                Status = STATUS_BUFFER_OVERFLOW;
            break;
        }
        if (Written)
            *(PULONG)(Buffer + LastOffset) = Offset - LastOffset;
        LastOffset = Offset;
        Used = Offset + Size;
        Written++;
        Ccb->NextIndex++;
        if (Single)
            break;
    }

    if (!Written)
    {
        if (Status == STATUS_BUFFER_OVERFLOW)
            return Status;
        Status = Ccb->AnyReturned ? STATUS_NO_MORE_FILES :
                 (Ccb->PatternIsStar ? STATUS_NO_MORE_FILES : STATUS_NO_SUCH_FILE);
        return Status;
    }
    Ccb->AnyReturned = TRUE;
    Irp->IoStatus.Information = Used;
    return STATUS_SUCCESS;
}
