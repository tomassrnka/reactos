/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IRP_MJ_CREATE (open existing only), IRP_MJ_CLEANUP, IRP_MJ_CLOSE
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsng.h"

#define NG_WRITE_ACCESS (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | \
                         FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL)

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

NTSTATUS NgCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = Stack->FileObject;
    PFILE_OBJECT Related = FileObject->RelatedFileObject;
    PNG_VCB Vcb = DeviceObject->DeviceExtension;
    ULONG Disposition = (Stack->Parameters.Create.Options >> 24) & 0xff;
    ULONG Options = Stack->Parameters.Create.Options & FILE_VALID_OPTION_FLAGS;
    ACCESS_MASK Access = Stack->Parameters.Create.SecurityContext->DesiredAccess;
    UNICODE_STRING Full, Rest, Comp, Stream;
    PNG_FCB RelatedFcb = NULL, Fcb = NULL, Found;
    PNG_CCB RelatedCcb = NULL, Ccb = NULL;
    ngc_node *Node = NULL, *Next;
    BOOLEAN Trailing = FALSE, IsDir;
    struct ngc_stat St;
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
    /* Read-only driver: anything that could modify the volume is refused up front. */
    if (Disposition != FILE_OPEN && Disposition != FILE_OPEN_IF)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if ((Access & NG_WRITE_ACCESS) || (Options & FILE_DELETE_ON_CLOSE))
        return STATUS_MEDIA_WRITE_PROTECTED;

    if (FileObject->FileName.Length == 0 && (!RelatedFcb || RelatedFcb->IsVolume))
        return NgOpenVolume(Vcb, FileObject, Stack);

    /* Absolute path from the volume root, built from the related open if any. */
    Full.MaximumLength = FileObject->FileName.Length + sizeof(WCHAR) * 2 +
                         (RelatedCcb ? RelatedCcb->Path.Length : 0);
    Full.Buffer = ExAllocatePoolWithTag(PagedPool, Full.MaximumLength, TAG_NTFSNG);
    if (!Full.Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
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

    NgAcquireCore(Vcb);
    Node = ngc_root(Vcb->Core);
    Rest.Buffer = Full.Buffer + 1;
    Rest.Length = Rest.MaximumLength = Full.Length - sizeof(WCHAR);
    RtlInitEmptyUnicodeString(&Stream, NULL, 0);
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
        if (Comp.Length == 0 || Comp.Length > 255 * sizeof(WCHAR))
        {
            Status = STATUS_OBJECT_NAME_INVALID;
            break;
        }
        Err = ngc_lookup(Vcb->Core, Node, Comp.Buffer, Comp.Length / sizeof(WCHAR), &Next);
        if (Err)
        {
            Status = NgErrnoToStatus(Err);
            if (Err == -NGC_ENOENT || Err == -NGC_ENOTDIR)
                Status = LastComp ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
            break;
        }
        ngc_put(Node);
        Node = Next;
    }
    if (NT_SUCCESS(Status) && Stream.Length)
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
    if (NT_SUCCESS(Status))
        ngc_stat(Node, &St);
    NgReleaseCore(Vcb);

    if (!NT_SUCCESS(Status))
    {
        /* FILE_OPEN_IF would have to create the file. */
        if (Status == STATUS_OBJECT_NAME_NOT_FOUND && Disposition == FILE_OPEN_IF)
            Status = STATUS_MEDIA_WRITE_PROTECTED;
        goto out;
    }

    IsDir = St.is_dir ? TRUE : FALSE;
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
    Fcb->Node = Node;
    Fcb->HasNode = TRUE;
    Node = NULL;
    Fcb->IsDirectory = IsDir;
    Fcb->IsRoot = (Full.Length == sizeof(WCHAR));
    RtlInitEmptyUnicodeString(&Fcb->Stream, Fcb->StreamBuffer, sizeof(Fcb->StreamBuffer));
    RtlCopyUnicodeString(&Fcb->Stream, &Stream);
    Fcb->Stat = St;
    NgFillStat(Fcb);
    Found = NgInsertOrFindFcb(Vcb, Fcb);
    if (Found != Fcb)
    {
        NgDereferenceFcb(Fcb);
        Fcb = Found;
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
        Fcb->OpenHandles++;
    ExReleaseFastMutex(&Vcb->FcbListLock);
    if (!NT_SUCCESS(Status))
        goto out;

    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = Ccb;
    FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;
    Irp->IoStatus.Information = FILE_OPENED;
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
    if (Node)
    {
        NgAcquireCore(Vcb);
        ngc_put(Node);
        NgReleaseCore(Vcb);
    }
    if (Fcb)
        NgDereferenceFcb(Fcb);
    if (Ccb)
        NgFreeCcb(Ccb);
    ExFreePoolWithTag(Full.Buffer, TAG_NTFSNG);
    return Status;
}

NTSTATUS NgCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PFILE_OBJECT FileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;
    PNG_FCB Fcb = FileObject->FsContext;
    BOOLEAN Last;
    UNREFERENCED_PARAMETER(DeviceObject);

    if (!Fcb)
        return STATUS_SUCCESS;
    ExAcquireFastMutex(&Fcb->Vcb->FcbListLock);
    IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);
    Last = (--Fcb->OpenHandles == 0);
    ExReleaseFastMutex(&Fcb->Vcb->FcbListLock);
    if (!Fcb->IsDirectory && !Fcb->IsVolume)
        CcUninitializeCacheMap(FileObject, NULL, NULL);
    /* Mm may keep this FCB alive long after the last handle: park the core inode (see NgEnsureNode). */
    if (Last && Fcb->HasNode)
    {
        NgAcquireCore(Fcb->Vcb);
        ExAcquireFastMutex(&Fcb->Vcb->FcbListLock);
        Last = (Fcb->OpenHandles == 0);
        ExReleaseFastMutex(&Fcb->Vcb->FcbListLock);
        if (Last)
            NgParkNode(Fcb);
        NgReleaseCore(Fcb->Vcb);
    }
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
