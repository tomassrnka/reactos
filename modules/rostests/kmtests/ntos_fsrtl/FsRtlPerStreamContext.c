/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the locking of the FsRtl per-stream context routines
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

/* The Windows 8 layout, whatever NTDDI_VERSION this driver is built for */
typedef struct _TEST_FCB_HEADER
{
    FSRTL_COMMON_FCB_HEADER Common;
    PFAST_MUTEX FastMutex;
    LIST_ENTRY FilterContexts;
    ULONG_PTR PushLock;
    PVOID *FileContextSupportPointer;
    PVOID Oplock;
    PVOID ReservedContext;
} TEST_FCB_HEADER, *PTEST_FCB_HEADER;

#ifdef _WIN64
C_ASSERT(FIELD_OFFSET(TEST_FCB_HEADER, PushLock) == 0x48);
#else
C_ASSERT(FIELD_OFFSET(TEST_FCB_HEADER, PushLock) == 0x34);
#endif

#define ADV_HEADER(State) ((PFSRTL_ADVANCED_FCB_HEADER)&(State)->Fcb)

#define MAX_CONTEXTS 2

typedef enum _TEST_LOCK
{
    LockNone,
    LockMutex,
    LockPushLock,
    LockPushLockShared
} TEST_LOCK;

typedef enum _TEST_OP
{
    OpTeardown,
    OpInsert,
    OpRemove,
    OpLookup
} TEST_OP;

static const PCSTR LockNames[] = { "none", "mutex", "pushlock", "pushlock shared" };
static const PCSTR OpNames[] = { "teardown", "insert", "remove", "lookup" };

typedef struct _TEST_CONTEXT
{
    FSRTL_PER_STREAM_CONTEXT Context;
    struct _TEST_STATE *State;
    ULONG Index;
} TEST_CONTEXT, *PTEST_CONTEXT;

typedef struct _TEST_STATE
{
    TEST_FCB_HEADER Fcb;
    FAST_MUTEX Mutex;
    TEST_CONTEXT Contexts[MAX_CONTEXTS];
    KEVENT StartEvent;
    KEVENT ReadyEvent;
    KEVENT DoneEvent;
    BOOLEAN ApcsDisabledInThread;
    BOOLEAN CallerCriticalRegion;
    TEST_OP Op;
    PVOID OpResult;
    NTSTATUS OpStatus;
    BOOLEAN RemoveOtherInCallback;
    PFSRTL_PER_STREAM_CONTEXT RemovedInCallback;
    LONG Calls;
    ULONG Order[MAX_CONTEXTS];
    KIRQL CallbackIrql[MAX_CONTEXTS];
    BOOLEAN MutexFreeInCallback[MAX_CONTEXTS];
    BOOLEAN UnlinkedInCallback[MAX_CONTEXTS];
    BOOLEAN ApcsDisabledInCallback[MAX_CONTEXTS];
    ULONG_PTR PushLockInCallback[MAX_CONTEXTS];
} TEST_STATE, *PTEST_STATE;

static VOID (FASTCALL *pExfAcquirePushLockExclusive)(PEX_PUSH_LOCK PushLock);
static VOID (FASTCALL *pExfReleasePushLockExclusive)(PEX_PUSH_LOCK PushLock);
/* Windows 8 and later; only used on 64-bit, where the calling convention cannot be wrong */
static VOID (NTAPI *pExAcquirePushLockExclusiveEx)(PEX_PUSH_LOCK PushLock, ULONG Flags);
static VOID (NTAPI *pExReleasePushLockExclusiveEx)(PEX_PUSH_LOCK PushLock, ULONG Flags);
static VOID (FASTCALL *pExfAcquirePushLockShared)(PEX_PUSH_LOCK PushLock);
static VOID (FASTCALL *pExfReleasePushLockShared)(PEX_PUSH_LOCK PushLock);
static VOID (NTAPI *pExAcquirePushLockSharedEx)(PEX_PUSH_LOCK PushLock, ULONG Flags);
static VOID (NTAPI *pExReleasePushLockSharedEx)(PEX_PUSH_LOCK PushLock, ULONG Flags);

static
BOOLEAN
CanHold(
    _In_ TEST_LOCK Lock)
{
    if (Lock == LockPushLock)
        return (pExfAcquirePushLockExclusive && pExfReleasePushLockExclusive) ||
               (pExAcquirePushLockExclusiveEx && pExReleasePushLockExclusiveEx);
    if (Lock == LockPushLockShared)
        return (pExfAcquirePushLockShared && pExfReleasePushLockShared) ||
               (pExAcquirePushLockSharedEx && pExReleasePushLockSharedEx);
    return TRUE;
}

static
VOID
AcquireTestLock(
    _Inout_ PTEST_STATE State,
    _In_ TEST_LOCK Lock)
{
    if (Lock == LockMutex)
    {
        ExAcquireFastMutex(&State->Mutex);
    }
    else if (Lock == LockPushLock)
    {
        KeEnterCriticalRegion();
        if (pExfAcquirePushLockExclusive)
            pExfAcquirePushLockExclusive((PEX_PUSH_LOCK)&State->Fcb.PushLock);
        else
            pExAcquirePushLockExclusiveEx((PEX_PUSH_LOCK)&State->Fcb.PushLock, 0);
    }
    else if (Lock == LockPushLockShared)
    {
        KeEnterCriticalRegion();
        if (pExfAcquirePushLockShared)
            pExfAcquirePushLockShared((PEX_PUSH_LOCK)&State->Fcb.PushLock);
        else
            pExAcquirePushLockSharedEx((PEX_PUSH_LOCK)&State->Fcb.PushLock, 0);
    }
}

static
VOID
ReleaseTestLock(
    _Inout_ PTEST_STATE State,
    _In_ TEST_LOCK Lock)
{
    if (Lock == LockMutex)
    {
        ExReleaseFastMutex(&State->Mutex);
    }
    else if (Lock == LockPushLock)
    {
        if (pExfReleasePushLockExclusive)
            pExfReleasePushLockExclusive((PEX_PUSH_LOCK)&State->Fcb.PushLock);
        else
            pExReleasePushLockExclusiveEx((PEX_PUSH_LOCK)&State->Fcb.PushLock, 0);
        KeLeaveCriticalRegion();
    }
    else if (Lock == LockPushLockShared)
    {
        if (pExfReleasePushLockShared)
            pExfReleasePushLockShared((PEX_PUSH_LOCK)&State->Fcb.PushLock);
        else
            pExReleasePushLockSharedEx((PEX_PUSH_LOCK)&State->Fcb.PushLock, 0);
        KeLeaveCriticalRegion();
    }
}

static
VOID
NTAPI
FreeCallback(
    _In_ PVOID Buffer)
{
    PTEST_CONTEXT Context = Buffer;
    PTEST_STATE State = Context->State;
    LONG Call = InterlockedIncrement(&State->Calls) - 1;
    PLIST_ENTRY Head = &State->Fcb.FilterContexts;
    PLIST_ENTRY Entry;
    BOOLEAN Linked = FALSE;

    if (Call >= MAX_CONTEXTS)
        return;

    State->Order[Call] = Context->Index;
    State->CallbackIrql[Call] = KeGetCurrentIrql();
    State->ApcsDisabledInCallback[Call] = KeAreApcsDisabled();
    State->PushLockInCallback[Call] = *(volatile ULONG_PTR *)&State->Fcb.PushLock;

    /* Nothing else runs on this header now; a fast mutex is not recursive */
    State->MutexFreeInCallback[Call] = ExTryToAcquireFastMutex(&State->Mutex);
    if (State->MutexFreeInCallback[Call])
        ExReleaseFastMutex(&State->Mutex);

    for (Entry = Head->Flink; Entry != Head; Entry = Entry->Flink)
    {
        if (Entry == &Context->Context.Links)
            Linked = TRUE;
    }
    State->UnlinkedInCallback[Call] = !Linked;

    /* A teardown that still held the list lock would deadlock here */
    if (State->RemoveOtherInCallback && Call == 0 &&
        State->MutexFreeInCallback[Call] && State->PushLockInCallback[Call] == 0)
    {
        State->RemovedInCallback =
            FsRtlRemovePerStreamContext(ADV_HEADER(State),
                                        State->Contexts[!Context->Index].Context.OwnerId,
                                        NULL);
    }
}

static
VOID
NTAPI
OpThread(
    _In_ PVOID Parameter)
{
    PTEST_STATE State = Parameter;

    KeWaitForSingleObject(&State->StartEvent, Executive, KernelMode, FALSE, NULL);
    if (State->CallerCriticalRegion)
        KeEnterCriticalRegion();
    State->ApcsDisabledInThread = KeAreApcsDisabled();
    KeSetEvent(&State->ReadyEvent, IO_NO_INCREMENT, FALSE);
    switch (State->Op)
    {
        case OpTeardown:
            FsRtlTeardownPerStreamContexts(ADV_HEADER(State));
            break;
        case OpInsert:
            State->OpStatus = FsRtlInsertPerStreamContext(ADV_HEADER(State), &State->Contexts[1].Context);
            break;
        case OpRemove:
            State->OpResult = FsRtlRemovePerStreamContext(ADV_HEADER(State), State->Contexts[0].Context.OwnerId, NULL);
            break;
        case OpLookup:
            State->OpResult = FsRtlLookupPerStreamContextInternal(ADV_HEADER(State), State->Contexts[0].Context.OwnerId, NULL);
            break;
    }
    if (State->CallerCriticalRegion)
        KeLeaveCriticalRegion();
    KeSetEvent(&State->DoneEvent, IO_NO_INCREMENT, FALSE);
}

static
PTEST_STATE
CreateState(
    _In_ BOOLEAN SupportsFilterContexts,
    _In_ UCHAR Version)
{
    PTEST_STATE State;
    ULONG i;

    State = ExAllocatePoolWithTag(NonPagedPool, sizeof(*State), 'tCsP');
    if (!State)
        return NULL;

    RtlZeroMemory(State, sizeof(*State));
    ExInitializeFastMutex(&State->Mutex);
    KeInitializeEvent(&State->StartEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&State->ReadyEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&State->DoneEvent, NotificationEvent, FALSE);

    State->Fcb.Common.NodeByteSize = sizeof(State->Fcb);
    State->Fcb.Common.Flags = FSRTL_FLAG_ADVANCED_HEADER;
    if (SupportsFilterContexts)
        State->Fcb.Common.Flags2 = FSRTL_FLAG2_SUPPORTS_FILTER_CONTEXTS;
    State->Fcb.Common.Version = Version;
    State->Fcb.FastMutex = &State->Mutex;
    InitializeListHead(&State->Fcb.FilterContexts);

    for (i = 0; i < MAX_CONTEXTS; i++)
    {
        State->Contexts[i].State = State;
        State->Contexts[i].Index = i;
        FsRtlInitPerStreamContext(&State->Contexts[i].Context, &State->Contexts[i], NULL, FreeCallback);
        State->Order[i] = MAXULONG;
    }
    return State;
}

/* Runs Op on another thread while this one holds Lock; TRUE if Op did not finish until Lock was released */
static
BOOLEAN
RunOp(
    _Inout_ PTEST_STATE State,
    _In_ TEST_OP Op,
    _In_ TEST_LOCK Lock)
{
    LARGE_INTEGER Timeout;
    PKTHREAD Thread;
    NTSTATUS Status;
    BOOLEAN Blocked;

    State->Op = Op;
    KeClearEvent(&State->StartEvent);
    KeClearEvent(&State->ReadyEvent);
    KeClearEvent(&State->DoneEvent);
    Thread = KmtStartThread(OpThread, State);
    if (!Thread)
        return FALSE;

    AcquireTestLock(State, Lock);
    KeSetEvent(&State->StartEvent, IO_NO_INCREMENT, FALSE);

    /* Measure from the moment the thread is about to call the routine, not from its scheduling */
    Status = KeWaitForSingleObject(&State->ReadyEvent, Executive, KernelMode, FALSE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Short enough to keep the whole test under kmtest's 30-second user-mode request timeout */
    Timeout.QuadPart = -500 * 1000 * 10LL;
    Status = KeWaitForSingleObject(&State->DoneEvent, Executive, KernelMode, FALSE, &Timeout);
    Blocked = (Status == STATUS_TIMEOUT);
    ok(Status == STATUS_SUCCESS || Status == STATUS_TIMEOUT, "Status = 0x%lx\n", Status);

    ReleaseTestLock(State, Lock);

    if (Blocked)
    {
        Timeout.QuadPart = -30 * 1000 * 1000 * 10LL;
        Status = KeWaitForSingleObject(&State->DoneEvent, Executive, KernelMode, FALSE, &Timeout);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    KmtFinishThread(Thread, NULL);
    return Blocked;
}

static
BOOLEAN
MutexIsFree(
    _In_ PTEST_STATE State)
{
    if (!ExTryToAcquireFastMutex(&State->Mutex))
        return FALSE;
    ExReleaseFastMutex(&State->Mutex);
    return TRUE;
}

static
VOID
CheckListEmpty(
    _In_ PTEST_STATE State)
{
    PLIST_ENTRY Head = &State->Fcb.FilterContexts;

    ok_eq_pointer(Head->Flink, Head);
    ok_eq_pointer(Head->Blink, Head);
}

static
VOID
CheckLocksFree(
    _In_ PTEST_STATE State)
{
    ok_bool_true(MutexIsFree(State), "Mutex is free:");
    ok_eq_ulongptr(State->Fcb.PushLock, (ULONG_PTR)0);
}

/* Windows 7 and 10: a V0 header is guarded by the fast mutex, a V1 or later header by its push lock */
static
TEST_LOCK
ExpectedLock(
    _In_ UCHAR Version)
{
    return Version >= FSRTL_FCB_HEADER_V1 ? LockPushLock : LockMutex;
}

static
BOOLEAN
ExpectedBlocked(
    _In_ UCHAR Version,
    _In_ TEST_OP Op,
    _In_ TEST_LOCK Lock)
{
    if (Lock == LockPushLockShared)
        return ExpectedLock(Version) == LockPushLock && Op != OpLookup;
    return Lock == ExpectedLock(Version);
}

static
VOID
TestEmpty(
    _In_ BOOLEAN SupportsFilterContexts,
    _In_ UCHAR Version,
    _In_ TEST_LOCK Lock)
{
    PTEST_STATE State;
    BOOLEAN Blocked;

    if (skip(CanHold(Lock), "No exported push lock routines\n"))
        return;

    State = CreateState(SupportsFilterContexts, Version);
    if (skip(State != NULL, "Out of memory\n"))
        return;

    Blocked = RunOp(State, OpTeardown, Lock);
    trace("teardown empty, filter contexts %d, version %u, held %s: blocked %d\n",
          SupportsFilterContexts, Version, LockNames[Lock], Blocked);
    /* Windows 10 takes no lock for an empty list, Server 2008 R2 does; ReactOS follows Windows 10 */
    if (is_reactos() || GetNTVersion() >= 0x0A00)
        ok(!Blocked, "Teardown of an empty list, filter contexts %d, version %u, held %s: blocked\n",
           SupportsFilterContexts, Version, LockNames[Lock]);
    else if (GetNTVersion() <= 0x0601)
        ok(Blocked == ExpectedBlocked(Version, OpTeardown, Lock),
           "Teardown of an empty list, filter contexts %d, version %u, held %s: blocked %d\n",
           SupportsFilterContexts, Version, LockNames[Lock], Blocked);
    ok_eq_long(State->Calls, 0L);
    CheckListEmpty(State);
    CheckLocksFree(State);

    ExFreePoolWithTag(State, 'tCsP');
}

static
VOID
TestTeardown(
    _In_ UCHAR Version,
    _In_ ULONG Count,
    _In_ TEST_LOCK Lock,
    _In_ BOOLEAN RemoveOtherInCallback)
{
    PTEST_STATE State;
    BOOLEAN Blocked;
    NTSTATUS Status;
    ULONG i;
    LONG ExpectedCalls;

    if (skip(CanHold(Lock), "No exported push lock routines\n"))
        return;

    State = CreateState(TRUE, Version);
    if (skip(State != NULL, "Out of memory\n"))
        return;

    State->RemoveOtherInCallback = RemoveOtherInCallback;
    for (i = 0; i < Count; i++)
    {
        Status = FsRtlInsertPerStreamContext(ADV_HEADER(State), &State->Contexts[i].Context);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    Blocked = RunOp(State, OpTeardown, Lock);
    ExpectedCalls = RemoveOtherInCallback ? 1 : Count;
    trace("teardown %lu, version %u, held %s, remove %d: blocked %d calls %ld order %lu %lu irql %u %u "
          "mutexfree %d %d unlinked %d %d apcsoff %d %d pushlock %p %p removed %p\n",
          Count, Version, LockNames[Lock], RemoveOtherInCallback, Blocked, State->Calls,
          State->Order[0], State->Order[1], State->CallbackIrql[0], State->CallbackIrql[1],
          State->MutexFreeInCallback[0], State->MutexFreeInCallback[1],
          State->UnlinkedInCallback[0], State->UnlinkedInCallback[1],
          State->ApcsDisabledInCallback[0], State->ApcsDisabledInCallback[1],
          (PVOID)State->PushLockInCallback[0], (PVOID)State->PushLockInCallback[1], State->RemovedInCallback);

    ok(Blocked == ExpectedBlocked(Version, OpTeardown, Lock),
       "Teardown of %lu contexts, version %u, held %s: blocked %d\n", Count, Version, LockNames[Lock], Blocked);
    ok_eq_long(State->Calls, ExpectedCalls);
    for (i = 0; i < (ULONG)ExpectedCalls && i < MAX_CONTEXTS; i++)
    {
        ok(State->CallbackIrql[i] == PASSIVE_LEVEL, "Callback %lu IRQL %u\n", i, State->CallbackIrql[i]);
        ok(State->ApcsDisabledInCallback[i] == State->ApcsDisabledInThread,
           "Callback %lu APCs disabled %d, thread %d\n", i, State->ApcsDisabledInCallback[i], State->ApcsDisabledInThread);
        ok(State->UnlinkedInCallback[i], "Callback %lu called with its entry still linked\n", i);
        /* A lock the teardown ignores is still held by the test while the callback runs */
        if ((Lock != LockPushLock && Lock != LockPushLockShared) || ExpectedLock(Version) == LockPushLock)
            ok(State->PushLockInCallback[i] == 0, "Callback %lu called with the push lock held\n", i);
        if (Lock != LockMutex || ExpectedLock(Version) == LockMutex)
            ok(State->MutexFreeInCallback[i], "Callback %lu called with the mutex held\n", i);
    }
    if (Count == 2)
    {
        /* Insertion is at the head, so the last inserted context comes first */
        ok_eq_ulong(State->Order[0], 1UL);
        if (RemoveOtherInCallback)
            ok_eq_pointer(State->RemovedInCallback, &State->Contexts[0].Context);
        else
            ok_eq_ulong(State->Order[1], 0UL);
    }
    CheckListEmpty(State);
    CheckLocksFree(State);

    ExFreePoolWithTag(State, 'tCsP');
}

static
VOID
TestListOp(
    _In_ UCHAR Version,
    _In_ TEST_OP Op,
    _In_ TEST_LOCK Lock)
{
    PTEST_STATE State;
    BOOLEAN Blocked;
    NTSTATUS Status;

    if (skip(CanHold(Lock), "No exported push lock routines\n"))
        return;

    State = CreateState(TRUE, Version);
    if (skip(State != NULL, "Out of memory\n"))
        return;

    Status = FsRtlInsertPerStreamContext(ADV_HEADER(State), &State->Contexts[0].Context);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Blocked = RunOp(State, Op, Lock);
    trace("%s, version %u, held %s: blocked %d\n", OpNames[Op], Version, LockNames[Lock], Blocked);
    ok(Blocked == ExpectedBlocked(Version, Op, Lock),
       "%s, version %u, held %s: blocked %d\n", OpNames[Op], Version, LockNames[Lock], Blocked);

    switch (Op)
    {
        case OpInsert:
            ok_eq_hex(State->OpStatus, STATUS_SUCCESS);
            ok_eq_pointer(State->Fcb.FilterContexts.Flink, &State->Contexts[1].Context.Links);
            break;
        case OpRemove:
            ok_eq_pointer(State->OpResult, &State->Contexts[0].Context);
            CheckListEmpty(State);
            break;
        case OpLookup:
            ok_eq_pointer(State->OpResult, &State->Contexts[0].Context);
            break;
        default:
            break;
    }
    CheckLocksFree(State);

    ExFreePoolWithTag(State, 'tCsP');
}

/* The callback runs in the caller's critical region */
static
VOID
TestTeardownInCriticalRegion(
    _In_ UCHAR Version)
{
    PTEST_STATE State;
    BOOLEAN Blocked;
    NTSTATUS Status;

    State = CreateState(TRUE, Version);
    if (skip(State != NULL, "Out of memory\n"))
        return;

    State->CallerCriticalRegion = TRUE;
    Status = FsRtlInsertPerStreamContext(ADV_HEADER(State), &State->Contexts[0].Context);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Blocked = RunOp(State, OpTeardown, LockNone);
    trace("teardown in a critical region, version %u: blocked %d calls %ld apcsoff %d irql %u\n",
          Version, Blocked, State->Calls, State->ApcsDisabledInCallback[0], State->CallbackIrql[0]);
    ok(!Blocked, "Teardown blocked with no lock held\n");
    ok_eq_long(State->Calls, 1L);
    ok_bool_true(State->ApcsDisabledInThread, "APCs disabled in the thread:");
    ok_bool_true(State->ApcsDisabledInCallback[0], "APCs disabled in the callback:");
    ok(State->CallbackIrql[0] == PASSIVE_LEVEL, "Callback IRQL %u\n", State->CallbackIrql[0]);
    CheckListEmpty(State);
    CheckLocksFree(State);

    ExFreePoolWithTag(State, 'tCsP');
}

/* A context linked by hand into a header that does not claim filter context support */
static
VOID
TestUnsupportedWithContext(
    _In_ UCHAR Version)
{
    PTEST_STATE State;
    BOOLEAN Blocked;

    State = CreateState(FALSE, Version);
    if (skip(State != NULL, "Out of memory\n"))
        return;

    InsertHeadList(&State->Fcb.FilterContexts, &State->Contexts[0].Context.Links);
    Blocked = RunOp(State, OpTeardown, LockNone);
    trace("teardown unsupported with a context, version %u: blocked %d calls %ld\n", Version, Blocked, State->Calls);
    ok(!Blocked, "Teardown blocked with no lock held\n");
    ok_eq_long(State->Calls, 1L);
    CheckListEmpty(State);
    CheckLocksFree(State);

    ExFreePoolWithTag(State, 'tCsP');
}

START_TEST(FsRtlPerStreamContext)
{
    UCHAR Version;
    TEST_LOCK Lock;

    pExfAcquirePushLockExclusive = KmtGetSystemRoutineAddress(L"ExfAcquirePushLockExclusive");
    pExfReleasePushLockExclusive = KmtGetSystemRoutineAddress(L"ExfReleasePushLockExclusive");
#ifdef _WIN64
    pExAcquirePushLockExclusiveEx = KmtGetSystemRoutineAddress(L"ExAcquirePushLockExclusiveEx");
    pExReleasePushLockExclusiveEx = KmtGetSystemRoutineAddress(L"ExReleasePushLockExclusiveEx");
#endif
    pExfAcquirePushLockShared = KmtGetSystemRoutineAddress(L"ExfAcquirePushLockShared");
    pExfReleasePushLockShared = KmtGetSystemRoutineAddress(L"ExfReleasePushLockShared");
#ifdef _WIN64
    pExAcquirePushLockSharedEx = KmtGetSystemRoutineAddress(L"ExAcquirePushLockSharedEx");
    pExReleasePushLockSharedEx = KmtGetSystemRoutineAddress(L"ExReleasePushLockSharedEx");
#endif
    trace("push lock routines: Exf %d %d %d %d, Ex %d %d %d %d\n",
          pExfAcquirePushLockExclusive != NULL, pExfReleasePushLockExclusive != NULL,
          pExfAcquirePushLockShared != NULL, pExfReleasePushLockShared != NULL,
          pExAcquirePushLockExclusiveEx != NULL, pExReleasePushLockExclusiveEx != NULL,
          pExAcquirePushLockSharedEx != NULL, pExReleasePushLockSharedEx != NULL);

    for (Version = FSRTL_FCB_HEADER_V0; Version <= 2; Version++)
    {
        for (Lock = LockMutex; Lock <= LockPushLockShared; Lock++)
        {
            TestEmpty(TRUE, Version, Lock);
            TestEmpty(FALSE, Version, Lock);
            TestTeardown(Version, 1, Lock, FALSE);
            TestListOp(Version, OpInsert, Lock);
            TestListOp(Version, OpRemove, Lock);
            TestListOp(Version, OpLookup, Lock);
        }
        TestTeardown(Version, 1, LockNone, FALSE);
        TestTeardown(Version, 2, LockNone, FALSE);
        TestTeardown(Version, 2, LockNone, TRUE);
        TestTeardownInCriticalRegion(Version);
        TestUnsupportedWithContext(Version);
    }
}
