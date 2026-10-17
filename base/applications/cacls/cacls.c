/*
 * ReactOS Control ACLs Program
 * Copyright (C) 2006 Thomas Weidenmueller
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "precomp.h"

/* command line options */
BOOL OptionT = FALSE, OptionE = FALSE, OptionC = FALSE;
BOOL OptionG = FALSE, OptionR = FALSE, OptionP = FALSE, OptionD = FALSE;
LPCTSTR GUser, GPerm, RUser, PUser, PPerm, DUser;

static GENERIC_MAPPING FileGenericMapping =
{
    FILE_GENERIC_READ,
    FILE_GENERIC_WRITE,
    FILE_GENERIC_EXECUTE,
    FILE_ALL_ACCESS
};


static VOID
PrintError(DWORD dwError)
{
    if (dwError == ERROR_SUCCESS)
        return;

    ConMsgPuts(StdErr, FORMAT_MESSAGE_FROM_SYSTEM,
               NULL, dwError, LANG_USER_DEFAULT);
}

static BOOL
PrintFileDacl(IN LPTSTR FilePath,
              IN LPTSTR FileName)
{
    SIZE_T Length;
    PSECURITY_DESCRIPTOR SecurityDescriptor;
    DWORD SDSize = 0;
    TCHAR FullFileName[MAX_PATH + 1];
    BOOL Error = FALSE, Ret = FALSE;

    Length = _tcslen(FilePath) + _tcslen(FileName);
    if (Length > MAX_PATH)
    {
        /* file name too long */
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }

    _tcscpy(FullFileName, FilePath);
    _tcscat(FullFileName, FileName);

    /* find out how much memory we need */
    if (!GetFileSecurity(FullFileName,
                         DACL_SECURITY_INFORMATION,
                         NULL,
                         0,
                         &SDSize) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        return FALSE;
    }

    SecurityDescriptor = (PSECURITY_DESCRIPTOR)HeapAlloc(GetProcessHeap(),
                                                         0,
                                                         SDSize);
    if (SecurityDescriptor != NULL)
    {
        if (GetFileSecurity(FullFileName,
                            DACL_SECURITY_INFORMATION,
                            SecurityDescriptor,
                            SDSize,
                            &SDSize))
        {
            PACL Dacl;
            BOOL DaclPresent;
            BOOL DaclDefaulted;

            if (GetSecurityDescriptorDacl(SecurityDescriptor,
                                          &DaclPresent,
                                          &Dacl,
                                          &DaclDefaulted))
            {
                if (DaclPresent)
                {
                    PACCESS_ALLOWED_ACE Ace;
                    DWORD AceIndex = 0;

                    /* dump the ACL */
                    while (GetAce(Dacl,
                                  AceIndex,
                                  (PVOID*)&Ace))
                    {
                        SID_NAME_USE Use;
                        DWORD NameSize = 0;
                        DWORD DomainSize = 0;
                        LPTSTR Name = NULL;
                        LPTSTR Domain = NULL;
                        LPTSTR SidString = NULL;
                        size_t IndentAccess;
                        DWORD AccessMask = Ace->Mask;
                        PSID Sid = (PSID)&Ace->SidStart;

                        /* attempt to translate the SID into a readable string */
                        if (!LookupAccountSid(NULL,
                                              Sid,
                                              Name,
                                              &NameSize,
                                              Domain,
                                              &DomainSize,
                                              &Use))
                        {
                            if (GetLastError() == ERROR_NONE_MAPPED || NameSize == 0)
                            {
                                goto BuildSidString;
                            }
                            else
                            {
                                if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                                {
                                    Error = TRUE;
                                    break;
                                }

                                Name = (LPTSTR)HeapAlloc(GetProcessHeap(),
                                                         0,
                                                         (NameSize + DomainSize) * sizeof(TCHAR));
                                if (Name == NULL)
                                {
                                    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                                    Error = TRUE;
                                    break;
                                }

                                Domain = Name + NameSize;
                                Name[0] = _T('\0');
                                if (DomainSize != 0)
                                    Domain[0] = _T('\0');
                                if (!LookupAccountSid(NULL,
                                                      Sid,
                                                      Name,
                                                      &NameSize,
                                                      Domain,
                                                      &DomainSize,
                                                      &Use))
                                {
                                    HeapFree(GetProcessHeap(),
                                             0,
                                             Name);
                                    Name = NULL;
                                    goto BuildSidString;
                                }
                            }
                        }
                        else
                        {
BuildSidString:
                            if (!ConvertSidToStringSid(Sid,
                                                       &SidString))
                            {
                                Error = TRUE;
                                break;
                            }
                        }

                        /* print the file name or space */
                        ConPrintf(StdOut, L"%s ", FullFileName);

                        /* attempt to map the SID to a user name */
                        if (AceIndex == 0)
                        {
                            size_t i = 0;

                            /* overwrite the full file name with spaces so we
                               only print the file name once */
                            while (FullFileName[i] != _T('\0'))
                                FullFileName[i++] = _T(' ');
                        }

                        /* print the domain and/or user if possible, or the SID string */
                        if (Name != NULL && Domain[0] != _T('\0'))
                        {
                            ConPrintf(StdOut, L"%s\\%s:", Domain, Name);
                            IndentAccess = _tcslen(Domain) + _tcslen(Name);
                        }
                        else
                        {
                            LPTSTR DisplayString = (Name != NULL ? Name : SidString);

                            ConPrintf(StdOut, L"%s:", DisplayString);
                            IndentAccess = _tcslen(DisplayString);
                        }

                        /* print the ACE Flags */
                        if (Ace->Header.AceFlags & CONTAINER_INHERIT_ACE)
                        {
                            IndentAccess += (size_t)ConResPuts(StdOut, IDS_ABBR_CI);
                        }
                        if (Ace->Header.AceFlags & OBJECT_INHERIT_ACE)
                        {
                            IndentAccess += (size_t)ConResPuts(StdOut, IDS_ABBR_OI);
                        }
                        if (Ace->Header.AceFlags & INHERIT_ONLY_ACE)
                        {
                            IndentAccess += (size_t)ConResPuts(StdOut, IDS_ABBR_IO);
                        }

                        IndentAccess += 2;

                        /* print the access rights */
                        MapGenericMask(&AccessMask,
                                       &FileGenericMapping);
                        if (Ace->Header.AceType & ACCESS_DENIED_ACE_TYPE)
                        {
                            if (AccessMask == FILE_ALL_ACCESS)
                            {
                                ConResPuts(StdOut, IDS_ABBR_NONE);
                            }
                            else
                            {
                                ConResPuts(StdOut, IDS_DENY);
                                goto PrintSpecialAccess;
                            }
                        }
                        else
                        {
                            if (AccessMask == FILE_ALL_ACCESS)
                            {
                                ConResPuts(StdOut, IDS_ABBR_FULL);
                            }
                            else if (!(Ace->Mask & (GENERIC_READ | GENERIC_EXECUTE)) &&
                                     AccessMask == (FILE_GENERIC_READ | FILE_EXECUTE))
                            {
                                ConResPuts(StdOut, IDS_ABBR_READ);
                            }
                            else if (AccessMask == (FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_EXECUTE | DELETE))
                            {
                                ConResPuts(StdOut, IDS_ABBR_CHANGE);
                            }
                            else if (AccessMask == FILE_GENERIC_WRITE)
                            {
                                ConResPuts(StdOut, IDS_ABBR_WRITE);
                            }
                            else
                            {
                                DWORD x;
                                static const struct
                                {
                                    DWORD Access;
                                    UINT uID;
                                }
                                AccessRights[] =
                                {
                                    {FILE_WRITE_ATTRIBUTES, IDS_FILE_WRITE_ATTRIBUTES},
                                    {FILE_READ_ATTRIBUTES, IDS_FILE_READ_ATTRIBUTES},
                                    {FILE_DELETE_CHILD, IDS_FILE_DELETE_CHILD},
                                    {FILE_EXECUTE, IDS_FILE_EXECUTE},
                                    {FILE_WRITE_EA, IDS_FILE_WRITE_EA},
                                    {FILE_READ_EA, IDS_FILE_READ_EA},
                                    {FILE_APPEND_DATA, IDS_FILE_APPEND_DATA},
                                    {FILE_WRITE_DATA, IDS_FILE_WRITE_DATA},
                                    {FILE_READ_DATA, IDS_FILE_READ_DATA},
                                    {FILE_GENERIC_EXECUTE, IDS_FILE_GENERIC_EXECUTE},
                                    {FILE_GENERIC_WRITE, IDS_FILE_GENERIC_WRITE},
                                    {FILE_GENERIC_READ, IDS_FILE_GENERIC_READ},
                                    {GENERIC_ALL, IDS_GENERIC_ALL},
                                    {GENERIC_EXECUTE, IDS_GENERIC_EXECUTE},
                                    {GENERIC_WRITE, IDS_GENERIC_WRITE},
                                    {GENERIC_READ, IDS_GENERIC_READ},
                                    {MAXIMUM_ALLOWED, IDS_MAXIMUM_ALLOWED},
                                    {ACCESS_SYSTEM_SECURITY, IDS_ACCESS_SYSTEM_SECURITY},
                                    {SPECIFIC_RIGHTS_ALL, IDS_SPECIFIC_RIGHTS_ALL},
                                    {STANDARD_RIGHTS_REQUIRED, IDS_STANDARD_RIGHTS_REQUIRED},
                                    {SYNCHRONIZE, IDS_SYNCHRONIZE},
                                    {WRITE_OWNER, IDS_WRITE_OWNER},
                                    {WRITE_DAC, IDS_WRITE_DAC},
                                    {READ_CONTROL, IDS_READ_CONTROL},
                                    {DELETE, IDS_DELETE},
                                    {STANDARD_RIGHTS_ALL, IDS_STANDARD_RIGHTS_ALL},
                                };

                                ConResPuts(StdOut, IDS_ALLOW);

PrintSpecialAccess:
                                ConResPuts(StdOut, IDS_SPECIAL_ACCESS);

                                /* print the special access rights */
                                x = ARRAYSIZE(AccessRights);
                                while (x-- != 0)
                                {
                                    if ((Ace->Mask & AccessRights[x].Access) == AccessRights[x].Access)
                                    {
                                        ConPrintf(StdOut, L"\n%s ", FullFileName);
                                        ConPrintf(StdOut, L"%*s", IndentAccess, L"");
                                        ConResPuts(StdOut, AccessRights[x].uID);
                                    }
                                }

                                ConPuts(StdOut, L"\n");
                            }
                        }

                        ConPuts(StdOut, L"\n");

                        /* free up all resources */
                        if (Name != NULL)
                        {
                            HeapFree(GetProcessHeap(),
                                     0,
                                     Name);
                        }

                        if (SidString != NULL)
                        {
                            LocalFree((HLOCAL)SidString);
                        }

                        AceIndex++;
                    }

                    if (!Error)
                        Ret = TRUE;
                }
                else
                {
                    SetLastError(ERROR_NO_SECURITY_ON_OBJECT);
                }
            }
        }

        HeapFree(GetProcessHeap(),
                 0,
                 SecurityDescriptor);
    }
    else
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    }

    return Ret;
}

/* Add a backslash at the end of a path string if necessary.
 * Returns FALSE without writing when the backslash would not fit. */
static BOOL
AddBackslash(LPTSTR FilePath, SIZE_T cchBuffer)
{
    /* Find the last backslash. If there is none, or if it doesn't
     * terminate the string, then append a backslash. */
    LPTSTR pch = _tcsrchr(FilePath, _T('\\'));
    if (!pch || *(pch+1))
    {
        SIZE_T Length = _tcslen(FilePath);
        if (Length + 2 > cchBuffer) /* no room for the backslash and the NUL */
            return FALSE;
        FilePath[Length] = _T('\\');
        FilePath[Length + 1] = _T('\0');
    }
    return TRUE;
}

static BOOL
GetPathOfFile(LPTSTR FilePath, LPCTSTR pszFiles)
{
    TCHAR FullPath[MAX_PATH];
    LPTSTR pch;
    DWORD attrs, Length;

    /* _tcsncpy leaves the buffer unterminated when the path fills it; bound, terminate and reject truncation */
    if (FAILED(StringCchCopy(FilePath, MAX_PATH, pszFiles)))
    {
        PrintError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    pch = _tcsrchr(FilePath, _T('\\'));
    if (pch != NULL)
    {
        *pch = 0;
        /* reject failure (0) and an over-long required length (>= MAX_PATH) before copying */
        Length = GetFullPathName(FilePath, MAX_PATH, FullPath, NULL);
        if (Length == 0 || Length >= MAX_PATH)
        {
            PrintError(Length == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE);
            return FALSE;
        }
        _tcsncpy(FilePath, FullPath, MAX_PATH);

        attrs = GetFileAttributes(FilePath);
        if (attrs == 0xFFFFFFFF || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            PrintError(ERROR_DIRECTORY);
            return FALSE;
        }
    }
    else
    {
        /* same bounds: a failure or an over-long directory leaves FilePath unusable */
        Length = GetCurrentDirectory(MAX_PATH, FilePath);
        if (Length == 0 || Length >= MAX_PATH)
        {
            PrintError(Length == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE);
            return FALSE;
        }
    }

    if (!AddBackslash(FilePath, MAX_PATH))
    {
        PrintError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    return TRUE;
}

/* FAT and other volumes without stored ACLs have nothing to show or change */
static BOOL
VolumeHasAcls(LPCTSTR pszFiles)
{
    TCHAR FullPath[MAX_PATH], Root[MAX_PATH], FsName[MAX_PATH];
    DWORD Flags = 0, Length;
    LPTSTR pch;
    INT Slashes;

    Length = GetFullPathName(pszFiles, MAX_PATH, FullPath, NULL);
    if (Length == 0 || Length >= MAX_PATH)
        return TRUE;

    if (!GetVolumePathName(FullPath, Root, MAX_PATH))
    {
        /* Derive the root from the path: "X:\" or "\\server\share\" */
        _tcsncpy(Root, FullPath, MAX_PATH);
        Root[MAX_PATH - 1] = _T('\0');
        if (Root[0] != _T('\0') && Root[1] == _T(':'))
        {
            Root[2] = _T('\\');
            Root[3] = _T('\0');
        }
        else if (Root[0] == _T('\\') && Root[1] == _T('\\'))
        {
            for (pch = Root + 2, Slashes = 0; *pch; pch++)
            {
                if (*pch == _T('\\') && ++Slashes == 2)
                {
                    pch[1] = _T('\0');
                    break;
                }
            }
            if (Slashes < 2)
            {
                if (!AddBackslash(Root, MAX_PATH))
                    return TRUE;
            }
        }
        else
        {
            return TRUE;
        }
    }

    /* An unknown volume is left to the later steps, which report the error */
    if (!GetVolumeInformation(Root, NULL, 0, NULL, NULL, &Flags, FsName, MAX_PATH))
        return TRUE;

    /* NTFS keeps ACLs even where its driver does not report the flag */
    return (Flags & FILE_PERSISTENT_ACLS) || _tcsicmp(FsName, _T("NTFS")) == 0;
}

static BOOL
PrintDaclsOfFiles(LPCTSTR pszFiles)
{
    TCHAR FilePath[MAX_PATH];
    WIN32_FIND_DATA FindData;
    HANDLE hFind;
    DWORD LastError;

    /*
     * get the file path
     */
    if (!GetPathOfFile(FilePath, pszFiles))
        return FALSE;

    /*
     * search for the files
     */
    hFind = FindFirstFile(pszFiles, &FindData);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        PrintError(GetLastError());
        ConPuts(StdErr, L"\n");
        return FALSE;
    }
    do
    {
        if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (!PrintFileDacl(FilePath, FindData.cFileName))
        {
            LastError = GetLastError();
            if (LastError == ERROR_ACCESS_DENIED)
            {
                PrintError(LastError);
                if (!OptionC)
                {
                    FindClose(hFind);
                    return FALSE;
                }
            }
            else
            {
                break;
            }
        }
        else
        {
            ConPuts(StdOut, L"\n");
        }
    } while (FindNextFile(hFind, &FindData));
    LastError = GetLastError();
    FindClose(hFind);

    if (LastError != ERROR_NO_MORE_FILES)
    {
        PrintError(LastError);
        return FALSE;
    }

    return TRUE;
}

int _tmain(int argc, const TCHAR *argv[])
{
    INT i;
    LPTSTR pch;
    BOOL InvalidParameter = FALSE;

    /* Initialize the Console Standard Streams */
    ConInitStdStreams();

    if (argc <= 1 || argv[1][0] == _T('/'))
    {
        ConResPuts(StdOut, IDS_HELP);
        return (argc > 1 && _tcscmp(argv[1], _T("/?")) == 0) ? 0 : ERROR_BAD_ARGUMENTS;
    }

    // FIXME: Convert to proper parsing, with support for /?

    /*
     * parse command line options
     */
    for (i = 2; i < argc; i++)
    {
        if (_tcsicmp(argv[i], _T("/T")) == 0)
        {
            OptionT = TRUE;
        }
        else if (_tcsicmp(argv[i], _T("/E")) == 0)
        {
            OptionE = TRUE;
        }
        else if (_tcsicmp(argv[i], _T("/C")) == 0)
        {
            OptionC = TRUE;
        }
        else if (_tcsicmp(argv[i], _T("/G")) == 0)
        {
            if (i + 1 < argc)
            {
                pch = _tcschr(argv[++i], _T(':'));
                if (pch != NULL)
                {
                    OptionG = TRUE;
                    *pch = 0;
                    GUser = argv[i];
                    GPerm = pch + 1;
                    continue;
                }
            }
            InvalidParameter = TRUE;
            break;
        }
        else if (_tcsicmp(argv[i], _T("/R")) == 0)
        {
            if (i + 1 < argc)
            {
                RUser = argv[++i];
                OptionR = TRUE;
                continue;
            }
            InvalidParameter = TRUE;
            break;
        }
        else if (_tcsicmp(argv[i], _T("/P")) == 0)
        {
            if (i + 1 < argc)
            {
                pch = _tcschr(argv[++i], _T(':'));
                if (pch != NULL)
                {
                    OptionP = TRUE;
                    *pch = 0;
                    PUser = argv[i];
                    PPerm = pch + 1;
                    continue;
                }
            }
            InvalidParameter = TRUE;
            break;
        }
        else if (_tcsicmp(argv[i], _T("/D")) == 0)
        {
            if (i + 1 < argc)
            {
                OptionD = TRUE;
                DUser = argv[++i];
                continue;
            }
            InvalidParameter = TRUE;
            break;
        }
        else
        {
            InvalidParameter = TRUE;
            break;
        }
    }

    if (InvalidParameter)
    {
        PrintError(ERROR_INVALID_PARAMETER);
        ConResPuts(StdOut, IDS_HELP);
        return 1;
    }

    /* /R is only valid with /E */
    if (OptionR && !OptionE)
    {
        OptionR = FALSE;
    }

    if (!VolumeHasAcls(argv[1]))
    {
        ConResPuts(StdErr, IDS_NOT_NTFS);
        return 1;
    }

    /* Changing ACLs is not implemented: say so instead of pretending */
    if (OptionG || OptionR || OptionP || OptionD)
    {
        ConResPuts(StdErr, IDS_EDIT_NOT_SUPPORTED);
        return 1;
    }

    if (!PrintDaclsOfFiles(argv[1]))
        return 2;

    return 0;
}
