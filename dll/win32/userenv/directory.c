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

#include <winioctl.h>

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

            if (FindFileData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                /*
                 * Do not follow the legacy profile junctions (see
                 * CreateLegacyProfileJunctions()): they point back into the
                 * profile and cannot be listed. The ones of the new profile
                 * are created by the caller.
                 */
                DPRINT("Skip reparse point: %S\n", szFullSrcName);
            }
            else if (FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
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


static
BOOL
RecursiveRemoveDir(LPCWSTR lpPath)
{
    WCHAR szPath[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE hFind;
    BOOL bResult;

    wcscpy(szPath, lpPath);
    wcscat(szPath, L"\\*.*");
    DPRINT("Search path: '%S'\n", szPath);

    hFind = FindFirstFileW(szPath,
                           &FindData);
    if (hFind == INVALID_HANDLE_VALUE)
        return FALSE;

    bResult = TRUE;
    while (TRUE)
    {
        if (wcscmp(FindData.cFileName, L".") &&
            wcscmp(FindData.cFileName, L".."))
        {
            wcscpy(szPath, lpPath);
            wcscat(szPath, L"\\");
            wcscat(szPath, FindData.cFileName);
            DPRINT("File name: '%S'\n", szPath);

            if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                DPRINT("Delete directory: '%S'\n", szPath);

                /* Remove a junction itself, never the files it points to */
                if (!(FindData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    !RecursiveRemoveDir(szPath))
                {
                    bResult = FALSE;
                    break;
                }

                if (FindData.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                {
                    SetFileAttributesW(szPath,
                                       FindData.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                }

                if (!RemoveDirectoryW(szPath))
                {
                    bResult = FALSE;
                    break;
                }
            }
            else
            {
                DPRINT("Delete file: '%S'\n", szPath);

                if (FindData.dwFileAttributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM))
                {
                    SetFileAttributesW(szPath,
                                       FILE_ATTRIBUTE_NORMAL);
                }

                if (!DeleteFileW(szPath))
                {
                    bResult = FALSE;
                    break;
                }
            }
        }

        if (!FindNextFileW(hFind, &FindData))
        {
            if (GetLastError() != ERROR_NO_MORE_FILES)
            {
                DPRINT1("Error: %lu\n", GetLastError());
                bResult = FALSE;
                break;
            }

            break;
        }
    }

    FindClose(hFind);

    return bResult;
}


BOOL
RemoveDirectoryPath(LPCWSTR lpPathName)
{
    DWORD dwAttributes = GetFileAttributesW(lpPathName);

    /* A junction is removed itself, never the files it points to */
    if (dwAttributes != INVALID_FILE_ATTRIBUTES &&
        (dwAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return RemoveDirectoryW(lpPathName);

    if (!RecursiveRemoveDir(lpPathName))
        return FALSE;

    DPRINT("Delete directory: '%S'\n", lpPathName);
    return RemoveDirectoryW(lpPathName);
}


/*
 * Legacy profile junctions
 *
 * Since Windows Vista the profiles live in "Users" and the old, hardcoded
 * NT 5 locations ("Documents and Settings\<user>\Application Data" etc.) are
 * NTFS directory junctions to the new ones. They are hidden system
 * directories that deny listing to Everyone, so that enumerating a profile
 * does not walk into them twice (or forever, for "Local\Application Data").
 */

typedef struct _JUNCTIONDATA
{
    PCWSTR pszLink;     /* Relative to the link base */
    PCWSTR pszTarget;   /* Relative to the target base, "" for the base itself */
} JUNCTIONDATA, *PJUNCTIONDATA;

static const JUNCTIONDATA
ProfileJunctions[] =
{
    {L"Application Data", L"AppData\\Roaming"},
    {L"Cookies", L"AppData\\Local\\Microsoft\\Windows\\INetCookies"},
    {L"Local Settings", L"AppData\\Local"},
    {L"My Documents", L"Documents"},
    {L"NetHood", L"AppData\\Roaming\\Microsoft\\Windows\\Network Shortcuts"},
    {L"PrintHood", L"AppData\\Roaming\\Microsoft\\Windows\\Printer Shortcuts"},
    {L"Recent", L"AppData\\Roaming\\Microsoft\\Windows\\Recent"},
    {L"SendTo", L"AppData\\Roaming\\Microsoft\\Windows\\SendTo"},
    {L"Start Menu", L"AppData\\Roaming\\Microsoft\\Windows\\Start Menu"},
    /* No "Templates": the Templates folder of a profile is a real directory here */
    {L"AppData\\Local\\Application Data", L"AppData\\Local"},
    {L"AppData\\Local\\History", L"AppData\\Local\\Microsoft\\Windows\\History"},
    {L"AppData\\Local\\Temporary Internet Files", L"AppData\\Local\\Microsoft\\Windows\\INetCache"},
};

static const JUNCTIONDATA
ProgramDataJunctions[] =
{
    {L"Application Data", L""},
    {L"Start Menu", L"Microsoft\\Windows\\Start Menu"},
    {L"Templates", L"Microsoft\\Windows\\Templates"},
};

static const JUNCTIONDATA
PublicJunctions[] =
{
    {L"Desktop", L"Desktop"},
    {L"Documents", L"Documents"},
};

/*
 * Deny Everyone the listing of the directory, keep the rest of its DACL. Windows also denies
 * SYNCHRONIZE here; an open of the link itself (which asks for it) is not refused for that on
 * Windows, but is by our NTFS, so only the listing is denied.
 */
static BOOL
DenyListingToEveryone(
    _In_ HANDLE hDirectory)
{
    SID_IDENTIFIER_AUTHORITY WorldSidAuthority = {SECURITY_WORLD_SID_AUTHORITY};
    PSECURITY_DESCRIPTOR pOldSd = NULL;
    SECURITY_DESCRIPTOR NewSd;
    PACL pOldAcl, pNewAcl = NULL;
    PSID pWorldSid = NULL;
    PVOID pAce;
    DWORD dwSize = 0, dwAclSize;
    BOOL bPresent, bDefaulted, bResult = FALSE;
    BYTE bRevision = ACL_REVISION;
    ULONG i;

    if (!AllocateAndInitializeSid(&WorldSidAuthority, 1, SECURITY_WORLD_RID,
                                  0, 0, 0, 0, 0, 0, 0, &pWorldSid))
        return FALSE;

    GetKernelObjectSecurity(hDirectory, DACL_SECURITY_INFORMATION, NULL, 0, &dwSize);
    pOldSd = HeapAlloc(GetProcessHeap(), 0, dwSize);
    if (!pOldSd ||
        !GetKernelObjectSecurity(hDirectory, DACL_SECURITY_INFORMATION, pOldSd, dwSize, &dwSize) ||
        !GetSecurityDescriptorDacl(pOldSd, &bPresent, &pOldAcl, &bDefaulted))
        goto done;

    /* A missing DACL grants everything, keep it that way for the others */
    dwAclSize = sizeof(ACL) +
                2 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(pWorldSid));
    if (bPresent && pOldAcl)
    {
        dwAclSize += pOldAcl->AclSize - sizeof(ACL);
        bRevision = max(bRevision, pOldAcl->AclRevision);
    }

    pNewAcl = HeapAlloc(GetProcessHeap(), 0, dwAclSize);
    if (!pNewAcl ||
        !InitializeAcl(pNewAcl, dwAclSize, bRevision) ||
        !AddAccessDeniedAce(pNewAcl, bRevision, FILE_LIST_DIRECTORY, pWorldSid))
        goto done;

    if (bPresent && pOldAcl)
    {
        for (i = 0; i < pOldAcl->AceCount; i++)
        {
            if (!GetAce(pOldAcl, i, &pAce) ||
                !AddAce(pNewAcl, bRevision, MAXDWORD, pAce, ((PACE_HEADER)pAce)->AceSize))
                goto done;
        }
    }
    else if (!AddAccessAllowedAce(pNewAcl, bRevision, GENERIC_ALL, pWorldSid))
    {
        goto done;
    }

    if (!InitializeSecurityDescriptor(&NewSd, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&NewSd, TRUE, pNewAcl, FALSE))
        goto done;

    bResult = SetKernelObjectSecurity(hDirectory, DACL_SECURITY_INFORMATION, &NewSd);

done:
    if (pNewAcl) HeapFree(GetProcessHeap(), 0, pNewAcl);
    if (pOldSd) HeapFree(GetProcessHeap(), 0, pOldSd);
    if (pWorldSid) FreeSid(pWorldSid);
    return bResult;
}

/* There is no API for creating directory junctions (see also cmd's mklink) */
static BOOL
CreateDirectoryJunction(
    _In_ PCWSTR pszLink,
    _In_ PCWSTR pszTarget)
{
    typedef struct _MOUNTPOINT_REPARSE
    {
        ULONG ReparseTag;
        USHORT ReparseDataLength;
        USHORT Reserved;
        USHORT SubstituteNameOffset;
        USHORT SubstituteNameLength;
        USHORT PrintNameOffset;
        USHORT PrintNameLength;
        WCHAR PathBuffer[ANYSIZE_ARRAY];
    } MOUNTPOINT_REPARSE, *PMOUNTPOINT_REPARSE;

    PMOUNTPOINT_REPARSE pData;
    HANDLE hLink;
    DWORD dwSize, dwError;
    USHORT cbNtTarget, cbTarget;
    BOOL bResult = FALSE;

    /* \??\<target> is the substitute name, <target> the print name */
    cbTarget = (USHORT)(wcslen(pszTarget) * sizeof(WCHAR));
    cbNtTarget = cbTarget + 4 * sizeof(WCHAR);
    dwSize = FIELD_OFFSET(MOUNTPOINT_REPARSE, PathBuffer) +
             cbNtTarget + cbTarget + 2 * sizeof(WCHAR);

    pData = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwSize);
    if (!pData)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    pData->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    pData->ReparseDataLength = (USHORT)(dwSize - FIELD_OFFSET(MOUNTPOINT_REPARSE, SubstituteNameOffset));
    pData->SubstituteNameLength = cbNtTarget;
    pData->PrintNameOffset = cbNtTarget + sizeof(WCHAR);
    pData->PrintNameLength = cbTarget;
    wcscpy(pData->PathBuffer, L"\\??\\");
    wcscat(pData->PathBuffer, pszTarget);
    wcscpy(&pData->PathBuffer[pData->PrintNameOffset / sizeof(WCHAR)], pszTarget);

    /* The junction starts as an empty directory */
    if (!CreateDirectoryW(pszLink, NULL))
    {
        HeapFree(GetProcessHeap(), 0, pData);
        return FALSE;
    }

    /* Hidden and system like on Windows, set while it is still a plain directory */
    SetFileAttributesW(pszLink, FILE_ATTRIBUTE_HIDDEN |
                                FILE_ATTRIBUTE_SYSTEM |
                                FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);

    hLink = CreateFileW(pszLink,
                        GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
                        0,
                        NULL,
                        OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                        NULL);
    if (hLink != INVALID_HANDLE_VALUE)
    {
        bResult = DeviceIoControl(hLink, FSCTL_SET_REPARSE_POINT,
                                  pData, dwSize, NULL, 0, &dwSize, NULL) &&
                  DenyListingToEveryone(hLink);
        dwError = GetLastError();
        CloseHandle(hLink);
    }
    else
    {
        dwError = GetLastError();
    }

    HeapFree(GetProcessHeap(), 0, pData);

    if (!bResult)
    {
        RemoveDirectoryW(pszLink);
        SetLastError(dwError);
    }

    return bResult;
}

/* Only NTFS has junctions (the NTFS drivers do not report FILE_SUPPORTS_REPARSE_POINTS) */
static BOOL
VolumeHasJunctions(
    _In_ PCWSTR pszPath)
{
    WCHAR szRoot[MAX_PATH];
    WCHAR szFileSystem[MAX_PATH];

    return GetVolumePathNameW(pszPath, szRoot, ARRAYSIZE(szRoot)) &&
           GetVolumeInformationW(szRoot, NULL, 0, NULL, NULL, NULL,
                                 szFileSystem, ARRAYSIZE(szFileSystem)) &&
           !_wcsicmp(szFileSystem, L"NTFS");
}

static VOID
CreateJunctions(
    _In_ PCWSTR pszLinkBase,
    _In_ PCWSTR pszTargetBase,
    _In_ const JUNCTIONDATA *pJunctions,
    _In_ ULONG ulCount)
{
    WCHAR szLink[MAX_PATH];
    WCHAR szTarget[MAX_PATH];
    ULONG i;

    for (i = 0; i < ulCount; i++)
    {
        if (FAILED(StringCchPrintfW(szLink, ARRAYSIZE(szLink), L"%s\\%s",
                                    pszLinkBase, pJunctions[i].pszLink)))
            continue;

        if (pJunctions[i].pszTarget[0])
        {
            if (FAILED(StringCchPrintfW(szTarget, ARRAYSIZE(szTarget), L"%s\\%s",
                                        pszTargetBase, pJunctions[i].pszTarget)))
                continue;
        }
        else if (FAILED(StringCchCopyW(szTarget, ARRAYSIZE(szTarget), pszTargetBase)))
        {
            continue;
        }

        /* An existing directory or junction is left alone */
        if (!CreateDirectoryJunction(szLink, szTarget) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
        {
            DPRINT1("Failed to create the junction %S -> %S, error: %lu\n",
                    szLink, szTarget, GetLastError());
        }
    }
}

/*
 * Create the junctions from the NT 5 profile paths of the profile
 * (Default or a user) to the Vista ones. Not on file systems without
 * junctions (FAT).
 */
VOID
CreateLegacyProfileJunctions(
    _In_ PCWSTR pszProfilePath)
{
    if (!VolumeHasJunctions(pszProfilePath))
        return;

    CreateJunctions(pszProfilePath, pszProfilePath,
                    ProfileJunctions, ARRAYSIZE(ProfileJunctions));
}

/*
 * Create the junctions in ProgramData to the folders that were in
 * "All Users" in NT 5, and "Documents and Settings" to the profiles
 * directory.
 */
VOID
CreateLegacyProgramDataJunctions(
    _In_ PCWSTR pszProgramDataPath,
    _In_ PCWSTR pszPublicPath,
    _In_ PCWSTR pszProfilesPath)
{
    static const JUNCTIONDATA UsersJunction = {L"Documents and Settings", L""};
    WCHAR szSystemDrive[MAX_PATH];

    if (!VolumeHasJunctions(pszProgramDataPath))
        return;

    CreateJunctions(pszProgramDataPath, pszProgramDataPath,
                    ProgramDataJunctions, ARRAYSIZE(ProgramDataJunctions));
    CreateJunctions(pszProgramDataPath, pszPublicPath,
                    PublicJunctions, ARRAYSIZE(PublicJunctions));

    if (ExpandEnvironmentStringsW(L"%SystemDrive%", szSystemDrive, ARRAYSIZE(szSystemDrive)))
        CreateJunctions(szSystemDrive, pszProfilesPath, &UsersJunction, 1);
}

/* EOF */
