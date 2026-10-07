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

/*
 * NgMatchExpression: does Name match Expr (Expr already upcased, Name upcased per character)?
 * Semantics as documented for FsRtlIsNameInExpression: '*' any run, '?' one character,
 * DOS_STAR '<' any run that does not go past the last '.' of the name (a run that starts after
 * the last '.' may take the rest), DOS_QM '>' one character, or nothing at a '.' or at the end
 * (then the whole run of '>' is skipped), DOS_DOT '"' a '.' or nothing at the end.  DotEntry: the
 * "." and ".." entries match as the name "." on which DOS_QM may take the '.'.
 * Work: a (E+1) x (N+1) table filled backwards; Tab must hold that many bytes.
 */
static BOOLEAN NgMatchExpression(const WCHAR *Expr, USHORT E, const WCHAR *Name, USHORT N,
                                 BOOLEAN DotEntry, UCHAR *Tab)
{
    LONG Last = -1;
    USHORT i;
    LONG p, j;
#define T(p, j) Tab[(p) * (N + 1) + (j)]
    for (i = 0; i < N; i++)
        if (Name[i] == L'.')
            Last = i;
    for (j = 0; j <= N; j++)
        T(E, j) = (j == N);
    for (p = E - 1; p >= 0; p--)
    {
        WCHAR c = Expr[p];
        for (j = N; j >= 0; j--)
        {
            BOOLEAN r = FALSE;
            switch (c)
            {
            case L'*':
                r = T(p + 1, j) || (j < N && T(p, j + 1));
                break;
            case L'<':
                if (Last >= j)
                    r = T(p + 1, j) || (j < Last ? T(p, j + 1) : T(p + 1, Last + 1));
                else
                    r = T(p + 1, j) || (j < N && T(p, j + 1));
                break;
            case L'?':
                r = j < N && T(p + 1, j + 1);
                break;
            case L'>':
                if (j < N && (Name[j] != L'.' || DotEntry) && T(p + 1, j + 1))
                    r = TRUE;
                else if (j == N || Name[j] == L'.')
                {
                    LONG q = p;
                    while (q < E && Expr[q] == L'>')
                        q++;
                    r = T(q, j);
                }
                break;
            case L'"':
                if (j < N && Name[j] == L'.')
                    r = T(p + 1, j + 1);
                else if (j == N)
                    r = T(p + 1, j);
                break;
            default:
                r = j < N && RtlUpcaseUnicodeChar(Name[j]) == c && T(p + 1, j + 1);
                break;
            }
            T(p, j) = r;
        }
    }
    return T(0, 0);
#undef T
}

/* Writes one entry; returns its unaligned size, or 0 if it does not fit in Room.  Tag: a reparse point's tag, in EaSize. */
static ULONG NgFillEntry(FILE_INFORMATION_CLASS Class, PUCHAR Out, ULONG Room, PNG_DIRENT E,
                         const struct ngc_stat *St, ULONG Index, PCWSTR Short, ULONG ShortChars, ULONG Tag)
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
            P->EaSize = Tag;
            return Need;
        }
        case FileIdFullDirectoryInformation:
        {
            PFILE_ID_FULL_DIR_INFORMATION P = (PVOID)Out;
            Need = FIELD_OFFSET(FILE_ID_FULL_DIR_INFORMATION, FileName) + E->NameLength;
            if (Need > Room) return 0;
            NG_COMMON(P);
            P->EaSize = Tag;
            P->FileId.QuadPart = St->mft_ref;
            return Need;
        }
        case FileBothDirectoryInformation:
        {
            PFILE_BOTH_DIR_INFORMATION P = (PVOID)Out;
            Need = FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName) + E->NameLength;
            if (Need > Room) return 0;
            NG_COMMON(P);
            P->EaSize = Tag;
            RtlZeroMemory(P->ShortName, sizeof(P->ShortName));
            P->ShortNameLength = (CCHAR)(ShortChars * sizeof(WCHAR));
            RtlCopyMemory(P->ShortName, Short, ShortChars * sizeof(WCHAR));
            return Need;
        }
        case FileIdBothDirectoryInformation:
        {
            PFILE_ID_BOTH_DIR_INFORMATION P = (PVOID)Out;
            Need = FIELD_OFFSET(FILE_ID_BOTH_DIR_INFORMATION, FileName) + E->NameLength;
            if (Need > Room) return 0;
            NG_COMMON(P);
            P->EaSize = Tag;
            RtlZeroMemory(P->ShortName, sizeof(P->ShortName));
            P->ShortNameLength = (CCHAR)(ShortChars * sizeof(WCHAR));
            RtlCopyMemory(P->ShortName, Short, ShortChars * sizeof(WCHAR));
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
    PUCHAR MatchTab = NULL;
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

    if (!Irp->MdlAddress && Length)
    {
        /* The volume device does neither buffered nor direct I/O: lock the caller's buffer
         * (attached to the IRP, freed at completion) so a bad or vanishing buffer cannot fault here. */
        NTSTATUS Lock = STATUS_SUCCESS;
        if (!IoAllocateMdl(Irp->UserBuffer, Length, FALSE, FALSE, Irp))
            return STATUS_INSUFFICIENT_RESOURCES;
        _SEH2_TRY
        {
            MmProbeAndLockPages(Irp->MdlAddress, Irp->RequestorMode, IoWriteAccess);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Lock = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        if (!NT_SUCCESS(Lock))
        {
            IoFreeMdl(Irp->MdlAddress);
            Irp->MdlAddress = NULL;
            return Lock;
        }
    }
    Buffer = Irp->MdlAddress ? MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority) : Irp->UserBuffer;
    if (!Buffer && Length)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (!Ccb->PatternIsStar)
    {
        /* Work table of the matcher: (pattern length + 1) x (longest name + 1). */
        MatchTab = ExAllocatePoolWithTag(PagedPool, (Ccb->Pattern.Length / sizeof(WCHAR) + 1) * 256, TAG_NTFSNG);
        if (!MatchTab)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    while (Ccb->NextIndex < Ccb->EntryCount)
    {
        PNG_DIRENT E = Ccb->Entries[Ccb->NextIndex];
        UNICODE_STRING Name;
        struct ngc_stat St;
        ngc_node *Node;
        ULONG Offset, Size;
        WCHAR Short[12];
        unsigned int ShortChars;
        ULONG Tag = 0;
        BOOLEAN Matched, WantShort, Spaces;

        Name.Buffer = E->Name;
        Name.Length = Name.MaximumLength = E->NameLength;
        Matched = Ccb->PatternIsStar ||
                  NgMatchExpression(Ccb->Pattern.Buffer, Ccb->Pattern.Length / sizeof(WCHAR),
                                    E->IsDot ? L"." : E->Name, E->IsDot ? 1 : E->NameLength / sizeof(WCHAR),
                                    E->IsDot, MatchTab);
        /* A name that is not a valid 8.3 name may have a DOS name: patterns match either. */
        Spaces = FALSE;
        WantShort = !E->IsDot && (Class == FileBothDirectoryInformation || Class == FileIdBothDirectoryInformation ||
                    !Matched) && !(RtlIsNameLegalDOS8Dot3(&Name, NULL, &Spaces) && !Spaces);
        ShortChars = 0;
        if ((!Matched && !WantShort) || (!Matched && E->IsDot))
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
                if (St.file_attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                {
                    void *Data;
                    unsigned int Len;
                    if (!ngc_get_reparse(Node, &Data, &Len))
                    {
                        Tag = ((PREPARSE_DATA_BUFFER)Data)->ReparseTag;
                        ngc_free(Data);
                    }
                }
                if (WantShort && ngc_short_name(Node, Fcb->Stat.mft_ref, Short, &ShortChars))
                    ShortChars = 0;
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
        if (!Matched && !(ShortChars &&
            NgMatchExpression(Ccb->Pattern.Buffer, Ccb->Pattern.Length / sizeof(WCHAR), Short, ShortChars, FALSE, MatchTab)))
        {
            Ccb->NextIndex++;
            continue;
        }
        Offset = Written ? ALIGN_UP_BY(Used, 8) : 0;
        Size = Offset < Length ? NgFillEntry(Class, Buffer + Offset, Length - Offset, E, &St, Ccb->NextIndex,
                                             Short, ShortChars, Tag) : 0;
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

    if (MatchTab)
        ExFreePoolWithTag(MatchTab, TAG_NTFSNG);
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
