/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the process snapshot functions (PssCaptureSnapshot and friends)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <processsnapshot.h>

static DWORD (WINAPI *pPssCaptureSnapshot)(HANDLE, PSS_CAPTURE_FLAGS, DWORD, HPSS *);
static DWORD (WINAPI *pPssFreeSnapshot)(HANDLE, HPSS);
static DWORD (WINAPI *pPssQuerySnapshot)(HPSS, PSS_QUERY_INFORMATION_CLASS, void *, DWORD);
static DWORD (WINAPI *pPssDuplicateSnapshot)(HANDLE, HPSS, HANDLE, HPSS *, PSS_DUPLICATE_FLAGS);
static DWORD (WINAPI *pPssWalkMarkerCreate)(const PSS_ALLOCATOR *, HPSSWALK *);
static DWORD (WINAPI *pPssWalkMarkerFree)(HPSSWALK);
static DWORD (WINAPI *pPssWalkMarkerGetPosition)(HPSSWALK, ULONG_PTR *);
static DWORD (WINAPI *pPssWalkMarkerSetPosition)(HPSSWALK, ULONG_PTR);
static DWORD (WINAPI *pPssWalkMarkerSeekToBeginning)(HPSSWALK);
static DWORD (WINAPI *pPssWalkSnapshot)(HPSS, PSS_WALK_INFORMATION_CLASS, HPSSWALK, void *, DWORD);

#ifndef ERROR_BAD_LENGTH
#define ERROR_BAD_LENGTH 24
#endif

static void *WINAPI HeapAllocRoutine(void *Context, DWORD Size)
{
    return HeapAlloc(GetProcessHeap(), 0, Size);
}

static void WINAPI HeapFreeRoutine(void *Context, void *Address)
{
    HeapFree(GetProcessHeap(), 0, Address);
}

/* Windows takes the allocator for granted when a marker is used, so that a NULL one does not work in the end */
static PSS_ALLOCATOR g_Allocator = { NULL, HeapAllocRoutine, HeapFreeRoutine };


static
DWORD
WINAPI
BlockedThread(
    _In_ PVOID Parameter)
{
    WaitForSingleObject((HANDLE)Parameter, INFINITE);
    return 7;
}

/* A new marker has to be put at the beginning before it is used: Windows walks from nowhere until then */
static
DWORD
CreateMarker(
    _Out_ HPSSWALK *Marker)
{
    DWORD Error = pPssWalkMarkerCreate(&g_Allocator, Marker);

    if (Error == ERROR_SUCCESS)
        Error = pPssWalkMarkerSeekToBeginning(*Marker);
    return Error;
}

static
DWORD
WINAPI
ExitingThread(
    _In_ PVOID Parameter)
{
    return 0;
}

static
DWORD
Query(
    _In_ HPSS Snapshot,
    _In_ PSS_QUERY_INFORMATION_CLASS Class,
    _Out_ PVOID Buffer,
    _In_ DWORD Size)
{
    memset(Buffer, 0xCC, Size);
    return pPssQuerySnapshot(Snapshot, Class, Buffer, Size);
}

static
BOOL
EndsWithNoCase(
    _In_ PCWSTR String,
    _In_ PCWSTR Suffix)
{
    size_t Length = wcslen(String), SuffixLength = wcslen(Suffix);

    return Length >= SuffixLength && _wcsicmp(String + Length - SuffixLength, Suffix) == 0;
}

static
PCWSTR
BaseName(
    _In_ PCWSTR Path)
{
    PCWSTR Slash = wcsrchr(Path, L'\\');

    return Slash ? Slash + 1 : Path;
}

static
VOID
TestCaptureFlagsAndHandles(VOID)
{
    HPSS Snapshot;
    HANDLE Event, Process, Thread;
    DWORD Error;
    ULONG Index;
    static const DWORD Accepted[] =
    {
        PSS_CAPTURE_NONE, PSS_CAPTURE_RESERVED_00000002, PSS_CAPTURE_RESERVED_00000400, PSS_CAPTURE_RESERVED_00004000,
        PSS_CAPTURE_HANDLE_NAME_INFORMATION, PSS_CAPTURE_HANDLE_BASIC_INFORMATION, PSS_CAPTURE_HANDLE_TRACE,
        PSS_CAPTURE_IPT_TRACE, PSS_CAPTURE_VA_SPACE_SECTION_INFORMATION, PSS_CAPTURE_THREAD_CONTEXT,
        PSS_CAPTURE_THREAD_CONTEXT_EXTENDED, PSS_CREATE_MEASURE_PERFORMANCE, PSS_CREATE_RELEASE_SECTION
    };
    static const DWORD Invalid[] =
    {
        0x00008000, 0x00010000, 0x00100000, 0x01000000, 0x02000000, 0xFFFFFFFF
    };

    /* Flags that are not capture flags */
    for (Index = 0; Index < ARRAYSIZE(Invalid); Index++)
    {
        Snapshot = (HPSS)(ULONG_PTR)0x1234;
        Error = pPssCaptureSnapshot(GetCurrentProcess(), Invalid[Index], 0, &Snapshot);
        ok(Error == ERROR_INVALID_PARAMETER, "flags %lx: error %lu\n", Invalid[Index], Error);
        ok(Snapshot == NULL, "flags %lx: snapshot %p\n", Invalid[Index], Snapshot);
    }

    /* The optional breakaway needs the breakaway */
    Snapshot = (HPSS)(ULONG_PTR)0x1234;
    Error = pPssCaptureSnapshot(GetCurrentProcess(), PSS_CAPTURE_VA_CLONE | PSS_CREATE_BREAKAWAY_OPTIONAL, 0,
                                &Snapshot);
    ok(Error == ERROR_INVALID_PARAMETER, "error %lu\n", Error);
    ok(Snapshot == NULL, "snapshot %p\n", Snapshot);

    /* Forcing the breakaway takes a privilege that a program does not have */
    Error = pPssCaptureSnapshot(GetCurrentProcess(), PSS_CAPTURE_VA_CLONE | PSS_CREATE_FORCE_BREAKAWAY, 0, &Snapshot);
    ok(Error == ERROR_PRIVILEGE_NOT_HELD, "error %lu\n", Error);

    /* Flags that are accepted, and that capture nothing that is queried for here */
    for (Index = 0; Index < ARRAYSIZE(Accepted); Index++)
    {
        Snapshot = NULL;
        Error = pPssCaptureSnapshot(GetCurrentProcess(), Accepted[Index], CONTEXT_CONTROL, &Snapshot);
        ok(Error == ERROR_SUCCESS, "flags %lx: error %lu\n", Accepted[Index], Error);
        if (Error == ERROR_SUCCESS)
        {
            ok(Snapshot != NULL, "flags %lx: no snapshot\n", Accepted[Index]);
            Error = pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
            ok(Error == ERROR_SUCCESS, "flags %lx: free: error %lu\n", Accepted[Index], Error);
        }
    }

    /* The pseudo handle of the process is the process */
    Snapshot = NULL;
    Error = pPssCaptureSnapshot(INVALID_HANDLE_VALUE, PSS_CAPTURE_NONE, 0, &Snapshot);
    ok(Error == ERROR_SUCCESS, "error %lu\n", Error);
    if (Error == ERROR_SUCCESS)
        pPssFreeSnapshot(GetCurrentProcess(), Snapshot);

    /* Handles that are not process handles */
    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    Thread = CreateThread(NULL, 0, ExitingThread, NULL, 0, NULL);
    WaitForSingleObject(Thread, 10000);
    Snapshot = (HPSS)(ULONG_PTR)0x1234;
    Error = pPssCaptureSnapshot(NULL, PSS_CAPTURE_NONE, 0, &Snapshot);
    ok(Error == ERROR_INVALID_HANDLE, "NULL: error %lu\n", Error);
    ok(Snapshot == NULL, "NULL: snapshot %p\n", Snapshot);
    Error = pPssCaptureSnapshot((HANDLE)(ULONG_PTR)0x7777, PSS_CAPTURE_NONE, 0, &Snapshot);
    ok(Error == ERROR_INVALID_HANDLE, "bogus: error %lu\n", Error);
    Error = pPssCaptureSnapshot(Event, PSS_CAPTURE_NONE, 0, &Snapshot);
    ok(Error == ERROR_INVALID_HANDLE, "event: error %lu\n", Error);
    Error = pPssCaptureSnapshot(Thread, PSS_CAPTURE_NONE, 0, &Snapshot);
    ok(Error == ERROR_INVALID_HANDLE, "thread: error %lu\n", Error);
    CloseHandle(Thread);
    CloseHandle(Event);

    /* A handle that does not let the process be read further than its basic information */
    Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    ok(Process != NULL, "OpenProcess failed: %lu\n", GetLastError());
    if (Process)
    {
        Error = pPssCaptureSnapshot(Process, PSS_CAPTURE_NONE, 0, &Snapshot);
        ok(Error == ERROR_SUCCESS, "limited, none: error %lu\n", Error);
        if (Error == ERROR_SUCCESS)
            pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
        Error = pPssCaptureSnapshot(Process, PSS_CAPTURE_THREADS, 0, &Snapshot);
        ok(Error == ERROR_ACCESS_DENIED, "limited, threads: error %lu\n", Error);
        Error = pPssCaptureSnapshot(Process, PSS_CAPTURE_HANDLES, 0, &Snapshot);
        ok(Error == ERROR_ACCESS_DENIED, "limited, handles: error %lu\n", Error);
        CloseHandle(Process);
    }

    Process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    ok(Process != NULL, "OpenProcess failed: %lu\n", GetLastError());
    if (Process)
    {
        Error = pPssCaptureSnapshot(Process, PSS_CAPTURE_THREADS | PSS_CAPTURE_VA_SPACE, 0, &Snapshot);
        ok(Error == ERROR_SUCCESS, "query, threads and VA space: error %lu\n", Error);
        if (Error == ERROR_SUCCESS)
            pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
        /* Handles are read by duplicating them */
        Error = pPssCaptureSnapshot(Process, PSS_CAPTURE_HANDLES, 0, &Snapshot);
        ok(Error == ERROR_ACCESS_DENIED, "query, handles: error %lu\n", Error);
        CloseHandle(Process);
    }
}

static
VOID
TestProcessInformation(VOID)
{
    HPSS Snapshot;
    PSS_PROCESS_INFORMATION Info;
    union
    {
        PSS_VA_CLONE_INFORMATION Clone;
        PSS_AUXILIARY_PAGES_INFORMATION Aux;
        PSS_VA_SPACE_INFORMATION VaSpace;
        PSS_HANDLE_INFORMATION Handles;
        PSS_THREAD_INFORMATION Threads;
        PSS_HANDLE_TRACE_INFORMATION Trace;
        PSS_PERFORMANCE_COUNTERS Performance;
        BYTE Bytes[256];
    } Buffer;
    PSS_PERFORMANCE_COUNTERS Zero;
    WCHAR Path[MAX_PATH];
    FILETIME Create, Exit, Kernel, User;
    DWORD Error;
    ULONG Class;
    static const struct
    {
        PSS_QUERY_INFORMATION_CLASS Class;
        DWORD Size;
    } NotCaptured[] =
    {
        { PSS_QUERY_VA_CLONE_INFORMATION, sizeof(PSS_VA_CLONE_INFORMATION) },
        { PSS_QUERY_AUXILIARY_PAGES_INFORMATION, sizeof(PSS_AUXILIARY_PAGES_INFORMATION) },
        { PSS_QUERY_VA_SPACE_INFORMATION, sizeof(PSS_VA_SPACE_INFORMATION) },
        { PSS_QUERY_HANDLE_INFORMATION, sizeof(PSS_HANDLE_INFORMATION) },
        { PSS_QUERY_THREAD_INFORMATION, sizeof(PSS_THREAD_INFORMATION) },
        { PSS_QUERY_HANDLE_TRACE_INFORMATION, sizeof(PSS_HANDLE_TRACE_INFORMATION) },
    };

    Error = pPssCaptureSnapshot(GetCurrentProcess(), PSS_CAPTURE_NONE, 0, &Snapshot);
    ok(Error == ERROR_SUCCESS, "error %lu\n", Error);
    if (Error != ERROR_SUCCESS)
        return;

    Error = Query(Snapshot, PSS_QUERY_PROCESS_INFORMATION, &Info, sizeof(Info));
    ok(Error == ERROR_SUCCESS, "error %lu\n", Error);
    if (Error == ERROR_SUCCESS)
    {
        ok(Info.ExitStatus == STILL_ACTIVE, "ExitStatus %lx\n", Info.ExitStatus);
        ok(Info.ProcessId == GetCurrentProcessId(), "ProcessId %lu\n", Info.ProcessId);
        ok(Info.ParentProcessId != 0, "ParentProcessId is 0\n");
        ok(Info.PebBaseAddress == (PVOID)NtCurrentTeb()->ProcessEnvironmentBlock, "PebBaseAddress %p\n",
           Info.PebBaseAddress);
        ok(Info.AffinityMask != 0, "AffinityMask is 0\n");
        ok(Info.PriorityClass == GetPriorityClass(GetCurrentProcess()), "PriorityClass %lx\n", Info.PriorityClass);
        ok((Info.Flags & PSS_PROCESS_FLAGS_FROZEN) == 0, "Flags %lx\n", Info.Flags);
        ok(Info.VirtualSize != 0 && Info.VirtualSize <= Info.PeakVirtualSize, "VirtualSize %Iu, peak %Iu\n",
           Info.VirtualSize, Info.PeakVirtualSize);
        ok(Info.WorkingSetSize != 0 && Info.WorkingSetSize <= Info.PeakWorkingSetSize, "WorkingSetSize %Iu, peak %Iu\n",
           Info.WorkingSetSize, Info.PeakWorkingSetSize);
        ok(Info.PrivateUsage != 0, "PrivateUsage is 0\n");

        ok(GetModuleFileNameW(NULL, Path, ARRAYSIZE(Path)) != 0, "GetModuleFileName failed\n");
        ok(_wcsicmp(Info.ImageFileName, Path) == 0, "ImageFileName %ls, module %ls\n", Info.ImageFileName, Path);

        ok(GetProcessTimes(GetCurrentProcess(), &Create, &Exit, &Kernel, &User), "GetProcessTimes failed\n");
        ok(CompareFileTime(&Info.CreateTime, &Create) == 0, "CreateTime differs\n");
        ok(Info.ExitTime.dwLowDateTime == 0 && Info.ExitTime.dwHighDateTime == 0, "ExitTime is not 0\n");
    }

    /* The buffer has exactly the size of the structure */
    Error = pPssQuerySnapshot(Snapshot, PSS_QUERY_PROCESS_INFORMATION, &Info, sizeof(Info) - 1);
    ok(Error == ERROR_BAD_LENGTH, "short: error %lu\n", Error);
    Error = pPssQuerySnapshot(Snapshot, PSS_QUERY_PROCESS_INFORMATION, &Buffer, sizeof(Info) + 8);
    ok(Error == ERROR_BAD_LENGTH, "long: error %lu\n", Error);
    Error = pPssQuerySnapshot(Snapshot, PSS_QUERY_PROCESS_INFORMATION, &Buffer, 0);
    ok(Error == ERROR_BAD_LENGTH, "zero: error %lu\n", Error);
    Error = pPssQuerySnapshot(Snapshot, (PSS_QUERY_INFORMATION_CLASS)99, &Buffer, sizeof(Buffer));
    ok(Error == ERROR_INVALID_PARAMETER, "class 99: error %lu\n", Error);
    Error = pPssQuerySnapshot(NULL, PSS_QUERY_PROCESS_INFORMATION, &Info, sizeof(Info));
    ok(Error == ERROR_INVALID_HANDLE, "NULL snapshot: error %lu\n", Error);

    /* Nothing but the process was captured */
    for (Class = 0; Class < ARRAYSIZE(NotCaptured); Class++)
    {
        Error = Query(Snapshot, NotCaptured[Class].Class, &Buffer, NotCaptured[Class].Size);
        ok(Error == ERROR_NOT_FOUND, "class %d: error %lu\n", NotCaptured[Class].Class, Error);
        Error = pPssQuerySnapshot(Snapshot, NotCaptured[Class].Class, &Buffer, NotCaptured[Class].Size + 1);
        ok(Error == ERROR_BAD_LENGTH, "class %d, long: error %lu\n", NotCaptured[Class].Class, Error);
    }

    /* The counters are zero when they are not measured */
    memset(&Zero, 0, sizeof(Zero));
    Error = Query(Snapshot, PSS_QUERY_PERFORMANCE_COUNTERS, &Buffer, sizeof(PSS_PERFORMANCE_COUNTERS));
    ok(Error == ERROR_SUCCESS, "counters: error %lu\n", Error);
    ok(memcmp(&Buffer, &Zero, sizeof(Zero)) == 0, "the counters are not zero\n");

    Error = pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
    ok(Error == ERROR_SUCCESS, "free: error %lu\n", Error);

    /* Measured */
    Error = pPssCaptureSnapshot(GetCurrentProcess(), PSS_CREATE_MEASURE_PERFORMANCE | PSS_CAPTURE_THREADS, 0,
                                &Snapshot);
    ok(Error == ERROR_SUCCESS, "error %lu\n", Error);
    if (Error == ERROR_SUCCESS)
    {
        Error = Query(Snapshot, PSS_QUERY_PERFORMANCE_COUNTERS, &Buffer, sizeof(PSS_PERFORMANCE_COUNTERS));
        ok(Error == ERROR_SUCCESS, "counters: error %lu\n", Error);
        ok(Buffer.Performance.TotalCycleCount != 0, "TotalCycleCount is 0\n");
        ok(Buffer.Performance.ThreadsCycleCount != 0, "ThreadsCycleCount is 0\n");
        ok(Buffer.Performance.TotalCycleCount >= Buffer.Performance.ThreadsCycleCount, "total %I64u, threads %I64u\n",
           Buffer.Performance.TotalCycleCount, Buffer.Performance.ThreadsCycleCount);
        ok(Buffer.Performance.VaSpaceCycleCount == 0, "VaSpaceCycleCount %I64u\n",
           Buffer.Performance.VaSpaceCycleCount);
        pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
    }
}

static
VOID
TestThreads(VOID)
{
    HPSS Snapshot;
    HPSSWALK Marker;
    PSS_THREAD_ENTRY Entry;
    PSS_THREAD_INFORMATION Info;
    HANDLE Thread, Release;
    DWORD ThreadId, Error, Count, FoundCurrent, FoundThread;
    ULONG_PTR Position, Position1 = 0, Position2 = 0;
    ULONG Pass;

    Release = CreateEventW(NULL, TRUE, FALSE, NULL);
    Thread = CreateThread(NULL, 0, BlockedThread, Release, 0, &ThreadId);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Thread)
        return;
    Sleep(200); /* let it get to the wait */

    for (Pass = 0; Pass < 2; Pass++)
    {
        BOOL WithContext = Pass == 1;

        Error = pPssCaptureSnapshot(GetCurrentProcess(),
                                    PSS_CAPTURE_THREADS | (WithContext ? PSS_CAPTURE_THREAD_CONTEXT : 0),
                                    WithContext ? CONTEXT_CONTROL | CONTEXT_INTEGER : 0,
                                    &Snapshot);
        ok(Error == ERROR_SUCCESS, "pass %lu: error %lu\n", Pass, Error);
        if (Error != ERROR_SUCCESS)
            continue;

        Error = Query(Snapshot, PSS_QUERY_THREAD_INFORMATION, &Info, sizeof(Info));
        ok(Error == ERROR_SUCCESS, "pass %lu: query: error %lu\n", Pass, Error);
        ok(Info.ThreadsCaptured >= 2, "pass %lu: %lu threads\n", Pass, Info.ThreadsCaptured);
        if (WithContext)
            ok(Info.ContextLength == sizeof(CONTEXT), "ContextLength %lu\n", Info.ContextLength);
        else
            ok(Info.ContextLength == 0, "ContextLength %lu\n", Info.ContextLength);

        Error = CreateMarker(&Marker);
        ok(Error == ERROR_SUCCESS, "marker: error %lu\n", Error);
        if (Error != ERROR_SUCCESS)
        {
            pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
            continue;
        }
        Position = 0xDEAD;
        Error = pPssWalkMarkerGetPosition(Marker, &Position);
        ok(Error == ERROR_SUCCESS, "position: error %lu\n", Error);
        ok(Position == 0, "new marker at %Iu\n", Position);

        Count = FoundCurrent = FoundThread = 0;
        for (;;)
        {
            memset(&Entry, 0xCC, sizeof(Entry));
            Error = pPssWalkSnapshot(Snapshot, PSS_WALK_THREADS, Marker, &Entry, sizeof(Entry));
            if (Error != ERROR_SUCCESS)
                break;

            ok(Entry.ProcessId == GetCurrentProcessId(), "ProcessId %lu\n", Entry.ProcessId);
            ok(Entry.ThreadId != 0, "ThreadId is 0\n");
            ok(Entry.ExitStatus == STILL_ACTIVE, "thread %lu: ExitStatus %lx\n", Entry.ThreadId, Entry.ExitStatus);
            ok(Entry.TebBaseAddress != NULL, "thread %lu: no TEB\n", Entry.ThreadId);
            ok(Entry.AffinityMask != 0, "thread %lu: AffinityMask is 0\n", Entry.ThreadId);
            ok((Entry.Flags & PSS_THREAD_FLAGS_TERMINATED) == 0, "thread %lu: Flags %x\n", Entry.ThreadId, Entry.Flags);
            ok(Entry.Win32StartAddress != NULL, "thread %lu: no start address\n", Entry.ThreadId);

            if (Entry.ThreadId == GetCurrentThreadId())
            {
                FoundCurrent++;
                ok(Entry.TebBaseAddress == NtCurrentTeb(), "TebBaseAddress %p\n", Entry.TebBaseAddress);
            }
            if (Entry.ThreadId == ThreadId)
            {
                FoundThread++;
                ok(Entry.Win32StartAddress == (PVOID)BlockedThread, "Win32StartAddress %p\n",
                   Entry.Win32StartAddress);
            }

            if (WithContext)
            {
                ok(Entry.SizeOfContextRecord == sizeof(CONTEXT), "thread %lu: SizeOfContextRecord %u\n",
                   Entry.ThreadId, Entry.SizeOfContextRecord);
                ok(Entry.ContextRecord != NULL, "thread %lu: no context\n", Entry.ThreadId);
                if (Entry.ContextRecord)
                {
                    ok((Entry.ContextRecord->ContextFlags & (CONTEXT_CONTROL | CONTEXT_INTEGER)) ==
                       (CONTEXT_CONTROL | CONTEXT_INTEGER), "thread %lu: ContextFlags %lx\n", Entry.ThreadId,
                       Entry.ContextRecord->ContextFlags);
#ifdef _M_AMD64
                    ok(Entry.ContextRecord->Rip != 0 && Entry.ContextRecord->Rsp != 0, "thread %lu: empty context\n",
                       Entry.ThreadId);
#elif defined(_M_IX86)
                    ok(Entry.ContextRecord->Eip != 0 && Entry.ContextRecord->Esp != 0, "thread %lu: empty context\n",
                       Entry.ThreadId);
#endif
                }
            }
            else
            {
                ok(Entry.ContextRecord == NULL, "thread %lu: context %p\n", Entry.ThreadId, Entry.ContextRecord);
                ok(Entry.SizeOfContextRecord == 0, "thread %lu: SizeOfContextRecord %u\n", Entry.ThreadId,
                   Entry.SizeOfContextRecord);
            }
            Count++;
        }
        ok(Error == ERROR_NO_MORE_ITEMS, "end of the walk: error %lu\n", Error);
        ok(Count == Info.ThreadsCaptured, "walked %lu, captured %lu\n", Count, Info.ThreadsCaptured);
        ok(FoundCurrent == 1, "found the current thread %lu times\n", FoundCurrent);
        ok(FoundThread == 1, "found the other thread %lu times\n", FoundThread);

        /* The position moves forward as entries are walked, and can be set (Windows does not tolerate more, such as
           a position that was not taken from the same walk, or a marker moved after the end) */
        Error = pPssWalkMarkerFree(Marker);
        ok(Error == ERROR_SUCCESS, "marker free: error %lu\n", Error);
        Error = CreateMarker(&Marker);
        ok(Error == ERROR_SUCCESS, "marker: error %lu\n", Error);
        Error = pPssWalkSnapshot(Snapshot, PSS_WALK_THREADS, Marker, &Entry, sizeof(Entry));
        ok(Error == ERROR_SUCCESS, "first walk: error %lu\n", Error);
        pPssWalkMarkerGetPosition(Marker, &Position1);
        Error = pPssWalkSnapshot(Snapshot, PSS_WALK_THREADS, Marker, &Entry, sizeof(Entry));
        ok(Error == ERROR_SUCCESS, "second walk: error %lu\n", Error);
        pPssWalkMarkerGetPosition(Marker, &Position2);
        ok(Position1 > 0 && Position2 > Position1, "positions %Iu, %Iu\n", Position1, Position2);
        Error = pPssWalkMarkerSetPosition(Marker, Position1);
        ok(Error == ERROR_SUCCESS, "set position: error %lu\n", Error);
        pPssWalkMarkerGetPosition(Marker, &Position);
        ok(Position == Position1, "position %Iu, expected %Iu\n", Position, Position1);
        Error = pPssWalkSnapshot(Snapshot, PSS_WALK_THREADS, Marker, &Entry, sizeof(Entry));
        ok(Error == ERROR_SUCCESS, "walk from the position: error %lu\n", Error);

        /* Wrong requests */
        Error = pPssWalkSnapshot(Snapshot, PSS_WALK_THREADS, Marker, &Entry, sizeof(Entry) - 1);
        ok(Error == ERROR_BAD_LENGTH, "short: error %lu\n", Error);
        Error = pPssWalkSnapshot(Snapshot, PSS_WALK_THREADS, Marker, &Entry, sizeof(Entry) + 8);
        ok(Error == ERROR_BAD_LENGTH, "long: error %lu\n", Error);
        Error = pPssWalkSnapshot(Snapshot, (PSS_WALK_INFORMATION_CLASS)9, Marker, &Entry, sizeof(Entry));
        ok(Error == ERROR_INVALID_PARAMETER, "class 9: error %lu\n", Error);
        {
            PSS_HANDLE_ENTRY HandleEntry;

            Error = pPssWalkSnapshot(Snapshot, PSS_WALK_HANDLES, Marker, &HandleEntry, sizeof(HandleEntry));
            ok(Error == ERROR_NOT_FOUND, "handles that were not captured: error %lu\n", Error);
        }
        Error = pPssWalkSnapshot(NULL, PSS_WALK_THREADS, Marker, &Entry, sizeof(Entry));
        ok(Error == ERROR_INVALID_HANDLE, "NULL snapshot: error %lu\n", Error);

        Error = pPssWalkMarkerFree(Marker);
        ok(Error == ERROR_SUCCESS, "marker free: error %lu\n", Error);
        Error = pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
        ok(Error == ERROR_SUCCESS, "free: error %lu\n", Error);
    }

    SetEvent(Release);
    WaitForSingleObject(Thread, 10000);
    CloseHandle(Thread);
    CloseHandle(Release);
}

/* The strings of an entry are only good until the next entry is walked, so they are copied */
typedef struct _HANDLE_COPY
{
    PSS_HANDLE_ENTRY Entry;
    WCHAR TypeName[64];
    WCHAR ObjectName[MAX_PATH];
} HANDLE_COPY;

static
BOOL
FindHandle(
    _In_ HPSS Snapshot,
    _In_ HANDLE Handle,
    _Out_ HANDLE_COPY *Copy,
    _Out_ PDWORD Count)
{
    HPSSWALK Marker;
    PSS_HANDLE_ENTRY Current;
    BOOL Found = FALSE;

    *Count = 0;
    memset(Copy, 0, sizeof(*Copy));
    if (CreateMarker(&Marker) != ERROR_SUCCESS)
        return FALSE;
    while (pPssWalkSnapshot(Snapshot, PSS_WALK_HANDLES, Marker, &Current, sizeof(Current)) == ERROR_SUCCESS)
    {
        (*Count)++;
        if (Current.Handle != Handle)
            continue;

        Copy->Entry = Current;
        if (Current.TypeName && Current.TypeNameLength < sizeof(Copy->TypeName))
        {
            memcpy(Copy->TypeName, Current.TypeName, Current.TypeNameLength);
            Copy->Entry.TypeName = Copy->TypeName;
        }
        else
        {
            Copy->Entry.TypeName = NULL;
        }
        if (Current.ObjectName && Current.ObjectNameLength < sizeof(Copy->ObjectName))
        {
            memcpy(Copy->ObjectName, Current.ObjectName, Current.ObjectNameLength);
            Copy->Entry.ObjectName = Copy->ObjectName;
        }
        else
        {
            Copy->Entry.ObjectName = NULL;
        }
        Found = TRUE;
    }
    pPssWalkMarkerFree(Marker);
    return Found;
}

static
VOID
TestHandles(VOID)
{
    WCHAR EventName[64], MutexName[64], SemaphoreName[64], SectionName[64];
    HANDLE Event, Mutex, Semaphore, Section, Thread, Process, Release;
    HPSS Snapshot;
    PSS_HANDLE_INFORMATION Info;
    HANDLE_COPY Copy;
    PSS_HANDLE_ENTRY *Entry = &Copy.Entry;
    BOOL Found;
    DWORD Error, Count, ThreadId;
    const DWORD All = PSS_CAPTURE_HANDLES | PSS_CAPTURE_HANDLE_NAME_INFORMATION |
                      PSS_CAPTURE_HANDLE_BASIC_INFORMATION | PSS_CAPTURE_HANDLE_TYPE_SPECIFIC_INFORMATION;

    _snwprintf(EventName, 64, L"PssSnapshotEvent%lu", GetCurrentProcessId());
    _snwprintf(MutexName, 64, L"PssSnapshotMutex%lu", GetCurrentProcessId());
    _snwprintf(SemaphoreName, 64, L"PssSnapshotSemaphore%lu", GetCurrentProcessId());
    _snwprintf(SectionName, 64, L"PssSnapshotSection%lu", GetCurrentProcessId());
    Event = CreateEventW(NULL, TRUE, FALSE, EventName);
    Mutex = CreateMutexW(NULL, TRUE, MutexName);
    Semaphore = CreateSemaphoreW(NULL, 1, 3, SemaphoreName);
    Section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 0x2000, SectionName);
    Release = CreateEventW(NULL, TRUE, FALSE, NULL);
    Thread = CreateThread(NULL, 0, BlockedThread, Release, 0, &ThreadId);
    Process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    ok(Event && Mutex && Semaphore && Section && Thread && Process, "could not create the objects\n");
    if (!(Event && Mutex && Semaphore && Section && Thread && Process))
        return;

    /* (Windows is not reliable with fewer of the handle flags: it crashes in the capture with the handles alone, or
       without the basic information, or with the type specific information but not the basic) */
    Error = pPssCaptureSnapshot(GetCurrentProcess(), All, 0, &Snapshot);
    ok(Error == ERROR_SUCCESS, "error %lu\n", Error);
    if (Error != ERROR_SUCCESS)
        return;

    Error = Query(Snapshot, PSS_QUERY_HANDLE_INFORMATION, &Info, sizeof(Info));
    ok(Error == ERROR_SUCCESS, "query: error %lu\n", Error);
    ok(Info.HandlesCaptured >= 6, "%lu handles\n", Info.HandlesCaptured);

    Found = FindHandle(Snapshot, Event, &Copy, &Count);
    ok(Count == Info.HandlesCaptured, "walked %lu, captured %lu\n", Count, Info.HandlesCaptured);
    ok(Found, "the event is not in the snapshot\n");
    if (Found)
    {
        ok(Entry->ObjectType == PSS_OBJECT_TYPE_EVENT, "ObjectType %d\n", Entry->ObjectType);
        ok(Entry->TypeNameLength == sizeof(L"Event") && Entry->TypeName && wcscmp(Entry->TypeName, L"Event") == 0,
           "TypeName %ls (%u)\n", Entry->TypeName ? Entry->TypeName : L"", Entry->TypeNameLength);
        ok((Entry->Flags & (PSS_HANDLE_HAVE_TYPE | PSS_HANDLE_HAVE_BASIC_INFORMATION |
                           PSS_HANDLE_HAVE_TYPE_SPECIFIC_INFORMATION)) ==
           (PSS_HANDLE_HAVE_TYPE | PSS_HANDLE_HAVE_BASIC_INFORMATION | PSS_HANDLE_HAVE_TYPE_SPECIFIC_INFORMATION),
           "Flags %x\n", Entry->Flags);
        ok(Entry->GrantedAccess == EVENT_ALL_ACCESS, "GrantedAccess %lx\n", Entry->GrantedAccess);
        ok(Entry->HandleCount >= 1, "HandleCount %lu\n", Entry->HandleCount);
        ok(Entry->PointerCount >= 1, "PointerCount %lu\n", Entry->PointerCount);
        ok(Entry->TypeSpecificInformation.Event.ManualReset, "the event is not manual reset\n");
        ok(!Entry->TypeSpecificInformation.Event.Signaled, "the event is signaled\n");
        ok((Entry->Flags & PSS_HANDLE_HAVE_NAME) != 0, "Flags %x\n", Entry->Flags);
        if (Entry->Flags & PSS_HANDLE_HAVE_NAME)
        {
            ok(Entry->ObjectName != NULL && EndsWithNoCase(Entry->ObjectName, EventName), "ObjectName %ls\n",
               Entry->ObjectName ? Entry->ObjectName : L"");
            ok(Entry->ObjectNameLength == (wcslen(Entry->ObjectName) + 1) * sizeof(WCHAR), "ObjectNameLength %u\n",
               Entry->ObjectNameLength);
        }
    }

    Found = FindHandle(Snapshot, Semaphore, &Copy, &Count);
    ok(Found, "the semaphore is not in the snapshot\n");
    if (Found)
    {
        ok(Entry->ObjectType == PSS_OBJECT_TYPE_SEMAPHORE, "ObjectType %d\n", Entry->ObjectType);
        ok(Entry->TypeSpecificInformation.Semaphore.CurrentCount == 1, "CurrentCount %lu\n",
           Entry->TypeSpecificInformation.Semaphore.CurrentCount);
        ok(Entry->TypeSpecificInformation.Semaphore.MaximumCount == 3, "MaximumCount %lu\n",
           Entry->TypeSpecificInformation.Semaphore.MaximumCount);
    }

    Found = FindHandle(Snapshot, Mutex, &Copy, &Count);
    ok(Found, "the mutex is not in the snapshot\n");
    if (Found)
    {
        ok(Entry->ObjectType == PSS_OBJECT_TYPE_MUTANT, "ObjectType %d\n", Entry->ObjectType);
        ok((Entry->Flags & PSS_HANDLE_HAVE_TYPE_SPECIFIC_INFORMATION) != 0, "Flags %x\n", Entry->Flags);
        ok(!Entry->TypeSpecificInformation.Mutant.Abandoned, "the mutex is abandoned\n");
    }

    Found = FindHandle(Snapshot, Section, &Copy, &Count);
    ok(Found, "the section is not in the snapshot\n");
    if (Found)
    {
        ok(Entry->ObjectType == PSS_OBJECT_TYPE_SECTION, "ObjectType %d\n", Entry->ObjectType);
        ok(Entry->TypeSpecificInformation.Section.MaximumSize.QuadPart == 0x2000, "MaximumSize %I64x\n",
           Entry->TypeSpecificInformation.Section.MaximumSize.QuadPart);
    }

    Found = FindHandle(Snapshot, Thread, &Copy, &Count);
    ok(Found, "the thread is not in the snapshot\n");
    if (Found)
    {
        ok(Entry->ObjectType == PSS_OBJECT_TYPE_THREAD, "ObjectType %d\n", Entry->ObjectType);
        ok(Entry->TypeSpecificInformation.Thread.ThreadId == ThreadId, "ThreadId %lu, expected %lu\n",
           Entry->TypeSpecificInformation.Thread.ThreadId, ThreadId);
        ok(Entry->TypeSpecificInformation.Thread.ProcessId == GetCurrentProcessId(), "ProcessId %lu\n",
           Entry->TypeSpecificInformation.Thread.ProcessId);
    }

    Found = FindHandle(Snapshot, Process, &Copy, &Count);
    ok(Found, "the process is not in the snapshot\n");
    if (Found)
    {
        ok(Entry->ObjectType == PSS_OBJECT_TYPE_PROCESS, "ObjectType %d\n", Entry->ObjectType);
        ok(Entry->TypeSpecificInformation.Process.ProcessId == GetCurrentProcessId(), "ProcessId %lu\n",
           Entry->TypeSpecificInformation.Process.ProcessId);
        ok(Entry->TypeSpecificInformation.Process.ExitStatus == STILL_ACTIVE, "ExitStatus %lx\n",
           Entry->TypeSpecificInformation.Process.ExitStatus);
    }
    pPssFreeSnapshot(GetCurrentProcess(), Snapshot);

    /* A snapshot is a picture of the moment */
    ok(SetEvent(Event), "SetEvent failed\n");
    ok(ReleaseSemaphore(Semaphore, 1, NULL), "ReleaseSemaphore failed\n");
    /* (Windows crashes when the type specific information is asked for without the basic information) */
    Error = pPssCaptureSnapshot(GetCurrentProcess(), All, 0, &Snapshot);
    ok(Error == ERROR_SUCCESS, "error %lu\n", Error);
    if (Error == ERROR_SUCCESS)
    {
        Found = FindHandle(Snapshot, Event, &Copy, &Count);
        ok(Found && Entry->TypeSpecificInformation.Event.Signaled, "the event is not signaled\n");
        Found = FindHandle(Snapshot, Semaphore, &Copy, &Count);
        ok(Found && Entry->TypeSpecificInformation.Semaphore.CurrentCount == 2, "CurrentCount %lu\n",
           Found ? Entry->TypeSpecificInformation.Semaphore.CurrentCount : 0);
        pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
    }

    CloseHandle(Process);
    SetEvent(Release);
    WaitForSingleObject(Thread, 10000);
    CloseHandle(Release);
    CloseHandle(Thread);
    CloseHandle(Section);
    CloseHandle(Semaphore);
    ReleaseMutex(Mutex);
    CloseHandle(Mutex);
    CloseHandle(Event);
}

static
VOID
TestVaSpace(VOID)
{
    HPSS Snapshot;
    HPSSWALK Marker;
    PSS_VA_SPACE_INFORMATION Info;
    PSS_VA_SPACE_ENTRY Entry, Previous;
    MEMORY_BASIC_INFORMATION Memory;
    PIMAGE_DOS_HEADER Dos = (PIMAGE_DOS_HEADER)GetModuleHandleW(NULL);
    PIMAGE_NT_HEADERS Nt = (PIMAGE_NT_HEADERS)((PBYTE)Dos + Dos->e_lfanew);
    WCHAR Path[MAX_PATH];
    PBYTE Block;
    DWORD Error, Count, FoundBlock, FoundImage, Gaps;
    ULONG Pass;

    Block = VirtualAlloc(NULL, 0x5000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok(Block != NULL, "VirtualAlloc failed\n");
    if (!Block)
        return;
    GetModuleFileNameW(NULL, Path, ARRAYSIZE(Path));

    for (Pass = 0; Pass < 2; Pass++)
    {
        BOOL WithSections = Pass == 1;

        Error = pPssCaptureSnapshot(GetCurrentProcess(),
                                    PSS_CAPTURE_VA_SPACE | (WithSections ? PSS_CAPTURE_VA_SPACE_SECTION_INFORMATION : 0),
                                    0, &Snapshot);
        ok(Error == ERROR_SUCCESS, "pass %lu: error %lu\n", Pass, Error);
        if (Error != ERROR_SUCCESS)
            continue;

        Error = Query(Snapshot, PSS_QUERY_VA_SPACE_INFORMATION, &Info, sizeof(Info));
        ok(Error == ERROR_SUCCESS, "pass %lu: query: error %lu\n", Pass, Error);
        ok(Info.RegionCount > 10, "pass %lu: %lu regions\n", Pass, Info.RegionCount);

        Error = CreateMarker(&Marker);
        ok(Error == ERROR_SUCCESS, "marker: error %lu\n", Error);
        if (Error != ERROR_SUCCESS)
        {
            pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
            continue;
        }

        Count = FoundBlock = FoundImage = Gaps = 0;
        memset(&Previous, 0, sizeof(Previous));
        for (;;)
        {
            memset(&Entry, 0xCC, sizeof(Entry));
            Error = pPssWalkSnapshot(Snapshot, PSS_WALK_VA_SPACE, Marker, &Entry, sizeof(Entry));
            if (Error != ERROR_SUCCESS)
                break;

            /* The regions follow one another from the bottom up, free ones included */
            if (Count == 0)
                ok(Entry.BaseAddress == NULL, "the first region is at %p\n", Entry.BaseAddress);
            else if ((PBYTE)Previous.BaseAddress + Previous.RegionSize != (PBYTE)Entry.BaseAddress)
                Gaps++;
            Previous = Entry;
            Count++;

            if (Entry.BaseAddress == Block)
            {
                FoundBlock++;
                ok(VirtualQuery(Block, &Memory, sizeof(Memory)) == sizeof(Memory), "VirtualQuery failed\n");
                ok(Entry.AllocationBase == Block, "AllocationBase %p\n", Entry.AllocationBase);
                ok(Entry.RegionSize == Memory.RegionSize, "RegionSize %Ix, expected %Ix\n", Entry.RegionSize,
                   Memory.RegionSize);
                ok(Entry.State == MEM_COMMIT, "State %lx\n", Entry.State);
                ok(Entry.Protect == PAGE_READWRITE, "Protect %lx\n", Entry.Protect);
                ok(Entry.AllocationProtect == PAGE_READWRITE, "AllocationProtect %lx\n", Entry.AllocationProtect);
                ok(Entry.Type == MEM_PRIVATE, "Type %lx\n", Entry.Type);
            }
            if (Entry.BaseAddress == (PVOID)Dos)
            {
                FoundImage++;
                ok(Entry.Type == MEM_IMAGE, "Type %lx\n", Entry.Type);
                if (WithSections)
                {
                    ok(Entry.ImageBase == (PVOID)Dos, "ImageBase %p\n", Entry.ImageBase);
                    ok(Entry.TimeDateStamp == Nt->FileHeader.TimeDateStamp, "TimeDateStamp %lx, expected %lx\n",
                       Entry.TimeDateStamp, Nt->FileHeader.TimeDateStamp);
                    ok(Entry.SizeOfImage == Nt->OptionalHeader.SizeOfImage, "SizeOfImage %lx, expected %lx\n",
                       Entry.SizeOfImage, Nt->OptionalHeader.SizeOfImage);
                    ok(Entry.CheckSum == Nt->OptionalHeader.CheckSum, "CheckSum %lx, expected %lx\n", Entry.CheckSum,
                       Nt->OptionalHeader.CheckSum);
                    ok(Entry.MappedFileName != NULL && Entry.MappedFileNameLength != 0, "no file name\n");
                    if (Entry.MappedFileName)
                    {
                        ok(Entry.MappedFileNameLength == (wcslen(Entry.MappedFileName) + 1) * sizeof(WCHAR),
                           "MappedFileNameLength %u\n", Entry.MappedFileNameLength);
                        ok(EndsWithNoCase(Entry.MappedFileName, BaseName(Path)), "MappedFileName %ls\n",
                           Entry.MappedFileName);
                    }
                }
                else
                {
                    ok(Entry.MappedFileName == NULL && Entry.MappedFileNameLength == 0, "file name %ls\n",
                       Entry.MappedFileName ? Entry.MappedFileName : L"");
                }
            }
        }
        ok(Error == ERROR_NO_MORE_ITEMS, "end of the walk: error %lu\n", Error);
        ok(Count == Info.RegionCount, "walked %lu, captured %lu\n", Count, Info.RegionCount);
        ok(FoundBlock == 1, "found the allocation %lu times\n", FoundBlock);
        ok(FoundImage == 1, "found the image %lu times\n", FoundImage);
        ok(Gaps == 0, "%lu gaps between the regions\n", Gaps);

        pPssWalkMarkerFree(Marker);
        pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
    }
    VirtualFree(Block, 0, MEM_RELEASE);
}

static void *WINAPI MarkerAlloc(void *Context, DWORD Size)
{
    (*(LONG *)Context)++;
    return HeapAlloc(GetProcessHeap(), 0, Size);
}

static void WINAPI MarkerFree(void *Context, void *Address)
{
    (*(LONG *)Context)--;
    HeapFree(GetProcessHeap(), 0, Address);
}

static
VOID
TestMarkersAndDuplicates(VOID)
{
    LONG Live = 0;
    PSS_ALLOCATOR Allocator = { &Live, MarkerAlloc, MarkerFree };
    HPSSWALK Marker;
    HPSS Snapshot, Duplicate = NULL;
    PSS_PROCESS_INFORMATION Info;
    ULONG_PTR Position;
    DWORD Error;

    Error = pPssWalkMarkerCreate(&Allocator, &Marker);
    ok(Error == ERROR_SUCCESS, "error %lu\n", Error);
    ok(Live == 1, "%ld live blocks\n", Live);
    Error = pPssWalkMarkerSetPosition(Marker, 3);
    ok(Error == ERROR_SUCCESS, "set position: error %lu\n", Error);
    Position = 0;
    Error = pPssWalkMarkerGetPosition(Marker, &Position);
    ok(Error == ERROR_SUCCESS && Position == 3, "position %Iu, error %lu\n", Position, Error);
    Error = pPssWalkMarkerSeekToBeginning(Marker);
    ok(Error == ERROR_SUCCESS, "seek: error %lu\n", Error);
    Error = pPssWalkMarkerGetPosition(Marker, &Position);
    ok(Error == ERROR_SUCCESS && Position == 0, "position %Iu, error %lu\n", Position, Error);
    Error = pPssWalkMarkerFree(Marker);
    ok(Error == ERROR_SUCCESS, "free: error %lu\n", Error);
    ok(Live == 0, "%ld live blocks\n", Live);

    Error = pPssWalkMarkerGetPosition(NULL, &Position);
    ok(Error == ERROR_INVALID_HANDLE, "NULL marker: error %lu\n", Error);
    Error = pPssWalkMarkerSetPosition(NULL, 0);
    ok(Error == ERROR_INVALID_HANDLE, "NULL marker, set: error %lu\n", Error);
    Error = pPssWalkMarkerSeekToBeginning(NULL);
    ok(Error == ERROR_INVALID_HANDLE, "NULL marker, seek: error %lu\n", Error);
    Error = pPssWalkMarkerFree(NULL);
    ok(Error == ERROR_INVALID_HANDLE, "NULL marker, free: error %lu\n", Error);

    Error = pPssCaptureSnapshot(GetCurrentProcess(), PSS_CAPTURE_THREADS, 0, &Snapshot);
    ok(Error == ERROR_SUCCESS, "capture: error %lu\n", Error);
    if (Error != ERROR_SUCCESS)
        return;

    /* A duplicate is another snapshot of the same picture, which the original does not have to outlive */
    Error = pPssDuplicateSnapshot(GetCurrentProcess(), Snapshot, GetCurrentProcess(), &Duplicate, PSS_DUPLICATE_NONE);
    ok(Error == ERROR_SUCCESS, "duplicate: error %lu\n", Error);
    ok(Duplicate != NULL && Duplicate != Snapshot, "duplicate %p, original %p\n", Duplicate, Snapshot);
    if (Error == ERROR_SUCCESS)
    {
        Error = pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
        ok(Error == ERROR_SUCCESS, "free: error %lu\n", Error);
        Error = Query(Duplicate, PSS_QUERY_PROCESS_INFORMATION, &Info, sizeof(Info));
        ok(Error == ERROR_SUCCESS, "query the duplicate: error %lu\n", Error);
        ok(Info.ProcessId == GetCurrentProcessId(), "ProcessId %lu\n", Info.ProcessId);
        Error = pPssFreeSnapshot(GetCurrentProcess(), Duplicate);
        ok(Error == ERROR_SUCCESS, "free the duplicate: error %lu\n", Error);
    }
    else
    {
        pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
    }

    Error = pPssDuplicateSnapshot(GetCurrentProcess(), NULL, GetCurrentProcess(), &Duplicate, PSS_DUPLICATE_NONE);
    ok(Error == ERROR_INVALID_HANDLE, "NULL snapshot: error %lu\n", Error);
    Error = pPssFreeSnapshot(GetCurrentProcess(), NULL);
    ok(Error == ERROR_INVALID_HANDLE, "free NULL: error %lu\n", Error);
    Error = pPssFreeSnapshot(NULL, (HPSS)(ULONG_PTR)0x1234);
    ok(Error == ERROR_INVALID_HANDLE, "free with NULL process: error %lu\n", Error);
}

static
VOID
TestOtherProcess(VOID)
{
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    WCHAR FileName[MAX_PATH], CommandLine[MAX_PATH + 32];
    HPSS Snapshot;
    HPSSWALK Marker;
    PSS_PROCESS_INFORMATION Info;
    PSS_THREAD_INFORMATION Threads;
    PSS_THREAD_ENTRY Entry;
    DWORD Error, Count;

    GetModuleFileNameW(NULL, FileName, ARRAYSIZE(FileName));
    _snwprintf(CommandLine, ARRAYSIZE(CommandLine), L"\"%ls\" PssSnapshot child", FileName);
    if (!CreateProcessW(NULL, CommandLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &StartupInfo,
                        &ProcessInfo))
    {
        skip("CreateProcess failed: %lu\n", GetLastError());
        return;
    }
    Sleep(500);

    Error = pPssCaptureSnapshot(ProcessInfo.hProcess, PSS_CAPTURE_THREADS | PSS_CAPTURE_VA_SPACE, 0, &Snapshot);
    ok(Error == ERROR_SUCCESS, "error %lu\n", Error);
    if (Error == ERROR_SUCCESS)
    {
        Error = Query(Snapshot, PSS_QUERY_PROCESS_INFORMATION, &Info, sizeof(Info));
        ok(Error == ERROR_SUCCESS, "query: error %lu\n", Error);
        ok(Info.ProcessId == ProcessInfo.dwProcessId, "ProcessId %lu, expected %lu\n", Info.ProcessId,
           ProcessInfo.dwProcessId);
        ok(Info.ParentProcessId == GetCurrentProcessId(), "ParentProcessId %lu\n", Info.ParentProcessId);
        ok(Info.ExitStatus == STILL_ACTIVE, "ExitStatus %lx\n", Info.ExitStatus);
        ok(EndsWithNoCase(Info.ImageFileName, BaseName(FileName)), "ImageFileName %ls\n", Info.ImageFileName);

        Error = Query(Snapshot, PSS_QUERY_THREAD_INFORMATION, &Threads, sizeof(Threads));
        ok(Error == ERROR_SUCCESS, "threads: error %lu\n", Error);
        ok(Threads.ThreadsCaptured >= 1, "%lu threads\n", Threads.ThreadsCaptured);

        Error = CreateMarker(&Marker);
        ok(Error == ERROR_SUCCESS, "marker: error %lu\n", Error);
        Count = 0;
        while (Error == ERROR_SUCCESS &&
               pPssWalkSnapshot(Snapshot, PSS_WALK_THREADS, Marker, &Entry, sizeof(Entry)) == ERROR_SUCCESS)
        {
            ok(Entry.ProcessId == ProcessInfo.dwProcessId, "ProcessId %lu\n", Entry.ProcessId);
            Count++;
        }
        ok(Count == Threads.ThreadsCaptured, "walked %lu, captured %lu\n", Count, Threads.ThreadsCaptured);
        if (Error == ERROR_SUCCESS)
            pPssWalkMarkerFree(Marker);

        /* The snapshot is in this process, not in the one that it is of */
        Error = pPssFreeSnapshot(ProcessInfo.hProcess, Snapshot);
        ok(Error == ERROR_PARTIAL_COPY, "free with the process that was captured: error %lu\n", Error);
        Error = pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
        ok(Error == ERROR_SUCCESS, "free: error %lu\n", Error);

        /* The process was not stopped to be captured */
        ok(WaitForSingleObject(ProcessInfo.hProcess, 0) == WAIT_TIMEOUT, "the process exited\n");
    }

    TerminateProcess(ProcessInfo.hProcess, 3);
    WaitForSingleObject(ProcessInfo.hProcess, 10000);

    /* One that has exited can still be captured */
    Error = pPssCaptureSnapshot(ProcessInfo.hProcess, PSS_CAPTURE_NONE, 0, &Snapshot);
    ok(Error == ERROR_SUCCESS, "exited: error %lu\n", Error);
    if (Error == ERROR_SUCCESS)
    {
        Error = Query(Snapshot, PSS_QUERY_PROCESS_INFORMATION, &Info, sizeof(Info));
        ok(Error == ERROR_SUCCESS, "exited: query: error %lu\n", Error);
        ok(Info.ExitStatus == 3, "exited: ExitStatus %lx\n", Info.ExitStatus);
        ok(Info.ProcessId == ProcessInfo.dwProcessId, "exited: ProcessId %lu\n", Info.ProcessId);
        ok(Info.ImageFileName[0] == UNICODE_NULL, "exited: ImageFileName %ls\n", Info.ImageFileName);
        ok(Info.ExitTime.dwHighDateTime != 0 || Info.ExitTime.dwLowDateTime != 0, "exited: no ExitTime\n");
        pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
    }

    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
}

static
VOID
TestVaClone(VOID)
{
    HPSS Snapshot;
    PSS_VA_CLONE_INFORMATION Info;
    DWORD Error;

    Error = pPssCaptureSnapshot(GetCurrentProcess(), PSS_CAPTURE_VA_CLONE, 0, &Snapshot);
    if (Error == ERROR_NOT_SUPPORTED)
    {
        skip("Cloning the address space is not supported\n");
        return;
    }

    ok(Error == ERROR_SUCCESS, "error %lu\n", Error);
    if (Error != ERROR_SUCCESS)
        return;

    Error = Query(Snapshot, PSS_QUERY_VA_CLONE_INFORMATION, &Info, sizeof(Info));
    ok(Error == ERROR_SUCCESS, "query: error %lu\n", Error);
    if (Error == ERROR_SUCCESS)
    {
        ok(Info.VaCloneHandle != NULL, "no clone\n");
        ok(GetProcessId(Info.VaCloneHandle) != GetCurrentProcessId(), "the clone is this process\n");
    }
    pPssFreeSnapshot(GetCurrentProcess(), Snapshot);
}

START_TEST(PssSnapshot)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    char **argv;
    int argc = winetest_get_mainargs(&argv);

    /* The process that is captured by TestOtherProcess */
    if (argc >= 3 && !strcmp(argv[2], "child"))
    {
        Sleep(120000);
        return;
    }

#define LOAD(Name) \
    p##Name = (PVOID)GetProcAddress(Kernel32, #Name); \
    if (!p##Name) \
    { \
        skip(#Name " is not available\n"); \
        return; \
    }
    LOAD(PssCaptureSnapshot)
    LOAD(PssFreeSnapshot)
    LOAD(PssQuerySnapshot)
    LOAD(PssDuplicateSnapshot)
    LOAD(PssWalkMarkerCreate)
    LOAD(PssWalkMarkerFree)
    LOAD(PssWalkMarkerGetPosition)
    LOAD(PssWalkMarkerSetPosition)
    LOAD(PssWalkMarkerSeekToBeginning)
    LOAD(PssWalkSnapshot)
#undef LOAD

    /* kernelbase.dll exports them as well */
    {
        static const PCSTR Names[] =
        {
            "PssCaptureSnapshot", "PssDuplicateSnapshot", "PssFreeSnapshot", "PssQuerySnapshot",
            "PssWalkMarkerCreate", "PssWalkMarkerFree", "PssWalkMarkerGetPosition", "PssWalkMarkerSeekToBeginning",
            "PssWalkMarkerSetPosition", "PssWalkSnapshot"
        };
        HMODULE KernelBase = LoadLibraryW(L"kernelbase.dll");
        ULONG Index;

        ok(KernelBase != NULL, "kernelbase.dll did not load: %lu\n", GetLastError());
        for (Index = 0; KernelBase && Index < ARRAYSIZE(Names); Index++)
            ok(GetProcAddress(KernelBase, Names[Index]) != NULL, "kernelbase.dll does not export %s\n", Names[Index]);
    }

    trace("TestCaptureFlagsAndHandles\n"); TestCaptureFlagsAndHandles();
    trace("TestProcessInformation\n"); TestProcessInformation();
    trace("TestThreads\n"); TestThreads();
    trace("TestHandles\n"); TestHandles();
    trace("TestVaSpace\n"); TestVaSpace();
    trace("TestMarkersAndDuplicates\n"); TestMarkersAndDuplicates();
    trace("TestVaClone\n"); TestVaClone();
    trace("TestOtherProcess\n"); TestOtherProcess();
}
