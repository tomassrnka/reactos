/*
 * PROJECT:     ReactOS Named Pipe FileSystem
 * LICENSE:     BSD - See COPYING.ARM in the top level directory
 * FILE:        drivers/filesystems/npfs/dirctl.c
 * PURPOSE:     Directory control: listing the pipes of \Device\NamedPipe\
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES *******************************************************************/

#include "npfs.h"

/* GLOBALS ********************************************************************/

/*
 * What Windows reports for a pipe (checked on Windows 10): attributes FILE_ATTRIBUTE_NORMAL, no
 * times, EndOfFile the number of server instances now open, AllocationSize the maximum number of
 * instances.  The listing is in creation order and the name has no leading backslash.
 */

/* FUNCTIONS ******************************************************************/

static
ULONG
NpDirFixedSize(IN FILE_INFORMATION_CLASS InfoClass)
{
    switch (InfoClass)
    {
        case FileDirectoryInformation:
            return FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);
        case FileFullDirectoryInformation:
            return FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);
        case FileBothDirectoryInformation:
            return FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);
        case FileNamesInformation:
            return FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);
        default:
            return 0;
    }
}

static
VOID
NpFillDirEntry(IN FILE_INFORMATION_CLASS InfoClass,
               IN PUCHAR Entry,
               IN ULONG Fixed,
               IN PNP_FCB Fcb,
               IN PCUNICODE_STRING Name,
               IN ULONG CopyLength)
{
    PWCHAR FileName;

    RtlZeroMemory(Entry, Fixed);
    switch (InfoClass)
    {
        case FileDirectoryInformation:
        {
            PFILE_DIRECTORY_INFORMATION Info = (PVOID)Entry;
            Info->EndOfFile.QuadPart = Fcb->ServerOpenCount;
            Info->AllocationSize.QuadPart = Fcb->MaximumInstances;
            Info->FileAttributes = FILE_ATTRIBUTE_NORMAL;
            Info->FileNameLength = Name->Length;
            FileName = Info->FileName;
            break;
        }
        case FileFullDirectoryInformation:
        {
            PFILE_FULL_DIR_INFORMATION Info = (PVOID)Entry;
            Info->EndOfFile.QuadPart = Fcb->ServerOpenCount;
            Info->AllocationSize.QuadPart = Fcb->MaximumInstances;
            Info->FileAttributes = FILE_ATTRIBUTE_NORMAL;
            Info->FileNameLength = Name->Length;
            FileName = Info->FileName;
            break;
        }
        case FileBothDirectoryInformation:
        {
            PFILE_BOTH_DIR_INFORMATION Info = (PVOID)Entry;
            Info->EndOfFile.QuadPart = Fcb->ServerOpenCount;
            Info->AllocationSize.QuadPart = Fcb->MaximumInstances;
            Info->FileAttributes = FILE_ATTRIBUTE_NORMAL;
            Info->FileNameLength = Name->Length;
            FileName = Info->FileName;
            break;
        }
        default:
        {
            PFILE_NAMES_INFORMATION Info = (PVOID)Entry;
            Info->FileNameLength = Name->Length;
            FileName = Info->FileName;
            break;
        }
    }

    RtlCopyMemory(FileName, Name->Buffer, CopyLength);
}

static
NTSTATUS
NpSetQueryTemplate(IN PNP_ROOT_DCB_FCB RootCcb,
                   IN PUNICODE_STRING FileName OPTIONAL)
{
    static const WCHAR Star[] = L"*";
    PUNICODE_STRING Template;
    USHORT Length;

    /* The pattern of the first query stays for the life of the handle, as on Windows */
    if (RootCcb->QueryTemplate) return STATUS_SUCCESS;

    Length = (FileName && FileName->Length) ? FileName->Length : sizeof(Star) - sizeof(WCHAR);
    Template = ExAllocatePoolWithTag(PagedPool,
                                     sizeof(*Template) + Length + sizeof(WCHAR),
                                     NPFS_QUERY_TEMPLATE_TAG);
    if (!Template) return STATUS_INSUFFICIENT_RESOURCES;

    Template->Buffer = (PWCHAR)(Template + 1);
    Template->Length = Length;
    Template->MaximumLength = Length + sizeof(WCHAR);
    RtlCopyMemory(Template->Buffer,
                  (FileName && FileName->Length) ? FileName->Buffer : Star,
                  Length);
    Template->Buffer[Length / sizeof(WCHAR)] = UNICODE_NULL;

    /* FsRtlIsNameInExpression wants an upper case expression for a case insensitive match */
    RtlUpcaseUnicodeString(Template, Template, FALSE);

    RootCcb->QueryTemplate = Template;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NpQueryDirectory(IN PIRP Irp,
                 IN PIO_STACK_LOCATION IoStack,
                 IN PNP_DCB Dcb,
                 IN PNP_ROOT_DCB_FCB RootCcb)
{
    FILE_INFORMATION_CLASS InfoClass = IoStack->Parameters.QueryDirectory.FileInformationClass;
    ULONG Length = IoStack->Parameters.QueryDirectory.Length;
    BOOLEAN SingleEntry = (IoStack->Flags & SL_RETURN_SINGLE_ENTRY) != 0;
    PUCHAR Buffer = Irp->UserBuffer;
    PLIST_ENTRY Link;
    ULONG Fixed, Seen = 0, Emitted = 0, Start = 0, PrevStart = 0, Written = 0;
    BOOLEAN Wild;
    NTSTATUS Status;

    Irp->IoStatus.Information = 0;

    Fixed = NpDirFixedSize(InfoClass);
    if (!Fixed) return STATUS_INVALID_INFO_CLASS;
    if (Length < Fixed) return STATUS_INFO_LENGTH_MISMATCH;
    if (!Buffer) return STATUS_INVALID_USER_BUFFER;

    Status = NpSetQueryTemplate(RootCcb, IoStack->Parameters.QueryDirectory.FileName);
    if (!NT_SUCCESS(Status)) return Status;
    if (IoStack->Flags & SL_RESTART_SCAN) RootCcb->NextIndex = 0;

    Wild = FsRtlDoesNameContainWildCards(RootCcb->QueryTemplate);
    Status = STATUS_SUCCESS;

    _SEH2_TRY
    {
        for (Link = Dcb->FcbList.Flink; Link != &Dcb->FcbList; Link = Link->Flink)
        {
            PNP_FCB Fcb = CONTAINING_RECORD(Link, NP_FCB, DcbEntry);
            UNICODE_STRING Name;
            ULONG EntryLength;

            if (Fcb->NodeType != NPFS_NTC_FCB || Fcb->NameRemoved) continue;

            Name = Fcb->FullName;
            if (Name.Length && Name.Buffer[0] == L'\\')
            {
                Name.Buffer++;
                Name.Length -= sizeof(WCHAR);
                Name.MaximumLength -= sizeof(WCHAR);
            }
            if (!Name.Length) continue;

            if (Wild ? !FsRtlIsNameInExpression(RootCcb->QueryTemplate, &Name, TRUE, NULL)
                     : !RtlEqualUnicodeString(RootCcb->QueryTemplate, &Name, TRUE))
            {
                continue;
            }
            if (Seen++ < RootCcb->NextIndex) continue;

            EntryLength = Fixed + Name.Length;
            if (Emitted) Start = ALIGN_UP_BY(Written, 8);
            if (Start + EntryLength > Length)
            {
                if (!Emitted)
                {
                    /* Not even the first name fits: what fits, and the warning; the entry stays unread */
                    NpFillDirEntry(InfoClass, Buffer, Fixed, Fcb, &Name, Length - Fixed);
                    Written = Length;
                    Status = STATUS_BUFFER_OVERFLOW;
                }
                break;
            }

            NpFillDirEntry(InfoClass, Buffer + Start, Fixed, Fcb, &Name, Name.Length);
            if (Emitted) *(PULONG)(Buffer + PrevStart) = Start - PrevStart;    /* NextEntryOffset */
            PrevStart = Start;
            Written = Start + EntryLength;
            Emitted++;
            if (SingleEntry) break;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
        Emitted = 0;
        Written = 0;
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW)
    {
        Irp->IoStatus.Information = NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW ? Written : 0;
        return Status;
    }

    if (!Emitted) return STATUS_NO_MORE_FILES;

    RootCcb->NextIndex += Emitted;
    Irp->IoStatus.Information = Written;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NpCommonDirectoryControl(IN PDEVICE_OBJECT DeviceObject,
                         IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    NODE_TYPE_CODE NodeTypeCode;
    PNP_DCB Dcb;
    PNP_CCB Ccb;
    PAGED_CODE();

    Irp->IoStatus.Information = 0;

    NodeTypeCode = NpDecodeFileObject(IoStack->FileObject, (PVOID*)&Dcb, &Ccb, NULL);
    if (!NodeTypeCode) return STATUS_PIPE_DISCONNECTED;
    if (NodeTypeCode != NPFS_NTC_ROOT_DCB) return STATUS_INVALID_PARAMETER;
    if (!Ccb || Ccb->NodeType != NPFS_NTC_ROOT_DCB_CCB) return STATUS_INVALID_PARAMETER;

    switch (IoStack->MinorFunction)
    {
        case IRP_MN_QUERY_DIRECTORY:
            return NpQueryDirectory(Irp, IoStack, NpVcb->RootDcb, (PNP_ROOT_DCB_FCB)Ccb);

        default:
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS
NTAPI
NpFsdDirectoryControl(IN PDEVICE_OBJECT DeviceObject,
                      IN PIRP Irp)
{
    NTSTATUS Status;
    PAGED_CODE();

    FsRtlEnterFileSystem();
    NpAcquireExclusiveVcb();

    Status = NpCommonDirectoryControl(DeviceObject, Irp);

    NpReleaseVcb();
    FsRtlExitFileSystem();

    if (Status != STATUS_PENDING)
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NAMED_PIPE_INCREMENT);
    }

    return Status;
}

/* EOF */
