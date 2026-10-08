/*
 * PROJECT:     ReactOS Kernel (fork-only verifier extensions)
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Lock-order checker in the spirit of Linux lockdep. Records the
 *              acquisition-order pairs seen between lock classes and reports a
 *              cycle (an A-before-B / B-before-A inversion) the first time the
 *              new edge would close one. Design follows the public description
 *              of lockdep; no code is taken from it.
 *
 * A lock class is the acquisition call site. Each context (a processor for
 * spin and queued spin locks, a thread for fast mutexes and push locks) keeps
 * a small stack of the locks it currently holds. On acquire we add an edge
 * from every held class to the new class; the first time a given edge is new
 * we check whether the new class can already reach the held class, which would
 * mean the two have been taken in both orders.
 *
 * Gated at build time by CONFIG_KERNEL_VERIFIER and at run time by
 * VERIFIER=LOCKDEP (KvFlags & KV_LOCKDEP).
 */

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#if defined(CONFIG_KERNEL_VERIFIER)

#define KV_MAX_CPUS     64
#define KV_LD_CTX       4096                 /* per-CPU slots then a thread hash */
#define KV_LD_DEPTH     24                   /* held locks tracked per context   */
#define KV_LD_CLASSES   2048                 /* distinct acquisition sites       */
#define KV_LD_WORDS     (KV_LD_CLASSES / 32) /* adjacency row width in ULONGs    */

typedef struct _KV_LD_HELD
{
    PVOID Lock;
    USHORT ClassId;
} KV_LD_HELD;

typedef struct _KV_LD_CONTEXT
{
    PVOID Owner;        /* thread for the hashed slots; NULL for per-CPU slots */
    ULONG Depth;
    KV_LD_HELD Held[KV_LD_DEPTH];
} KV_LD_CONTEXT;

/* Tables allocated once by KvLockdepInit. */
static KV_LD_CONTEXT *KvLdCtx = NULL;        /* [KV_LD_CTX] */
static PVOID *KvLdClassSite = NULL;          /* [KV_LD_CLASSES] site per class  */
static ULONG *KvLdEdges = NULL;              /* [KV_LD_CLASSES][KV_LD_WORDS]     */
static ULONG *KvLdVisited = NULL;            /* [KV_LD_WORDS] BFS scratch        */
static USHORT *KvLdQueue = NULL;             /* [KV_LD_CLASSES] BFS scratch      */
static ULONG KvLdNextClass = 1;              /* class 0 means "none"             */
static BOOLEAN KvLdReady = FALSE;

/*
 * The graph lock is a raw test-and-set lock, not a KSPIN_LOCK: lockdep for a
 * queued spin lock runs above DISPATCH_LEVEL, where KeAcquireSpinLock would
 * assert, and it must never be routed back through the lockdep hooks. Hold
 * times are short and the hot path only takes it on a genuinely new edge.
 */
static volatile LONG KvLdGraphLock = 0;
static VOID KvLdEnter(VOID)
{
    while (InterlockedCompareExchange(&KvLdGraphLock, 1, 0) != 0)
        YieldProcessor();
}
static VOID KvLdLeave(VOID) { InterlockedExchange(&KvLdGraphLock, 0); }

/* INIT *********************************************************************/

BOOLEAN
NTAPI
KvLockdepInit(VOID)
{
    SIZE_T CtxSize = (SIZE_T)KV_LD_CTX * sizeof(KV_LD_CONTEXT);
    SIZE_T EdgeSize = (SIZE_T)KV_LD_CLASSES * KV_LD_WORDS * sizeof(ULONG);

    KvLdCtx = ExAllocatePoolWithTag(NonPagedPool, CtxSize, 'DLvK');
    KvLdClassSite = ExAllocatePoolWithTag(NonPagedPool,
                                          KV_LD_CLASSES * sizeof(PVOID), 'DLvK');
    KvLdEdges = ExAllocatePoolWithTag(NonPagedPool, EdgeSize, 'DLvK');
    KvLdVisited = ExAllocatePoolWithTag(NonPagedPool,
                                        KV_LD_WORDS * sizeof(ULONG), 'DLvK');
    KvLdQueue = ExAllocatePoolWithTag(NonPagedPool,
                                      KV_LD_CLASSES * sizeof(USHORT), 'DLvK');

    if (!KvLdCtx || !KvLdClassSite || !KvLdEdges || !KvLdVisited || !KvLdQueue)
        return FALSE;

    RtlZeroMemory(KvLdCtx, CtxSize);
    RtlZeroMemory(KvLdClassSite, KV_LD_CLASSES * sizeof(PVOID));
    RtlZeroMemory(KvLdEdges, EdgeSize);
    KvLdReady = TRUE;
    return TRUE;
}

/* GRAPH ********************************************************************/

static USHORT
KvLdClassForSite(PVOID Site)
{
    /* Direct-mapped hash of the site with linear probing. Runs under the
     * graph lock, so the class table is consistent. */
    ULONG Index = (ULONG)(((ULONG_PTR)Site >> 4) & (KV_LD_CLASSES - 1));
    ULONG Probe;

    for (Probe = 0; Probe < KV_LD_CLASSES; Probe++)
    {
        if (KvLdClassSite[Index] == Site)
            return (USHORT)Index;
        if (KvLdClassSite[Index] == NULL)
        {
            if (KvLdNextClass >= KV_LD_CLASSES)
                return 0;               /* table full: stop assigning classes */
            KvLdClassSite[Index] = Site;
            KvLdNextClass++;
            return (USHORT)Index;
        }
        Index = (Index + 1) & (KV_LD_CLASSES - 1);
    }
    return 0;
}

static VOID KvLdSetBit(ULONG *Row, ULONG Bit) { Row[Bit >> 5] |= (1UL << (Bit & 31)); }
static BOOLEAN KvLdTestBit(ULONG *Row, ULONG Bit) { return (Row[Bit >> 5] >> (Bit & 31)) & 1; }
static ULONG *KvLdRow(ULONG Class) { return &KvLdEdges[(SIZE_T)Class * KV_LD_WORDS]; }

/* Can From reach To by following edges? BFS under the graph lock. */
static BOOLEAN
KvLdReaches(ULONG From, ULONG To)
{
    ULONG Head = 0, Tail = 0, i, bit;

    RtlZeroMemory(KvLdVisited, KV_LD_WORDS * sizeof(ULONG));
    KvLdQueue[Tail++] = (USHORT)From;
    KvLdSetBit(KvLdVisited, From);

    while (Head < Tail)
    {
        ULONG Node = KvLdQueue[Head++];
        ULONG *Row = KvLdRow(Node);
        if (Node == To)
            return TRUE;
        for (i = 0; i < KV_LD_WORDS; i++)
        {
            ULONG Word = Row[i];
            while (Word)
            {
                bit = (i << 5) + (ULONG)__builtin_ctz(Word);
                Word &= Word - 1;
                if (!KvLdTestBit(KvLdVisited, bit))
                {
                    KvLdSetBit(KvLdVisited, bit);
                    if (bit == To) return TRUE;
                    if (Tail < KV_LD_CLASSES) KvLdQueue[Tail++] = (USHORT)bit;
                }
            }
        }
    }
    return FALSE;
}

/* CONTEXT ******************************************************************/

static KV_LD_CONTEXT *
KvLdGetContext(KV_LOCK_KIND Kind, BOOLEAN ForAcquire)
{
    if (Kind == KvLockSpin || Kind == KvLockQueued)
    {
        ULONG Cpu = KeGetCurrentProcessorNumber();
        if (Cpu >= KV_MAX_CPUS) return NULL;
        return &KvLdCtx[Cpu];                 /* per-CPU, lock-free */
    }
    else
    {
        /* Per-thread, direct-mapped into the slots above the CPU range. */
        PVOID Thread = KeGetCurrentThread();
        ULONG Slot = KV_MAX_CPUS +
            (ULONG)(((ULONG_PTR)Thread >> 5) % (KV_LD_CTX - KV_MAX_CPUS));
        KV_LD_CONTEXT *Ctx = &KvLdCtx[Slot];
        if (Ctx->Owner == Thread)
            return Ctx;
        if (ForAcquire && Ctx->Owner == NULL &&
            InterlockedCompareExchangePointer(&Ctx->Owner, Thread, NULL) == NULL)
            return Ctx;
        return NULL;                          /* slot busy: best-effort skip */
    }
}

/* HOOKS ********************************************************************/

VOID
NTAPI
KvLockAcquireImpl(IN PVOID Lock, IN KV_LOCK_KIND Kind, IN PVOID Site)
{
    KV_LD_CONTEXT *Ctx;
    USHORT NewClass;
    ULONG i;

    if (!KvLdReady) return;

    Ctx = KvLdGetContext(Kind, TRUE);
    if (Ctx == NULL || Ctx->Depth >= KV_LD_DEPTH)
        return;                               /* too deep: stop tracking here */

    KvLdEnter();

    NewClass = KvLdClassForSite(Site);
    if (NewClass != 0)
    {
        for (i = 0; i < Ctx->Depth; i++)
        {
            USHORT Held = Ctx->Held[i].ClassId;
            if (Held == 0 || Held == NewClass)
                continue;
            if (!KvLdTestBit(KvLdRow(Held), NewClass))
            {
                /* New ordering Held -> NewClass. If NewClass already reaches
                 * Held, the two locks have been taken in both orders. */
                if (KvLdReaches(NewClass, Held))
                {
                    KvLdLeave();
                    KvReport("LOCKDEP", Site,
                             "lock-order inversion: class %p taken while holding "
                             "%p, but the reverse order was seen before",
                             KvLdClassSite[NewClass], KvLdClassSite[Held]);
                    KvLdEnter();
                }
                KvLdSetBit(KvLdRow(Held), NewClass);
            }
        }
    }

    Ctx->Held[Ctx->Depth].Lock = Lock;
    Ctx->Held[Ctx->Depth].ClassId = NewClass;
    Ctx->Depth++;
    KvLdLeave();
}

VOID
NTAPI
KvLockReleaseImpl(IN PVOID Lock, IN KV_LOCK_KIND Kind)
{
    KV_LD_CONTEXT *Ctx;
    ULONG i;

    if (!KvLdReady) return;

    Ctx = KvLdGetContext(Kind, FALSE);
    if (Ctx == NULL || Ctx->Depth == 0) return;

    /* Remove the matching lock (usually the top; handle out-of-order too). */
    for (i = Ctx->Depth; i > 0; i--)
    {
        if (Ctx->Held[i - 1].Lock == Lock)
        {
            ULONG j;
            for (j = i - 1; j + 1 < Ctx->Depth; j++)
                Ctx->Held[j] = Ctx->Held[j + 1];
            Ctx->Depth--;
            break;
        }
    }

    if (Ctx->Depth == 0 && Ctx->Owner != NULL &&
        Ctx->Owner == KeGetCurrentThread())
    {
        Ctx->Owner = NULL;                    /* release the per-thread slot */
    }
}

#endif /* CONFIG_KERNEL_VERIFIER */
