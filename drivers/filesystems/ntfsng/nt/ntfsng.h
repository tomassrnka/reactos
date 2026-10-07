/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NT glue around the vendored Linux fs/ntfs core
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
    BOOLEAN ReadOnly;               /* mounted read-only: policy, registry or request */
    BOOLEAN WriteThrough;           /* after IRP_MJ_SHUTDOWN: every change ends with a full sync */
    LIST_ENTRY GlobalLinks;         /* NgGlobal.VcbList */
    PKTHREAD Flusher;               /* writes back core metadata every NG_FLUSH_PERIOD_MS */
    PNOTIFY_SYNC NotifySync;        /* directory change notification (FsRtl) */
    LIST_ENTRY DirNotifyList;
    KEVENT FlusherStop;
    ULONG Syncs;
    ULONG NonCachedViaCache;            /* non-cached writes sent through Cc: a view could not be purged */
} NG_VCB, *PNG_VCB;

#define NG_FLUSH_PERIOD_MS 2000

/* File control block: one per (MFT record, stream) open on a volume. */
typedef struct _NG_FCB
{
    FSRTL_COMMON_FCB_HEADER Header; /* must be first: FsContext */
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    LONGLONG CachedEnd;             /* highest AllocationSize the header has had: bounds Mm's pages */
    LONGLONG LogicalVdl;            /* valid data length as SetFileValidData sees it (writes raise it) */
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
    BOOLEAN Modified;               /* data changed since the last time update (cleanup/flush) */
    BOOLEAN UserSetWriteTime;       /* LastWriteTime set through FileBasicInformation: keep it */
    BOOLEAN DeletePending;          /* unlink at the last cleanup */
    BOOLEAN Deleted;                /* unlinked: paging writes are dropped */
    ULONGLONG DelParentMftNo;       /* the name the delete removes (from the handle that asked) */
    USHORT DelNameLength;           /* in WCHARs */
    WCHAR DelName[256];
    UNICODE_STRING DelPath;         /* full path for the change notification */
    FILE_LOCK FileLock;             /* byte-range locks (FsRtl) */
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
    ULONGLONG ParentMftNo;          /* directory holding the name this handle was opened by */
    USHORT NameLength;              /* that name as stored on disk, in WCHARs (0 for the root) */
    WCHAR Name[256];
    BOOLEAN DeleteOnClose;
    BOOLEAN AppendOnly;             /* opened with FILE_APPEND_DATA but not FILE_WRITE_DATA */
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
    ULONG ForceReadOnly;            /* "ReadOnly" DWORD in the service key: mount every volume read-only */
    FAST_MUTEX VcbListLock;
    LIST_ENTRY VcbList;
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
PNG_FCB NgFindFcb(PNG_VCB Vcb, ULONGLONG MftNo);
VOID NgUnlistFcb(PNG_FCB Fcb);
VOID NgSetDeletePending(PNG_FCB Fcb, PNG_CCB Ccb);
BOOLEAN NgValidName(PCUNICODE_STRING Name);
VOID NgNotify(PNG_VCB Vcb, PCUNICODE_STRING Path, ULONG Filter, ULONG Action);
NTSTATUS NgCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NgClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* read.c */
NTSTATUS NgRead(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* write.c */
NTSTATUS NgWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NgFlushBuffers(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NgShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NgSetFileSize(PNG_FCB Fcb, PFILE_OBJECT FileObject, LONGLONG NewSize);
VOID NgFlushVolume(PNG_VCB Vcb);
VOID NgFlushStream(PNG_FCB Fcb, PIO_STATUS_BLOCK Iosb);
BOOLEAN NgPurgeFrom(PNG_FCB Fcb, LONGLONG Start);
BOOLEAN NgPurgeForNonCached(PNG_FCB Fcb, LONGLONG Offset);
VOID NgAfterChange(PNG_VCB Vcb);
VOID NgApplyModified(PNG_FCB Fcb);
NTSTATUS NgStartFlusher(PNG_VCB Vcb);
BOOLEAN NTAPI NgAcquireForLazyWrite(PVOID Context, BOOLEAN Wait);
VOID NTAPI NgReleaseFromLazyWrite(PVOID Context);
BOOLEAN NTAPI NgAcquireForReadAhead(PVOID Context, BOOLEAN Wait);
VOID NTAPI NgReleaseFromReadAhead(PVOID Context);

/* dirctl.c */
NTSTATUS NgDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
VOID NgFreeDirSnapshot(PNG_CCB Ccb);

/* lock control */
NTSTATUS NgLockControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* info.c */
NTSTATUS NgQueryInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NgSetInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NgQueryVolumeInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp);
ULONG NgFileAttributes(PNG_FCB Fcb, const struct ngc_stat *St);
