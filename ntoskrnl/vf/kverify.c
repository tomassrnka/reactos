/*
 * PROJECT:     ReactOS Kernel (fork-only verifier extensions)
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Kernel Verifier Extensions - core: option parsing, one-shot
 *              reporting, IRQL validation, the per-tag pool-leak sampler and
 *              the deadlock watchdog.
 *
 * Everything here is gated at build time by CONFIG_KERNEL_VERIFIER and at run
 * time by KvFlags (the VERIFIER= boot option). It is fork-only and is never
 * proposed upstream.
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#if defined(CONFIG_KERNEL_VERIFIER)

/* kverify.h is pulled in by <ntoskrnl.h> (via internal includes). */

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

/* PoolTrackTableSize is a private pool.c global; declare it here. */
extern SIZE_T PoolTrackTableSize;

/* Lock-order checker init (kvlockdep.c). */
extern BOOLEAN NTAPI KvLockdepInit(VOID);

/* GLOBALS *******************************************************************/

ULONG KvFlags = 0;
ULONG KvDeadlockSeconds = 30;   /* default watchdog threshold, seconds */

/* Pool-leak sampler tunables */
#define KV_LEAK_STREAK  10      /* consecutive growing samples before we warn */
#define KV_LEAK_MIN     512     /* ignore tags below this many outstanding     */
#define KV_LEAK_PERIOD  5       /* seconds between samples                      */

/* Deadlock watchdog scan period */
#define KV_WATCH_PERIOD 2       /* seconds between thread scans                 */

/* One-shot report table: open-addressed set of site keys. */
#define KV_ONCE_SLOTS   4096
static PVOID KvOnceTable[KV_ONCE_SLOTS];

/* Per-tag history for the leak sampler, allocated to PoolTrackTableSize. */
typedef struct _KV_LEAK_SLOT
{
    LONG Prev;          /* previous outstanding count   */
    ULONG Streak;       /* consecutive growing samples  */
    ULONG Peak;         /* max outstanding seen         */
    BOOLEAN Reported;   /* warned already               */
} KV_LEAK_SLOT, *PKV_LEAK_SLOT;

static PKV_LEAK_SLOT KvLeakHistory = NULL;
static BOOLEAN KvThreadsStarted = FALSE;

/* All-CPU stack dump buffers, filled by the IPI broadcast worker. */
#define KV_MAX_CPUS     64
#define KV_BT_FRAMES    24
static PVOID KvCpuBackTrace[KV_MAX_CPUS][KV_BT_FRAMES];
static ULONG KvCpuBtCount[KV_MAX_CPUS];

/* ONE-SHOT REPORTING ********************************************************/

BOOLEAN
NTAPI
KvLogOnce(IN PVOID SiteKey)
{
    ULONG Index = (ULONG)(((ULONG_PTR)SiteKey >> 4) & (KV_ONCE_SLOTS - 1));
    ULONG Probe;

    for (Probe = 0; Probe < KV_ONCE_SLOTS; Probe++)
    {
        PVOID Current = KvOnceTable[Index];
        if (Current == SiteKey)
        {
            /* Already reported. */
            return FALSE;
        }
        if (Current == NULL)
        {
            /* Try to claim this slot. */
            if (InterlockedCompareExchangePointer(&KvOnceTable[Index],
                                                  SiteKey, NULL) == NULL)
            {
                return TRUE;
            }
            /* Lost the race; re-read this slot. */
            Probe--;
            continue;
        }
        Index = (Index + 1) & (KV_ONCE_SLOTS - 1);
    }

    /* Table full: report to be safe rather than silently drop. */
    return TRUE;
}

VOID
__cdecl
KvReport(IN PCSTR Feature, IN PVOID SiteKey, IN PCSTR Format, ...)
{
    char Buffer[256];
    va_list Args;
    int Len;

    if (SiteKey != NULL && !KvLogOnce(SiteKey))
        return;

    va_start(Args, Format);
    Len = _vsnprintf(Buffer, sizeof(Buffer) - 1, Format, Args);
    va_end(Args);
    if (Len < 0) Len = sizeof(Buffer) - 1;
    Buffer[Len] = '\0';

    /* The campaign harness greps for the "KVERIFY:" prefix. */
    DbgPrint("KVERIFY: [%s] %s (site %p)\n", Feature, Buffer, SiteKey);
}

/* (b) IRQL VALIDATION *******************************************************/

VOID
NTAPI
KvCheckIrqlMaxImpl(IN KIRQL Max, IN PCSTR Api, IN PVOID Site)
{
    KIRQL Current = KeGetCurrentIrql();
    if (Current > Max)
    {
        KvReport("IRQL", Site, "%s called at IRQL %u, documented max %u",
                 Api, Current, Max);
    }
}

/* (e) DEADLOCK WATCHDOG *****************************************************/

/*
 * Walk the frame-pointer chain by hand into Buffer, bounded to the current
 * kernel stack so a bad frame pointer can never fault. RtlWalkFrameChain
 * returns nothing above DISPATCH_LEVEL, so it is unusable from the IPI worker;
 * this reads only memory and works at any IRQL (ReactOS keeps frame pointers,
 * -fno-omit-frame-pointer).
 */
static
ULONG
KvCaptureStack(PVOID *Buffer, ULONG Max)
{
    PKTHREAD Thread = KeGetCurrentThread();
    ULONG_PTR Lo = (ULONG_PTR)Thread->StackLimit;
    ULONG_PTR Hi = (ULONG_PTR)Thread->StackBase;
    ULONG_PTR Prev = 0;
    void **Fp = (void **)__builtin_frame_address(0);
    ULONG Count = 0;

    while (Count < Max)
    {
        ULONG_PTR Cur = (ULONG_PTR)Fp;
        if ((Cur & (sizeof(void *) - 1)) != 0) break;       /* misaligned     */
        if (Cur < Lo || Cur + 2 * sizeof(void *) > Hi) break; /* off-stack    */
        if (Cur <= Prev) break;                             /* not ascending  */
        Buffer[Count++] = Fp[1];                            /* return address */
        Prev = Cur;
        Fp = (void **)Fp[0];                                /* saved frame ptr*/
    }
    return Count;
}

static
ULONG_PTR
NTAPI
KvCaptureCpuBroadcast(IN ULONG_PTR Context)
{
    /*
     * Runs on every processor at IPI level with all processors synchronized
     * (KeIpiGenericCall). We only touch our own slot and never take a lock,
     * so there is no deadlock risk. Printing is deferred to the caller.
     */
    ULONG Cpu = KeGetCurrentProcessorNumber();
    UNREFERENCED_PARAMETER(Context);

    if (Cpu < KV_MAX_CPUS)
    {
        KvCpuBtCount[Cpu] = KvCaptureStack(&KvCpuBackTrace[Cpu][0],
                                           KV_BT_FRAMES);
    }
    return 0;
}

static
VOID
KvDumpAllCpuStacks(VOID)
{
    ULONG Cpu, Frame;

    /* Sentinel: the broadcast worker overwrites only the slots that exist, so
     * we need no processor count (whose declaration is config-dependent). */
    RtlFillMemory(KvCpuBtCount, sizeof(KvCpuBtCount), 0xFF);
    KeIpiGenericCall(KvCaptureCpuBroadcast, 0);

    for (Cpu = 0; Cpu < KV_MAX_CPUS; Cpu++)
    {
        if (KvCpuBtCount[Cpu] == 0xFFFFFFFF)
            continue;
        DbgPrint("KVERIFY: [DEADLOCK] CPU %lu stack (%lu frames):\n",
                 Cpu, KvCpuBtCount[Cpu]);
        for (Frame = 0; Frame < KvCpuBtCount[Cpu]; Frame++)
            DbgPrint("KVERIFY:     %p\n", KvCpuBackTrace[Cpu][Frame]);
    }
}

static
ULONG
KvTicksToSeconds(ULONG Ticks)
{
    /* KeMaximumIncrement is 100ns units per tick. */
    ULONGLONG Hundreds = (ULONGLONG)Ticks * KeMaximumIncrement;
    return (ULONG)(Hundreds / 10000000ULL);
}

static
BOOLEAN
KvIsLockWait(UCHAR WaitReason)
{
    /*
     * Allowlist the wait reasons that mean "blocked on a kernel lock", rather
     * than blocklisting the benign ones: ReactOS uses the plain reasons
     * (DelayExecution, UserRequest, Executive) for ordinary long waits, so a
     * blocklist would flag every Sleep() and idle worker as a deadlock.
     */
    switch (WaitReason)
    {
        case WrExecutive:
        case WrResource:
        case WrPushLock:
        case WrMutex:
        case WrFastMutex:
        case WrGuardedMutex:
            return TRUE;
        default:
            return FALSE;
    }
}

static
VOID
NTAPI
KvWatchdogThread(IN PVOID Context)
{
    LARGE_INTEGER Interval;
    UNREFERENCED_PARAMETER(Context);

    Interval.QuadPart = -(LONGLONG)KV_WATCH_PERIOD * 10000000LL;

    for (;;)
    {
        PEPROCESS Process;

        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
        if (!KvEnabled(KV_DEADLOCK))
            continue;

        /*
         * Enumerate with the reference-counted helpers so we never follow a
         * freed process or thread: each returned object is referenced and the
         * list walk is serialized inside the helper. Runs at PASSIVE_LEVEL.
         */
        Process = PsGetNextProcess(NULL);
        while (Process != NULL)
        {
            PETHREAD Thread = PsGetNextProcessThread(Process, NULL);
            while (Thread != NULL)
            {
                PKTHREAD Tcb = &Thread->Tcb;
                /* Only kernel-mode waits on a lock count; a guarded mutex waits
                 * in the GateWait state rather than Waiting. */
                if ((Tcb->State == Waiting || Tcb->State == GateWait) &&
                    Tcb->WaitMode == KernelMode &&
                    KvIsLockWait(Tcb->WaitReason))
                {
                    LARGE_INTEGER Tick;
                    LONG Age;

                    /* WaitTime is the low 32 bits of the tick count; subtract in
                     * 32 bits so the comparison is wrap-safe, and read the tick
                     * per thread so a wait that began during the scan is not
                     * seen as ancient. */
                    KeQueryTickCount(&Tick);
                    Age = (LONG)((ULONG)Tick.LowPart - Tcb->WaitTime);
                    if (Age > 0)
                    {
                        ULONG Secs = KvTicksToSeconds((ULONG)Age);
                        /* Key the one-shot on the thread and this wait episode so
                         * a later wait on the same thread is still reported, and
                         * report every distinct stuck thread (do not stop at the
                         * first). */
                        PVOID Key = (PVOID)((ULONG_PTR)Thread ^
                                            (ULONG_PTR)Tcb->WaitTime);
                        if (Secs >= KvDeadlockSeconds && KvLogOnce(Key))
                        {
                            DbgPrint("KVERIFY: [DEADLOCK] thread %p (pid %p "
                                     "tid %p) waiting %lu s, WaitReason %u\n",
                                     Thread, PsGetProcessId(Process),
                                     Thread->Cid.UniqueThread, Secs,
                                     Tcb->WaitReason);
                            KvDumpAllCpuStacks();
                        }
                    }
                }
                Thread = PsGetNextProcessThread(Process, Thread);
            }
            Process = PsGetNextProcess(Process);
        }
    }
}

/* (d) POOL-LEAK SAMPLER *****************************************************/

VOID
NTAPI
KvPoolLeakSample(VOID)
{
    SIZE_T i;

    if (KvLeakHistory == NULL || PoolTrackTable == NULL)
        return;

    for (i = 0; i < PoolTrackTableSize; i++)
    {
        PPOOL_TRACKER_TABLE Entry = &PoolTrackTable[i];
        PKV_LEAK_SLOT Slot = &KvLeakHistory[i];
        LONG Outstanding;

        if (Entry->Key == 0)
            continue;

        Outstanding = (Entry->NonPagedAllocs - Entry->NonPagedFrees) +
                      (Entry->PagedAllocs - Entry->PagedFrees);

        if (Outstanding > Slot->Prev)
            Slot->Streak++;
        else
            Slot->Streak = 0;
        Slot->Prev = Outstanding;
        if (Outstanding > (LONG)Slot->Peak)
            Slot->Peak = Outstanding;

        if (!Slot->Reported &&
            Slot->Streak >= KV_LEAK_STREAK &&
            Outstanding >= KV_LEAK_MIN)
        {
            char Tag[5];
            Tag[0] = (char)(Entry->Key);
            Tag[1] = (char)(Entry->Key >> 8);
            Tag[2] = (char)(Entry->Key >> 16);
            Tag[3] = (char)(Entry->Key >> 24);
            Tag[4] = '\0';
            Slot->Reported = TRUE;
            DbgPrint("KVERIFY: [POOLLEAK] tag '%s' outstanding %ld grew for "
                     "%lu consecutive samples (peak %lu)\n",
                     Tag, Outstanding, Slot->Streak, Slot->Peak);
        }
    }
}

static
VOID
NTAPI
KvPoolLeakThread(IN PVOID Context)
{
    LARGE_INTEGER Interval;
    UNREFERENCED_PARAMETER(Context);

    Interval.QuadPart = -(LONGLONG)KV_LEAK_PERIOD * 10000000LL;

    for (;;)
    {
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
        if (KvEnabled(KV_POOLLEAK))
            KvPoolLeakSample();
    }
}

/* INITIALISATION ************************************************************/

static
VOID
KvParseOption(IN PCHAR CommandLine)
{
    PCHAR Opt, Value;

    /* CommandLine is already upper-cased by the caller. */
    Opt = strstr(CommandLine, "VERIFIER");
    if (Opt == NULL)
        return;

    Value = strstr(Opt, "=");
    if (Value == NULL)
    {
        /* Bare VERIFIER with no list: enable the cheap, safe features. */
        KvFlags = KV_IRQL | KV_POOLLEAK | KV_DEADLOCK;
        return;
    }
    Value++;

    if (strstr(Value, "IRQL"))     KvFlags |= KV_IRQL;
    if (strstr(Value, "LOCKDEP"))  KvFlags |= KV_LOCKDEP;
    if (strstr(Value, "POOLLEAK")) KvFlags |= KV_POOLLEAK;
    if (strstr(Value, "DEADLOCK")) KvFlags |= KV_DEADLOCK;
    if (strstr(Value, "UBSAN"))    KvFlags |= KV_UBSAN;
    if (strstr(Value, "ALL"))
        KvFlags = KV_IRQL | KV_LOCKDEP | KV_POOLLEAK | KV_DEADLOCK | KV_UBSAN;

    /* Optional DEADLOCK=N seconds. */
    Opt = strstr(Value, "DEADLOCK=");
    if (Opt != NULL)
    {
        ULONG N = (ULONG)atol(Opt + sizeof("DEADLOCK=") - 1);
        if (N != 0) KvDeadlockSeconds = N;
    }
}

VOID
NTAPI
KvInitialize(IN ULONG Phase, IN PVOID LoaderBlock)
{
    PLOADER_PARAMETER_BLOCK Block = (PLOADER_PARAMETER_BLOCK)LoaderBlock;

    if (Phase == 0)
    {
        if (Block != NULL && Block->LoadOptions != NULL)
            KvParseOption((PCHAR)Block->LoadOptions);

        if (KvFlags != 0)
            DbgPrint("KVERIFY: enabled, flags 0x%lx, deadlock %lu s\n",
                     KvFlags, KvDeadlockSeconds);
        return;
    }

    /* Phase 1: start the worker threads once. They idle unless their feature
     * bit is set, so they are cheap even in a verifier image run without
     * POOLLEAK/DEADLOCK. */
    if (KvThreadsStarted || KvFlags == 0)
        return;
    KvThreadsStarted = TRUE;

    /* The phase-0 banner can be lost before the debugger serial is up; print
     * it again here, where the debug channel is live. */
    DbgPrint("KVERIFY: active, flags 0x%lx, deadlock %lu s\n",
             KvFlags, KvDeadlockSeconds);

    if ((KvFlags & KV_LOCKDEP) && !KvLockdepInit())
    {
        KvFlags &= ~KV_LOCKDEP;
        DbgPrint("KVERIFY: lockdep allocation failed, feature disabled\n");
    }

    if (KvFlags & KV_POOLLEAK)
    {
        SIZE_T Size = PoolTrackTableSize * sizeof(KV_LEAK_SLOT);
        KvLeakHistory = ExAllocatePoolWithTag(NonPagedPool, Size, 'LreV');
        if (KvLeakHistory != NULL)
            RtlZeroMemory(KvLeakHistory, Size);
    }

    {
        HANDLE Handle;
        if (NT_SUCCESS(PsCreateSystemThread(&Handle, THREAD_ALL_ACCESS, NULL,
                                            NULL, NULL, KvPoolLeakThread, NULL)))
            ZwClose(Handle);
        if (NT_SUCCESS(PsCreateSystemThread(&Handle, THREAD_ALL_ACCESS, NULL,
                                            NULL, NULL, KvWatchdogThread, NULL)))
            ZwClose(Handle);
    }
}

#endif /* CONFIG_KERNEL_VERIFIER */
