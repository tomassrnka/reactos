/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NT implementation of the ngos bridge used by the Linux-API shim
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ntfsng.h"
#include "../shim/include/ngos.h"

C_ASSERT(sizeof(KEVENT) <= sizeof(struct ngos_ev));
C_ASSERT(sizeof(KSPIN_LOCK) == sizeof(uintptr_t));
C_ASSERT(NGP_STAGES == NG_PROFILE_STAGES);

NG_PROFILE NgProfile;

unsigned long long ngos_ticks(void)
{
    return __rdtsc();
}

void ngos_prof(int id, unsigned long long since, unsigned long long bytes)
{
    NG_PROF_STAT *S = &NgProfile.Stage[id];
    S->Count++;
    S->Ticks += __rdtsc() - since;
    S->Bytes += bytes;
}

void *ngos_alloc(size_t n)
{
    return ExAllocatePoolWithTag(NonPagedPool, n, TAG_NTFSNG_CORE);
}

void ngos_free(void *p)
{
    if (p)
        ExFreePoolWithTag(p, TAG_NTFSNG_CORE);
}

void ngos_print(const char *s)
{
    DbgPrint("%s", s);
}

void ngos_bugcheck(const char *what, const char *file, int line)
{
    DbgPrint("ntfsng: fatal: %s (%s:%d)\n", what, file, line);
    KeBugCheckEx(FILE_SYSTEM, (ULONG_PTR)what, (ULONG_PTR)file, (ULONG_PTR)line, 0x4e544e47);
}

void ngos_ev_init(struct ngos_ev *ev, int synchronization, int signaled)
{
    KeInitializeEvent((PKEVENT)ev, synchronization ? SynchronizationEvent : NotificationEvent,
                      signaled ? TRUE : FALSE);
}

void ngos_ev_set(struct ngos_ev *ev)
{
    KeSetEvent((PKEVENT)ev, IO_NO_INCREMENT, FALSE);
}

void ngos_ev_clear(struct ngos_ev *ev)
{
    KeClearEvent((PKEVENT)ev);
}

void ngos_ev_wait(struct ngos_ev *ev)
{
    KeWaitForSingleObject(ev, Executive, KernelMode, FALSE, NULL);
}

int ngos_ev_trywait(struct ngos_ev *ev)
{
    LARGE_INTEGER Zero;
    Zero.QuadPart = 0;
    return KeWaitForSingleObject(ev, Executive, KernelMode, FALSE, &Zero) == STATUS_SUCCESS;
}

int ngos_ev_state(struct ngos_ev *ev)
{
    return KeReadStateEvent((PKEVENT)ev) != 0;
}

unsigned char ngos_spin_lock(uintptr_t *lock)
{
    KIRQL Old;
    KeAcquireSpinLock((PKSPIN_LOCK)lock, &Old);
    return Old;
}

void ngos_spin_unlock(uintptr_t *lock, unsigned char irql)
{
    KeReleaseSpinLock((PKSPIN_LOCK)lock, irql);
}

void ngos_yield(void)
{
    LARGE_INTEGER Delay;
    if (KeGetCurrentIrql() >= DISPATCH_LEVEL)
        return;
    Delay.QuadPart = -10000; /* 1 ms */
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
}

void ngos_sleep_ms(unsigned int ms)
{
    LARGE_INTEGER Delay;
    Delay.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
}

unsigned long ngos_jiffies(void)
{
    LARGE_INTEGER Ticks;
    KeQueryTickCount(&Ticks);
    return (unsigned long)Ticks.QuadPart;
}

void ngos_time(long long *sec, long *nsec)
{
    LARGE_INTEGER Now;
    ULONGLONG Unix100ns;
    KeQuerySystemTime(&Now);
    Unix100ns = (ULONGLONG)Now.QuadPart - 116444736000000000ULL;
    *sec = (long long)(Unix100ns / 10000000ULL);
    *nsec = (long)((Unix100ns % 10000000ULL) * 100);
}

static IO_COMPLETION_ROUTINE NgReadCompletion;
static NTSTATUS NTAPI NgReadCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/*
 * Transfers to or from the volume's storage device.  The IRP is freed here rather than
 * by the I/O manager, so completion needs no APC: core I/O can happen on a paging-I/O
 * path entered at APC_LEVEL, where IoBuildSynchronousFsdRequest's APC-based completion
 * would never run.
 */
static int NgDevIo(UCHAR Major, PDEVICE_OBJECT Device, unsigned long long off, void *buf, unsigned int len)
{
    LARGE_INTEGER Offset;
    IO_STATUS_BLOCK Iosb;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;
    ULONGLONG T0 = __rdtsc();

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Offset.QuadPart = (LONGLONG)off;
    Irp = IoBuildAsynchronousFsdRequest(Major, Device, Major == IRP_MJ_FLUSH_BUFFERS ? NULL : buf,
                                        Major == IRP_MJ_FLUSH_BUFFERS ? 0 : len,
                                        Major == IRP_MJ_FLUSH_BUFFERS ? NULL : &Offset, &Iosb);
    if (!Irp)
        return -1;
    IoGetNextIrpStackLocation(Irp)->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    if (Major == IRP_MJ_WRITE)
        IoGetNextIrpStackLocation(Irp)->Flags |= SL_WRITE_THROUGH;
    IoSetCompletionRoutine(Irp, NgReadCompletion, &Event, TRUE, TRUE, TRUE);
    Status = IoCallDriver(Device, Irp);
    if (Status == STATUS_PENDING)
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    Status = Irp->IoStatus.Status;
    if (Irp->Flags & IRP_BUFFERED_IO)
    {
        /* Completion stopped before the I/O manager's copy-back: do it here. */
        if (NT_SUCCESS(Status) && Major == IRP_MJ_READ)
            RtlCopyMemory(buf, Irp->AssociatedIrp.SystemBuffer, len);
        if (Irp->Flags & IRP_DEALLOCATE_BUFFER)
            ExFreePool(Irp->AssociatedIrp.SystemBuffer);
        Irp->AssociatedIrp.SystemBuffer = NULL;
    }
    if (Irp->MdlAddress)
    {
        MmUnlockPages(Irp->MdlAddress);
        IoFreeMdl(Irp->MdlAddress);
        Irp->MdlAddress = NULL;
    }
    IoFreeIrp(Irp);
    ngos_prof(Major == IRP_MJ_READ ? NGP_DEV_READ : Major == IRP_MJ_WRITE ? NGP_DEV_WRITE : NGP_DEV_FLUSH, T0,
              Major == IRP_MJ_FLUSH_BUFFERS ? 0 : len);
    NgStackSample();
    if (!NT_SUCCESS(Status))
    {
        /* A disk without a write cache may not implement flush. */
        if (Major == IRP_MJ_FLUSH_BUFFERS && (Status == STATUS_INVALID_DEVICE_REQUEST || Status == STATUS_NOT_SUPPORTED ||
                                             Status == STATUS_NOT_IMPLEMENTED))
            return 0;
        DPRINT1("ntfsng: device %s at %I64u len %u failed 0x%lx\n",
                Major == IRP_MJ_READ ? "read" : Major == IRP_MJ_WRITE ? "write" : "flush", off, len, Status);
        return -1;
    }
    return 0;
}

int ngos_dev_read(void *dev, unsigned long long off, void *buf, unsigned int len)
{
    return NgDevIo(IRP_MJ_READ, dev, off, buf, len);
}

int ngos_dev_write(void *dev, unsigned long long off, void *buf, unsigned int len)
{
    return NgDevIo(IRP_MJ_WRITE, dev, off, buf, len);
}

int ngos_dev_flush(void *dev)
{
    return NgDevIo(IRP_MJ_FLUSH_BUFFERS, dev, 0, NULL, 0);
}
