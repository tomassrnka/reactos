/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Diagnostics: log refused and unsupported requests (what a boot tries to write)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsng.h"
#include <stdio.h>

/* Every event is logged up to this count, then one in NG_DIAG_EVERY, plus a periodic summary. */
#define NG_DIAG_FULL    3000
#define NG_DIAG_EVERY   100
#define NG_DIAG_SUMMARY 500

static LONG NgDiagSeq;
static LONG NgDiagWrite;
static LONG NgDiagByMajor[IRP_MJ_MAXIMUM_FUNCTION + 1];

static const char *const NgMajorNames[IRP_MJ_MAXIMUM_FUNCTION + 1] =
{
    "CREATE", "CREATE_NAMED_PIPE", "CLOSE", "READ", "WRITE", "QUERY_INFO", "SET_INFO",
    "QUERY_EA", "SET_EA", "FLUSH", "QUERY_VOLUME", "SET_VOLUME", "DIRCTL", "FSCTL",
    "DEVICE_CONTROL", "INTERNAL_DEVICE_CONTROL", "SHUTDOWN", "LOCK_CONTROL", "CLEANUP",
    "CREATE_MAILSLOT", "QUERY_SECURITY", "SET_SECURITY", "POWER", "SYSTEM_CONTROL",
    "DEVICE_CHANGE", "QUERY_QUOTA", "SET_QUOTA", "PNP"
};

static const char *const NgDispNames[] =
{
    "SUPERSEDE", "OPEN", "CREATE", "OPEN_IF", "OVERWRITE", "OVERWRITE_IF"
};

static BOOLEAN NgDiagIsRefusal(PIO_STACK_LOCATION Stack, NTSTATUS Status)
{
    UCHAR Major = Stack->MajorFunction;
    if (Major == IRP_MJ_CREATE && NT_SUCCESS(Status))
        return (Stack->Parameters.Create.SecurityContext->DesiredAccess & NG_WRITE_ACCESS) != 0;
    if (Major == IRP_MJ_FLUSH_BUFFERS || Major == IRP_MJ_SHUTDOWN)
        return TRUE;    /* succeeds as a no-op: still a write-path request */
    return Status == STATUS_MEDIA_WRITE_PROTECTED || Status == STATUS_INVALID_DEVICE_REQUEST ||
           Status == STATUS_NOT_IMPLEMENTED || Status == STATUS_NOT_SUPPORTED ||
           Status == STATUS_ACCESS_DENIED || Status == STATUS_INVALID_INFO_CLASS ||
           Status == STATUS_INVALID_PARAMETER;
}

static BOOLEAN NgDiagIsWrite(PIO_STACK_LOCATION Stack, NTSTATUS Status)
{
    UCHAR Major = Stack->MajorFunction;
    /* An FSCTL that requires FILE_WRITE_ACCESS modifies the file or the volume. */
    if (Major == IRP_MJ_FILE_SYSTEM_CONTROL &&
        (Stack->MinorFunction == IRP_MN_USER_FS_REQUEST || Stack->MinorFunction == IRP_MN_KERNEL_CALL) &&
        ((Stack->Parameters.FileSystemControl.FsControlCode >> 14) & FILE_WRITE_ACCESS))
        return TRUE;
    return Status == STATUS_MEDIA_WRITE_PROTECTED || Major == IRP_MJ_WRITE || Major == IRP_MJ_SET_INFORMATION ||
           Major == IRP_MJ_SET_EA || Major == IRP_MJ_SET_VOLUME_INFORMATION || Major == IRP_MJ_SET_SECURITY ||
           Major == IRP_MJ_FLUSH_BUFFERS;
}

/* Path of an open file object on one of our volumes, or an empty string. */
static WCHAR NgDiagEmpty[1];

static VOID NgDiagPath(PDEVICE_OBJECT DeviceObject, PFILE_OBJECT FileObject, PUNICODE_STRING Path)
{
    RtlInitEmptyUnicodeString(Path, NgDiagEmpty, 0);
    if (DeviceObject == NgGlobal.ControlDevice || !FileObject || !FileObject->FsContext2)
        return;
    *Path = ((PNG_CCB)FileObject->FsContext2)->Path;
}

static VOID NgDiagSetInfo(PIO_STACK_LOCATION Stack, PIRP Irp, PCHAR Out, SIZE_T Size, PUNICODE_STRING Target)
{
    FILE_INFORMATION_CLASS Class = Stack->Parameters.SetFile.FileInformationClass;
    PVOID Buf = Irp->AssociatedIrp.SystemBuffer;
    ULONG Len = Stack->Parameters.SetFile.Length;

    switch (Class)
    {
        case FileEndOfFileInformation:
        case FileAllocationInformation:
        case FileValidDataLengthInformation:
            if (Buf && Len >= sizeof(LARGE_INTEGER))
                _snprintf(Out, Size, " value=0x%I64x%s", ((PLARGE_INTEGER)Buf)->QuadPart,
                          (Class == FileEndOfFileInformation && Stack->Parameters.SetFile.AdvanceOnly) ? " advance-only" : "");
            break;
        case FileDispositionInformation:
            if (Buf && Len >= sizeof(FILE_DISPOSITION_INFORMATION))
                _snprintf(Out, Size, " delete=%u", ((PFILE_DISPOSITION_INFORMATION)Buf)->DeleteFile);
            break;
        case FileBasicInformation:
            if (Buf && Len >= sizeof(FILE_BASIC_INFORMATION))
            {
                PFILE_BASIC_INFORMATION B = Buf;
                _snprintf(Out, Size, " attr=0x%lx times=%s%s%s%s", B->FileAttributes,
                          B->CreationTime.QuadPart ? "C" : "-", B->LastAccessTime.QuadPart ? "A" : "-",
                          B->LastWriteTime.QuadPart ? "W" : "-", B->ChangeTime.QuadPart ? "H" : "-");
            }
            break;
        case FileRenameInformation:
        case FileLinkInformation:
            if (Buf && Len >= FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName))
            {
                PFILE_RENAME_INFORMATION R = Buf;
                Target->Buffer = R->FileName;
                Target->Length = Target->MaximumLength =
                    (USHORT)min(R->FileNameLength, Len - FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName));
                _snprintf(Out, Size, " replace=%u root=%p", R->ReplaceIfExists, R->RootDirectory);
            }
            break;
        default:
            break;
    }
}

VOID NgDiagLogRequest(PDEVICE_OBJECT DeviceObject, PIRP Irp, NTSTATUS Status)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    UCHAR Major = Stack->MajorFunction, Minor = Stack->MinorFunction;
    PFILE_OBJECT FileObject = Stack->FileObject;
    ULONG Ms = (ULONG)(KeQueryInterruptTime() / 10000);
    CHAR Detail[200];
    UNICODE_STRING Path, Target;
    LONG Seq;
    BOOLEAN Write;

    if (!NgDiagIsRefusal(Stack, Status))
        return;
    Write = NgDiagIsWrite(Stack, Status) ||
            (Major == IRP_MJ_CREATE && (Stack->Flags & SL_OPEN_PAGING_FILE));
    Seq = InterlockedIncrement(&NgDiagSeq);
    InterlockedIncrement(&NgDiagByMajor[Major]);
    if (Write)
        InterlockedIncrement(&NgDiagWrite);
    if (Seq % NG_DIAG_SUMMARY == 0)
    {
        DbgPrint("NGDIAG summary: %ld events, %ld write-path; create %ld write %ld setinfo %ld flush %ld fsctl %ld dirctl %ld "
                 "queryinfo %ld queryvol %ld lock %ld security %ld other\n",
                 Seq, NgDiagWrite, NgDiagByMajor[IRP_MJ_CREATE], NgDiagByMajor[IRP_MJ_WRITE],
                 NgDiagByMajor[IRP_MJ_SET_INFORMATION], NgDiagByMajor[IRP_MJ_FLUSH_BUFFERS],
                 NgDiagByMajor[IRP_MJ_FILE_SYSTEM_CONTROL], NgDiagByMajor[IRP_MJ_DIRECTORY_CONTROL],
                 NgDiagByMajor[IRP_MJ_QUERY_INFORMATION], NgDiagByMajor[IRP_MJ_QUERY_VOLUME_INFORMATION],
                 NgDiagByMajor[IRP_MJ_LOCK_CONTROL],
                 NgDiagByMajor[IRP_MJ_QUERY_SECURITY] + NgDiagByMajor[IRP_MJ_SET_SECURITY]);
    }
    if (Seq > NG_DIAG_FULL && Seq % NG_DIAG_EVERY != 0)
        return;

    Detail[0] = 0;
    if (Major == IRP_MJ_CREATE)
    {
        ULONG Disp = (Stack->Parameters.Create.Options >> 24) & 0xff;
        PFILE_OBJECT Related = FileObject ? FileObject->RelatedFileObject : NULL;
        UNICODE_STRING RelPath;
        NgDiagPath(DeviceObject, Related, &RelPath);
        DbgPrint("NGDIAG #%ld t=%lu.%03lus %c CREATE st=%08lx disp=%s acc=%08lx opt=%06lx share=%lx attr=%lx%s rel=%wZ name=%wZ\n",
                 Seq, Ms / 1000, Ms % 1000, NT_SUCCESS(Status) ? 'I' : (Write ? 'W' : 'U'), Status,
                 Disp < RTL_NUMBER_OF(NgDispNames) ? NgDispNames[Disp] : "?",
                 Stack->Parameters.Create.SecurityContext->DesiredAccess,
                 Stack->Parameters.Create.Options & 0xffffff, (ULONG)Stack->Parameters.Create.ShareAccess,
                 (ULONG)Stack->Parameters.Create.FileAttributes,
                 (Stack->Flags & SL_OPEN_PAGING_FILE) ? " PAGINGFILE" : "",
                 &RelPath, FileObject ? &FileObject->FileName : &RelPath);
        return;
    }

    NgDiagPath(DeviceObject, FileObject, &Path);
    RtlInitEmptyUnicodeString(&Target, NgDiagEmpty, 0);
    switch (Major)
    {
        case IRP_MJ_WRITE:
            _snprintf(Detail, sizeof(Detail), " off=0x%I64x len=0x%lx%s%s%s",
                      Stack->Parameters.Write.ByteOffset.QuadPart, Stack->Parameters.Write.Length,
                      (Irp->Flags & IRP_PAGING_IO) ? " paging" : "", (Irp->Flags & IRP_NOCACHE) ? " nocache" : "",
                      (Irp->Flags & IRP_SYNCHRONOUS_PAGING_IO) ? " syncpaging" : "");
            break;
        case IRP_MJ_READ:
            _snprintf(Detail, sizeof(Detail), " off=0x%I64x len=0x%lx", Stack->Parameters.Read.ByteOffset.QuadPart,
                      Stack->Parameters.Read.Length);
            break;
        case IRP_MJ_SET_INFORMATION:
        {
            CHAR Extra[160];
            Extra[0] = 0;
            NgDiagSetInfo(Stack, Irp, Extra, sizeof(Extra), &Target);
            _snprintf(Detail, sizeof(Detail), " class=%u len=%lu%s",
                      Stack->Parameters.SetFile.FileInformationClass, Stack->Parameters.SetFile.Length, Extra);
            break;
        }
        case IRP_MJ_QUERY_INFORMATION:
            _snprintf(Detail, sizeof(Detail), " class=%u", Stack->Parameters.QueryFile.FileInformationClass);
            break;
        case IRP_MJ_QUERY_VOLUME_INFORMATION:
        case IRP_MJ_SET_VOLUME_INFORMATION:
            _snprintf(Detail, sizeof(Detail), " class=%u", Stack->Parameters.QueryVolume.FsInformationClass);
            break;
        case IRP_MJ_FILE_SYSTEM_CONTROL:
        {
            ULONG Code = Stack->Parameters.FileSystemControl.FsControlCode;
            _snprintf(Detail, sizeof(Detail), " fsctl=0x%08lx (dev 0x%lx func %lu method %lu) in=%lu out=%lu",
                      Code, Code >> 16, (Code >> 2) & 0xfff, Code & 3,
                      Stack->Parameters.FileSystemControl.InputBufferLength,
                      Stack->Parameters.FileSystemControl.OutputBufferLength);
            break;
        }
        case IRP_MJ_DIRECTORY_CONTROL:
            if (Minor == IRP_MN_NOTIFY_CHANGE_DIRECTORY)
                _snprintf(Detail, sizeof(Detail), " notify filter=0x%lx tree=%u",
                          Stack->Parameters.NotifyDirectory.CompletionFilter, !!(Stack->Flags & SL_WATCH_TREE));
            break;
        case IRP_MJ_LOCK_CONTROL:
            _snprintf(Detail, sizeof(Detail), " off=0x%I64x len=0x%I64x",
                      Stack->Parameters.LockControl.ByteOffset.QuadPart,
                      Stack->Parameters.LockControl.Length ? Stack->Parameters.LockControl.Length->QuadPart : 0);
            break;
        case IRP_MJ_QUERY_SECURITY:
            _snprintf(Detail, sizeof(Detail), " info=0x%lx", Stack->Parameters.QuerySecurity.SecurityInformation);
            break;
        case IRP_MJ_SET_SECURITY:
            _snprintf(Detail, sizeof(Detail), " info=0x%lx", Stack->Parameters.SetSecurity.SecurityInformation);
            break;
        default:
            break;
    }
    Detail[sizeof(Detail) - 1] = 0;
    DbgPrint("NGDIAG #%ld t=%lu.%03lus %c %s mn=%u st=%08lx%s%s path=%wZ%s%wZ\n",
             Seq, Ms / 1000, Ms % 1000, Write ? 'W' : 'U', NgMajorNames[Major], Minor, Status, Detail,
             DeviceObject == NgGlobal.ControlDevice ? " (control device)" : "", &Path,
             Target.Length ? " target=" : "", &Target);
}
