/*
 *  ReactOS kernel
 *  Copyright (C) 2004 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/userenv/directory.c
 * PURPOSE:         User profile code
 * PROGRAMMER:      Eric Kohl
 */

#include "precomp.h"

#include <ndk/iofuncs.h>
#include <ndk/obfuncs.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ***************************************************************/

BOOL
WINAPI
CopyProfileDirectoryA(LPCSTR lpSourcePath,
                      LPCSTR lpDestinationPath,
                      DWORD dwFlags)
{
    UNICODE_STRING SrcPath;
    UNICODE_STRING DstPath;
    BOOL bResult;

    if (!RtlCreateUnicodeStringFromAsciiz(&SrcPath,
                                          (LPSTR)lpSourcePath))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    if (!RtlCreateUnicodeStringFromAsciiz(&DstPath,
                                          (LPSTR)lpDestinationPath))
    {
        RtlFreeUnicodeString(&SrcPath);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    bResult = CopyProfileDirectoryW(SrcPath.Buffer,
                                    DstPath.Buffer,
                                    dwFlags);

    RtlFreeUnicodeString(&DstPath);
    RtlFreeUnicodeString(&SrcPath);

    return bResult;
}


BOOL
WINAPI
CopyProfileDirectoryW(LPCWSTR lpSourcePath,
                      LPCWSTR lpDestinationPath,
                      DWORD dwFlags)
{
    /* FIXME: dwFlags are ignored! */
    return CopyDirectory(lpDestinationPath, lpSourcePath);
}


BOOL
CopyDirectory(LPCWSTR lpDestinationPath,
              LPCWSTR lpSourcePath)
{
    WCHAR szFileName[MAX_PATH];
    WCHAR szFullSrcName[MAX_PATH];
    WCHAR szFullDstName[MAX_PATH];
    WIN32_FIND_DATAW FindFileData;
    LPWSTR lpSrcPtr;
    LPWSTR lpDstPtr;
    HANDLE hFind;

    DPRINT("CopyDirectory (%S, %S) called\n",
            lpDestinationPath, lpSourcePath);

    wcscpy(szFileName, lpSourcePath);
    wcscat(szFileName, L"\\*.*");

    hFind = FindFirstFileW(szFileName,
                           &FindFileData);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        DPRINT1("Error: %lu\n", GetLastError());
        return FALSE;
    }

    wcscpy(szFullSrcName, lpSourcePath);
    lpSrcPtr = AppendBackslash(szFullSrcName);

    wcscpy(szFullDstName, lpDestinationPath);
    lpDstPtr = AppendBackslash(szFullDstName);

    for (;;)
    {
        if (wcscmp(FindFileData.cFileName, L".") &&
            wcscmp(FindFileData.cFileName, L".."))
        {
            wcscpy(lpSrcPtr, FindFileData.cFileName);
            wcscpy(lpDstPtr, FindFileData.cFileName);

            if (FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                DPRINT("Create directory: %S\n", szFullDstName);
                if (!CreateDirectoryExW(szFullSrcName, szFullDstName, NULL))
                {
                    if (GetLastError() != ERROR_ALREADY_EXISTS)
                    {
                        DPRINT1("Error: %lu\n", GetLastError());

                        FindClose(hFind);
                        return FALSE;
                    }
                }

                if (!CopyDirectory(szFullDstName, szFullSrcName))
                {
                    DPRINT1("Error: %lu\n", GetLastError());

                    FindClose(hFind);
                    return FALSE;
                }
            }
            else
            {
                DPRINT("Copy file: %S -> %S\n", szFullSrcName, szFullDstName);
                if (!CopyFileW(szFullSrcName, szFullDstName, FALSE))
                {
                    DPRINT1("Error: %lu\n", GetLastError());

                    FindClose(hFind);
                    return FALSE;
                }
            }
        }

        if (!FindNextFileW(hFind, &FindFileData))
        {
            if (GetLastError() != ERROR_NO_MORE_FILES)
            {
                DPRINT1("Error: %lu\n", GetLastError());
            }

            break;
        }
    }

    FindClose(hFind);

    DPRINT("CopyDirectory() done\n");

    return TRUE;
}


BOOL
CreateDirectoryPath(LPCWSTR lpPathName,
                    LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    WCHAR szPath[MAX_PATH];
    LPWSTR Ptr;
    DWORD dwError;

    DPRINT("CreateDirectoryPath() called\n");

    if (lpPathName == NULL || *lpPathName == 0)
        return TRUE;

    if (CreateDirectoryW(lpPathName,
                         lpSecurityAttributes))
        return TRUE;

    dwError = GetLastError();
    if (dwError == ERROR_ALREADY_EXISTS)
        return TRUE;

    wcscpy(szPath, lpPathName);

    if (wcslen(szPath) > 3 && szPath[1] == ':' && szPath[2] == '\\')
    {
        Ptr = &szPath[3];
    }
    else
    {
        Ptr = szPath;
    }

    while (Ptr != NULL)
    {
        Ptr = wcschr(Ptr, L'\\');
        if (Ptr != NULL)
            *Ptr = 0;

        DPRINT("CreateDirectory(%S)\n", szPath);
        if (!CreateDirectoryW(szPath,
                              lpSecurityAttributes))
        {
            dwError = GetLastError();
            if (dwError != ERROR_ALREADY_EXISTS)
                return FALSE;
        }

        if (Ptr != NULL)
        {
            *Ptr = L'\\';
            Ptr++;
        }
    }

    DPRINT("CreateDirectoryPath() done\n");

    return TRUE;
}


/*
 * Remove a directory tree without ever following a reparse point. Every entry
 * is opened relative to the handle of its parent directory, without following
 * reparse points, and what happens to it is decided on the attributes of that
 * same handle: a real directory is emptied through it, and every entry is
 * deleted through its handle. Nothing is reopened by name, so an entry that
 * is replaced by a junction while the tree is removed cannot redirect the
 * removal into the target of the junction.
 */
#define REMOVE_TREE_MAX_DEPTH 512

static
NTSTATUS
OpenTreeEntry(
    _Out_ PHANDLE phEntry,
    _In_opt_ HANDLE hParent,
    _In_ PUNICODE_STRING Name,
    _Out_ PBOOLEAN pbCanList)
{
    static const ACCESS_MASK DesiredAccess[] =
    {
        DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_WRITE_ATTRIBUTES,
        DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
        DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES,
    };
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;
    ULONG i;

    InitializeObjectAttributes(&ObjectAttributes,
                               Name,
                               OBJ_CASE_INSENSITIVE,
                               hParent,
                               NULL);

    /*
     * Listing is needed only to empty a real directory and writing the attributes
     * only to clear the read-only one: a junction may deny listing, an ACL either.
     */
    for (i = 0; i < ARRAYSIZE(DesiredAccess); i++)
    {
        Status = NtOpenFile(phEntry,
                            DesiredAccess[i],
                            &ObjectAttributes,
                            &IoStatusBlock,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT |
                            FILE_SYNCHRONOUS_IO_NONALERT);
        if (Status != STATUS_ACCESS_DENIED)
            break;
    }
    *pbCanList = NT_SUCCESS(Status) && (DesiredAccess[i] & FILE_LIST_DIRECTORY);

    return Status;
}

static
NTSTATUS
DeleteTreeEntry(
    _In_ HANDLE hEntry,
    _In_ BOOLEAN bCanList,
    _In_ ULONG ulDepth);

static
NTSTATUS
DeleteTreeContents(
    _In_ HANDLE hDirectory,
    _In_ ULONG ulDepth)
{
    const ULONG BufferSize = 16 * 1024;
    PFILE_DIRECTORY_INFORMATION pBuffer, pInfo;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING Name;
    HANDLE hEntry;
    BOOLEAN bCanList, bRestart = TRUE, bDeleted;
    NTSTATUS Status;

    if (ulDepth > REMOVE_TREE_MAX_DEPTH)
        return STATUS_NAME_TOO_LONG;

    pBuffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, BufferSize);
    if (pBuffer == NULL)
        return STATUS_NO_MEMORY;

    for (;;)
    {
        Status = NtQueryDirectoryFile(hDirectory,
                                      NULL,
                                      NULL,
                                      NULL,
                                      &IoStatusBlock,
                                      pBuffer,
                                      BufferSize,
                                      FileDirectoryInformation,
                                      FALSE,
                                      NULL,
                                      bRestart);
        if (Status == STATUS_NO_MORE_FILES)
        {
            Status = STATUS_SUCCESS;
            break;
        }
        if (!NT_SUCCESS(Status))
            break;

        /*
         * Deleting entries can move the ones still to come before the position of
         * the enumeration, so start again after a batch that deleted something.
         * An entry already pending deletion is skipped, so this ends.
         */
        bDeleted = FALSE;
        for (pInfo = pBuffer; ; pInfo = (PVOID)((ULONG_PTR)pInfo + pInfo->NextEntryOffset))
        {
            Name.Buffer = pInfo->FileName;
            Name.Length = Name.MaximumLength = (USHORT)pInfo->FileNameLength;

            if (!(Name.Length == sizeof(WCHAR) && Name.Buffer[0] == L'.') &&
                !(Name.Length == 2 * sizeof(WCHAR) && Name.Buffer[0] == L'.' && Name.Buffer[1] == L'.'))
            {
                DPRINT("Delete: '%wZ'\n", &Name);

                Status = OpenTreeEntry(&hEntry, hDirectory, &Name, &bCanList);
                if (NT_SUCCESS(Status))
                {
                    Status = DeleteTreeEntry(hEntry, bCanList, ulDepth + 1);
                    NtClose(hEntry);
                    bDeleted = bDeleted || NT_SUCCESS(Status);
                }
                else if (Status == STATUS_DELETE_PENDING)
                {
                    Status = STATUS_SUCCESS;
                }
                if (!NT_SUCCESS(Status))
                {
                    DPRINT1("Cannot delete '%wZ': 0x%lx\n", &Name, Status);
                    goto done;
                }
            }

            if (pInfo->NextEntryOffset == 0)
                break;
        }

        bRestart = bDeleted;
    }

done:
    RtlFreeHeap(RtlGetProcessHeap(), 0, pBuffer);
    return Status;
}

static
NTSTATUS
DeleteTreeEntry(
    _In_ HANDLE hEntry,
    _In_ BOOLEAN bCanList,
    _In_ ULONG ulDepth)
{
    FILE_BASIC_INFORMATION BasicInfo;
    FILE_DISPOSITION_INFORMATION Disposition;
    IO_STATUS_BLOCK IoStatusBlock;
    ULONG ulAttributes;
    NTSTATUS Status;

    /* The attributes of this very handle (FAT has no FileAttributeTagInformation) */
    Status = NtQueryInformationFile(hEntry,
                                    &IoStatusBlock,
                                    &BasicInfo,
                                    sizeof(BasicInfo),
                                    FileBasicInformation);
    if (!NT_SUCCESS(Status))
        return Status;
    ulAttributes = BasicInfo.FileAttributes;

    /* The tree to remove must be a directory (or a junction) */
    if (ulDepth == 0 && !(ulAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return STATUS_NOT_A_DIRECTORY;

    /* Empty a real directory; a junction is removed itself, never the files it points to */
    if ((ulAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(ulAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        if (!bCanList)
            return STATUS_ACCESS_DENIED;

        Status = DeleteTreeContents(hEntry, ulDepth);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    if (ulAttributes & FILE_ATTRIBUTE_READONLY)
    {
        ulAttributes &= ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT);
        RtlZeroMemory(&BasicInfo, sizeof(BasicInfo));
        BasicInfo.FileAttributes = ulAttributes ? ulAttributes : FILE_ATTRIBUTE_NORMAL;
        Status = NtSetInformationFile(hEntry,
                                      &IoStatusBlock,
                                      &BasicInfo,
                                      sizeof(BasicInfo),
                                      FileBasicInformation);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Disposition.DeleteFile = TRUE;
    return NtSetInformationFile(hEntry,
                                &IoStatusBlock,
                                &Disposition,
                                sizeof(Disposition),
                                FileDispositionInformation);
}


BOOL
RemoveDirectoryPath(LPCWSTR lpPathName)
{
    UNICODE_STRING NtPathName;
    HANDLE hDirectory;
    BOOLEAN bCanList;
    NTSTATUS Status;

    DPRINT("Delete directory: '%S'\n", lpPathName);

    if (!RtlDosPathNameToNtPathName_U(lpPathName, &NtPathName, NULL, NULL))
    {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }

    Status = OpenTreeEntry(&hDirectory, NULL, &NtPathName, &bCanList);
    RtlFreeHeap(RtlGetProcessHeap(), 0, NtPathName.Buffer);
    if (NT_SUCCESS(Status))
    {
        Status = DeleteTreeEntry(hDirectory, bCanList, 0);
        NtClose(hDirectory);
    }

    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}

/* EOF */
