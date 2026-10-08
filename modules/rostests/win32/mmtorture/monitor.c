/*
 * PROJECT:     ReactOS memory manager torture suite
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Orchestrator: starts the workers, samples commit, pool by tag
 *              and working sets every minute, detects stalls, and compares
 *              the state after the workers ended with the baseline
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "mmtorture.h"

#define MAX_WORKERS 32
#define MAX_TAGS 4096

typedef struct _SAMPLE
{
    ULONG Avail, Commit, Limit, Peak, PagedPages, NonPagedPages, FreePtes;
    ULONG Processes, Threads, Handles;
    ULONG TagCount;
    SYSTEM_POOLTAG Tags[MAX_TAGS];
} SAMPLE;

typedef struct _WORKER
{
    char Command[256];
    WCHAR CommandW[256];
    HANDLE Process;
    ULONG Restarts;
} WORKER;

static SAMPLE Baseline, Current;
static WORKER Workers[MAX_WORKERS];
static ULONG WorkerCount;
static PVOID InfoBuffer;
static ULONG InfoBufferSize = 1024 * 1024;

static NTSTATUS
QueryInfo(SYSTEM_INFORMATION_CLASS Class)
{
    NTSTATUS Status;
    ULONG Needed;

    for (;;)
    {
        if (!InfoBuffer)
        {
            InfoBuffer = VirtualAlloc(NULL, InfoBufferSize, MEM_COMMIT, PAGE_READWRITE);
            if (!InfoBuffer)
                return STATUS_NO_MEMORY;
        }
        Needed = 0;
        Status = NtQuerySystemInformation(Class, InfoBuffer, InfoBufferSize, &Needed);
        if (Status != STATUS_INFO_LENGTH_MISMATCH)
            return Status;
        VirtualFree(InfoBuffer, 0, MEM_RELEASE);
        InfoBuffer = NULL;
        InfoBufferSize = max(InfoBufferSize * 2, Needed + 65536);
    }
}

static BOOL
IsTestProcess(PUNICODE_STRING Name)
{
    static const WCHAR *Names[] = { L"mmtorture.exe", L"goalloc.exe", L"mmtchild.exe" };
    ULONG i;

    if (!Name->Buffer)
        return FALSE;
    for (i = 0; i < _countof(Names); i++)
    {
        if (!_wcsnicmp(Name->Buffer, Names[i], Name->Length / sizeof(WCHAR)) &&
            wcslen(Names[i]) == Name->Length / sizeof(WCHAR))
        {
            return TRUE;
        }
    }
    return FALSE;
}

static VOID
TakeSample(SAMPLE *S, ULONG Seconds, BOOL PrintProcesses)
{
    SYSTEM_PERFORMANCE_INFORMATION Perf;
    PSYSTEM_PROCESS_INFORMATION Proc;
    NTSTATUS Status;

    ZeroMemory(S, FIELD_OFFSET(SAMPLE, Tags));
    Status = NtQuerySystemInformation(SystemPerformanceInformation, &Perf, sizeof(Perf), NULL);
    if (NT_SUCCESS(Status))
    {
        S->Avail = Perf.AvailablePages;
        S->Commit = Perf.CommittedPages;
        S->Limit = Perf.CommitLimit;
        S->Peak = Perf.PeakCommitment;
        S->PagedPages = Perf.PagedPoolPages;
        S->NonPagedPages = Perf.NonPagedPoolPages;
        S->FreePtes = Perf.FreeSystemPtes;
    }
    else
    {
        MmtLog("perf query failed %08lx", Status);
    }

    Status = QueryInfo(SystemProcessInformation);
    if (NT_SUCCESS(Status))
    {
        Proc = InfoBuffer;
        for (;;)
        {
            S->Processes++;
            S->Threads += Proc->NumberOfThreads;
            S->Handles += Proc->HandleCount;
            if (PrintProcesses && Proc->UniqueProcessId && !IsTestProcess(&Proc->ImageName))
            {
                MmtLog("PR t=%lu pid=%lu name=%.*S ws=%lu pf=%lu priv=%lu hnd=%lu thr=%lu",
                       Seconds, HandleToUlong(Proc->UniqueProcessId),
                       (int)(Proc->ImageName.Length / sizeof(WCHAR)), Proc->ImageName.Buffer,
                       (ULONG)(Proc->WorkingSetSize / 1024), (ULONG)(Proc->PagefileUsage / 1024),
                       (ULONG)(Proc->PrivatePageCount / 1024), Proc->HandleCount, Proc->NumberOfThreads);
            }
            if (!Proc->NextEntryOffset)
                break;
            Proc = (PSYSTEM_PROCESS_INFORMATION)((PUCHAR)Proc + Proc->NextEntryOffset);
        }
    }

    Status = QueryInfo(SystemPoolTagInformation);
    if (NT_SUCCESS(Status))
    {
        PSYSTEM_POOLTAG_INFORMATION Info = InfoBuffer;
        S->TagCount = min(Info->Count, MAX_TAGS);
        memcpy(S->Tags, Info->TagInfo, S->TagCount * sizeof(SYSTEM_POOLTAG));
    }
    else
    {
        MmtLog("pool tag query failed %08lx", Status);
    }
}

static LONG
SumOps(VOID)
{
    LONG i, Sum = 0, Count = min(MmtShared->SlotCount, MMT_MAX_SLOTS);

    for (i = 0; i < Count; i++)
        Sum += MmtShared->Slot[i].Ops;
    return Sum;
}

static VOID
TagName(ULONG Tag, char *Out)
{
    ULONG i;

    for (i = 0; i < 4; i++)
    {
        char c = (char)((Tag >> (i * 8)) & 0xFF);
        Out[i] = (c >= 0x20 && c < 0x7F && c != ' ') ? c : '_';
    }
    Out[4] = 0;
}

static VOID
PrintSample(const char *Label, SAMPLE *S, ULONG Seconds, BOOL AllTags)
{
    ULONG i;
    char Tag[5];

    MmtLog("S %s t=%lu avail=%lu commit=%lu limit=%lu peak=%lu ppool=%lu nppool=%lu ptes=%lu procs=%lu thr=%lu hnd=%lu ops=%ld fail=%ld children=%ld childfail=%ld",
           Label, Seconds, S->Avail, S->Commit, S->Limit, S->Peak, S->PagedPages, S->NonPagedPages,
           S->FreePtes, S->Processes, S->Threads, S->Handles, SumOps(), MmtShared->Failures,
           MmtShared->Children, MmtShared->ChildFailures);
    for (i = 0; i < S->TagCount; i++)
    {
        SYSTEM_POOLTAG *T = &S->Tags[i];
        if (!AllTags && T->PagedUsed + T->NonPagedUsed < 64 * 1024)
            continue;
        TagName(T->TagUlong, Tag);
        MmtLog("T t=%lu tag=%s np=%lu npa=%lu p=%lu pa=%lu",
               Seconds, Tag, (ULONG)T->NonPagedUsed, T->NonPagedAllocs - T->NonPagedFrees,
               (ULONG)T->PagedUsed, T->PagedAllocs - T->PagedFrees);
    }
}

static SYSTEM_POOLTAG *
FindTag(SAMPLE *S, ULONG Tag)
{
    ULONG i;

    for (i = 0; i < S->TagCount; i++)
    {
        if (S->Tags[i].TagUlong == Tag)
            return &S->Tags[i];
    }
    return NULL;
}

static VOID
CompareWithBaseline(ULONG Seconds)
{
    ULONG i;
    char Tag[5];

    MmtLog("D t=%lu commit=%ld avail=%ld ppool=%ld nppool=%ld ptes=%ld procs=%ld thr=%ld hnd=%ld",
           Seconds, (LONG)(Current.Commit - Baseline.Commit), (LONG)(Current.Avail - Baseline.Avail),
           (LONG)(Current.PagedPages - Baseline.PagedPages),
           (LONG)(Current.NonPagedPages - Baseline.NonPagedPages),
           (LONG)(Current.FreePtes - Baseline.FreePtes),
           (LONG)(Current.Processes - Baseline.Processes), (LONG)(Current.Threads - Baseline.Threads),
           (LONG)(Current.Handles - Baseline.Handles));
    for (i = 0; i < Current.TagCount; i++)
    {
        SYSTEM_POOLTAG *T = &Current.Tags[i], *B = FindTag(&Baseline, Current.Tags[i].TagUlong);
        LONG DeltaBytes, DeltaAllocs;

        DeltaBytes = (LONG)(T->PagedUsed + T->NonPagedUsed) - (B ? (LONG)(B->PagedUsed + B->NonPagedUsed) : 0);
        DeltaAllocs = (LONG)((T->PagedAllocs - T->PagedFrees) + (T->NonPagedAllocs - T->NonPagedFrees)) -
                      (B ? (LONG)((B->PagedAllocs - B->PagedFrees) + (B->NonPagedAllocs - B->NonPagedFrees)) : 0);
        if (DeltaBytes > 16 * 1024 || DeltaAllocs > 64)
        {
            TagName(T->TagUlong, Tag);
            MmtLog("DT t=%lu tag=%s bytes=%ld allocs=%ld", Seconds, Tag, DeltaBytes, DeltaAllocs);
        }
    }
}

static BOOL
StartWorker(WORKER *W)
{
    if (!MmtSpawn(W->CommandW, FALSE, 0, NULL, &W->Process))
    {
        MmtFail("cannot start worker '%s': %lu", W->Command, GetLastError());
        W->Process = NULL;
        return FALSE;
    }
    MmtLog("worker started: %s", W->Command);
    return TRUE;
}

int
MmtMonitorMain(int argc, char **argv)
{
    ULONG Seconds = MmtArgUlong(argc, argv, 2, 600);
    ULONG StallSeconds = MmtArgUlong(argc, argv, 3, 300);
    ULONG Start, Now, Elapsed, LastSample = 0, LastProgressTick, LastOpsTick, i;
    LONG LastOps = -1, Ops;
    BOOL Stalled = FALSE, Stopping = FALSE;
    LONG SlotStalled[MMT_MAX_SLOTS] = {0};

    if (!MmtOpenShared(TRUE))
    {
        MmtFail("cannot create the shared section: %lu", GetLastError());
        return 1;
    }
    for (i = 4; (int)i < argc && WorkerCount < MAX_WORKERS; i++)
    {
        WORKER *W = &Workers[WorkerCount++];
        lstrcpynA(W->Command, argv[i], sizeof(W->Command));
        MultiByteToWideChar(CP_ACP, 0, W->Command, -1, W->CommandW, _countof(W->CommandW));
    }

    MmtLog("run seconds=%lu stall=%lu workers=%lu", Seconds, StallSeconds, WorkerCount);
    Sleep(15000);
    TakeSample(&Baseline, 0, TRUE);
    PrintSample("base", &Baseline, 0, TRUE);

    for (i = 0; i < WorkerCount; i++)
        StartWorker(&Workers[i]);

    Start = GetTickCount();
    LastProgressTick = LastOpsTick = Start;
    for (;;)
    {
        Sleep(10000);
        Now = GetTickCount();
        Elapsed = (Now - Start) / 1000;
        Ops = SumOps();
        MmtLog("HB t=%lu ops=%ld fail=%ld", Elapsed, Ops, MmtShared->Failures);

        if (Ops != LastOps)
        {
            LastOps = Ops;
            LastOpsTick = Now;
            if (Stalled)
                MmtLog("STALL-END t=%lu", Elapsed);
            Stalled = FALSE;
        }
        else if (!Stalled && (Now - LastOpsTick) / 1000 >= StallSeconds)
        {
            Stalled = TRUE;
            MmtLog("STALL t=%lu no worker progress for %lu s", Elapsed, (Now - LastOpsTick) / 1000);
        }

        /* Per-worker stalls */
        for (i = 0; i < (ULONG)min(MmtShared->SlotCount, MMT_MAX_SLOTS); i++)
        {
            MMT_SLOT *Slot = &MmtShared->Slot[i];
            if (!Slot->InUse)
                continue;
            if ((ULONG)(Now - (ULONG)Slot->LastOpTick) / 1000 >= StallSeconds)
            {
                if (!SlotStalled[i])
                    MmtLog("SLOTSTALL t=%lu slot=%lu name=%s pid=%ld ops=%ld", Elapsed, i, Slot->Name, Slot->ProcessId, Slot->Ops);
                SlotStalled[i] = 1;
            }
            else if (SlotStalled[i])
            {
                MmtLog("SLOTSTALL-END t=%lu slot=%lu name=%s", Elapsed, i, Slot->Name);
                SlotStalled[i] = 0;
            }
        }

        /* Workers that ended early: count a failure and restart them */
        for (i = 0; i < WorkerCount && !Stopping; i++)
        {
            DWORD Code;
            WORKER *W = &Workers[i];
            if (!W->Process || WaitForSingleObject(W->Process, 0) != WAIT_OBJECT_0)
                continue;
            GetExitCodeProcess(W->Process, &Code);
            CloseHandle(W->Process);
            W->Process = NULL;
            MmtFail("worker '%s' ended early with %08lx", W->Command, Code);
            if (W->Restarts++ < 20)
                StartWorker(W);
        }

        if (Elapsed - LastSample >= 60)
        {
            BOOL Full = (Elapsed / 60) % 10 == 0;
            LastSample = Elapsed;
            TakeSample(&Current, Elapsed, Full);
            PrintSample("run", &Current, Elapsed, Full);
        }

        if (Elapsed >= Seconds)
            break;
        (void)LastProgressTick;
    }

    /* Stop the workers and wait for them */
    Stopping = TRUE;
    MmtLog("stopping t=%lu", (GetTickCount() - Start) / 1000);
    SetEvent(MmtStopEvent);
    for (i = 0; i < WorkerCount; i++)
    {
        DWORD Code;
        WORKER *W = &Workers[i];
        if (!W->Process)
            continue;
        if (WaitForSingleObject(W->Process, 240000) != WAIT_OBJECT_0)
        {
            MmtFail("worker '%s' did not stop within 240 s", W->Command);
            TerminateProcess(W->Process, 0xDEAD);
            WaitForSingleObject(W->Process, 60000);
        }
        GetExitCodeProcess(W->Process, &Code);
        if (Code != 0)
            MmtFail("worker '%s' exited with %08lx", W->Command, Code);
        CloseHandle(W->Process);
        W->Process = NULL;
    }

    /* Let deferred work settle, then compare with the baseline */
    Sleep(60000);
    Elapsed = (GetTickCount() - Start) / 1000;
    TakeSample(&Current, Elapsed, TRUE);
    PrintSample("final", &Current, Elapsed, TRUE);
    CompareWithBaseline(Elapsed);
    MmtLog("DONE t=%lu failures=%ld", Elapsed, MmtShared->Failures);
    return MmtShared->Failures ? 1 : 0;
}
