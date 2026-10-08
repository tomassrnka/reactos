/*
 * PROJECT:     ReactOS Kernel (fork-only verifier extensions)
 * LICENSE:     GPL-2.0-or-later (BSD for files that were already BSD)
 * PURPOSE:     Kernel Verifier Extensions - deliberate bug-finding layer.
 *
 * Build gate:   CONFIG_KERNEL_VERIFIER  (CMake option KERNEL_VERIFIER).
 *               When undefined, every hook below compiles to nothing.
 * Runtime gate: KvFlags, parsed from the "VERIFIER=" boot option. Every
 *               feature is off by default even in a verifier build, so one
 *               image can run with any subset of features selected per boot.
 *
 * Boot option:  VERIFIER=IRQL,LOCKDEP,POOLLEAK,DEADLOCK,UBSAN[,DEADLOCK=N]
 *               (special pool is driven by the separate SPECIALPOOL= option,
 *               handled in the memory manager).
 */

#pragma once

#if defined(CONFIG_KERNEL_VERIFIER)

/* Feature bits in KvFlags */
#define KV_IRQL      0x0001UL   /* (b) IRQL validation on common APIs       */
#define KV_LOCKDEP   0x0002UL   /* (c) lock-order (lockdep-style) checker   */
#define KV_POOLLEAK  0x0004UL   /* (d) per-tag pool-leak sampler            */
#define KV_DEADLOCK  0x0008UL   /* (e) wait-too-long deadlock watchdog      */
#define KV_UBSAN     0x0010UL   /* (f) undefined-behaviour sanitizer log    */

extern ULONG KvFlags;
extern ULONG KvDeadlockSeconds;

/* Lock classes understood by the lock-order checker. */
typedef enum _KV_LOCK_KIND
{
    KvLockSpin = 0,
    KvLockQueued,
    KvLockFastMutex,
    KvLockPushLock,
    KvLockKindMax
} KV_LOCK_KIND;

/* --- core (kverify.c) --- */
VOID NTAPI KvInitialize(IN ULONG Phase, IN PVOID LoaderBlock);
BOOLEAN NTAPI KvLogOnce(IN PVOID SiteKey);
VOID __cdecl KvReport(IN PCSTR Feature, IN PVOID SiteKey, IN PCSTR Format, ...);

/* (b) IRQL validation - logs once per call site, never bugchecks. */
VOID NTAPI KvCheckIrqlMaxImpl(IN KIRQL Max, IN PCSTR Api, IN PVOID Site);

/* (c) lockdep engine (kvlockdep.c). */
VOID NTAPI KvLockAcquireImpl(IN PVOID Lock, IN KV_LOCK_KIND Kind, IN PVOID Site);
VOID NTAPI KvLockReleaseImpl(IN PVOID Lock, IN KV_LOCK_KIND Kind);

/* (d) pool-leak sampler step - called by the sampler thread. */
VOID NTAPI KvPoolLeakSample(VOID);

/* (f) UBSan reports are queued by the handlers and printed here (kvubsan.c). */
VOID NTAPI KvUbsanDrain(VOID);
VOID NTAPI KvUbsanInitDrain(VOID);

FORCEINLINE BOOLEAN KvEnabled(ULONG Feature)
{
    return (BOOLEAN)((KvFlags & Feature) != 0);
}

/*
 * Fast hooks placed on hot paths. When the feature bit is clear they cost a
 * single global load and a predicted-not-taken branch, so a verifier image
 * run without LOCKDEP/DEADLOCK/IRQL is close to a normal build.
 */
FORCEINLINE VOID KvCheckIrqlMax(KIRQL Max, PCSTR Api, PVOID Site)
{
    if (KvFlags & KV_IRQL) KvCheckIrqlMaxImpl(Max, Api, Site);
}
FORCEINLINE VOID KvLockAcquire(PVOID Lock, KV_LOCK_KIND Kind, PVOID Site)
{
    if (KvFlags & KV_LOCKDEP) KvLockAcquireImpl(Lock, Kind, Site);
}
FORCEINLINE VOID KvLockRelease(PVOID Lock, KV_LOCK_KIND Kind)
{
    if (KvFlags & KV_LOCKDEP) KvLockReleaseImpl(Lock, Kind);
}
#else /* !CONFIG_KERNEL_VERIFIER */

#define KvFlags 0UL
#define KvEnabled(f) (FALSE)
#define KvInitialize(Phase, Block) ((void)0)
#define KvCheckIrqlMax(Max, Api, Site) ((void)0)
#define KvLockAcquire(Lock, Kind, Site) ((void)0)
#define KvLockRelease(Lock, Kind) ((void)0)

#endif /* CONFIG_KERNEL_VERIFIER */
