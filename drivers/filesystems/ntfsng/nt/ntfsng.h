/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NT glue around the vendored Linux fs/ntfs core (read-only prototype)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#pragma once

#include <ntifs.h>
#include <ntdddisk.h>
#include <pseh/pseh2.h>
#include "../core/ngapi.h"

#define NDEBUG
#include <debug.h>

#define TAG_NTFSNG      'GNTN'
#define TAG_NTFSNG_CORE 'cNTN'

#define NG_WRITE_ACCESS (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | \
                         FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL)

#define NG_NODE_VCB 0x4e47
#define NG_NODE_FCB 0x4e48

/* Volume control block: the device extension of the volume device object. */
typedef struct _NG_VCB
{
    USHORT NodeType;
    PDEVICE_OBJECT VolumeDevice;
    PDEVICE_OBJECT StorageDevice;   /* target of the mount, receives our reads */
    PVPB Vpb;
    ngc_vol *Core;
    ERESOURCE CoreLock;             /* serialises every call into the fs/ntfs core */
    FAST_MUTEX FcbListLock;
    LIST_ENTRY FcbList;
    struct _NG_FCB *VolumeFcb;
    struct ngc_volinfo Info;
    ULONG SectorSize;
} NG_VCB, *PNG_VCB;

/* File control block: one per (MFT record, stream) open on a volume. */
typedef struct _NG_FCB
{
    FSRTL_COMMON_FCB_HEADER Header; /* must be first: FsContext */
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    ERESOURCE MainResource;
    ERESOURCE PagingIoResource;
    LIST_ENTRY VcbLinks;
    PNG_VCB Vcb;
    LONG RefCount;                  /* file objects not yet closed */
    LONG OpenHandles;               /* file objects not yet cleaned up */
    SHARE_ACCESS ShareAccess;
    ngc_node *Node;                 /* referenced core inode while handles are open; NULL when parked */
    BOOLEAN HasNode;                /* a regular FCB (not the volume) that can re-acquire its node */
    struct ngc_stat Stat;
    BOOLEAN IsVolume;
    BOOLEAN IsDirectory;
    BOOLEAN IsRoot;
    ULONGLONG MftNo;
    UNICODE_STRING Stream;          /* empty for the unnamed $DATA */
    WCHAR StreamBuffer[256];
} NG_FCB, *PNG_FCB;

typedef struct _NG_DIRENT
{
    ULONGLONG MftNo;
    USHORT NameLength;              /* bytes */
    BOOLEAN IsDot;
    WCHAR Name[ANYSIZE_ARRAY];
} NG_DIRENT, *PNG_DIRENT;

/* Context control block: one per file object. */
typedef struct _NG_CCB
{
    UNICODE_STRING Path;            /* "\\dir\\file[:stream]" as opened, from the volume root */
    PNG_DIRENT *Entries;            /* directory snapshot taken at the first query */
    ULONG EntryCount;
    ULONG EntryCapacity;
    ULONG NextIndex;
    UNICODE_STRING Pattern;         /* upcased search expression */
    BOOLEAN PatternIsStar;
    BOOLEAN Enumerated;
    BOOLEAN AnyReturned;
} NG_CCB, *PNG_CCB;

typedef struct _NG_GLOBAL
{
    PDRIVER_OBJECT DriverObject;
    PDEVICE_OBJECT ControlDevice;
    CACHE_MANAGER_CALLBACKS CacheCallbacks;
    ULONG MaxStackUsed;             /* fill-pattern measurement, whole thread stack */
    ULONG MaxStackAtIo;             /* stack depth when the core issues a device read */
    UCHAR MaxStackMajor;
    LONG FcbLive;
    LONG Opens;
    ULONG PermissiveOpen;           /* diagnostic: grant write access at open, refuse the modification itself */
} NG_GLOBAL;

extern NG_GLOBAL NgGlobal;

/* ntfsng.c */
NTSTATUS NgErrnoToStatus(int Err);
VOID NgAcquireCore(PNG_VCB Vcb);
VOID NgReleaseCore(PNG_VCB Vcb);
VOID NgStackSample(VOID);
PNG_FCB NgAllocateFcb(PNG_VCB Vcb);
VOID NgDereferenceFcb(PNG_FCB Fcb);
VOID NgFillStat(PNG_FCB Fcb);
int NgEnsureNode(PNG_FCB Fcb);
VOID NgParkNode(PNG_FCB Fcb);

/* diag.c */
VOID NgDiagLogRequest(PDEVICE_OBJECT DeviceObject, PIRP Irp, NTSTATUS Status);

/* fsctl.c */
NTSTATUS NgFileSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* create.c */
NTSTATUS NgCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NgCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NgClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* read.c */
NTSTATUS NgRead(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* dirctl.c */
NTSTATUS NgDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
VOID NgFreeDirSnapshot(PNG_CCB Ccb);

/* info.c */
NTSTATUS NgQueryInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NgSetInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NgQueryVolumeInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp);
ULONG NgFileAttributes(PNG_FCB Fcb, const struct ngc_stat *St);
