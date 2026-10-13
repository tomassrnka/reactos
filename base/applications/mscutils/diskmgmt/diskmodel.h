/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Disk, partition and volume model (no user interface)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _DISKMODEL_H_
#define _DISKMODEL_H_

#define DM_MIB ((ULONGLONG)1024 * 1024)
#define DM_GIB (DM_MIB * 1024)

typedef enum _DM_STYLE
{
    DM_STYLE_RAW = 0,
    DM_STYLE_MBR,
    DM_STYLE_GPT
} DM_STYLE;

typedef enum _DM_DEVICE
{
    DM_DEV_DISK = 0,
    DM_DEV_REMOVABLE,
    DM_DEV_CDROM
} DM_DEVICE;

typedef enum _DM_KIND
{
    DM_KIND_UNALLOCATED = 0,    /* gap on the disk */
    DM_KIND_PRIMARY,            /* primary partition, GPT partition, or the medium of a CD-ROM */
    DM_KIND_LOGICAL,            /* logical drive in the extended partition */
    DM_KIND_FREE                /* free space inside the extended partition */
} DM_KIND;

/* One block of the disk map. */
typedef struct _DM_REGION
{
    DM_KIND Kind;
    BOOL InExtended;            /* drawn inside the frame of the extended partition */
    ULONGLONG Offset;           /* bytes */
    ULONGLONG Length;           /* bytes */
    ULONG PartitionNumber;      /* as the driver numbers it; 0 for gaps */
    BOOL Active;                /* MBR boot indicator */
    UCHAR MbrType;
    GUID GptType;
    GUID GptId;
    WCHAR GptName[37];
    BOOL IsEfi;
    BOOL IsRecovery;
    BOOL IsDynamic;

    /* the volume that lives in this partition, when there is one */
    BOOL HasVolume;
    WCHAR VolumeGuid[64];       /* \\?\Volume{...}\ */
    WCHAR Letter[4];            /* "F:" or empty */
    WCHAR MountPaths[256];      /* every path, double-NUL terminated; empty when there are none */
    WCHAR Label[64];
    WCHAR FileSystem[32];
    BOOL HasFsInfo;             /* the label and the file system name were read */
    BOOL HasSpace;              /* VolTotal and VolFree were read */
    ULONGLONG VolTotal;
    ULONGLONG VolFree;

    /* filled by DmFinishModel */
    BOOL IsBoot;
    BOOL IsSystem;
    BOOL IsPageFile;
    BOOL IsCrashDump;
    WCHAR Status[160];          /* "Healthy (Primary Partition)" */

    struct _DM_DISK *Disk;
} DM_REGION, *PDM_REGION;

typedef struct _DM_DISK
{
    DM_DEVICE Device;
    ULONG Number;               /* PhysicalDriveN, or the CD-ROM number */
    DM_STYLE Style;
    BOOL Dynamic;
    ULONGLONG Length;           /* bytes, as the driver reports it */
    ULONG BytesPerSector;
    ULONGLONG CapacityMB;       /* usable size without reserved partitions (disk properties) */
    ULONGLONG UnallocatedMB;    /* usable size minus the partitions (disk properties) */
    ULONGLONG ReservedMB;       /* hidden reserved partitions */
    BOOL HasMedia;
    WCHAR StatusText[32];       /* "Online", "Offline", "Not Initialized", "No Media", "Unknown" */
    WCHAR DeviceName[128];      /* friendly name from the storage descriptor */
    ULONG BusType;              /* STORAGE_BUS_TYPE, 0xFF when unknown */
    WCHAR BusText[16];          /* "SCSI", "IDE", ... shown as Device Type */
    LONG ScsiPath, ScsiTarget, ScsiLun;  /* -1 when unknown */
    DWORD Signature;            /* MBR */
    GUID DiskId;                /* GPT */
    WCHAR MediaKind[16];        /* "CD-ROM" or "DVD" for optical drives */
    WCHAR DriveLetter[4];       /* first drive letter of a volume on the disk, for the label box */

    ULONG RegionCount;
    PDM_REGION Regions;         /* in disk order, hidden partitions left out */
} DM_DISK, *PDM_DISK;

typedef struct _DM_MODEL
{
    ULONG DiskCount;
    PDM_DISK *Disks;            /* hard disks and removable disks ordered by number, then optical drives */
} DM_MODEL, *PDM_MODEL;

/* What the system tells about the running installation. */
typedef struct _DM_SYSINFO
{
    BOOL HaveBootDevice;
    ULONG BootDisk;             /* disk that holds the Windows directory */
    ULONG BootPartition;
    BOOL BootIsCdrom;
    BOOL PagefileOnBoot;        /* system managed paging file ("?:") */
    ULONG PagefileLetters;      /* bit n = drive letter 'A'+n has a paging file */
    BOOL CrashDumpEnabled;
} DM_SYSINFO, *PDM_SYSINFO;

/* What the system tells about one disk. */
typedef struct _DM_DISK_INPUT
{
    DM_DEVICE Device;
    ULONG Number;
    ULONG BytesPerSector;
    ULONGLONG DiskLength;
    BOOL Removable;
    BOOL HasMedia;
    ULONG LayoutError;          /* 0 when Layout is valid */
    const DRIVE_LAYOUT_INFORMATION_EX *Layout;
    WCHAR DeviceName[128];
    ULONG BusType;
    LONG ScsiPath, ScsiTarget, ScsiLun;
} DM_DISK_INPUT, *PDM_DISK_INPUT;

/* What the system tells about one volume. */
typedef struct _DM_VOLUME_INFO
{
    BOOL HaveDeviceNumber;
    ULONG DeviceType;           /* FILE_DEVICE_DISK or FILE_DEVICE_CD_ROM */
    ULONG DeviceNumber;
    ULONG PartitionNumber;
    UINT DriveType;
    WCHAR Guid[64];
    WCHAR Letter[4];
    WCHAR MountPaths[256];
    BOOL HasFsInfo;
    WCHAR Label[64];
    WCHAR FileSystem[32];
    BOOL HasSpace;
    ULONGLONG Total;
    ULONGLONG Free;
    BOOL HasMedia;
    WCHAR MediaKind[16];
} DM_VOLUME_INFO, *PDM_VOLUME_INFO;

/* One row of the volume list. */
typedef struct _DM_ROW
{
    PDM_DISK Disk;
    PDM_REGION Region;
    WCHAR Name[96];
    WCHAR Layout[16];
    WCHAR Type[16];
    WCHAR FileSystem[32];
    WCHAR Status[160];
    WCHAR Capacity[24];
    WCHAR Free[24];
    WCHAR PercentFree[16];
    WCHAR FaultTolerance[8];
    WCHAR Overhead[8];
    ULONGLONG CapacityBytes;
    ULONGLONG FreeBytes;
} DM_ROW, *PDM_ROW;

/* Text of the three lines in a disk-map block and of the label box. */
typedef struct _DM_BLOCK_TEXT
{
    WCHAR Line1[96];            /* bold: "Label (F:)" */
    WCHAR Line2[96];            /* "1.00 GB FAT32" */
    WCHAR Line3[160];           /* "Healthy (Primary Partition)" */
} DM_BLOCK_TEXT, *PDM_BLOCK_TEXT;

typedef struct _DM_LABEL_TEXT
{
    WCHAR Line1[32];            /* "Disk 0" */
    WCHAR Line2[48];            /* "Basic", "Removable (E:)", "DVD (D:)" */
    WCHAR Line3[32];            /* "64.00 GB" or "No Media" */
    WCHAR Line4[32];            /* "Online" */
} DM_LABEL_TEXT, *PDM_LABEL_TEXT;

/* ---- formatting ---- */
VOID DmFormatSize(ULONGLONG Bytes, PWSTR Buffer, SIZE_T Count);
VOID DmFormatSizeMB(ULONGLONG MegaBytes, PWSTR Buffer, SIZE_T Count);
VOID DmFormatPercent(ULONGLONG Part, ULONGLONG Whole, PWSTR Buffer, SIZE_T Count);
PCWSTR DmBusText(ULONG BusType);

/* ---- building the model ---- */
PDM_MODEL DmCreateModel(VOID);
VOID DmFreeModel(PDM_MODEL Model);
BOOL DmBuildDisk(const DM_DISK_INPUT *Input, PDM_DISK *Disk);
VOID DmFreeDisk(PDM_DISK Disk);
BOOL DmAddDisk(PDM_MODEL Model, PDM_DISK Disk);
BOOL DmAttachVolume(PDM_MODEL Model, const DM_VOLUME_INFO *Volume);
VOID DmFinishModel(PDM_MODEL Model, const DM_SYSINFO *Info);

/* ---- texts ---- */
VOID DmBlockText(const DM_DISK *Disk, const DM_REGION *Region, PDM_BLOCK_TEXT Text);
VOID DmLabelText(const DM_DISK *Disk, PDM_LABEL_TEXT Text);
PDM_ROW DmBuildVolumeRows(const DM_MODEL *Model, PULONG Count);
PDM_REGION DmFindRegion(const DM_MODEL *Model, DM_DEVICE Device, ULONG Number, ULONGLONG Offset, PDM_DISK *Disk);
int DmCompareRowNames(const DM_ROW *a, const DM_ROW *b);

/* ---- live data (diskcollect.c) ---- */
PDM_MODEL DmCollect(VOID);

/* ---- text dump, one line per fact (diskmodel.c) ---- */
BOOL DmDumpModel(const DM_MODEL *Model, HANDLE File);

#endif /* _DISKMODEL_H_ */
