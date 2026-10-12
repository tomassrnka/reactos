//======================================================================
//
// Chkdskx
//
// Copyright (c) 1998 Mark Russinovich
// Systems Internals
// http://www.sysinternals.com
//
// Chkdsk clone that demonstrates the use of the FMIFS file system
// utility library.
//
// --------------------------------------------------------------------
//
// This software is free software; you can redistribute it and/or
// modify it under the terms of the GNU Library General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
//
// This software is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Library General Public License for more details.
//
// You should have received a copy of the GNU Library General Public
// License along with this software; see the file COPYING.LIB. If
// not, write to the Free Software Foundation, Inc., 675 Mass Ave,
// Cambridge, MA 02139, USA.
//
// --------------------------------------------------------------------
//
// 1999 February (Emanuele Aliberti)
//      Adapted for ReactOS and lcc-win32.
//
// 1999 April (Emanuele Aliberti)
//      Adapted for ReactOS and egcs.
//
// 2008 July (Aleksey Bragin)
//      Cleanup, use ReactOS's fmifs.h
//
// 2026 October (Tomas Srnka)
//      The switches, messages and exit codes of the CHKDSK of
//      Windows Server 2008 R2.
//
//======================================================================

#include <stdio.h>
#include <stdlib.h>

/* PSDK/NDK Headers */
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <wincon.h>

#include <conutils.h>

/* Resource header */
#include "resource.h"

#define NTOS_MODE_USER
#include <ndk/ntndk.h>

/* FMIFS Public Header */
#include <fmifs/fmifs.h>
#include <fslib/ntfsck.h>

#define FMIFS_IMPORT_DLL

/* Exit codes, as Windows CHKDSK documents them */
#define CHKDSK_EXIT_CLEAN       0   /* no errors were found */
#define CHKDSK_EXIT_FIXED       1   /* errors were found and fixed */
#define CHKDSK_EXIT_CLEANUP     2   /* disk cleanup was done, or not done because /F was not given */
#define CHKDSK_EXIT_FAILED      3   /* the disk could not be checked, or errors were not fixed */

//
// Globals
//
BOOL    Error = FALSE;

/* Passed to the provider through fmifs; a provider that knows it fills ExitCode. */
CHKDSK_EXTENSION Extension;

// Switches
BOOL    FixErrors = FALSE;      // /F
BOOL    Verbose = FALSE;        // /V
BOOL    ScanSectors = FALSE;    // /R (implies /F)
BOOL    ForceDismount = FALSE;  // /X (implies /F)
BOOL    SkipIndex = FALSE;      // /I (NTFS)
BOOL    SkipCycles = FALSE;     // /C (NTFS)
BOOL    ShowLogSize = FALSE;    // /L (NTFS)
BOOL    SetLogSize = FALSE;     // /L:size (NTFS)
BOOL    ReevalBad = FALSE;      // /B (NTFS, implies /R)
ULONG   LogSizeKB = 0;
PWCHAR  Drive = NULL;
WCHAR   CurrentDirectory[1024];

/*
 * Switches of the CHKDSK of Windows 8 and later. They are recognized and answered
 * with a message; the ones that only read the volume do not stop the check.
 */
typedef struct _NEWER_SWITCH
{
    PCWSTR Name;
    BOOL Repairs;
    BOOL Given;
} NEWER_SWITCH;

NEWER_SWITCH NewerSwitches[] =
{
    { L"scan",               FALSE, FALSE },
    { L"perf",               FALSE, FALSE },
    { L"spotfix",            TRUE,  FALSE },
    { L"offlinescanandfix",  TRUE,  FALSE },
    { L"forceofflinefix",    TRUE,  FALSE },
    { L"sdcleanup",          TRUE,  FALSE },
    { L"freeorphanedchains", TRUE,  FALSE },
    { L"markclean",          TRUE,  FALSE },
};

#ifndef FMIFS_IMPORT_DLL
//
// Functions in FMIFS.DLL
//
PCHKDSK Chkdsk;
#endif


//----------------------------------------------------------------------
//
// PrintWin32Error
//
// Takes the win32 error code and prints the text version.
//
//----------------------------------------------------------------------
static VOID PrintWin32Error(UINT Message, DWORD ErrorCode)
{
    ConResPuts(StdErr, Message);
    ConMsgPuts(StdErr, FORMAT_MESSAGE_FROM_SYSTEM,
               NULL, ErrorCode, LANG_USER_DEFAULT);
    ConPuts(StdErr, L"\n");
}


//--------------------------------------------------------------------
//
// CtrlCIntercept
//
// Intercepts Ctrl-C's so that the program can't be quit with the
// disk in an inconsistent state.
//
//--------------------------------------------------------------------
BOOL
WINAPI
CtrlCIntercept(DWORD dwCtrlType)
{
    //
    // Handle the event so that the default handler doesn't
    //
    return TRUE;
}


//----------------------------------------------------------------------
//
// Usage
//
// Tell the user how to use the program
//
//----------------------------------------------------------------------
static VOID
Usage(VOID)
{
    ConResPuts(StdOut, IDS_USAGE);
}


//----------------------------------------------------------------------
//
// ParseCommandLine
//
// Get the switches. Returns the index of the first bad argument,
// -1 for /?, 0 when all is well.
//
//----------------------------------------------------------------------
static int
ParseCommandLine(int argc, WCHAR *argv[])
{
    int i;
    ULONG j;

    for (i = 1; i < argc; i++)
    {
        PWCHAR Arg = argv[i];

        if (Arg[0] == L'/' || Arg[0] == L'-')
        {
            PWCHAR Name = Arg + 1;

            if (!_wcsicmp(Name, L"?"))
                return -1;
            if (!_wcsicmp(Name, L"F"))
            {
                FixErrors = TRUE;
                continue;
            }
            if (!_wcsicmp(Name, L"V"))
            {
                Verbose = TRUE;
                continue;
            }
            if (!_wcsicmp(Name, L"R"))
            {
                ScanSectors = FixErrors = TRUE;
                continue;
            }
            if (!_wcsicmp(Name, L"X"))
            {
                ForceDismount = FixErrors = TRUE;
                continue;
            }
            if (!_wcsicmp(Name, L"I"))
            {
                SkipIndex = TRUE;
                continue;
            }
            if (!_wcsicmp(Name, L"C"))
            {
                SkipCycles = TRUE;
                continue;
            }
            if (!_wcsicmp(Name, L"B"))
            {
                ReevalBad = ScanSectors = FixErrors = TRUE;
                continue;
            }
            if (!_wcsicmp(Name, L"L"))
            {
                ShowLogSize = TRUE;
                continue;
            }
            if ((Name[0] == L'L' || Name[0] == L'l') && Name[1] == L':')
            {
                PWCHAR End;
                ULONG Size = wcstoul(Name + 2, &End, 10);
                if (Name[2] == L'\0' || *End != L'\0' || Size == 0)
                    return i;
                SetLogSize = TRUE;
                LogSizeKB = Size;
                continue;
            }
            for (j = 0; j < ARRAYSIZE(NewerSwitches); j++)
            {
                if (!_wcsicmp(Name, NewerSwitches[j].Name))
                {
                    NewerSwitches[j].Given = TRUE;
                    break;
                }
            }
            if (j < ARRAYSIZE(NewerSwitches))
                continue;
            return i;
        }

        if (Drive)
            return i;
        if (!iswalpha(Arg[0]) || Arg[1] != L':' ||
            (Arg[2] != L'\0' && !(Arg[2] == L'\\' && Arg[3] == L'\0')))
        {
            return i;
        }
        Drive = Arg;
    }
    return 0;
}


//----------------------------------------------------------------------
//
// ChkdskCallback
//
// The file system library will call us back with commands that we
// can interpret. If we wanted to halt the chkdsk we could return FALSE.
//
//----------------------------------------------------------------------
BOOLEAN
WINAPI
ChkdskCallback(
    CALLBACKCOMMAND Command,
    DWORD       Modifier,
    PVOID       Argument)
{
    BOOLEAN     Ret;
    PDWORD      percent;
    PBOOLEAN    status;
    PTEXTOUTPUT output;

    //
    // We get other types of commands,
    // but we don't have to pay attention to them
    //
    Ret = TRUE;
    switch (Command)
    {
        case UNKNOWN2:
            ConPuts(StdOut, L"UNKNOWN2\r");
            break;

        case UNKNOWN3:
            ConPuts(StdOut, L"UNKNOWN3\n");
            break;

        case UNKNOWN4:
            ConPuts(StdOut, L"UNKNOWN4\n");
            break;

        case UNKNOWN5:
            ConPuts(StdOut, L"UNKNOWN5\n");
            break;

        case FSNOTSUPPORTED:
            ConPuts(StdOut, L"FSNOTSUPPORTED\n");
            break;

        case VOLUMEINUSE:
            ConResPuts(StdOut, IDS_VOLUME_IN_USE);
            Ret = FALSE;
            break;

        case UNKNOWN9:
            ConPuts(StdOut, L"UNKNOWN9\n");
            break;

        case UNKNOWNA:
            ConPuts(StdOut, L"UNKNOWNA\n");
            break;

        case UNKNOWNC:
            ConPuts(StdOut, L"UNKNOWNC\n");
            break;

        case UNKNOWND:
            ConPuts(StdOut, L"UNKNOWND\n");
            break;

        case INSUFFICIENTRIGHTS:
            ConPuts(StdOut, L"INSUFFICIENTRIGHTS\n");
            break;

        case STRUCTUREPROGRESS:
            ConPuts(StdOut, L"STRUCTUREPROGRESS\n");
            break;

        case DONEWITHSTRUCTURE:
            ConPuts(StdOut, L"DONEWITHSTRUCTURE\n");
            break;

        case CLUSTERSIZETOOSMALL:
            ConPuts(StdOut, L"CLUSTERSIZETOOSMALL\n");
            break;

        case PROGRESS:
            percent = (PDWORD)Argument;
            ConResPrintf(StdOut, IDS_PERCENT_COMPL, *percent);
            break;

        case OUTPUT:
            output = (PTEXTOUTPUT)Argument;
            ConPrintf(StdOut, L"%S", output->Output);
            break;

        case DONE:
            status = (PBOOLEAN)Argument;
            if (*status == FALSE)
            {
                Error = TRUE;
                /* A provider that fills the CHKDSK_EXTENSION has said why itself. */
                if (Extension.ExitCode != ~0UL)
                    break;
                ConResPrintf(StdOut, IDS_CHKDSK_FAIL, Modifier);
                if (Modifier == STATUS_DISK_CORRUPT_ERROR)
                    ConResPuts(StdOut, IDS_RUN_AGAIN);
                else
                    ConPrintf(StdOut, L"\n");
            }
            break;
    }
    return Ret;
}

#ifndef FMIFS_IMPORT_DLL
//----------------------------------------------------------------------
//
// LoadFMIFSEntryPoints
//
// Loads FMIFS.DLL and locates the entry point(s) we are going to use
//
//----------------------------------------------------------------------
static BOOLEAN
LoadFMIFSEntryPoints(VOID)
{
    HMODULE hFmifs = LoadLibraryW(L"fmifs.dll");
    if (hFmifs == NULL)
        return FALSE;

    Chkdsk = (PCHKDSK)GetProcAddress(hFmifs, "Chkdsk");
    if (!Chkdsk)
    {
        FreeLibrary(hFmifs);
        return FALSE;
    }

    return TRUE;
}
#endif


//----------------------------------------------------------------------
//
// WMain
//
// Engine. Just get command line switches and fire off a chkdsk. This
// could also be done in a GUI like Explorer does when you select a
// drive and run a check on it.
//
// We do this in UNICODE because the chkdsk command expects PWCHAR
// arguments.
//
//----------------------------------------------------------------------
int
wmain(int argc, WCHAR *argv[])
{
    int badArg;
    HANDLE volumeHandle;
    WCHAR  fileSystem[1024];
    WCHAR  volumeName[1024];
    DWORD  serialNumber;
    DWORD  flags, maxComponent;
    BOOL   IsNtfs;
    BOOL   RepairSwitch = FALSE;
    ULONG  j;

    /* Initialize the Console Standard Streams */
    ConInitStdStreams();

#ifndef FMIFS_IMPORT_DLL
    //
    // Get function pointers
    //
    if (!LoadFMIFSEntryPoints())
    {
        ConResPuts(StdErr, IDS_NO_ENTRY_POINT);
        return CHKDSK_EXIT_FAILED;
    }
#endif

    //
    // Parse command line
    //
    badArg = ParseCommandLine(argc, argv);
    if (badArg < 0)
    {
        /* Windows CHKDSK also ends /? with exit code 3. */
        Usage();
        return CHKDSK_EXIT_FAILED;
    }
    if (badArg)
    {
        ConResPrintf(StdOut, IDS_BAD_ARGUMENT, argv[badArg]);
        return CHKDSK_EXIT_FAILED;
    }

    //
    // Get the drive's format
    //
    if (!Drive)
    {
        if (!GetCurrentDirectoryW(ARRAYSIZE(CurrentDirectory), CurrentDirectory))
        {
            PrintWin32Error(IDS_NO_CURRENT_DIR, GetLastError());
            return CHKDSK_EXIT_FAILED;
        }
    }
    else
    {
        wcscpy(CurrentDirectory, Drive);
    }
    CurrentDirectory[2] = L'\\';
    CurrentDirectory[3] = L'\0';
    Drive = CurrentDirectory;

    //
    // Determine the drive's file system format, which we need to
    // tell chkdsk
    //
    if (!GetVolumeInformationW(Drive,
                               volumeName,
                               ARRAYSIZE(volumeName),
                               &serialNumber,
                               &maxComponent,
                               &flags,
                               fileSystem,
                               ARRAYSIZE(fileSystem)))
    {
        PrintWin32Error(IDS_NO_QUERY_VOL, GetLastError());
        return CHKDSK_EXIT_FAILED;
    }

    ConResPrintf(StdOut, IDS_FILE_SYSTEM, fileSystem);
    if (!_wcsicmp(fileSystem, L"RAW"))
    {
        ConResPuts(StdOut, IDS_RAW_DRIVE);
        return CHKDSK_EXIT_FAILED;
    }
    if (volumeName[0])
        ConResPrintf(StdOut, IDS_VOLUME_LABEL, volumeName);

    IsNtfs = !_wcsicmp(fileSystem, L"NTFS");
    if (!IsNtfs)
    {
        if (SkipIndex)
        {
            ConResPrintf(StdOut, IDS_NTFS_ONLY, L"/I");
            return CHKDSK_EXIT_FAILED;
        }
        if (SkipCycles)
        {
            ConResPrintf(StdOut, IDS_NTFS_ONLY, L"/C");
            return CHKDSK_EXIT_FAILED;
        }
        if (ShowLogSize || SetLogSize)
        {
            ConResPrintf(StdOut, IDS_NTFS_ONLY, L"/L");
            return CHKDSK_EXIT_FAILED;
        }
        if (ReevalBad)
        {
            ConResPrintf(StdOut, IDS_NTFS_ONLY, L"/B");
            return CHKDSK_EXIT_FAILED;
        }
    }

    for (j = 0; j < ARRAYSIZE(NewerSwitches); j++)
    {
        if (NewerSwitches[j].Given)
        {
            ConResPrintf(StdOut, IDS_NOT_SUPPORTED, NewerSwitches[j].Name);
            if (NewerSwitches[j].Repairs)
                RepairSwitch = TRUE;
        }
    }
    if (RepairSwitch)
        return CHKDSK_EXIT_FAILED;

    //
    // If they want to fix, we need to have access to the drive. The
    // NTFS checker takes the volume itself.
    //
    if (FixErrors && !IsNtfs)
    {
        swprintf(volumeName, L"\\\\.\\%C:", Drive[0]);
        if (ForceDismount)
        {
            /* /X: dismount first, so the open handles of others do not stop the check. */
            DWORD Bytes;
            volumeHandle = CreateFileW(volumeName, GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, 0);
            if (volumeHandle != INVALID_HANDLE_VALUE)
            {
                if (DeviceIoControl(volumeHandle, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &Bytes, NULL))
                    ConResPuts(StdOut, IDS_DISMOUNTED);
                CloseHandle(volumeHandle);
            }
        }
        volumeHandle = CreateFileW(volumeName,
                                   GENERIC_WRITE,
                                   0,
                                   NULL,
                                   OPEN_EXISTING,
                                   0,
                                   0);
        if (volumeHandle == INVALID_HANDLE_VALUE)
        {
            ConResPuts(StdErr, IDS_VOLUME_IN_USE_PROC);
            return CHKDSK_EXIT_FAILED;
        }
        CloseHandle(volumeHandle);
    }
    if (FixErrors)
    {
        //
        // Can't let the user break out of a chkdsk that can modify the drive
        //
        SetConsoleCtrlHandler(CtrlCIntercept, TRUE);
    }
    else if (!(ShowLogSize && IsNtfs))
    {
        /* /L alone only shows the log file size; nothing is checked. */
        ConResPuts(StdOut, IDS_READ_ONLY);
    }

    RtlZeroMemory(&Extension, sizeof(Extension));
    Extension.Magic = CHKDSK_EXTENSION_MAGIC;
    Extension.Size = sizeof(Extension);
    Extension.ExitCode = ~0UL;
    if (ForceDismount)
        Extension.Flags |= NTFSCK_FLAG_FORCE_DISMOUNT;
    if (SkipIndex)
        Extension.Flags |= NTFSCK_FLAG_SKIP_INDEX;
    if (SkipCycles)
        Extension.Flags |= NTFSCK_FLAG_SKIP_CYCLES;
    if (ShowLogSize)
        Extension.Flags |= NTFSCK_FLAG_LOG_SIZE;
    if (SetLogSize)
    {
        Extension.Flags |= NTFSCK_FLAG_SET_LOG_SIZE;
        Extension.LogSizeKB = LogSizeKB;
    }
    if (ReevalBad)
        Extension.Flags |= NTFSCK_FLAG_REEVAL_BAD;

    //
    // Just do it
    //
    Chkdsk(Drive,
           fileSystem,
           FixErrors,
           Verbose,
           FALSE,
           ScanSectors,
           &Extension,
           NULL,
           ChkdskCallback);

    if (Extension.ExitCode != ~0UL)
        return (int)Extension.ExitCode;
    if (Error)
        return CHKDSK_EXIT_FAILED;
    return CHKDSK_EXIT_CLEAN;
}

/* EOF */
