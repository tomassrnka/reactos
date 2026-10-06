/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IRP_MJ_CREATE, IRP_MJ_CLEANUP (delete on last close), IRP_MJ_CLOSE
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsng.h"

static PNG_CCB NgAllocateCcb(PCUNICODE_STRING Path)
{
    PNG_CCB Ccb = ExAllocatePoolWithTag(PagedPool, sizeof(NG_CCB), TAG_NTFSNG);
    if (!Ccb)
        return NULL;
    RtlZeroMemory(Ccb, sizeof(*Ccb));
    Ccb->Path.Buffer = ExAllocatePoolWithTag(PagedPool, Path->Length + sizeof(WCHAR), TAG_NTFSNG);
    if (!Ccb->Path.Buffer)
    {
        ExFreePoolWithTag(Ccb, TAG_NTFSNG);
        return NULL;
    }
    RtlCopyMemory(Ccb->Path.Buffer, Path->Buffer, Path->Length);
    Ccb->Path.Length = Path->Length;
    Ccb->Path.MaximumLength = Path->Length + sizeof(WCHAR);
    return Ccb;
}

static VOID NgFreeCcb(PNG_CCB Ccb)
{
    NgFreeDirSnapshot(Ccb);
    if (Ccb->Pattern.Buffer)
        ExFreePoolWithTag(Ccb->Pattern.Buffer, TAG_NTFSNG);
    if (Ccb->Path.Buffer)
        ExFreePoolWithTag(Ccb->Path.Buffer, TAG_NTFSNG);
    ExFreePoolWithTag(Ccb, TAG_NTFSNG);
}

/* Finds the FCB for (MFT record, stream) or inserts Candidate; returns the FCB to use. */
static PNG_FCB NgInsertOrFindFcb(PNG_VCB Vcb, PNG_FCB Candidate)
{
    PLIST_ENTRY Entry;
    ExAcquireFastMutex(&Vcb->FcbListLock);
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink)
    {
        PNG_FCB Fcb = CONTAINING_RECORD(Entry, NG_FCB, VcbLinks);
        if (Fcb->MftNo == Candidate->MftNo &&
            RtlEqualUnicodeString(&Fcb->Stream, &Candidate->Stream, TRUE))
        {
            if (Fcb->Deleted)
                DPRINT1("ntfsng: BUG: deleted FCB %p for %I64x still listed\n", Fcb, Fcb->MftNo);
            InterlockedIncrement(&Fcb->RefCount);
            ExReleaseFastMutex(&Vcb->FcbListLock);
            return Fcb;
        }
    }
    InsertTailList(&Vcb->FcbList, &Candidate->VcbLinks);
    ExReleaseFastMutex(&Vcb->FcbListLock);
    return Candidate;
}

static NTSTATUS NgOpenVolume(PNG_VCB Vcb, PFILE_OBJECT FileObject, PIO_STACK_LOCATION Stack)
{
    UNICODE_STRING Empty = RTL_CONSTANT_STRING(L"");
    PNG_FCB Fcb;
    PNG_CCB Ccb = NgAllocateCcb(&Empty);
    NTSTATUS Status;

    if (!Ccb)
        return STATUS_INSUFFICIENT_RESOURCES;
    ExAcquireFastMutex(&Vcb->FcbListLock);
    Fcb = Vcb->VolumeFcb;
    if (Fcb)
        InterlockedIncrement(&Fcb->RefCount);
    ExReleaseFastMutex(&Vcb->FcbListLock);
    if (!Fcb)
    {
        Fcb = NgAllocateFcb(Vcb);
        if (!Fcb)
        {
            NgFreeCcb(Ccb);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Fcb->IsVolume = TRUE;
        RtlInitEmptyUnicodeString(&Fcb->Stream, Fcb->StreamBuffer, sizeof(Fcb->StreamBuffer));
        ExAcquireFastMutex(&Vcb->FcbListLock);
        if (Vcb->VolumeFcb)
        {
            InterlockedIncrement(&Vcb->VolumeFcb->RefCount);
            ExReleaseFastMutex(&Vcb->FcbListLock);
            ExDeleteResourceLite(&Fcb->MainResource);
            ExDeleteResourceLite(&Fcb->PagingIoResource);
            ExFreePoolWithTag(Fcb, TAG_NTFSNG);
            ExAcquireFastMutex(&Vcb->FcbListLock);
            Fcb = Vcb->VolumeFcb;
        }
        else
        {
            Vcb->VolumeFcb = Fcb;
        }
        ExReleaseFastMutex(&Vcb->FcbListLock);
    }
    ExAcquireFastMutex(&Vcb->FcbListLock);
    if (Fcb->OpenHandles)
    {
        Status = IoCheckShareAccess(Stack->Parameters.Create.SecurityContext->DesiredAccess,
                                    Stack->Parameters.Create.ShareAccess, FileObject, &Fcb->ShareAccess, TRUE);
    }
    else
    {
        IoSetShareAccess(Stack->Parameters.Create.SecurityContext->DesiredAccess,
                         Stack->Parameters.Create.ShareAccess, FileObject, &Fcb->ShareAccess);
        Status = STATUS_SUCCESS;
    }
    if (NT_SUCCESS(Status))
        Fcb->OpenHandles++;
    ExReleaseFastMutex(&Vcb->FcbListLock);
    if (!NT_SUCCESS(Status))
    {
        NgFreeCcb(Ccb);
        NgDereferenceFcb(Fcb);
        return Status;
    }
    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = Ccb;
    FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;
    return STATUS_SUCCESS;
}

/* Splits "name[:stream[:$DATA]]"; returns FALSE for an attribute type other than $DATA. */
static BOOLEAN NgSplitStream(PUNICODE_STRING Last, PUNICODE_STRING Stream)
{
    UNICODE_STRING Type, Data = RTL_CONSTANT_STRING(L"$DATA");
    USHORT i, n = Last->Length / sizeof(WCHAR);

    RtlInitEmptyUnicodeString(Stream, NULL, 0);
    for (i = 0; i < n && Last->Buffer[i] != L':'; i++)
        ;
    if (i == n)
        return TRUE;
    Stream->Buffer = Last->Buffer + i + 1;
    Stream->Length = Stream->MaximumLength = (n - i - 1) * sizeof(WCHAR);
    Last->Length = i * sizeof(WCHAR);
    if (Stream->Length == 0)
        return FALSE;   /* "name:" names no stream */
    for (i = 0; i < Stream->Length / sizeof(WCHAR) && Stream->Buffer[i] != L':'; i++)
        ;
    if (i < Stream->Length / sizeof(WCHAR))
    {
        Type.Buffer = Stream->Buffer + i + 1;
        Type.Length = Type.MaximumLength = Stream->Length - (i + 1) * sizeof(WCHAR);
        if (!RtlEqualUnicodeString(&Type, &Data, TRUE))
            return FALSE;
        Stream->Length = i * sizeof(WCHAR);
    }
    return TRUE;
}

/* The FCB of an MFT record's unnamed stream, referenced, or NULL. */
PNG_FCB NgFindFcb(PNG_VCB Vcb, ULONGLONG MftNo)
{
    PLIST_ENTRY Entry;
    PNG_FCB Found = NULL;
    ExAcquireFastMutex(&Vcb->FcbListLock);
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink)
    {
        PNG_FCB Fcb = CONTAINING_RECORD(Entry, NG_FCB, VcbLinks);
        if (Fcb->MftNo == MftNo && Fcb->Stream.Length == 0)
        {
            InterlockedIncrement(&Fcb->RefCount);
            Found = Fcb;
            break;
        }
    }
    ExReleaseFastMutex(&Vcb->FcbListLock);
    return Found;
}

/* Takes a deleted FCB out of the lookup list: its MFT record number may be reused. */
VOID NgUnlistFcb(PNG_FCB Fcb)
{
    ExAcquireFastMutex(&Fcb->Vcb->FcbListLock);
    if (Fcb->VcbLinks.Flink)
    {
        RemoveEntryList(&Fcb->VcbLinks);
        Fcb->VcbLinks.Flink = Fcb->VcbLinks.Blink = NULL;
    }
    ExReleaseFastMutex(&Fcb->Vcb->FcbListLock);
}

BOOLEAN NgValidName(PCUNICODE_STRING Name)
{
    USHORT i, n = Name->Length / sizeof(WCHAR);
    if (n == 0 || n > 255)
        return FALSE;
    if ((n == 1 && Name->Buffer[0] == L'.') || (n == 2 && Name->Buffer[0] == L'.' && Name->Buffer[1] == L'.'))
        return FALSE;
    for (i = 0; i < n; i++)
    {
        WCHAR c = Name->Buffer[i];
        if (c < 0x20 || c == L'"' || c == L'*' || c == L'/' || c == L':' || c == L'<' || c == L'>' ||
            c == L'?' || c == L'\\' || c == L'|')
            return FALSE;
    }
    return TRUE;
}

/* Characters no path component may hold (after the stream suffix is split off). */
static BOOLEAN NgBadComponent(PCUNICODE_STRING Comp)
{
    USHORT i;
    for (i = 0; i < Comp->Length / sizeof(WCHAR); i++)
    {
        WCHAR c = Comp->Buffer[i];
        if (c < 0x20 || c == L'"' || c == L'*' || c == L'/' || c == L':' || c == L'<' || c == L'>' ||
            c == L'?' || c == L'|')
            return TRUE;
    }
    return FALSE;
}

/* Reports a change of the object at Path ("\\dir\\name") to directory change notification. */
VOID NgNotify(PNG_VCB Vcb, PCUNICODE_STRING Path, ULONG Filter, ULONG Action)
{
    USHORT i = Path->Length / sizeof(WCHAR);
    if (!Vcb->NotifySync || !Path->Length)
        return;
    while (i > 0 && Path->Buffer[i - 1] != L'\\')
        i--;
    FsRtlNotifyFullReportChange(Vcb->NotifySync, &Vcb->DirNotifyList, (PSTRING)Path, (USHORT)(i * sizeof(WCHAR)),
                                NULL, NULL, Filter, Action, NULL);
}

#define NG_SETTABLE_ATTRS (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | \
                           FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE | \
                           FILE_ATTRIBUTE_NOT_CONTENT_INDEXED)

NTSTATUS NgCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    PFILE_OBJECT Related = FileObject->RelatedFileObject;
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    ULONG Disposition = (Stack->Parameters.Create.Options >> 24) & 0xff;
    ULONG Options = Stack->Parameters.Create.Options & FILE_VALID_OPTION_FLAGS;
    ULONG FileAttributes = Stack->Parameters.Create.FileAttributes;
    ACCESS_MASK Access = Stack->Parameters.Create.SecurityContext->DesiredAccess;
    BOOLEAN OpenTarget = (Stack->Flags & SL_OPEN_TARGET_DIRECTORY) != 0;
    UNICODE_STRING Full, Rest, Comp, Stream;
    PNG_FCB RelatedFcb = NULL, Fcb = NULL, Found;
    PNG_CCB RelatedCcb = NULL, Ccb = NULL;
    ngc_node *Node = NULL, *Parent = NULL, *Next;
    BOOLEAN Trailing = FALSE, IsDir, Missing = FALSE, TargetExists = FALSE, Created = FALSE, Shared = FALSE;
    ULONG_PTR Information = FILE_OPENED;
    struct ngc_stat St, PSt;
    PWCHAR Real = NULL;
    unsigned int RealLen = 0;
    NTSTATUS Status;
    USHORT i;
    int Err;

    if (Related)
    {
        RelatedFcb = Related->FsContext;
        RelatedCcb = Related->FsContext2;
    }
    if (Stack->Flags & SL_OPEN_PAGING_FILE)
        return STATUS_ACCESS_DENIED;
    if (Options & FILE_OPEN_BY_FILE_ID)
        return STATUS_NOT_IMPLEMENTED;
    if (Disposition > FILE_MAXIMUM_DISPOSITION)
        return STATUS_INVALID_PARAMETER;
    if (FileObject->FileName.Length == 0 && (!RelatedFcb || RelatedFcb->IsVolume))
    {
        if (Disposition != FILE_OPEN && Disposition != FILE_OPEN_IF)
            return Vcb->ReadOnly ? STATUS_MEDIA_WRITE_PROTECTED : STATUS_ACCESS_DENIED;
        if ((Access & NG_WRITE_ACCESS) && !NgGlobal.PermissiveOpen && Vcb->ReadOnly)
            return STATUS_MEDIA_WRITE_PROTECTED;
        return NgOpenVolume(Vcb, FileObject, Stack);
    }

    /* Absolute path from the volume root, built from the related open if any. */
    Full.MaximumLength = FileObject->FileName.Length + sizeof(WCHAR) * 2 +
                         (RelatedCcb ? RelatedCcb->Path.Length : 0);
    Full.Buffer = ExAllocatePoolWithTag(PagedPool, Full.MaximumLength, TAG_NTFSNG);
    Real = ExAllocatePoolWithTag(PagedPool, 256 * sizeof(WCHAR), TAG_NTFSNG);
    if (!Full.Buffer || !Real)
    {
        if (Full.Buffer)
            ExFreePoolWithTag(Full.Buffer, TAG_NTFSNG);
        if (Real)
            ExFreePoolWithTag(Real, TAG_NTFSNG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Full.Length = 0;
    if (RelatedCcb && !RelatedFcb->IsVolume)
    {
        if (FileObject->FileName.Length && FileObject->FileName.Buffer[0] == L'\\')
        {
            Status = STATUS_OBJECT_NAME_INVALID;
            goto out;
        }
        RtlCopyUnicodeString(&Full, &RelatedCcb->Path);
        if (FileObject->FileName.Length &&
            (Full.Length == 0 || Full.Buffer[Full.Length / sizeof(WCHAR) - 1] != L'\\'))
            RtlAppendUnicodeToString(&Full, L"\\");
    }
    RtlAppendUnicodeStringToString(&Full, &FileObject->FileName);
    if (Full.Length == 0 || Full.Buffer[0] != L'\\')
    {
        Status = STATUS_OBJECT_NAME_INVALID;
        goto out;
    }
    while (Full.Length > sizeof(WCHAR) && Full.Buffer[Full.Length / sizeof(WCHAR) - 1] == L'\\')
    {
        Full.Length -= sizeof(WCHAR);
        Trailing = TRUE;
    }
    if (OpenTarget && Full.Length == sizeof(WCHAR))
    {
        Status = STATUS_OBJECT_NAME_INVALID;
        goto out;
    }

    /* Walk the path.  Parent keeps the directory of the last component. */
    NgAcquireCore(Vcb);
    Node = ngc_root(Vcb->Core);
    Rest.Buffer = Full.Buffer + 1;
    Rest.Length = Rest.MaximumLength = Full.Length - sizeof(WCHAR);
    RtlInitEmptyUnicodeString(&Stream, NULL, 0);
    RtlInitEmptyUnicodeString(&Comp, NULL, 0);
    Status = STATUS_SUCCESS;
    while (Rest.Length)
    {
        BOOLEAN LastComp;
        for (i = 0; i < Rest.Length / sizeof(WCHAR) && Rest.Buffer[i] != L'\\'; i++)
            ;
        Comp.Buffer = Rest.Buffer;
        Comp.Length = Comp.MaximumLength = i * sizeof(WCHAR);
        LastComp = (i == Rest.Length / sizeof(WCHAR));
        if (LastComp)
        {
            Rest.Length = 0;
            if (!NgSplitStream(&Comp, &Stream))
            {
                Status = STATUS_OBJECT_NAME_INVALID;
                break;
            }
        }
        else
        {
            Rest.Buffer += i + 1;
            Rest.Length -= (i + 1) * sizeof(WCHAR);
        }
        if (Comp.Length == 0 || Comp.Length > 255 * sizeof(WCHAR) || NgBadComponent(&Comp))
        {
            Status = STATUS_OBJECT_NAME_INVALID;
            break;
        }
        if (LastComp && OpenTarget)
        {
            /* Rename/link target: open the parent, report whether the final name exists. */
            if (!ngc_lookup(Vcb->Core, Node, Comp.Buffer, Comp.Length / sizeof(WCHAR), &Next, NULL, NULL))
            {
                TargetExists = TRUE;
                ngc_put(Next);
            }
            break;
        }
        Err = ngc_lookup(Vcb->Core, Node, Comp.Buffer, Comp.Length / sizeof(WCHAR), &Next,
                         LastComp ? Real : NULL, LastComp ? &RealLen : NULL);
        if (Err)
        {
            if (LastComp && Err == -NGC_ENOENT)
            {
                Missing = TRUE;
                Parent = Node;
                Node = NULL;
                RtlCopyMemory(Real, Comp.Buffer, Comp.Length);
                RealLen = Comp.Length / sizeof(WCHAR);
                break;
            }
            Status = NgErrnoToStatus(Err);
            if (Err == -NGC_ENOENT)
                Status = LastComp ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
            else if (Err == -NGC_ENOTDIR)
                Status = STATUS_OBJECT_PATH_NOT_FOUND;   /* a file used as a directory */
            break;
        }
        if (LastComp)
            Parent = Node;
        else
            ngc_put(Node);
        Node = Next;
    }
    if (NT_SUCCESS(Status) && OpenTarget)
    {
        /* Node is the target's directory; the CCB path is the directory's. */
        while (Full.Length > sizeof(WCHAR) && Full.Buffer[Full.Length / sizeof(WCHAR) - 1] != L'\\')
            Full.Length -= sizeof(WCHAR);
        if (Full.Length > sizeof(WCHAR))
            Full.Length -= sizeof(WCHAR);
        Information = TargetExists ? FILE_EXISTS : FILE_DOES_NOT_EXIST;
        Stream.Length = 0;
        Disposition = FILE_OPEN;
        Options |= FILE_DIRECTORY_FILE;
    }
    if (NT_SUCCESS(Status) && !Missing && Stream.Length)
    {
        Err = ngc_open_stream(Vcb->Core, Node, Stream.Buffer, Stream.Length / sizeof(WCHAR), &Next);
        if (Err)
        {
            Status = Err == -NGC_ENOENT ? STATUS_OBJECT_NAME_NOT_FOUND : NgErrnoToStatus(Err);
        }
        else
        {
            ngc_put(Node);
            Node = Next;
        }
    }
    if (NT_SUCCESS(Status) && Missing)
    {
        /* Create the missing last component. */
        BOOLEAN WantDir = (Options & FILE_DIRECTORY_FILE) != 0;
        if (Disposition == FILE_OPEN || Disposition == FILE_OVERWRITE)
            Status = STATUS_OBJECT_NAME_NOT_FOUND;
        else if (Vcb->ReadOnly)
            Status = STATUS_MEDIA_WRITE_PROTECTED;
        else if (Stream.Length)
            Status = STATUS_ACCESS_DENIED;          /* named stream creation is not implemented */
        else if (!NgValidName(&Comp))
            Status = STATUS_OBJECT_NAME_INVALID;
        else if (Trailing && !WantDir)
            Status = STATUS_OBJECT_NAME_INVALID;
        else if (WantDir && (FileAttributes & FILE_ATTRIBUTE_TEMPORARY))
            Status = STATUS_INVALID_PARAMETER;
        else
        {
            Err = ngc_create(Vcb->Core, Parent, Comp.Buffer, Comp.Length / sizeof(WCHAR), WantDir, &Node);
            if (!Err)
            {
                unsigned int Attrs = (FileAttributes & NG_SETTABLE_ATTRS) | (WantDir ? 0 : FILE_ATTRIBUTE_ARCHIVE);
                Created = TRUE;
                Information = FILE_CREATED;
                Err = ngc_set_info(Vcb->Core, Node, NULL, Attrs, NG_SETTABLE_ATTRS);
            }
            if (Err)
                Status = NgErrnoToStatus(Err);
            else
                NgAfterChange(Vcb);
        }
    }
    if (NT_SUCCESS(Status))
        ngc_stat(Node, &St);
    if (Parent)
        ngc_stat(Parent, &PSt);
    else
        PSt.mft_ref = 5;    /* the root is its own parent */
    NgReleaseCore(Vcb);

    if (!NT_SUCCESS(Status))
    {
        if (Status == STATUS_OBJECT_NAME_NOT_FOUND && Missing == FALSE && Disposition != FILE_OPEN &&
            Disposition != FILE_OVERWRITE && Vcb->ReadOnly)
            Status = STATUS_MEDIA_WRITE_PROTECTED;
        goto out;
    }
    IsDir = St.is_dir ? TRUE : FALSE;
    if (!Created)
    {
        /* The type checks come first: CreateFile on a directory fails with access denied for any
         * disposition, and a directory request on a file reports that before anything else. */
        if ((Options & FILE_NON_DIRECTORY_FILE) && IsDir)
        {
            Status = STATUS_FILE_IS_A_DIRECTORY;
            goto out;
        }
        if ((Options & FILE_DIRECTORY_FILE) && !IsDir)
        {
            Status = STATUS_NOT_A_DIRECTORY;
            goto out;
        }
        if (Disposition == FILE_CREATE)
        {
            Status = STATUS_OBJECT_NAME_COLLISION;
            goto out;
        }
        if (Vcb->ReadOnly && ((Disposition != FILE_OPEN && Disposition != FILE_OPEN_IF) ||
                              (Options & FILE_DELETE_ON_CLOSE) || ((Access & NG_WRITE_ACCESS) && !NgGlobal.PermissiveOpen)))
        {
            Status = STATUS_MEDIA_WRITE_PROTECTED;
            goto out;
        }
        if (!IsDir && (St.file_attributes & FILE_ATTRIBUTE_READONLY))
        {
            /* Supersede replaces the file and is allowed; overwrite keeps it and is not. */
            if ((Access & (FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_WRITE | GENERIC_ALL)) ||
                Disposition == FILE_OVERWRITE || Disposition == FILE_OVERWRITE_IF)
            {
                Status = STATUS_ACCESS_DENIED;
                goto out;
            }
            if (Options & FILE_DELETE_ON_CLOSE)
            {
                Status = STATUS_CANNOT_DELETE;
                goto out;
            }
        }
        if ((Disposition == FILE_OVERWRITE || Disposition == FILE_OVERWRITE_IF || Disposition == FILE_SUPERSEDE))
        {
            if (IsDir)
            {
                Status = STATUS_OBJECT_NAME_COLLISION;
                goto out;
            }
            /* Win32 CREATE_ALWAYS on a hidden or system file must carry those attributes. */
            if (((St.file_attributes & FILE_ATTRIBUTE_HIDDEN) && !(FileAttributes & FILE_ATTRIBUTE_HIDDEN)) ||
                ((St.file_attributes & FILE_ATTRIBUTE_SYSTEM) && !(FileAttributes & FILE_ATTRIBUTE_SYSTEM)))
            {
                Status = STATUS_ACCESS_DENIED;
                goto out;
            }
        }
    }
    if ((Options & FILE_DIRECTORY_FILE) && !IsDir)
    {
        Status = STATUS_NOT_A_DIRECTORY;
        goto out;
    }
    if ((Options & FILE_NON_DIRECTORY_FILE) && IsDir)
    {
        Status = STATUS_FILE_IS_A_DIRECTORY;
        goto out;
    }
    if (Trailing && !IsDir)
    {
        Status = STATUS_OBJECT_NAME_INVALID;
        goto out;
    }

    Ccb = NgAllocateCcb(&Full);
    Fcb = NgAllocateFcb(Vcb);
    if (!Ccb || !Fcb || Stream.Length > sizeof(Fcb->StreamBuffer))
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    if (!OpenTarget && Full.Length > sizeof(WCHAR))
    {
        Ccb->ParentMftNo = PSt.mft_ref & 0xffffffffffffULL;
        Ccb->NameLength = (USHORT)RealLen;
        RtlCopyMemory(Ccb->Name, Real, RealLen * sizeof(WCHAR));
    }
    Ccb->DeleteOnClose = (Options & FILE_DELETE_ON_CLOSE) != 0;
    {
        ACCESS_MASK Mapped = Access;
        RtlMapGenericMask(&Mapped, IoGetFileObjectGenericMapping());
        Ccb->AppendOnly = (Mapped & FILE_APPEND_DATA) && !(Mapped & FILE_WRITE_DATA);
    }
    Fcb->Node = Node;
    Fcb->HasNode = TRUE;
    Node = NULL;
    Fcb->IsDirectory = IsDir;
    Fcb->IsRoot = (Full.Length == sizeof(WCHAR));
    RtlInitEmptyUnicodeString(&Fcb->Stream, Fcb->StreamBuffer, sizeof(Fcb->StreamBuffer));
    RtlCopyUnicodeString(&Fcb->Stream, &Stream);
    Fcb->Stat = St;
    NgFillStat(Fcb);
    Fcb->LogicalVdl = Fcb->Header.FileSize.QuadPart;
    Found = NgInsertOrFindFcb(Vcb, Fcb);
    if (Found != Fcb)
    {
        NgDereferenceFcb(Fcb);
        Fcb = Found;
    }
    if (Fcb->DeletePending)
    {
        Status = STATUS_DELETE_PENDING;
        goto out;
    }
    if (!IsDir && Fcb->SectionObjectPointers.ImageSectionObject)
    {
        /* A file that is mapped as an image (a running program) cannot be opened for writing. */
        ACCESS_MASK Mapped = Access;
        RtlMapGenericMask(&Mapped, IoGetFileObjectGenericMapping());
        if ((Mapped & FILE_WRITE_DATA) && !MmFlushImageSection(&Fcb->SectionObjectPointers, MmFlushForWrite))
        {
            Status = STATUS_SHARING_VIOLATION;
            goto out;
        }
    }

    ExAcquireFastMutex(&Vcb->FcbListLock);
    if (Fcb->OpenHandles)
    {
        Status = IoCheckShareAccess(Access, Stack->Parameters.Create.ShareAccess, FileObject,
                                    &Fcb->ShareAccess, TRUE);
    }
    else
    {
        IoSetShareAccess(Access, Stack->Parameters.Create.ShareAccess, FileObject, &Fcb->ShareAccess);
        Status = STATUS_SUCCESS;
    }
    if (NT_SUCCESS(Status))
    {
        Fcb->OpenHandles++;
        Shared = TRUE;
    }
    ExReleaseFastMutex(&Vcb->FcbListLock);
    if (!NT_SUCCESS(Status))
        goto out;

    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = Ccb;
    FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;

    if (!Created && (Disposition == FILE_OVERWRITE || Disposition == FILE_OVERWRITE_IF || Disposition == FILE_SUPERSEDE))
    {
        unsigned int Attrs = (FileAttributes & NG_SETTABLE_ATTRS) | FILE_ATTRIBUTE_ARCHIVE;
        ExAcquireResourceExclusiveLite(Fcb->Header.Resource, TRUE);
        Status = NgSetFileSize(Fcb, FileObject, 0);
        if (NT_SUCCESS(Status) && (Fcb->SectionObjectPointers.SharedCacheMap || Fcb->SectionObjectPointers.DataSectionObject))
            NgPurgeFrom(Fcb, 0);
        if (NT_SUCCESS(Status))
        {
            NgAcquireCore(Vcb);
            Err = NgEnsureNode(Fcb);
            if (!Err)
                Err = ngc_set_info(Vcb->Core, Fcb->Node, NULL, Attrs, Stream.Length ? 0 : NG_SETTABLE_ATTRS);
            if (!Err)
                ngc_stat(Fcb->Node, &Fcb->Stat);
            NgAfterChange(Vcb);
            NgReleaseCore(Vcb);
            if (Err)
                Status = NgErrnoToStatus(Err);
        }
        ExReleaseResourceLite(Fcb->Header.Resource);
        if (!NT_SUCCESS(Status))
        {
            FileObject->FsContext = NULL;
            FileObject->FsContext2 = NULL;
            goto out;
        }
        Information = Disposition == FILE_SUPERSEDE ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
        NgNotify(Vcb, &Full, FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES,
                 FILE_ACTION_MODIFIED);
    }
    if (Created)
        NgNotify(Vcb, &Full, IsDir ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME, FILE_ACTION_ADDED);

    Irp->IoStatus.Information = Information;
    Shared = FALSE;
    Fcb = NULL;
    Ccb = NULL;
    if ((InterlockedIncrement(&NgGlobal.Opens) % 2000) == 0)
    {
        DPRINT1("ntfsng: %ld opens, %ld FCBs live\n", NgGlobal.Opens, NgGlobal.FcbLive);
        NgAcquireCore(Vcb);
        ngc_debug_dump();
        NgReleaseCore(Vcb);
    }

out:
    if (Shared)
    {
        ExAcquireFastMutex(&Vcb->FcbListLock);
        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);
        Fcb->OpenHandles--;
        ExReleaseFastMutex(&Vcb->FcbListLock);
    }
    if (Node || Parent)
    {
        NgAcquireCore(Vcb);
        if (Node)
            ngc_put(Node);
        if (Parent)
            ngc_put(Parent);
        NgReleaseCore(Vcb);
    }
    if (Fcb)
        NgDereferenceFcb(Fcb);
    if (Ccb)
        NgFreeCcb(Ccb);
    ExFreePoolWithTag(Real, TAG_NTFSNG);
    ExFreePoolWithTag(Full.Buffer, TAG_NTFSNG);
    return Status;
}

/* Unlinks a delete-pending file at its last cleanup (caller holds MainResource exclusive). */
static VOID NgDeleteOnLastClose(PNG_FCB Fcb)
{
    PNG_VCB Vcb = Fcb->Vcb;
    ngc_node *Dir;
    int Err;

    NgAcquireCore(Vcb);
    Err = NgEnsureNode(Fcb);
    if (!Err)
        Err = ngc_iget(Vcb->Core, Fcb->DelParentMftNo, &Dir);
    if (!Err)
    {
        if (Fcb->IsDirectory && ngc_dir_empty(Vcb->Core, Fcb->Node) != 1)
            Err = -NGC_ENOTEMPTY;
        else
            Err = ngc_unlink(Vcb->Core, Dir, Fcb->DelName, Fcb->DelNameLength, Fcb->Node);
        ngc_put(Dir);
    }
    if (!Err)
    {
        Fcb->Deleted = TRUE;
        NgUnlistFcb(Fcb);
        NgParkNode(Fcb);
        NgAfterChange(Vcb);
    }
    NgReleaseCore(Vcb);
    if (Err)
    {
        DPRINT1("ntfsng: delete of %I64x at last close failed %d\n", Fcb->MftNo, Err);
        Fcb->DeletePending = FALSE;
    }
    else if (Fcb->DelPath.Buffer)
    {
        NgNotify(Vcb, &Fcb->DelPath, Fcb->IsDirectory ? FILE_NOTIFY_CHANGE_DIR_NAME : FILE_NOTIFY_CHANGE_FILE_NAME,
                 FILE_ACTION_REMOVED);
    }
}

/* Records the name a delete will remove: the one this handle was opened by. */
VOID NgSetDeletePending(PNG_FCB Fcb, PNG_CCB Ccb)
{
    Fcb->DeletePending = TRUE;
    Fcb->DelParentMftNo = Ccb->ParentMftNo;
    Fcb->DelNameLength = Ccb->NameLength;
    RtlCopyMemory(Fcb->DelName, Ccb->Name, Ccb->NameLength * sizeof(WCHAR));
    if (Fcb->DelPath.Buffer)
        ExFreePoolWithTag(Fcb->DelPath.Buffer, TAG_NTFSNG);
    Fcb->DelPath.Buffer = ExAllocatePoolWithTag(PagedPool, Ccb->Path.Length + sizeof(WCHAR), TAG_NTFSNG);
    Fcb->DelPath.Length = Fcb->DelPath.MaximumLength = 0;
    if (Fcb->DelPath.Buffer)
    {
        RtlCopyMemory(Fcb->DelPath.Buffer, Ccb->Path.Buffer, Ccb->Path.Length);
        Fcb->DelPath.Length = Ccb->Path.Length;
        Fcb->DelPath.MaximumLength = Ccb->Path.Length + sizeof(WCHAR);
    }
}

NTSTATUS NgCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    PNG_FCB Fcb = FileObject->FsContext;
    PNG_CCB Ccb = FileObject->FsContext2;
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    BOOLEAN Last, Delete;

    if (!Fcb)
        return STATUS_SUCCESS;
    if (Fcb->IsDirectory && Vcb->NotifySync && Ccb)
        FsRtlNotifyCleanup(Vcb->NotifySync, &Vcb->DirNotifyList, Ccb);
    if (!Fcb->IsDirectory && !Fcb->IsVolume)
        FsRtlFastUnlockAll(&Fcb->FileLock, FileObject, IoGetRequestorProcess(Irp), NULL);
    ExAcquireResourceExclusiveLite(Fcb->Header.Resource, TRUE);
    if (Ccb && Ccb->DeleteOnClose && !Fcb->IsRoot && !Fcb->IsVolume && Ccb->NameLength && !Fcb->Stream.Length)
        NgSetDeletePending(Fcb, Ccb);
    ExAcquireFastMutex(&Vcb->FcbListLock);
    IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);
    Last = (--Fcb->OpenHandles == 0);
    ExReleaseFastMutex(&Vcb->FcbListLock);
    Delete = Last && Fcb->DeletePending && !Fcb->Deleted;
    if (!Fcb->IsDirectory && !Fcb->IsVolume)
    {
        if (Delete)
        {
            LARGE_INTEGER Zero;
            Zero.QuadPart = 0;
            CcUninitializeCacheMap(FileObject, &Zero, NULL);
            if (Fcb->SectionObjectPointers.SharedCacheMap || Fcb->SectionObjectPointers.DataSectionObject)
                NgPurgeFrom(Fcb, 0);
        }
        else
        {
            CcUninitializeCacheMap(FileObject, NULL, NULL);
        }
    }
    if (Delete)
    {
        NgDeleteOnLastClose(Fcb);
    }
    else if (Fcb->Modified && Fcb->HasNode)
    {
        /* NTFS updates the last write time when a modified handle is cleaned up. */
        NgApplyModified(Fcb);
        if (Ccb)
            NgNotify(Vcb, &Ccb->Path, FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE, FILE_ACTION_MODIFIED);
    }
    /* Mm may keep this FCB alive long after the last handle: park the core inode (see NgEnsureNode). */
    if (Last && Fcb->HasNode)
    {
        NgAcquireCore(Vcb);
        ExAcquireFastMutex(&Vcb->FcbListLock);
        Last = (Fcb->OpenHandles == 0);
        ExReleaseFastMutex(&Vcb->FcbListLock);
        if (Last)
            NgParkNode(Fcb);
        NgReleaseCore(Vcb);
    }
    ExReleaseResourceLite(Fcb->Header.Resource);
    FileObject->Flags |= FO_CLEANUP_COMPLETE;
    return STATUS_SUCCESS;
}

NTSTATUS NgClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    PNG_FCB Fcb = FileObject->FsContext;
    PNG_CCB Ccb = FileObject->FsContext2;
    UNREFERENCED_PARAMETER(DeviceObject);

    if (Ccb)
        NgFreeCcb(Ccb);
    if (Fcb)
        NgDereferenceFcb(Fcb);
    FileObject->FsContext = NULL;
    FileObject->FsContext2 = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS NgLockControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    PNG_FCB Fcb = FileObject ? FileObject->FsContext : NULL;
    UNREFERENCED_PARAMETER(DeviceObject);
    if (!Fcb || Fcb->IsDirectory || Fcb->IsVolume)
    {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }
    /* FsRtlProcessFileLock completes the IRP. */
    return FsRtlProcessFileLock(&Fcb->FileLock, Irp, NULL);
}
