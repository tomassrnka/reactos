/*
 * PROJECT:     Partition manager driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     I/O statistics: IOCTL_DISK_PERFORMANCE for disks and partitions
 *
 * Every read and write that passes a disk (the FDO) or a partition (its PDO) is counted: bytes, requests,
 * the time they took, the number in flight, and the time with nothing in flight. Windows partmgr keeps the
 * same record and answers IOCTL_DISK_PERFORMANCE with it (\\.\PhysicalDriveN reports "Partmgr");
 * pdh and PerfMon read the PhysicalDisk counters from it.
 *
 * A partition's I/O passes its PDO and then the disk's FDO, so a disk counts everything done on it,
 * including what its partitions did.
 */

#include "partmgr.h"

/* A high resolution clock in units of 100 ns (the unit of the times in DISK_PERFORMANCE) */
static
ULONGLONG
PartMgrPerfNow(VOID)
{
    LARGE_INTEGER freq;
    LARGE_INTEGER counter = KeQueryPerformanceCounter(&freq);

    return (ULONGLONG)(counter.QuadPart / freq.QuadPart) * 10000000ULL +
           (ULONGLONG)(counter.QuadPart % freq.QuadPart) * 10000000ULL / (ULONGLONG)freq.QuadPart;
}

VOID
PartMgrIoStatsInit(
    _Out_ PPARTMGR_IO_STATS Stats)
{
    RtlZeroMemory(Stats, sizeof(*Stats));
    KeInitializeSpinLock(&Stats->Lock);
    Stats->IdleSince = PartMgrPerfNow();
}

typedef struct _PARTMGR_IO_RECORD
{
    ULONGLONG Start;
} PARTMGR_IO_RECORD;

static
IO_COMPLETION_ROUTINE
PartMgrIoStatsComplete;

static
NTSTATUS
NTAPI
PartMgrIoStatsComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    /* Our own stack location: the start time was left in its (otherwise unused) ByteOffset */
    PIO_STACK_LOCATION ioStack = IoGetCurrentIrpStackLocation(Irp);
    PPARTMGR_IO_STATS stats = Context;
    ULONGLONG now = PartMgrPerfNow();
    ULONGLONG elapsed = now - (ULONGLONG)ioStack->Parameters.Read.ByteOffset.QuadPart;
    ULONG length = ioStack->Parameters.Read.Length;
    BOOLEAN isWrite = (ioStack->MajorFunction == IRP_MJ_WRITE);
    KIRQL oldIrql;

    UNREFERENCED_PARAMETER(DeviceObject);

    KeAcquireSpinLock(&stats->Lock, &oldIrql);
    if (isWrite)
    {
        stats->BytesWritten += length;
        stats->WriteTime += elapsed;
        stats->WriteCount++;
    }
    else
    {
        stats->BytesRead += length;
        stats->ReadTime += elapsed;
        stats->ReadCount++;
    }
    if (--stats->QueueDepth == 0)
        stats->IdleSince = now;
    KeReleaseSpinLock(&stats->Lock, oldIrql);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);
    return STATUS_SUCCESS;
}

/*
 * Counts the request in Stats and sends it on: the caller has set up the next stack location. The
 * counting ends in the completion routine.
 */
NTSTATUS
PartMgrIoStatsCallDriver(
    _In_ PPARTMGR_IO_STATS Stats,
    _In_ PDEVICE_OBJECT LowerDevice,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION ioStack = IoGetCurrentIrpStackLocation(Irp);
    ULONGLONG now = PartMgrPerfNow();
    KIRQL oldIrql;

    KeAcquireSpinLock(&Stats->Lock, &oldIrql);
    if (Stats->QueueDepth++ == 0)
        Stats->IdleTime += now - Stats->IdleSince;
    KeReleaseSpinLock(&Stats->Lock, oldIrql);

    ioStack->Parameters.Read.ByteOffset.QuadPart = (LONGLONG)now;
    IoSetCompletionRoutine(Irp, PartMgrIoStatsComplete, Stats, TRUE, TRUE, TRUE);
    return IoCallDriver(LowerDevice, Irp);
}

NTSTATUS
PartMgrIoctlDiskPerformance(
    _In_ PPARTMGR_IO_STATS Stats,
    _In_ ULONG StorageDeviceNumber,
    _In_ PIRP Irp)
{
    static const WCHAR managerName[8] = L"Partmgr "; /* eight characters, blank padded, no terminator */
    PDISK_PERFORMANCE out = Irp->AssociatedIrp.SystemBuffer;
    ULONGLONG now = PartMgrPerfNow();
    LARGE_INTEGER systemTime;
    KIRQL oldIrql;

    if (!VerifyIrpOutBufferSize(Irp, sizeof(DISK_PERFORMANCE)))
    {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(out, sizeof(*out));

    KeAcquireSpinLock(&Stats->Lock, &oldIrql);
    out->BytesRead.QuadPart = Stats->BytesRead;
    out->BytesWritten.QuadPart = Stats->BytesWritten;
    out->ReadTime.QuadPart = Stats->ReadTime;
    out->WriteTime.QuadPart = Stats->WriteTime;
    out->IdleTime.QuadPart = Stats->IdleTime;
    if (Stats->QueueDepth == 0)
        out->IdleTime.QuadPart += now - Stats->IdleSince;
    out->ReadCount = Stats->ReadCount;
    out->WriteCount = Stats->WriteCount;
    out->QueueDepth = Stats->QueueDepth;
    out->SplitCount = Stats->SplitCount;
    KeReleaseSpinLock(&Stats->Lock, oldIrql);

    KeQuerySystemTime(&systemTime);
    out->QueryTime = systemTime;
    out->StorageDeviceNumber = StorageDeviceNumber;
    RtlCopyMemory(out->StorageManagerName, managerName, sizeof(managerName));

    Irp->IoStatus.Information = sizeof(DISK_PERFORMANCE);
    return STATUS_SUCCESS;
}
