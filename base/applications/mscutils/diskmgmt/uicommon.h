/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared helpers of the dialogs and wizards
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _UICOMMON_H_
#define _UICOMMON_H_

#include "diskmgmt.h"

#define WM_DM_DONE      (WM_APP + 20)
#define WM_DM_PROGRESS  (WM_APP + 21)
#define WM_DM_STARTED   (WM_APP + 22)

/* what an action needs to know about its target, copied from the model so that it outlives a refresh */
typedef struct _TARGET
{
    ULONG Disk;
    DM_DEVICE Device;
    DM_STYLE Style;
    ULONGLONG Offset;
    ULONGLONG Length;
    BOOL InExtended;
    BOOL Protected;
    BOOL Removable;
    BOOL IsEfi;
    WCHAR Guid[64];
    WCHAR Letter[4];
    WCHAR Paths[256];
    WCHAR Label[64];
    WCHAR FileSystem[32];
} TARGET, *PTARGET;

/* the model */
BOOL DiskWritable(const DM_DISK *Disk);
BOOL RegionIsPartition(const DM_REGION *Region);
BOOL RegionProtected(const DM_REGION *Region);
BOOL DiskHasPartitions(const DM_DISK *Disk);
BOOL DiskNeedsInit(const DM_DISK *Disk);
VOID FillTarget(PTARGET Target, const DM_DISK *Disk, const DM_REGION *Region);
PCWSTR VolumeDisplayName(const TARGET *Target, PWSTR Buffer, SIZE_T Count);

/* messages */
VOID ShowError(HWND hOwner, DWORD Error, PCWSTR Extra);
BOOL ConfirmBox(HWND hOwner, UINT CaptionId, UINT TextId, PCWSTR Argument);
VOID FormatMessageBox(HWND hOwner, UINT TextId, UINT Icon, PCWSTR Argument);

/* work on a thread behind a dialog the user cannot close */
DWORD RunBusy(HWND hOwner, PCWSTR Text, BOOL Bar, DWORD (*Func)(PVOID), PVOID Arg, VOID (**Progress)(ULONG));
VOID TypeFix(HWND hOwner, ULONG Disk, ULONGLONG Offset, PCWSTR Guid);
DWORD RunFormat(HWND hOwner, PCWSTR Guid, PCWSTR FileSystem, PCWSTR Label, BOOL Quick, ULONG Cluster, BOOL Removable);

/* drive letters, file systems, paths */
BOOL ParseNumber(PCWSTR Text, ULONGLONG Min, ULONGLONG Max, PULONGLONG Value);
VOID FillLetterCombo(HWND hCombo, WCHAR Current);
WCHAR ComboLetter(HWND hDlg, UINT Id);
VOID FillFsCombo(HWND hCombo, const DM_FS_LIST *Fs, PCWSTR Current, ULONGLONG VolumeBytes);
VOID FillClusterCombo(HWND hCombo, PCWSTR FileSystem, ULONGLONG VolumeBytes);
ULONG ComboCluster(HWND hDlg, UINT Id);
VOID CurrentPaths(PCWSTR Guid, PWSTR Buffer, DWORD Count);
BOOL IsLetterPath(PCWSTR Path);

/* Applies a drive letter, a folder or neither to a volume that has just been made. */
typedef enum _PATH_CHOICE
{
    PATH_LETTER = 0,
    PATH_FOLDER,
    PATH_NONE
} PATH_CHOICE;

DWORD ApplyPathChoice(PCWSTR Guid, PATH_CHOICE Choice, WCHAR Letter, PCWSTR Folder);

/* the dialogs of dialogs.c and the wizards of wizard.c; each returns TRUE when the disks changed */
BOOL DlgInitialize(HWND hOwner, PDM_MODEL Model, const DM_DISK *Only);
BOOL DlgFormat(HWND hOwner, const TARGET *Target);
BOOL DlgLetters(HWND hOwner, const TARGET *Target);
BOOL DlgShrink(HWND hOwner, const TARGET *Target);
BOOL WizNewVolume(HWND hOwner, const TARGET *Target);
BOOL WizExtendVolume(HWND hOwner, const TARGET *Target, const DM_DISK *Disk);

#endif /* _UICOMMON_H_ */
