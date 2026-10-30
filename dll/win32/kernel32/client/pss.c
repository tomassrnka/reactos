/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Process snapshotting (PssCaptureSnapshot and friends) of Windows 8.1 and later
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES *******************************************************************/

#include <k32.h>
#include <processsnapshot.h>
#include <intrin.h>

#define NDEBUG
#include <debug.h>

/*
 * A snapshot is kept in the memory of the process that took it, as it is on Windows (the process handle that
 * PssFreeSnapshot takes is that process, not the snapshotted one), and is complete when the capture returns:
 * the walk and query functions only read it. The result codes were measured on Windows 10 (version 22H2):
 *
 *  - classes that were not captured answer ERROR_NOT_FOUND, the end of a walk ERROR_NO_MORE_ITEMS, and buffers have
 *    to have exactly the size of the structure (ERROR_BAD_LENGTH)
 *  - capture flags outside the documented ones are ERROR_INVALID_PARAMETER
 *
 * ReactOS cannot clone an address space, so PSS_CAPTURE_VA_CLONE is not supported (ERROR_NOT_SUPPORTED). The
 * address space is still captured as a list of regions (PSS_CAPTURE_VA_SPACE), without the contents of the pages.
 * No handle tracing, Intel PT trace or auxiliary pages are captured, and a snapshot can only be duplicated into
 * the process that owns it.
 */

/* DEFINITIONS ****************************************************************/

/* Declared by winbase.h only from Windows Vista on */
BOOL WINAPI QueryFullProcessImageNameW(HANDLE, DWORD, LPWSTR, PDWORD);

#define PSSP_SNAPSHOT_MAGIC 0x4E535350 /* 'PSSN' */
#define PSSP_MARKER_MAGIC   0x4D535350 /* 'PSSM' */

#define PSSP_VALID_CAPTURE_FLAGS \
    (PSS_CAPTURE_VA_CLONE | PSS_CAPTURE_RESERVED_00000002 | PSS_CAPTURE_HANDLES | \
     PSS_CAPTURE_HANDLE_NAME_INFORMATION | PSS_CAPTURE_HANDLE_BASIC_INFORMATION | \
     PSS_CAPTURE_HANDLE_TYPE_SPECIFIC_INFORMATION | PSS_CAPTURE_HANDLE_TRACE | PSS_CAPTURE_THREADS | \
     PSS_CAPTURE_THREAD_CONTEXT | PSS_CAPTURE_THREAD_CONTEXT_EXTENDED | PSS_CAPTURE_RESERVED_00000400 | \
     PSS_CAPTURE_VA_SPACE | PSS_CAPTURE_VA_SPACE_SECTION_INFORMATION | PSS_CAPTURE_IPT_TRACE | \
     PSS_CAPTURE_RESERVED_00004000 | PSS_CREATE_BREAKAWAY_OPTIONAL | PSS_CREATE_BREAKAWAY | \
     PSS_CREATE_FORCE_BREAKAWAY | PSS_CREATE_USE_VM_ALLOCATIONS | PSS_CREATE_MEASURE_PERFORMANCE | \
     PSS_CREATE_RELEASE_SECTION)

#define PSSP_HANDLE_FLAGS \
    (PSS_CAPTURE_HANDLE_NAME_INFORMATION | PSS_CAPTURE_HANDLE_BASIC_INFORMATION | \
     PSS_CAPTURE_HANDLE_TYPE_SPECIFIC_INFORMATION)

/* The longest an object name query on a file handle may take (it can block behind a pending synchronous I/O) */
#define PSSP_NAME_QUERY_TIMEOUT 250
#define PSSP_NAME_BUFFER_SIZE   0x2000
#define PSSP_TYPE_BUFFER_SIZE   0x400

typedef struct _PSSP_BLOCK
{
    struct _PSSP_BLOCK *Next;
    ULONG_PTR Reserved; /* keeps the data that follows 16-byte aligned on 64-bit */
} PSSP_BLOCK;

typedef struct _PSSP_DATA
{
    LONG ReferenceCount;
    ULONG CaptureFlags;
    BOOLEAN HaveThreads;
    BOOLEAN HaveHandles;
    BOOLEAN HaveVaSpace;
    PSS_PROCESS_INFORMATION Process;
    ULONG ThreadCount;
    ULONG ContextLength;
    PSS_THREAD_ENTRY *Threads;
    ULONG HandleCount;
    PSS_HANDLE_ENTRY *Handles;
    ULONG RegionCount;
    PSS_VA_SPACE_ENTRY *Regions;
    PSS_PERFORMANCE_COUNTERS Performance;
    PSSP_BLOCK *Blocks;
} PSSP_DATA;

typedef struct _PSSP_SNAPSHOT
{
    ULONG Magic;
    PSSP_DATA *Data;
} PSSP_SNAPSHOT;

typedef struct _PSSP_MARKER
{
    ULONG Magic;
    PSS_ALLOCATOR Allocator;
    ULONG_PTR Position;
} PSSP_MARKER;

typedef struct _PSSP_TIMER
{
    ULONG64 Cycles;
    LARGE_INTEGER Counter;
} PSSP_TIMER;

typedef struct _PSSP_NAME_QUERY
{
    HANDLE Handle;
    NTSTATUS Status;
    UCHAR Buffer[PSSP_NAME_BUFFER_SIZE];
} PSSP_NAME_QUERY;

/* HELPERS ********************************************************************/

static
VOID
PsspTimerStart(
    _Out_ PSSP_TIMER *Timer)
{
    QueryPerformanceCounter(&Timer->Counter);
    Timer->Cycles = __rdtsc();
}

static
VOID
PsspTimerStop(
    _In_ const PSSP_TIMER *Timer,
    _Out_ ULONG64 *Cycles,
    _Out_ ULONG64 *Period)
{
    LARGE_INTEGER Now, Frequency;

    *Cycles = __rdtsc() - Timer->Cycles;
    QueryPerformanceCounter(&Now);
    if (!QueryPerformanceFrequency(&Frequency) || Frequency.QuadPart == 0)
    {
        *Period = 0;
        return;
    }
    *Period = (ULONG64)(Now.QuadPart - Timer->Counter.QuadPart) * PSS_PERF_RESOLUTION / Frequency.QuadPart;
}

static
PVOID
PsspAllocate(
    _Inout_ PSSP_DATA *Data,
    _In_ SIZE_T Size)
{
    PSSP_BLOCK *Block;

    if (Size > MAXULONG - sizeof(*Block))
        return NULL;

    Block = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Block) + Size);
    if (!Block)
        return NULL;

    Block->Next = Data->Blocks;
    Data->Blocks = Block;
    return Block + 1;
}

/* Copies a counted string into the snapshot, with a terminator */
static
PWCHAR
PsspCopyString(
    _Inout_ PSSP_DATA *Data,
    _In_ PCWSTR String,
    _In_ USHORT Length)
{
    PWCHAR Copy = PsspAllocate(Data, (SIZE_T)Length + sizeof(WCHAR));

    if (Copy)
        RtlCopyMemory(Copy, String, Length);
    return Copy;
}

static
VOID
PsspFreeData(
    _In_ PSSP_DATA *Data)
{
    PSSP_BLOCK *Block, *Next;

    for (Block = Data->Blocks; Block; Block = Next)
    {
        Next = Block->Next;
        RtlFreeHeap(RtlGetProcessHeap(), 0, Block);
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Data);
}

static
VOID
PsspRelease(
    _In_ PSSP_DATA *Data)
{
    if (InterlockedDecrement(&Data->ReferenceCount) == 0)
        PsspFreeData(Data);
}

static
VOID
PsspFileTime(
    _In_ const LARGE_INTEGER *Time,
    _Out_ FILETIME *FileTime)
{
    FileTime->dwLowDateTime = Time->LowPart;
    FileTime->dwHighDateTime = Time->HighPart;
}

static
BOOLEAN
PsspIsCurrentProcess(
    _In_ HANDLE ProcessHandle,
    _Out_ PDWORD Error)
{
    PROCESS_BASIC_INFORMATION Basic;
    NTSTATUS Status;

    if (ProcessHandle == NtCurrentProcess())
        return TRUE;

    Status = NtQueryInformationProcess(ProcessHandle, ProcessBasicInformation, &Basic, sizeof(Basic), NULL);
    if (!NT_SUCCESS(Status))
    {
        *Error = RtlNtStatusToDosError(Status);
        return FALSE;
    }
    *Error = ERROR_PARTIAL_COPY; /* the snapshot is not in that process */
    return Basic.UniqueProcessId == (ULONG_PTR)NtCurrentTeb()->ClientId.UniqueProcess;
}

static
BOOLEAN
PsspStringIs(
    _In_ const UNICODE_STRING *String,
    _In_ PCWSTR Name)
{
    UNICODE_STRING Other;

    RtlInitUnicodeString(&Other, Name);
    return RtlEqualUnicodeString(String, &Other, FALSE);
}

/* PROCESS ********************************************************************/

/* The handle has to allow reading the process in the way that the capture needs */
static
DWORD
PsspCheckAccess(
    _In_ HANDLE ProcessHandle,
    _In_ ACCESS_MASK Required)
{
    OBJECT_BASIC_INFORMATION Basic;
    NTSTATUS Status;

    if (ProcessHandle == NtCurrentProcess())
        return ERROR_SUCCESS;

    Status = NtQueryObject(ProcessHandle, ObjectBasicInformation, &Basic, sizeof(Basic), NULL);
    if (!NT_SUCCESS(Status))
        return RtlNtStatusToDosError(Status);
    return (Basic.GrantedAccess & Required) == Required ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

static
DWORD
PsspCaptureProcess(
    _In_ HANDLE ProcessHandle,
    _Out_ PSS_PROCESS_INFORMATION *Info)
{
    PROCESS_BASIC_INFORMATION Basic;
    KERNEL_USER_TIMES Times;
    VM_COUNTERS_EX Counters;
    ULONG_PTR Wow64;
    ULONG ExecuteFlags;
    DWORD Size;
    NTSTATUS Status;

    Status = NtQueryInformationProcess(ProcessHandle, ProcessBasicInformation, &Basic, sizeof(Basic), NULL);
    if (!NT_SUCCESS(Status))
        return RtlNtStatusToDosError(Status);

    Info->ExitStatus = Basic.ExitStatus;
    Info->PebBaseAddress = Basic.PebBaseAddress;
    Info->AffinityMask = Basic.AffinityMask;
    Info->BasePriority = Basic.BasePriority;
    Info->ProcessId = (DWORD)Basic.UniqueProcessId;
    Info->ParentProcessId = (DWORD)Basic.InheritedFromUniqueProcessId;

    Info->Flags = PSS_PROCESS_FLAGS_NONE;
    if (NT_SUCCESS(NtQueryInformationProcess(ProcessHandle, ProcessWow64Information, &Wow64, sizeof(Wow64), NULL)) &&
        Wow64 != 0)
    {
        Info->Flags |= PSS_PROCESS_FLAGS_WOW64;
    }
    if (Basic.ExitStatus != STATUS_PENDING)
        Info->Flags |= PSS_PROCESS_FLAGS_RESERVED_03; /* Windows sets this bit on a process that has exited */

    if (NT_SUCCESS(NtQueryInformationProcess(ProcessHandle, ProcessTimes, &Times, sizeof(Times), NULL)))
    {
        PsspFileTime(&Times.CreateTime, &Info->CreateTime);
        PsspFileTime(&Times.ExitTime, &Info->ExitTime);
        PsspFileTime(&Times.KernelTime, &Info->KernelTime);
        PsspFileTime(&Times.UserTime, &Info->UserTime);
    }

    Info->PriorityClass = GetPriorityClass(ProcessHandle);
    if (Info->PriorityClass == 0)
        Info->PriorityClass = NORMAL_PRIORITY_CLASS;

    RtlZeroMemory(&Counters, sizeof(Counters));
    if (NT_SUCCESS(NtQueryInformationProcess(ProcessHandle, ProcessVmCounters, &Counters, sizeof(Counters), NULL)) ||
        NT_SUCCESS(NtQueryInformationProcess(ProcessHandle, ProcessVmCounters, &Counters, sizeof(VM_COUNTERS), NULL)))
    {
        Info->PeakVirtualSize = Counters.PeakVirtualSize;
        Info->VirtualSize = Counters.VirtualSize;
        Info->PageFaultCount = Counters.PageFaultCount;
        Info->PeakWorkingSetSize = Counters.PeakWorkingSetSize;
        Info->WorkingSetSize = Counters.WorkingSetSize;
        Info->QuotaPeakPagedPoolUsage = Counters.QuotaPeakPagedPoolUsage;
        Info->QuotaPagedPoolUsage = Counters.QuotaPagedPoolUsage;
        Info->QuotaPeakNonPagedPoolUsage = Counters.QuotaPeakNonPagedPoolUsage;
        Info->QuotaNonPagedPoolUsage = Counters.QuotaNonPagedPoolUsage;
        Info->PagefileUsage = Counters.PagefileUsage;
        Info->PeakPagefileUsage = Counters.PeakPagefileUsage;
        Info->PrivateUsage = Counters.PrivateUsage ? Counters.PrivateUsage : Counters.PagefileUsage;
    }

    if (NT_SUCCESS(NtQueryInformationProcess(ProcessHandle, ProcessExecuteFlags, &ExecuteFlags, sizeof(ExecuteFlags),
                                             NULL)))
    {
        Info->ExecuteFlags = ExecuteFlags;
    }

    /* A process that has exited has no image name */
    Size = RTL_NUMBER_OF(Info->ImageFileName);
    if (!QueryFullProcessImageNameW(ProcessHandle, 0, Info->ImageFileName, &Size))
        Info->ImageFileName[0] = UNICODE_NULL;

    return ERROR_SUCCESS;
}

/* THREADS ********************************************************************/

static
PSYSTEM_PROCESS_INFORMATION
PsspQueryProcesses(VOID)
{
    PVOID Buffer = NULL;
    ULONG Size = 0x40000;
    NTSTATUS Status;

    for (;;)
    {
        Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Size);
        if (!Buffer)
            return NULL;
        Status = NtQuerySystemInformation(SystemProcessInformation, Buffer, Size, NULL);
        if (NT_SUCCESS(Status))
            return Buffer;

        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
        if (Status != STATUS_INFO_LENGTH_MISMATCH || Size >= 0x4000000)
            return NULL;
        Size *= 2;
    }
}

static
VOID
PsspCaptureThread(
    _Inout_ PSSP_DATA *Data,
    _In_ const SYSTEM_THREAD_INFORMATION *SystemThread,
    _In_ ULONG ThreadContextFlags,
    _Inout_ PSS_THREAD_ENTRY *Entry)
{
    THREAD_BASIC_INFORMATION Basic;
    KERNEL_USER_TIMES Times;
    PVOID StartAddress;
    OBJECT_ATTRIBUTES ObjectAttributes = { sizeof(ObjectAttributes) };
    ACCESS_MASK Access = THREAD_QUERY_INFORMATION;
    HANDLE Thread;
    NTSTATUS Status;
    BOOLEAN Current, Terminated = FALSE;

    Entry->ProcessId = (DWORD)(ULONG_PTR)SystemThread->ClientId.UniqueProcess;
    Entry->ThreadId = (DWORD)(ULONG_PTR)SystemThread->ClientId.UniqueThread;
    Entry->Priority = SystemThread->Priority;
    Entry->BasePriority = SystemThread->BasePriority;
    Entry->Win32StartAddress = SystemThread->StartAddress;
    Entry->ExitStatus = STATUS_PENDING;
    PsspFileTime(&SystemThread->CreateTime, &Entry->CreateTime);
    PsspFileTime(&SystemThread->KernelTime, &Entry->KernelTime);
    PsspFileTime(&SystemThread->UserTime, &Entry->UserTime);
    GetSystemTimeAsFileTime(&Entry->CaptureTime);

    Current = Entry->ThreadId == HandleToUlong(NtCurrentTeb()->ClientId.UniqueThread) &&
              Entry->ProcessId == HandleToUlong(NtCurrentTeb()->ClientId.UniqueProcess);

    if (ThreadContextFlags && !Current)
        Access |= THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME;
    Status = NtOpenThread(&Thread, Access, &ObjectAttributes, (PCLIENT_ID)&SystemThread->ClientId);
    if (!NT_SUCCESS(Status) && Access != THREAD_QUERY_INFORMATION)
    {
        Access = THREAD_QUERY_INFORMATION;
        Status = NtOpenThread(&Thread, Access, &ObjectAttributes, (PCLIENT_ID)&SystemThread->ClientId);
    }
    if (!NT_SUCCESS(Status))
    {
        /* The thread went away or cannot be opened: what the system list says is all there is */
        if (Status == STATUS_INVALID_CID)
            Entry->Flags |= PSS_THREAD_FLAGS_TERMINATED;
        return;
    }

    if (NT_SUCCESS(NtQueryInformationThread(Thread, ThreadBasicInformation, &Basic, sizeof(Basic), NULL)))
    {
        Entry->ExitStatus = Basic.ExitStatus;
        Entry->TebBaseAddress = Basic.TebBaseAddress;
        Entry->AffinityMask = Basic.AffinityMask;
        Entry->Priority = Basic.Priority;
        Entry->BasePriority = Basic.BasePriority - Data->Process.BasePriority;
        Terminated = Basic.ExitStatus != STATUS_PENDING;
    }
    if (NT_SUCCESS(NtQueryInformationThread(Thread, ThreadTimes, &Times, sizeof(Times), NULL)))
    {
        PsspFileTime(&Times.CreateTime, &Entry->CreateTime);
        PsspFileTime(&Times.ExitTime, &Entry->ExitTime);
        PsspFileTime(&Times.KernelTime, &Entry->KernelTime);
        PsspFileTime(&Times.UserTime, &Entry->UserTime);
    }
    if (NT_SUCCESS(NtQueryInformationThread(Thread, ThreadQuerySetWin32StartAddress, &StartAddress,
                                            sizeof(StartAddress), NULL)) &&
        StartAddress)
    {
        Entry->Win32StartAddress = StartAddress;
    }

    if (Terminated)
    {
        Entry->Flags |= PSS_THREAD_FLAGS_TERMINATED;
        Entry->TebBaseAddress = NULL;
    }
    else if (ThreadContextFlags)
    {
        PCONTEXT Context = PsspAllocate(Data, sizeof(CONTEXT));

        if (Context)
        {
            Context->ContextFlags = ThreadContextFlags;
            if (Current)
            {
                /* A thread cannot suspend itself to read its context */
                RtlCaptureContext(Context);
                Context->ContextFlags = ThreadContextFlags;
                Entry->ContextRecord = Context;
                Entry->SizeOfContextRecord = sizeof(CONTEXT);
            }
            else
            {
                ULONG SuspendCount = 0;

                if (NT_SUCCESS(NtSuspendThread(Thread, &SuspendCount)))
                {
                    Status = NtGetContextThread(Thread, Context);
                    NtResumeThread(Thread, NULL);
                    if (NT_SUCCESS(Status))
                    {
                        Entry->ContextRecord = Context;
                        Entry->SizeOfContextRecord = sizeof(CONTEXT);
                        Entry->SuspendCount = (WORD)SuspendCount;
                    }
                }
            }
        }
    }

    NtClose(Thread);
}

static
DWORD
PsspCaptureThreads(
    _Inout_ PSSP_DATA *Data,
    _In_ ULONG ThreadContextFlags,
    _In_ BOOLEAN WithContext)
{
    PSYSTEM_PROCESS_INFORMATION Processes, Process;
    PSYSTEM_THREAD_INFORMATION Threads;
    ULONG Index;
    DWORD Error = ERROR_SUCCESS;

    Processes = PsspQueryProcesses();
    if (!Processes)
        return ERROR_NOT_ENOUGH_MEMORY;

    Process = Processes;
    for (;;)
    {
        if ((ULONG_PTR)Process->UniqueProcessId == Data->Process.ProcessId)
            break;
        if (Process->NextEntryOffset == 0)
        {
            /* A process that has exited has no threads that the system lists */
            Process = NULL;
            break;
        }
        Process = (PSYSTEM_PROCESS_INFORMATION)((PUCHAR)Process + Process->NextEntryOffset);
    }

    if (Process && Process->NumberOfThreads)
    {
        Data->Threads = PsspAllocate(Data, Process->NumberOfThreads * sizeof(PSS_THREAD_ENTRY));
        if (!Data->Threads)
        {
            Error = ERROR_NOT_ENOUGH_MEMORY;
        }
        else
        {
            Threads = (PSYSTEM_THREAD_INFORMATION)(Process + 1);
            for (Index = 0; Index < Process->NumberOfThreads; Index++)
                PsspCaptureThread(Data, &Threads[Index], ThreadContextFlags, &Data->Threads[Index]);
            Data->ThreadCount = Process->NumberOfThreads;
        }
    }

    if (WithContext)
        Data->ContextLength = sizeof(CONTEXT);

    RtlFreeHeap(RtlGetProcessHeap(), 0, Processes);
    return Error;
}

/* HANDLES ********************************************************************/

static
DWORD
WINAPI
PsspQueryNameThread(
    _In_ PVOID Parameter)
{
    PSSP_NAME_QUERY *Query = Parameter;

    Query->Status = NtQueryObject(Query->Handle, ObjectNameInformation, Query->Buffer, sizeof(Query->Buffer), NULL);
    return 0;
}

/*
 * The name of a file object can only be read once any synchronous I/O that is pending on it is done, so the query
 * for a file runs in a thread that is given up when it does not return.
 */
static
NTSTATUS
PsspQueryFileName(
    _In_ HANDLE Handle,
    _Out_ PUNICODE_STRING Name,
    _Out_ PVOID *Storage)
{
    PSSP_NAME_QUERY *Query;
    HANDLE Thread;
    DWORD Wait;

    Query = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Query));
    if (!Query)
        return STATUS_NO_MEMORY;

    Query->Handle = Handle;
    Query->Status = STATUS_UNSUCCESSFUL;
    Thread = CreateThread(NULL, 0, PsspQueryNameThread, Query, 0, NULL);
    if (!Thread)
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Query);
        return STATUS_NO_MEMORY;
    }

    Wait = WaitForSingleObject(Thread, PSSP_NAME_QUERY_TIMEOUT);
    CloseHandle(Thread);
    if (Wait != WAIT_OBJECT_0)
    {
        /* The thread still owns the buffer and the handle: both are given up with it */
        return STATUS_TIMEOUT;
    }

    *Storage = Query;
    *Name = ((POBJECT_NAME_INFORMATION)Query->Buffer)->Name;
    return Query->Status;
}

static
VOID
PsspCaptureHandleTypeSpecific(
    _In_ HANDLE Handle,
    _Inout_ PSS_HANDLE_ENTRY *Entry)
{
    switch (Entry->ObjectType)
    {
        case PSS_OBJECT_TYPE_PROCESS:
        {
            PROCESS_BASIC_INFORMATION Basic;

            if (!NT_SUCCESS(NtQueryInformationProcess(Handle, ProcessBasicInformation, &Basic, sizeof(Basic), NULL)))
                return;
            Entry->TypeSpecificInformation.Process.ExitStatus = Basic.ExitStatus;
            Entry->TypeSpecificInformation.Process.PebBaseAddress = Basic.PebBaseAddress;
            Entry->TypeSpecificInformation.Process.AffinityMask = Basic.AffinityMask;
            Entry->TypeSpecificInformation.Process.BasePriority = Basic.BasePriority;
            Entry->TypeSpecificInformation.Process.ProcessId = (DWORD)Basic.UniqueProcessId;
            Entry->TypeSpecificInformation.Process.ParentProcessId = (DWORD)Basic.InheritedFromUniqueProcessId;
            break;
        }
        case PSS_OBJECT_TYPE_THREAD:
        {
            THREAD_BASIC_INFORMATION Basic;
            PVOID StartAddress;

            if (!NT_SUCCESS(NtQueryInformationThread(Handle, ThreadBasicInformation, &Basic, sizeof(Basic), NULL)))
                return;
            Entry->TypeSpecificInformation.Thread.ExitStatus = Basic.ExitStatus;
            Entry->TypeSpecificInformation.Thread.TebBaseAddress = Basic.TebBaseAddress;
            Entry->TypeSpecificInformation.Thread.ProcessId = (DWORD)(ULONG_PTR)Basic.ClientId.UniqueProcess;
            Entry->TypeSpecificInformation.Thread.ThreadId = (DWORD)(ULONG_PTR)Basic.ClientId.UniqueThread;
            Entry->TypeSpecificInformation.Thread.AffinityMask = Basic.AffinityMask;
            Entry->TypeSpecificInformation.Thread.Priority = Basic.Priority;
            Entry->TypeSpecificInformation.Thread.BasePriority = Basic.BasePriority;
            if (NT_SUCCESS(NtQueryInformationThread(Handle, ThreadQuerySetWin32StartAddress, &StartAddress,
                                                    sizeof(StartAddress), NULL)))
            {
                Entry->TypeSpecificInformation.Thread.Win32StartAddress = StartAddress;
            }
            break;
        }
        case PSS_OBJECT_TYPE_MUTANT:
        {
            MUTANT_BASIC_INFORMATION Basic;
            MUTANT_OWNER_INFORMATION Owner;

            if (!NT_SUCCESS(NtQueryMutant(Handle, MutantBasicInformation, &Basic, sizeof(Basic), NULL)))
                return;
            Entry->TypeSpecificInformation.Mutant.CurrentCount = Basic.CurrentCount;
            Entry->TypeSpecificInformation.Mutant.Abandoned = Basic.AbandonedState;
            if (NT_SUCCESS(NtQueryMutant(Handle, MutantOwnerInformation, &Owner, sizeof(Owner), NULL)))
            {
                Entry->TypeSpecificInformation.Mutant.OwnerProcessId = (DWORD)(ULONG_PTR)Owner.ClientId.UniqueProcess;
                Entry->TypeSpecificInformation.Mutant.OwnerThreadId = (DWORD)(ULONG_PTR)Owner.ClientId.UniqueThread;
            }
            break;
        }
        case PSS_OBJECT_TYPE_EVENT:
        {
            EVENT_BASIC_INFORMATION Basic;

            if (!NT_SUCCESS(NtQueryEvent(Handle, EventBasicInformation, &Basic, sizeof(Basic), NULL)))
                return;
            Entry->TypeSpecificInformation.Event.ManualReset = Basic.EventType == NotificationEvent;
            Entry->TypeSpecificInformation.Event.Signaled = Basic.EventState != 0;
            break;
        }
        case PSS_OBJECT_TYPE_SECTION:
        {
            SECTION_BASIC_INFORMATION Basic;

            if (!NT_SUCCESS(NtQuerySection(Handle, SectionBasicInformation, &Basic, sizeof(Basic), NULL)))
                return;
            Entry->TypeSpecificInformation.Section.BaseAddress = Basic.BaseAddress;
            Entry->TypeSpecificInformation.Section.AllocationAttributes = Basic.Attributes;
            Entry->TypeSpecificInformation.Section.MaximumSize = Basic.Size;
            break;
        }
        case PSS_OBJECT_TYPE_SEMAPHORE:
        {
            SEMAPHORE_BASIC_INFORMATION Basic;

            if (!NT_SUCCESS(NtQuerySemaphore(Handle, SemaphoreBasicInformation, &Basic, sizeof(Basic), NULL)))
                return;
            Entry->TypeSpecificInformation.Semaphore.CurrentCount = Basic.CurrentCount;
            Entry->TypeSpecificInformation.Semaphore.MaximumCount = Basic.MaximumCount;
            break;
        }
        default:
            return;
    }
    Entry->Flags |= PSS_HANDLE_HAVE_TYPE_SPECIFIC_INFORMATION;
}

static
VOID
PsspCaptureHandle(
    _Inout_ PSSP_DATA *Data,
    _In_ HANDLE ProcessHandle,
    _In_ const SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX *Source,
    _In_ ULONG CaptureFlags,
    _Inout_ PSS_HANDLE_ENTRY *Entry,
    _In_ PVOID Scratch)
{
    HANDLE Handle;
    OBJECT_BASIC_INFORMATION Basic;
    POBJECT_TYPE_INFORMATION Type = Scratch;
    PWCHAR String;
    PVOID Storage = NULL;
    UNICODE_STRING Name;
    BOOLEAN IsFile;
    NTSTATUS Status;

    Entry->Handle = (HANDLE)Source->HandleValue;
    Entry->Attributes = Source->HandleAttributes;
    GetSystemTimeAsFileTime(&Entry->CaptureTime);

    /* Everything else is read from a copy of the handle in this process */
    Status = NtDuplicateObject(ProcessHandle, Entry->Handle, NtCurrentProcess(), &Handle, 0, 0,
                               DUPLICATE_SAME_ACCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = NtQueryObject(Handle, ObjectTypeInformation, Type, PSSP_TYPE_BUFFER_SIZE, NULL);
    if (!NT_SUCCESS(Status))
    {
        NtClose(Handle);
        return;
    }

    String = PsspCopyString(Data, Type->TypeName.Buffer, Type->TypeName.Length);
    if (String)
    {
        Entry->TypeName = String;
        Entry->TypeNameLength = Type->TypeName.Length + sizeof(WCHAR); /* counts the terminator, as on Windows */
        Entry->Flags |= PSS_HANDLE_HAVE_TYPE;
    }

    /* Windows leaves the object type of an entry unknown unless the type specific information is asked for */
    if (CaptureFlags & PSS_CAPTURE_HANDLE_TYPE_SPECIFIC_INFORMATION)
    {
        if (PsspStringIs(&Type->TypeName, L"Process"))
            Entry->ObjectType = PSS_OBJECT_TYPE_PROCESS;
        else if (PsspStringIs(&Type->TypeName, L"Thread"))
            Entry->ObjectType = PSS_OBJECT_TYPE_THREAD;
        else if (PsspStringIs(&Type->TypeName, L"Mutant"))
            Entry->ObjectType = PSS_OBJECT_TYPE_MUTANT;
        else if (PsspStringIs(&Type->TypeName, L"Event"))
            Entry->ObjectType = PSS_OBJECT_TYPE_EVENT;
        else if (PsspStringIs(&Type->TypeName, L"Section"))
            Entry->ObjectType = PSS_OBJECT_TYPE_SECTION;
        else if (PsspStringIs(&Type->TypeName, L"Semaphore"))
            Entry->ObjectType = PSS_OBJECT_TYPE_SEMAPHORE;
    }
    IsFile = PsspStringIs(&Type->TypeName, L"File");

    if (CaptureFlags & PSS_CAPTURE_HANDLE_BASIC_INFORMATION)
    {
        if (NT_SUCCESS(NtQueryObject(Handle, ObjectBasicInformation, &Basic, sizeof(Basic), NULL)))
        {
            Entry->GrantedAccess = Basic.GrantedAccess;
            /* The copy of the handle holds one of each */
            Entry->HandleCount = Basic.HandleCount ? Basic.HandleCount - 1 : 0;
            Entry->PointerCount = Basic.PointerCount ? Basic.PointerCount - 1 : 0;
            Entry->PagedPoolCharge = Basic.PagedPoolCharge;
            Entry->NonPagedPoolCharge = Basic.NonPagedPoolCharge;
            PsspFileTime(&Basic.CreationTime, &Entry->CreationTime);
            Entry->Flags |= PSS_HANDLE_HAVE_BASIC_INFORMATION;
        }
    }

    if (CaptureFlags & PSS_CAPTURE_HANDLE_NAME_INFORMATION)
    {
        if (IsFile)
        {
            Status = PsspQueryFileName(Handle, &Name, &Storage);
        }
        else
        {
            Status = NtQueryObject(Handle, ObjectNameInformation, Scratch, PSSP_NAME_BUFFER_SIZE, NULL);
            Name = ((POBJECT_NAME_INFORMATION)Scratch)->Name;
        }

        if (NT_SUCCESS(Status) && Name.Length != 0)
        {
            String = PsspCopyString(Data, Name.Buffer, Name.Length);
            if (String)
            {
                Entry->ObjectName = String;
                Entry->ObjectNameLength = Name.Length + sizeof(WCHAR);
                Entry->Flags |= PSS_HANDLE_HAVE_NAME;
            }
        }
        if (Storage)
            RtlFreeHeap(RtlGetProcessHeap(), 0, Storage);
        if (Status == STATUS_TIMEOUT)
        {
            /* The query thread may still be using the handle */
            return;
        }
    }

    if (CaptureFlags & PSS_CAPTURE_HANDLE_TYPE_SPECIFIC_INFORMATION)
        PsspCaptureHandleTypeSpecific(Handle, Entry);

    NtClose(Handle);
}

/*
 * The table of all handles in the system, in the extended form. A 32-bit process on a 64-bit system cannot ask for
 * that form, and gets the old one, which has 16-bit process ids and handle values.
 */
static
NTSTATUS
PsspQueryHandleTable(
    _Out_ PSYSTEM_HANDLE_TABLE_ENTRY_INFO_EX *Table,
    _Out_ PULONG Count,
    _Out_ PVOID *Storage)
{
    PVOID Buffer;
    ULONG Size = 0x100000, Index;
    BOOLEAN Extended = TRUE;
    NTSTATUS Status;

    for (;;)
    {
        Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Size);
        if (!Buffer)
            return STATUS_NO_MEMORY;

        Status = NtQuerySystemInformation(Extended ? SystemExtendedHandleInformation : SystemHandleInformation,
                                          Buffer, Size, NULL);
        if (NT_SUCCESS(Status))
            break;

        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
        if (Status == STATUS_INFO_LENGTH_MISMATCH && Size < 0x10000000)
        {
            Size *= 2;
        }
        else if (Extended && (Status == STATUS_NOT_SUPPORTED || Status == STATUS_INVALID_INFO_CLASS ||
                              Status == STATUS_INVALID_PARAMETER))
        {
            Extended = FALSE;
        }
        else
        {
            return Status;
        }
    }

    if (Extended)
    {
        *Table = ((PSYSTEM_HANDLE_INFORMATION_EX)Buffer)->Handles;
        *Count = (ULONG)((PSYSTEM_HANDLE_INFORMATION_EX)Buffer)->NumberOfHandles;
        *Storage = Buffer;
        return STATUS_SUCCESS;
    }
    else
    {
        PSYSTEM_HANDLE_INFORMATION Old = Buffer;
        PSYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Converted;

        Converted = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY,
                                    (SIZE_T)(Old->NumberOfHandles ? Old->NumberOfHandles : 1) * sizeof(*Converted));
        if (!Converted)
        {
            RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
            return STATUS_NO_MEMORY;
        }
        for (Index = 0; Index < Old->NumberOfHandles; Index++)
        {
            Converted[Index].Object = Old->Handles[Index].Object;
            Converted[Index].UniqueProcessId = Old->Handles[Index].UniqueProcessId;
            Converted[Index].HandleValue = Old->Handles[Index].HandleValue;
            Converted[Index].GrantedAccess = Old->Handles[Index].GrantedAccess;
            Converted[Index].ObjectTypeIndex = Old->Handles[Index].ObjectTypeIndex;
            Converted[Index].HandleAttributes = Old->Handles[Index].HandleAttributes;
        }
        *Table = Converted;
        *Count = Old->NumberOfHandles;
        RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
        *Storage = Converted;
        return STATUS_SUCCESS;
    }
}

static
DWORD
PsspCaptureHandles(
    _Inout_ PSSP_DATA *Data,
    _In_ HANDLE ProcessHandle,
    _In_ ULONG CaptureFlags)
{
    PSYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles;
    PVOID Storage, Scratch;
    ULONG Total, Index, Count = 0, Next = 0;
    NTSTATUS Status;
    DWORD Error = ERROR_SUCCESS;

    Status = PsspQueryHandleTable(&Handles, &Total, &Storage);
    if (!NT_SUCCESS(Status))
        return RtlNtStatusToDosError(Status);

    for (Index = 0; Index < Total; Index++)
    {
        if (Handles[Index].UniqueProcessId == Data->Process.ProcessId)
            Count++;
    }

    Scratch = RtlAllocateHeap(RtlGetProcessHeap(), 0, PSSP_NAME_BUFFER_SIZE);
    if (Count)
        Data->Handles = PsspAllocate(Data, Count * sizeof(PSS_HANDLE_ENTRY));
    if (!Scratch || (Count && !Data->Handles))
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
    }
    else
    {
        for (Index = 0; Index < Total && Next < Count; Index++)
        {
            if (Handles[Index].UniqueProcessId != Data->Process.ProcessId)
                continue;
            PsspCaptureHandle(Data, ProcessHandle, &Handles[Index], CaptureFlags, &Data->Handles[Next], Scratch);
            Next++;
        }
        Data->HandleCount = Next;
    }

    if (Scratch)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Scratch);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Storage);
    return Error;
}

/* VIRTUAL ADDRESS SPACE ******************************************************/

typedef struct _PSSP_IMAGE
{
    PVOID Base;
    DWORD TimeDateStamp;
    DWORD SizeOfImage;
    DWORD CheckSum;
} PSSP_IMAGE;

static
VOID
PsspReadImage(
    _In_ HANDLE ProcessHandle,
    _In_ PVOID Base,
    _Out_ PSSP_IMAGE *Image)
{
    UCHAR Header[0x1000];
    PIMAGE_DOS_HEADER Dos = (PIMAGE_DOS_HEADER)Header;
    PIMAGE_NT_HEADERS32 Nt;

    Image->Base = Base;
    Image->TimeDateStamp = Image->SizeOfImage = Image->CheckSum = 0;

    if (!NT_SUCCESS(NtReadVirtualMemory(ProcessHandle, Base, Header, sizeof(Header), NULL)))
        return;
    if (Dos->e_magic != IMAGE_DOS_SIGNATURE || Dos->e_lfanew <= 0 ||
        (ULONG)Dos->e_lfanew > sizeof(Header) - sizeof(IMAGE_NT_HEADERS32))
        return;

    /* The checksum and the image size are at the same offsets in the 32 and 64-bit optional headers */
    Nt = (PIMAGE_NT_HEADERS32)(Header + Dos->e_lfanew);
    if (Nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    Image->TimeDateStamp = Nt->FileHeader.TimeDateStamp;
    Image->SizeOfImage = Nt->OptionalHeader.SizeOfImage;
    Image->CheckSum = Nt->OptionalHeader.CheckSum;
}

static
DWORD
PsspCaptureVaSpace(
    _Inout_ PSSP_DATA *Data,
    _In_ HANDLE ProcessHandle,
    _In_ BOOLEAN WithSections)
{
    SYSTEM_BASIC_INFORMATION SystemInfo;
    MEMORY_BASIC_INFORMATION Info;
    PSS_VA_SPACE_ENTRY *Regions = NULL, *Grown;
    ULONG Count = 0, Capacity = 0;
    PUCHAR Address = NULL;
    ULONG_PTR Limit;
    PSSP_IMAGE Image = { NULL };
    PVOID Scratch = NULL;
    DWORD Error = ERROR_SUCCESS;
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemBasicInformation, &SystemInfo, sizeof(SystemInfo), NULL);
    if (!NT_SUCCESS(Status))
        return RtlNtStatusToDosError(Status);
    Limit = SystemInfo.MaximumUserModeAddress;

    if (WithSections)
        Scratch = RtlAllocateHeap(RtlGetProcessHeap(), 0, PSSP_NAME_BUFFER_SIZE);

    while ((ULONG_PTR)Address <= Limit)
    {
        Status = NtQueryVirtualMemory(ProcessHandle, Address, MemoryBasicInformation, &Info, sizeof(Info), NULL);
        if (!NT_SUCCESS(Status))
        {
            /* The first query tells whether the process can be read at all */
            if (Count == 0)
                Error = RtlNtStatusToDosError(Status);
            break;
        }

        if (Count == Capacity)
        {
            Capacity = Capacity ? Capacity * 2 : 256;
            Grown = Regions ? RtlReAllocateHeap(RtlGetProcessHeap(), 0, Regions, Capacity * sizeof(*Regions)) :
                              RtlAllocateHeap(RtlGetProcessHeap(), 0, Capacity * sizeof(*Regions));
            if (!Grown)
            {
                Error = ERROR_NOT_ENOUGH_MEMORY;
                break;
            }
            Regions = Grown;
        }

        RtlZeroMemory(&Regions[Count], sizeof(Regions[Count]));
        Regions[Count].BaseAddress = Info.BaseAddress;
        Regions[Count].AllocationBase = Info.AllocationBase;
        Regions[Count].AllocationProtect = Info.AllocationProtect;
        Regions[Count].RegionSize = Info.RegionSize;
        Regions[Count].State = Info.State;
        Regions[Count].Protect = Info.Protect;
        Regions[Count].Type = Info.Type;

        if (Scratch && Info.State == MEM_COMMIT && (Info.Type == MEM_IMAGE || Info.Type == MEM_MAPPED))
        {
            POBJECT_NAME_INFORMATION Name = Scratch;
            PWCHAR String;

            if (Info.Type == MEM_IMAGE)
            {
                if (Image.Base != Info.AllocationBase)
                    PsspReadImage(ProcessHandle, Info.AllocationBase, &Image);
                Regions[Count].TimeDateStamp = Image.TimeDateStamp;
                Regions[Count].SizeOfImage = Image.SizeOfImage;
                Regions[Count].ImageBase = Info.AllocationBase;
                Regions[Count].CheckSum = Image.CheckSum;
            }

            if (NT_SUCCESS(NtQueryVirtualMemory(ProcessHandle, Info.BaseAddress, MemorySectionName, Name,
                                                PSSP_NAME_BUFFER_SIZE, NULL)) &&
                Name->Name.Length != 0)
            {
                String = PsspCopyString(Data, Name->Name.Buffer, Name->Name.Length);
                if (String)
                {
                    Regions[Count].MappedFileName = String;
                    Regions[Count].MappedFileNameLength = Name->Name.Length + sizeof(WCHAR);
                }
            }
        }
        Count++;

        if ((ULONG_PTR)Info.BaseAddress + Info.RegionSize <= (ULONG_PTR)Address)
            break;
        Address = (PUCHAR)Info.BaseAddress + Info.RegionSize;
        if (Address == NULL)
            break;
    }

    if (Error == ERROR_SUCCESS && Count != 0)
    {
        Data->Regions = PsspAllocate(Data, Count * sizeof(*Regions));
        if (!Data->Regions)
            Error = ERROR_NOT_ENOUGH_MEMORY;
        else
            RtlCopyMemory(Data->Regions, Regions, Count * sizeof(*Regions));
        Data->RegionCount = Count;
    }

    if (Scratch)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Scratch);
    if (Regions)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Regions);
    return Error;
}

/* SNAPSHOT *******************************************************************/

static
PSSP_DATA *
PsspLookupSnapshot(
    _In_opt_ HPSS SnapshotHandle)
{
    PSSP_SNAPSHOT *Snapshot = (PSSP_SNAPSHOT *)SnapshotHandle;
    PSSP_DATA *Data = NULL;

    if (!Snapshot)
        return NULL;

    _SEH2_TRY
    {
        if (Snapshot->Magic == PSSP_SNAPSHOT_MAGIC)
            Data = Snapshot->Data;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Data = NULL;
    }
    _SEH2_END;
    return Data;
}

static
DWORD
PsspNewSnapshot(
    _In_ PSSP_DATA *Data,
    _Out_ HPSS *SnapshotHandle)
{
    PSSP_SNAPSHOT *Snapshot;

    Snapshot = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(*Snapshot));
    if (!Snapshot)
        return ERROR_NOT_ENOUGH_MEMORY;

    Snapshot->Magic = PSSP_SNAPSHOT_MAGIC;
    Snapshot->Data = Data;
    *SnapshotHandle = (HPSS)Snapshot;
    return ERROR_SUCCESS;
}

/* PUBLIC API *****************************************************************/

DWORD
WINAPI
PssCaptureSnapshot(
    _In_ HANDLE ProcessHandle,
    _In_ PSS_CAPTURE_FLAGS CaptureFlags,
    _In_ DWORD ThreadContextFlags,
    _Out_ HPSS *SnapshotHandle)
{
    PSSP_DATA *Data;
    PSSP_TIMER Total, Part;
    DWORD Error;
    ULONG Flags = CaptureFlags;
    BOOLEAN Measure = (Flags & PSS_CREATE_MEASURE_PERFORMANCE) != 0;

    if (!SnapshotHandle)
        return ERROR_INVALID_PARAMETER;
    *SnapshotHandle = NULL;


    if ((Flags & ~PSSP_VALID_CAPTURE_FLAGS) ||
        ((Flags & PSS_CREATE_BREAKAWAY_OPTIONAL) && !(Flags & PSS_CREATE_BREAKAWAY)))
    {
        return ERROR_INVALID_PARAMETER;
    }
    if (Flags & PSS_CREATE_FORCE_BREAKAWAY)
        return ERROR_PRIVILEGE_NOT_HELD;
    if (Flags & PSS_CAPTURE_VA_CLONE)
        return ERROR_NOT_SUPPORTED;

    Data = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Data));
    if (!Data)
        return ERROR_NOT_ENOUGH_MEMORY;
    Data->ReferenceCount = 1;
    Data->CaptureFlags = Flags;

    if (Measure)
        PsspTimerStart(&Total);

    Error = PsspCaptureProcess(ProcessHandle, &Data->Process);
    if (Error == ERROR_SUCCESS && (Flags & (PSS_CAPTURE_THREADS | PSS_CAPTURE_HANDLES)))
    {
        Error = PsspCheckAccess(ProcessHandle,
                                PROCESS_QUERY_INFORMATION | ((Flags & PSS_CAPTURE_HANDLES) ? PROCESS_DUP_HANDLE : 0));
    }

    if (Error == ERROR_SUCCESS && (Flags & PSS_CAPTURE_VA_SPACE))
    {
        if (Measure)
            PsspTimerStart(&Part);
        Error = PsspCaptureVaSpace(Data, ProcessHandle, (Flags & PSS_CAPTURE_VA_SPACE_SECTION_INFORMATION) != 0);
        Data->HaveVaSpace = Error == ERROR_SUCCESS;
        if (Measure)
        {
            PsspTimerStop(&Part, &Data->Performance.VaSpaceCycleCount, &Data->Performance.VaSpaceWallClockPeriod);
        }
    }
    else if (Error == ERROR_SUCCESS && (Flags & PSS_CAPTURE_VA_SPACE_SECTION_INFORMATION))
    {
        /* Windows accepts the section information alone and captures nothing for it */
    }

    if (Error == ERROR_SUCCESS && (Flags & PSS_CAPTURE_HANDLES))
    {
        if (Measure)
            PsspTimerStart(&Part);
        Error = PsspCaptureHandles(Data, ProcessHandle, Flags & PSSP_HANDLE_FLAGS);
        Data->HaveHandles = Error == ERROR_SUCCESS;
        if (Measure)
        {
            PsspTimerStop(&Part, &Data->Performance.HandlesCycleCount, &Data->Performance.HandlesWallClockPeriod);
        }
    }

    if (Error == ERROR_SUCCESS && (Flags & PSS_CAPTURE_THREADS))
    {
        BOOLEAN WithContext = (Flags & PSS_CAPTURE_THREAD_CONTEXT) != 0;

        if (Measure)
            PsspTimerStart(&Part);
        Error = PsspCaptureThreads(Data,
                                   WithContext ? (ThreadContextFlags ? ThreadContextFlags : CONTEXT_ALL) : 0,
                                   WithContext);
        Data->HaveThreads = Error == ERROR_SUCCESS;
        if (Measure)
        {
            PsspTimerStop(&Part, &Data->Performance.ThreadsCycleCount, &Data->Performance.ThreadsWallClockPeriod);
        }
    }

    if (Error == ERROR_SUCCESS)
        Error = PsspNewSnapshot(Data, SnapshotHandle);

    if (Error != ERROR_SUCCESS)
    {
        PsspFreeData(Data);
        return Error;
    }

    if (Measure)
        PsspTimerStop(&Total, &Data->Performance.TotalCycleCount, &Data->Performance.TotalWallClockPeriod);
    return ERROR_SUCCESS;
}

DWORD
WINAPI
PssFreeSnapshot(
    _In_ HANDLE ProcessHandle,
    _In_ HPSS SnapshotHandle)
{
    PSSP_SNAPSHOT *Snapshot = (PSSP_SNAPSHOT *)SnapshotHandle;
    PSSP_DATA *Data;
    DWORD Error = ERROR_SUCCESS;

    if (!SnapshotHandle)
        return ERROR_INVALID_HANDLE;
    if (!PsspIsCurrentProcess(ProcessHandle, &Error))
        return Error;

    Data = PsspLookupSnapshot(SnapshotHandle);
    if (!Data)
        return ERROR_INVALID_PARAMETER;

    Snapshot->Magic = 0;
    Snapshot->Data = NULL;
    PsspRelease(Data);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Snapshot);
    return ERROR_SUCCESS;
}

DWORD
WINAPI
PssDuplicateSnapshot(
    _In_ HANDLE SourceProcessHandle,
    _In_ HPSS SnapshotHandle,
    _In_ HANDLE TargetProcessHandle,
    _Out_ HPSS *TargetSnapshotHandle,
    _In_ PSS_DUPLICATE_FLAGS Flags)
{
    PSSP_DATA *Data;
    DWORD Error = ERROR_SUCCESS;

    if (!SnapshotHandle)
        return ERROR_INVALID_HANDLE;
    if (!TargetSnapshotHandle || (Flags & ~PSS_DUPLICATE_CLOSE_SOURCE))
        return ERROR_INVALID_PARAMETER;
    *TargetSnapshotHandle = NULL;

    if (!PsspIsCurrentProcess(SourceProcessHandle, &Error))
        return Error;
    if (!PsspIsCurrentProcess(TargetProcessHandle, &Error))
    {
        /* Not the process that owns the snapshot, so it cannot be copied there from here */
        return Error == ERROR_PARTIAL_COPY ? ERROR_NOT_SUPPORTED : Error;
    }

    Data = PsspLookupSnapshot(SnapshotHandle);
    if (!Data)
        return ERROR_INVALID_PARAMETER;

    InterlockedIncrement(&Data->ReferenceCount);
    Error = PsspNewSnapshot(Data, TargetSnapshotHandle);
    if (Error != ERROR_SUCCESS)
    {
        PsspRelease(Data);
        return Error;
    }

    if (Flags & PSS_DUPLICATE_CLOSE_SOURCE)
        return PssFreeSnapshot(SourceProcessHandle, SnapshotHandle);
    return ERROR_SUCCESS;
}

DWORD
WINAPI
PssQuerySnapshot(
    _In_ HPSS SnapshotHandle,
    _In_ PSS_QUERY_INFORMATION_CLASS InformationClass,
    _Out_writes_bytes_(BufferLength) void *Buffer,
    _In_ DWORD BufferLength)
{
    PSSP_DATA *Data;
    DWORD Error = ERROR_SUCCESS;

    Data = PsspLookupSnapshot(SnapshotHandle);
    if (!Data)
        return ERROR_INVALID_HANDLE;
    if (!Buffer)
        return ERROR_INVALID_PARAMETER;

    _SEH2_TRY
    {
        switch ((ULONG)InformationClass)
        {
            case PSS_QUERY_PROCESS_INFORMATION:
                if (BufferLength != sizeof(PSS_PROCESS_INFORMATION))
                    Error = ERROR_BAD_LENGTH;
                else
                    *(PSS_PROCESS_INFORMATION *)Buffer = Data->Process;
                break;

            case PSS_QUERY_VA_CLONE_INFORMATION:
                /* There is never a clone */
                Error = BufferLength != sizeof(PSS_VA_CLONE_INFORMATION) ? ERROR_BAD_LENGTH : ERROR_NOT_FOUND;
                break;

            case PSS_QUERY_AUXILIARY_PAGES_INFORMATION:
                Error = BufferLength != sizeof(PSS_AUXILIARY_PAGES_INFORMATION) ? ERROR_BAD_LENGTH : ERROR_NOT_FOUND;
                break;

            case PSS_QUERY_VA_SPACE_INFORMATION:
                if (BufferLength != sizeof(PSS_VA_SPACE_INFORMATION))
                    Error = ERROR_BAD_LENGTH;
                else if (!Data->HaveVaSpace)
                    Error = ERROR_NOT_FOUND;
                else
                    ((PSS_VA_SPACE_INFORMATION *)Buffer)->RegionCount = Data->RegionCount;
                break;

            case PSS_QUERY_HANDLE_INFORMATION:
                if (BufferLength != sizeof(PSS_HANDLE_INFORMATION))
                    Error = ERROR_BAD_LENGTH;
                else if (!Data->HaveHandles)
                    Error = ERROR_NOT_FOUND;
                else
                    ((PSS_HANDLE_INFORMATION *)Buffer)->HandlesCaptured = Data->HandleCount;
                break;

            case PSS_QUERY_THREAD_INFORMATION:
                if (BufferLength != sizeof(PSS_THREAD_INFORMATION))
                {
                    Error = ERROR_BAD_LENGTH;
                }
                else if (!Data->HaveThreads)
                {
                    Error = ERROR_NOT_FOUND;
                }
                else
                {
                    ((PSS_THREAD_INFORMATION *)Buffer)->ThreadsCaptured = Data->ThreadCount;
                    ((PSS_THREAD_INFORMATION *)Buffer)->ContextLength = Data->ContextLength;
                }
                break;

            case PSS_QUERY_HANDLE_TRACE_INFORMATION:
                /* Handle tracing is never captured */
                Error = BufferLength != sizeof(PSS_HANDLE_TRACE_INFORMATION) ? ERROR_BAD_LENGTH : ERROR_NOT_FOUND;
                break;

            case PSS_QUERY_PERFORMANCE_COUNTERS:
                if (BufferLength != sizeof(PSS_PERFORMANCE_COUNTERS))
                    Error = ERROR_BAD_LENGTH;
                else
                    *(PSS_PERFORMANCE_COUNTERS *)Buffer = Data->Performance;
                break;

            case 8:
                /* Windows has a class here that is not documented; no buffer is the size it wants from here */
                Error = ERROR_BAD_LENGTH;
                break;

            default:
                Error = ERROR_INVALID_PARAMETER;
                break;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Error = ERROR_NOACCESS;
    }
    _SEH2_END;
    return Error;
}

/* WALK MARKERS ***************************************************************/

static
PSSP_MARKER *
PsspLookupMarker(
    _In_opt_ HPSSWALK WalkMarkerHandle)
{
    PSSP_MARKER *Marker = (PSSP_MARKER *)WalkMarkerHandle;
    BOOLEAN Valid = FALSE;

    if (!Marker)
        return NULL;

    _SEH2_TRY
    {
        Valid = Marker->Magic == PSSP_MARKER_MAGIC;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Valid = FALSE;
    }
    _SEH2_END;
    return Valid ? Marker : NULL;
}

DWORD
WINAPI
PssWalkMarkerCreate(
    _In_opt_ const PSS_ALLOCATOR *Allocator,
    _Out_ HPSSWALK *WalkMarkerHandle)
{
    PSSP_MARKER *Marker;

    if (!WalkMarkerHandle)
        return ERROR_INVALID_PARAMETER;
    *WalkMarkerHandle = NULL;

    if (Allocator)
    {
        if (!Allocator->AllocRoutine || !Allocator->FreeRoutine)
            return ERROR_INVALID_PARAMETER;
        Marker = Allocator->AllocRoutine(Allocator->Context, sizeof(*Marker));
    }
    else
    {
        Marker = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(*Marker));
    }
    if (!Marker)
        return ERROR_OUTOFMEMORY;

    Marker->Magic = PSSP_MARKER_MAGIC;
    Marker->Position = 0;
    if (Allocator)
        Marker->Allocator = *Allocator;
    else
        RtlZeroMemory(&Marker->Allocator, sizeof(Marker->Allocator));
    *WalkMarkerHandle = (HPSSWALK)Marker;
    return ERROR_SUCCESS;
}

DWORD
WINAPI
PssWalkMarkerFree(
    _In_ HPSSWALK WalkMarkerHandle)
{
    PSSP_MARKER *Marker = PsspLookupMarker(WalkMarkerHandle);
    PSS_ALLOCATOR Allocator;

    if (!Marker)
        return ERROR_INVALID_HANDLE;

    Allocator = Marker->Allocator;
    Marker->Magic = 0;
    if (Allocator.FreeRoutine)
        Allocator.FreeRoutine(Allocator.Context, Marker);
    else
        RtlFreeHeap(RtlGetProcessHeap(), 0, Marker);
    return ERROR_SUCCESS;
}

DWORD
WINAPI
PssWalkMarkerGetPosition(
    _In_ HPSSWALK WalkMarkerHandle,
    _Out_ ULONG_PTR *Position)
{
    PSSP_MARKER *Marker = PsspLookupMarker(WalkMarkerHandle);

    if (!Marker)
        return ERROR_INVALID_HANDLE;
    if (!Position)
        return ERROR_INVALID_PARAMETER;
    *Position = Marker->Position;
    return ERROR_SUCCESS;
}

DWORD
WINAPI
PssWalkMarkerSetPosition(
    _In_ HPSSWALK WalkMarkerHandle,
    _In_ ULONG_PTR Position)
{
    PSSP_MARKER *Marker = PsspLookupMarker(WalkMarkerHandle);

    if (!Marker)
        return ERROR_INVALID_HANDLE;
    Marker->Position = Position;
    return ERROR_SUCCESS;
}

DWORD
WINAPI
PssWalkMarkerSeekToBeginning(
    _In_ HPSSWALK WalkMarkerHandle)
{
    return PssWalkMarkerSetPosition(WalkMarkerHandle, 0);
}

DWORD
WINAPI
PssWalkSnapshot(
    _In_ HPSS SnapshotHandle,
    _In_ PSS_WALK_INFORMATION_CLASS InformationClass,
    _In_opt_ HPSSWALK WalkMarkerHandle,
    _Out_writes_bytes_(BufferLength) void *Buffer,
    _In_ DWORD BufferLength)
{
    PSSP_DATA *Data = PsspLookupSnapshot(SnapshotHandle);
    PSSP_MARKER *Marker;
    const VOID *Entries;
    ULONG Count, EntrySize;
    DWORD Error = ERROR_SUCCESS;

    if (!Data)
        return ERROR_INVALID_HANDLE;
    Marker = PsspLookupMarker(WalkMarkerHandle);
    if (!Marker)
        return ERROR_INVALID_HANDLE;

    switch ((ULONG)InformationClass)
    {
        case PSS_WALK_AUXILIARY_PAGES:
            Entries = NULL;
            Count = 0;
            EntrySize = sizeof(PSS_AUXILIARY_PAGE_ENTRY);
            break;
        case PSS_WALK_VA_SPACE:
            Entries = Data->HaveVaSpace ? Data->Regions : NULL;
            Count = Data->HaveVaSpace ? Data->RegionCount : 0;
            EntrySize = sizeof(PSS_VA_SPACE_ENTRY);
            break;
        case PSS_WALK_HANDLES:
            Entries = Data->HaveHandles ? Data->Handles : NULL;
            Count = Data->HaveHandles ? Data->HandleCount : 0;
            EntrySize = sizeof(PSS_HANDLE_ENTRY);
            break;
        case PSS_WALK_THREADS:
            Entries = Data->HaveThreads ? Data->Threads : NULL;
            Count = Data->HaveThreads ? Data->ThreadCount : 0;
            EntrySize = sizeof(PSS_THREAD_ENTRY);
            break;
        default:
            return ERROR_INVALID_PARAMETER;
    }

    if (BufferLength != EntrySize)
        return ERROR_BAD_LENGTH;
    if (!Buffer)
        return ERROR_INVALID_PARAMETER;

    /* A class that was not captured is not there, an empty one is just at its end */
    if ((InformationClass == PSS_WALK_AUXILIARY_PAGES) ||
        (InformationClass == PSS_WALK_VA_SPACE && !Data->HaveVaSpace) ||
        (InformationClass == PSS_WALK_HANDLES && !Data->HaveHandles) ||
        (InformationClass == PSS_WALK_THREADS && !Data->HaveThreads))
    {
        return ERROR_NOT_FOUND;
    }

    if (Marker->Position >= Count)
        return ERROR_NO_MORE_ITEMS;

    _SEH2_TRY
    {
        RtlCopyMemory(Buffer, (const UCHAR *)Entries + Marker->Position * EntrySize, EntrySize);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Error = ERROR_NOACCESS;
    }
    _SEH2_END;

    if (Error == ERROR_SUCCESS)
        Marker->Position++;
    return Error;
}
