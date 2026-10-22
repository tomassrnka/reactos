/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The writer: changes disks, partitions and volumes (no user interface)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _DISKOPS_H_
#define _DISKOPS_H_

#include "disklayout.h"

/* Win32 error codes this module adds to the system's; they are not in winerror.h. */
#define DM_ERR_BASE             0xE0DA0000
#define DM_ERR_FORMAT_FAILED    (DM_ERR_BASE + 1)   /* the file system provider reported failure */
#define DM_ERR_NO_PROVIDER      (DM_ERR_BASE + 2)   /* the file system is not installed */
#define DM_ERR_VOLUME_IN_USE    (DM_ERR_BASE + 3)   /* the volume cannot be locked */
#define DM_ERR_NOT_EMPTY        (DM_ERR_BASE + 4)   /* the disk holds partitions */
#define DM_ERR_LAYOUT           (DM_ERR_BASE + 5)   /* DL_STATUS of the layout edit in DM_ERR_LAYOUT_BASE + status */
#define DM_ERR_LAYOUT_BASE      (DM_ERR_BASE + 0x100)
#define DM_ERR_NO_VOLUME        (DM_ERR_BASE + 6)   /* the new partition did not get a volume in time */
#define DM_ERR_NOT_SUPPORTED    (DM_ERR_BASE + 7)   /* the system has no support for the operation */
#define DM_ERR_PROTECTED        (DM_ERR_BASE + 8)   /* the system, boot, paging file or crash dump volume */
#define DM_ERR_CHANGED          (DM_ERR_BASE + 9)   /* the disk is not as it was when the user confirmed */
#define DM_ERR_UNREADABLE       (DM_ERR_BASE + 10)  /* the table of the disk cannot be represented here */
#define DM_ERR_TYPE_NOT_UPDATED (DM_ERR_BASE + 11)  /* the format worked, the partition type could not be adjusted to it */
#define DM_ERR_VERIFY_FAILED    (DM_ERR_BASE + 12)  /* the disk does not hold the table that was written */
#define DM_ERR_SHRINK_PARTIAL   (DM_ERR_BASE + 15)  /* the file system shrank, the partition did not follow */
#define DM_ERR_EXTEND_PARTIAL   (DM_ERR_BASE + 14)  /* the partition grew, the file system did not follow */
#define DM_ERR_RESTORE_FAILED   (DM_ERR_BASE + 13)  /* a write failed and the table that was there could not be put back */
#define DM_ERR_PATHS_NOT_RESTORED (DM_ERR_BASE + 16) /* an operation failed and the drive letters or paths it had taken away could not all be put back */

#define DM_MAX_FS       16
#define DM_FS_NAME      16

typedef struct _DM_FS_LIST
{
    ULONG Count;
    WCHAR Name[DM_MAX_FS][DM_FS_NAME];
} DM_FS_LIST, *PDM_FS_LIST;

typedef VOID (*DM_PROGRESS_FN)(PVOID Context, ULONG Percent);

typedef struct _DM_FORMAT
{
    WCHAR Root[MAX_PATH];       /* "F:\" or "\\?\Volume{...}\" */
    WCHAR FileSystem[DM_FS_NAME];
    WCHAR Label[64];
    BOOL Quick;
    ULONG ClusterSize;          /* bytes; 0 is the default */
    BOOL Removable;
    BOOL Force;                 /* dismount a volume that is in use; without it such a volume is refused */
    DM_PROGRESS_FN Progress;
    PVOID Context;
    WCHAR Output[160];          /* first text line the provider printed, for the error box */
} DM_FORMAT, *PDM_FORMAT;

/* what a new partition looked like when it appeared */
typedef struct _DM_CREATED
{
    ULONGLONG Offset;
    ULONGLONG Length;
    BOOL Logical;
    BOOL MadeExtended;          /* the extended partition was made for it */
    BOOL MadeReserved;          /* the reserved partition of a GPT disk was made first */
    WCHAR VolumeGuid[64];       /* \\?\Volume{...}\ ; empty when the system gave none in time */
} DM_CREATED, *PDM_CREATED;

/* Which disk the user looked at: an operation refuses a disk that is not this one any more (the number may name another disk
 * after a disk was taken out and another put in). Style is DM_STYLE_RAW for a disk without a table. */
typedef struct _DM_DISK_ID
{
    DM_STYLE Style;
    ULONGLONG Length;
    ULONG SectorSize;
    DWORD Signature;            /* MBR */
    GUID DiskId;                /* GPT */
} DM_DISK_ID, *PDM_DISK_ID;

/* Faults the apitest puts into the writer; Data is the table that is about to be written (it may be changed), or NULL. A
 * TRUE answer means the real call is not made: a table write then reports success, a creation of a table reports failure. */
typedef BOOL (*DM_FAULT_FN)(ULONG Point, ULONG Disk, PVOID Data);
#define DM_FAULT_LAYOUT_WRITE   1
#define DM_FAULT_DISK_CREATE    2
/* The creation of a table is not made and reports success, as a driver that does not write would. */
#define DM_FAULT_DISK_CREATE_SILENT 3
/* A disk number is about to be opened; Data points at the number, which the hook may change (to let the number lead to
 * another disk). OPEN_RAW is the handle that goes to the disk and not to the cache, OPEN_DISK every other handle. */
#define DM_FAULT_OPEN_DISK      4
#define DM_FAULT_OPEN_RAW       5
VOID DmSetFaultHook(DM_FAULT_FN Hook);

/* ---- file systems ---- */
BOOL DmQueryFileSystems(PDM_FS_LIST List);

/* ---- disks ---- */
DWORD DmReadDiskLayout(ULONG Disk, PDL_LAYOUT Layout);
/* Writes Layout, which was made from Baseline (the table the caller read): the write is refused with DM_ERR_CHANGED when the disk
 * holds another table now. Baseline may be NULL where no table was read before. */
DWORD DmWriteDiskLayout(ULONG Disk, const DL_LAYOUT *Baseline, const DL_LAYOUT *Layout);
DWORD DmCheckScratchDisk(ULONG Disk);     /* ERROR_SUCCESS only for a disk that is empty; any other answer means: do not write */
/* DM_ERR_CHANGED when the disk is not the one that Id describes (Id may be NULL where the caller has none). */
DWORD DmVerifyDiskId(ULONG Disk, const DM_DISK_ID *Id);
/* One writer at a time per disk across the programs that use this module: Lock is for DmUnlockDisk. */
DWORD DmLockDisk(ULONG Disk, PHANDLE Lock);
VOID DmUnlockDisk(HANDLE Lock);
DWORD DmInitializeDisk(ULONG Disk, DM_STYLE Style, const DM_DISK_ID *Id);
DWORD DmConvertDisk(ULONG Disk, DM_STYLE Style, const DM_DISK_ID *Id);
DWORD DmRescanDisks(VOID);

/* Plans a new partition in the gap at FreeOffset. MaxLength is what fits; NeedsExtended says the
 * partition is made as a logical drive in a new extended partition (the fourth volume of an MBR disk). */
DWORD DmPlanPartition(ULONG Disk, ULONGLONG FreeOffset, BOOL InExtended, PDL_FREE Free, PBOOL NeedsExtended,
                      PDL_LAYOUT LayoutOut);
/* Planned is the table that DmPlanPartition returned to the caller: DM_ERR_CHANGED when the disk holds another one now. */
DWORD DmCreatePartition(ULONG Disk, ULONGLONG FreeOffset, BOOL InExtended, ULONGLONG Length, const DL_LAYOUT *Planned,
                        const DM_DISK_ID *Id, PDM_CREATED Created);
DWORD DmDeletePartition(ULONG Disk, ULONGLONG Offset, ULONGLONG Length, PCWSTR VolumeGuid, BOOL Force, const DM_DISK_ID *Id);
BOOL DmIsBootPartition(ULONG Disk, ULONGLONG Offset);   /* the system partition the kernel recorded at start up */
DWORD DmMarkActive(ULONG Disk, ULONGLONG Offset, const DM_DISK_ID *Id);
DWORD DmFixPartitionType(ULONG Disk, ULONGLONG Offset, PCWSTR VolumeGuid);

/* ---- growing and shrinking ----
 * The requests are the documented ones (IOCTL_DISK_GROW_PARTITION on the disk, FSCTL_EXTEND_VOLUME and
 * FSCTL_SHRINK_VOLUME on the volume). Each Probe answers DM_ERR_NOT_SUPPORTED while the driver below does not
 * implement the request, so that the menu can say so; the other calls work as soon as it does. */
DWORD DmProbeExtend(ULONG Disk, PCWSTR VolumeGuid);
DWORD DmProbeShrink(PCWSTR VolumeGuid);
DWORD DmQueryExtend(ULONG Disk, ULONGLONG Offset, PULONGLONG MaxBytes);
DWORD DmExtendVolume(ULONG Disk, ULONGLONG Offset, ULONGLONG Length, PCWSTR VolumeGuid, ULONGLONG AddBytes, const DM_DISK_ID *Id);
DWORD DmQueryShrink(ULONG Disk, ULONGLONG Offset, PCWSTR VolumeGuid, PULONGLONG TotalBytes, PULONGLONG MaxBytes);
DWORD DmShrinkVolume(ULONG Disk, ULONGLONG Offset, ULONGLONG Length, PCWSTR VolumeGuid, ULONGLONG ShrinkBytes, const DM_DISK_ID *Id);

/* ---- volumes ---- */
DWORD DmFindVolume(ULONG Disk, ULONGLONG Offset, ULONG TimeoutMs, PWSTR VolumeGuid, SIZE_T Count);
DWORD DmFormatVolume(PDM_FORMAT Format);
DWORD DmSetVolumeLabel(PCWSTR VolumeGuid, PCWSTR Label);
DWORD DmAddMountPath(PCWSTR VolumeGuid, PCWSTR Path);
DWORD DmRemoveMountPath(PCWSTR Path);
DWORD DmRemoveMountPathOf(PCWSTR VolumeGuid, PCWSTR Path);   /* DM_ERR_CHANGED when the path leads to another volume now */
DWORD DmChangeDriveLetter(PCWSTR VolumeGuid, PCWSTR MountPaths, WCHAR NewLetter);
DWORD DmReplaceMountPath(PCWSTR VolumeGuid, PCWSTR OldPath, PCWSTR NewPath);
PCWSTR DmDosError(DWORD Error, PWSTR Buffer, SIZE_T Count);

#endif /* _DISKOPS_H_ */
