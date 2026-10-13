/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     SetFileInformationByHandle
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *              Information class mapping as in Wine's kernelbase/file.c
 */

#include "k32_vista.h"

#include <ndk/rtlfuncs.h>
#include <ndk/iofuncs.h>

#define NDEBUG
#include <debug.h>

/* A name without a directory or drive part: a new name in the same
 * directory, or a stream name starting with a colon */
static
BOOL
IsSimpleFileName(
    _In_reads_(Length) PCWSTR Name,
    _In_ ULONG Length)
{
    ULONG i;

    if (Length >= 2 && Name[0] != L':' && Name[1] == L':')
        return FALSE;

    for (i = 0; i < Length; i++)
    {
        if (Name[i] == L'\\' || Name[i] == L'/')
            return FALSE;
    }

    return TRUE;
}

static
NTSTATUS
SetFileRenameInfo(
    _In_ HANDLE hFile,
    _In_reads_bytes_(Size) PFILE_RENAME_INFO Info,
    _In_ DWORD Size)
{
    PFILE_RENAME_INFORMATION RenameInfo;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING NtName;
    PWSTR DosName;
    ULONG NameChars, InfoSize;
    NTSTATUS Status;

    if (Size < FIELD_OFFSET(FILE_RENAME_INFO, FileName) ||
        Info->FileNameLength > Size - FIELD_OFFSET(FILE_RENAME_INFO, FileName))
    {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    /* The name is UTF-16: an odd byte count is invalid, as for NtSetInformationFile */
    if (Info->FileNameLength & 1)
        return STATUS_INVALID_PARAMETER;

    NameChars = Info->FileNameLength / sizeof(WCHAR);

    /* A plain name renames within the directory and a name relative to RootDirectory
     * is passed as is; any other path is a DOS path that needs an NT path */
    if (Info->RootDirectory || IsSimpleFileName(Info->FileName, NameChars))
    {
        return NtSetInformationFile(hFile, &IoStatusBlock, Info, Size, FileRenameInformation);
    }

    DosName = RtlAllocateHeap(RtlGetProcessHeap(), 0, (NameChars + 1) * sizeof(WCHAR));
    if (!DosName)
        return STATUS_NO_MEMORY;
    RtlCopyMemory(DosName, Info->FileName, NameChars * sizeof(WCHAR));
    DosName[NameChars] = UNICODE_NULL;

    Status = RtlDosPathNameToNtPathName_U_WithStatus(DosName, &NtName, NULL, NULL);
    RtlFreeHeap(RtlGetProcessHeap(), 0, DosName);
    if (!NT_SUCCESS(Status))
        return Status;

    InfoSize = FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) + NtName.Length;
    RenameInfo = RtlAllocateHeap(RtlGetProcessHeap(), 0, InfoSize);
    if (!RenameInfo)
    {
        RtlFreeUnicodeString(&NtName);
        return STATUS_NO_MEMORY;
    }

    RenameInfo->ReplaceIfExists = Info->ReplaceIfExists;
    RenameInfo->RootDirectory = NULL;
    RenameInfo->FileNameLength = NtName.Length;
    RtlCopyMemory(RenameInfo->FileName, NtName.Buffer, NtName.Length);

    Status = NtSetInformationFile(hFile, &IoStatusBlock, RenameInfo, InfoSize, FileRenameInformation);

    RtlFreeHeap(RtlGetProcessHeap(), 0, RenameInfo);
    RtlFreeUnicodeString(&NtName);
    return Status;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetFileInformationByHandle(
    _In_ HANDLE hFile,
    _In_ FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
    _In_reads_bytes_(dwBufferSize) LPVOID lpFileInformation,
    _In_ DWORD dwBufferSize)
{
    FILE_INFORMATION_CLASS NtClass;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    switch (FileInformationClass)
    {
        case FileBasicInfo:
            NtClass = FileBasicInformation;
            break;

        case FileDispositionInfo:
            NtClass = FileDispositionInformation;
            break;

        case FileAllocationInfo:
            NtClass = FileAllocationInformation;
            break;

        case FileEndOfFileInfo:
            NtClass = FileEndOfFileInformation;
            break;

        case FileIoPriorityHintInfo:
            /* Validated here; the I/O manager does not support this class yet,
             * so a valid hint still fails with ERROR_INVALID_PARAMETER */
            if (dwBufferSize < sizeof(FILE_IO_PRIORITY_HINT_INFO))
            {
                SetLastError(ERROR_BAD_LENGTH);
                return FALSE;
            }
            if ((ULONG)((FILE_IO_PRIORITY_HINT_INFO *)lpFileInformation)->PriorityHint >= MaximumIoPriorityHintType)
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            NtClass = FileIoPriorityHintInformation;
            break;

        case FileRenameInfo:
            Status = SetFileRenameInfo(hFile, lpFileInformation, dwBufferSize);
            goto Done;

        default:
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
    }

    Status = NtSetInformationFile(hFile, &IoStatusBlock, lpFileInformation, dwBufferSize, NtClass);

Done:
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}
