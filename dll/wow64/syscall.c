/* ReactOS WoW64 NT argument translation. SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64p.h"
#include <mountmgr.h>
#include <winsock2.h>
#include <tdi.h>
#include <tcpioctl.h>
#include <drivers/afd/shared.h>

enum
{
#define WOW64_SYSCALL(Name, Source, Target, SourceSlots, TargetSlots, Flags) W64_##Name = Source,
#include <reactos/wow64/nt32to64.inc>
#undef WOW64_SYSCALL
};

static const char *ServiceNames[] =
{
#define WOW64_SYSCALL(Name, Source, Target, SourceSlots, TargetSlots, Flags) #Name,
#include <reactos/wow64/nt32to64.inc>
#undef WOW64_SYSCALL
};

static UNICODE_STRING *String32(UNICODE_STRING *To, const WOW64_STRING *From)
{
    if (!From) return NULL;
    To->Length = From->Length;
    To->MaximumLength = From->MaximumLength;
    To->Buffer = PTR32(From->Buffer);
    return To;
}

static PSECURITY_DESCRIPTOR Security32(SECURITY_DESCRIPTOR *To, PSECURITY_DESCRIPTOR From)
{
    /* Self-relative descriptors have a fixed-width representation. Absolute
     * descriptors instead contain four guest pointers (NLS and registry APIs
     * routinely pass them). The kernel still captures/probes the SID/ACLs. */
    if (From && !(((PISECURITY_DESCRIPTOR)From)->Control & SE_SELF_RELATIVE))
    {
        const SECURITY_DESCRIPTOR_RELATIVE *FromSd = From;
        To->Revision = FromSd->Revision;
        To->Sbz1 = FromSd->Sbz1;
        To->Control = FromSd->Control;
        To->Owner = PTR32(FromSd->Owner);
        To->Group = PTR32(FromSd->Group);
        To->Sacl = PTR32(FromSd->Sacl);
        To->Dacl = PTR32(FromSd->Dacl);
        return To;
    }
    return From;
}

static NTSTATUS Attributes32(OBJECT_ATTRIBUTES *To, UNICODE_STRING *Name,
                             SECURITY_DESCRIPTOR *Descriptor, ULONG Address)
{
    const WOW64_OBJECT_ATTRIBUTES *From = PTR32(Address);
    RtlZeroMemory(To, sizeof(*To));
    if (!From) return STATUS_SUCCESS;
    if (From->Length != sizeof(*From)) return STATUS_INVALID_PARAMETER;
    InitializeObjectAttributes(To, String32(Name, PTR32(From->ObjectName)),
                               From->Attributes, HANDLE32(From->RootDirectory),
                               Security32(Descriptor, PTR32(From->SecurityDescriptor)));
    To->SecurityQualityOfService = PTR32(From->SecurityQualityOfService);
    return STATUS_SUCCESS;
}

static VOID StoreIo(ULONG Address, const IO_STATUS_BLOCK *Io)
{
    WOW64_IOSB *To = PTR32(Address);
    if (To)
    {
        To->Status = Io->Status;
        To->Information = (ULONG)Io->Information;
    }
}

/* Resources remain owned here until every translated output was copied.
 * Native calls only see native temporaries, so their own output probes cannot
 * validate the guest destinations. */
typedef struct
{
    HANDLE Handle;
    HANDLE HandleProcess;
    HANDLE Process;
    PVOID Base;
    BOOLEAN Mapped;
    PVOID Buffer;
    HANDLE Thread;
    HANDLE ThreadProcess;
} WOW64_CLEANUP;

typedef struct
{
    BOOLEAN Replace;
    ULONG Root, Length;
    WCHAR Name[1];
} WOW64_RENAME_INFORMATION;

typedef struct
{
    LARGE_INTEGER PerProcessUserTimeLimit, PerJobUserTimeLimit;
    ULONG LimitFlags, MinimumWorkingSetSize, MaximumWorkingSetSize;
    ULONG ActiveProcessLimit, Affinity, PriorityClass, SchedulingClass;
} WOW64_JOB_BASIC_LIMIT;

typedef struct
{
    WOW64_JOB_BASIC_LIMIT Basic;
    IO_COUNTERS IoInfo;
    ULONG ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
} WOW64_JOB_EXTENDED_LIMIT;
C_ASSERT(sizeof(WOW64_JOB_BASIC_LIMIT) == 48);
C_ASSERT(sizeof(WOW64_JOB_EXTENDED_LIMIT) == 112);

static VOID ProbeOutput(ULONG Address, ULONG Size)
{
    volatile UCHAR *Bytes = PTR32(Address);
    ULONG i;
    for (i = 0; i < Size; ++i) Bytes[i] = Bytes[i];
}

static NTSTATUS Dispatch(ULONG Number, ULONG *a, WOW64_CLEANUP *Cleanup)
{
    NTSTATUS Status;
    OBJECT_ATTRIBUTES Attributes;
    SECURITY_DESCRIPTOR Descriptor;
    UNICODE_STRING Name;
    IO_STATUS_BLOCK Io = {{0}};
    PVOID Base;
    SIZE_T Size;
    ULONG Length;
    ACCESS_MASK KeyAccess;
    BOOLEAN NativeSystemInformation = Number == 0x10001;

#define H(i) HANDLE32(a[i])
#define P(i) PTR32(a[i])
#define U(i) (*(ULONG *)P(i))
#define ATTR(i) do { Status = Attributes32(&Attributes, &Name, &Descriptor, a[i]); \
    if (!NT_SUCCESS(Status)) return Status; } while (0)
#define OA(i) (a[i] ? &Attributes : NULL)
#define FILEATTR(i) do { ATTR(i); \
    Status = Wow64RedirectFile(&Attributes, &Name, &Cleanup->Buffer); \
    if (!NT_SUCCESS(Status)) return Status; } while (0)
#define PROBE(i) ProbeOutput(a[i], sizeof(ULONG))
#define RETURN_HANDLE_AT(i) do { \
    if (NT_SUCCESS(Status)) U(i) = HandleToUlong(Cleanup->Handle); \
    Cleanup->Handle = NULL; return Status; } while (0)
#define RETURN_HANDLE() RETURN_HANDLE_AT(0)
    if (Number == 0x10000) return Wow64CsrDispatch(a[0], P(1));
    if (Number == 0x10003) return Wow64CreateProcess(P(0));
    if (Number == 0x10002)
    {
        WOW64_CPU_STATE *Cpu = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU];
        /* Write the old token before changing state: a bad output pointer
         * must leave redirection enabled, including nested disable calls. */
        U(1) = Cpu->FsRedirectionDisabled;
        Cpu->FsRedirectionDisabled = a[0];
        return STATUS_SUCCESS;
    }
    if (NativeSystemInformation) Number = W64_NtQuerySystemInformation;
    switch (Number)
    {
    case W64_NtAddAtom: return NtAddAtom(P(0), a[1], P(2));
    case W64_NtFindAtom: return NtFindAtom(P(0), a[1], P(2));
    case W64_NtDeleteAtom: return NtDeleteAtom(a[0]);
    case W64_NtQueryInformationAtom: return NtQueryInformationAtom(a[0], a[1], P(2), a[3], P(4));
    case W64_NtCallbackReturn: return Wow64WinCallbackReturn(P(0), a[1], a[2]);
    case W64_NtCreateThread:
    {
        CLIENT_ID Id;
        PROBE(0);
        if (a[4]) ProbeOutput(a[4], 2 * sizeof(ULONG));
        ATTR(2);
        Cleanup->ThreadProcess = H(3);
        Status = Wow64CreateThread(&Cleanup->Thread, THREAD_ALL_ACCESS, OA(2), H(3), &Id, P(5), P(6), TRUE);
        if (NT_SUCCESS(Status))
            Status = NtDuplicateObject(NtCurrentProcess(), Cleanup->Thread, NtCurrentProcess(),
                                        &Cleanup->Handle, a[1], a[2] ? Attributes.Attributes & OBJ_INHERIT : 0, 0);
        if (NT_SUCCESS(Status))
        {
            U(0) = HandleToUlong(Cleanup->Handle);
            if (a[4])
            {
                ((ULONG *)P(4))[0] = HandleToUlong(Id.UniqueProcess);
                ((ULONG *)P(4))[1] = HandleToUlong(Id.UniqueThread);
            }
            if (!a[7]) Status = NtResumeThread(Cleanup->Thread, NULL);
            if (NT_SUCCESS(Status))
            {
                NtClose(Cleanup->Thread);
                Cleanup->Thread = NULL;
                Cleanup->Handle = NULL;
            }
        }
        return Status;
    }
    case W64_NtGetContextThread:
        return NtQueryInformationThread(H(0), ThreadWow64Context, P(1), sizeof(WOW64_CONTEXT), NULL);
    case W64_NtSetContextThread:
        return NtSetInformationThread(H(0), ThreadWow64Context, P(1), sizeof(WOW64_CONTEXT));
    case W64_NtQueueApcThread:
        return NtQueueApcThread(H(0), (PKNORMAL_ROUTINE)(ULONG_PTR)(-((LONG64)a[1] << 2)), P(2), P(3), P(4));
    case W64_NtDuplicateObject:
        if (a[3]) PROBE(3);
        Cleanup->HandleProcess = H(2);
        Status = NtDuplicateObject(H(0), H(1), H(2), a[3] ? &Cleanup->Handle : NULL, a[4], a[5], a[6]);
        if (a[3]) { RETURN_HANDLE_AT(3); }
        return Status;
    case W64_NtSuspendThread: return NtSuspendThread(H(0), P(1));
    case W64_NtResumeThread: return NtResumeThread(H(0), P(1));
    case W64_NtAlertThread: return NtAlertThread(H(0));
    case W64_NtAlertResumeThread: return NtAlertResumeThread(H(0), P(1));
    case W64_NtQueryInformationThread: return Wow64QueryThread(H(0), a[1], P(2), a[3], P(4));
    case W64_NtAreMappedFilesTheSame: return NtAreMappedFilesTheSame(P(0), P(1));
    case W64_NtReadVirtualMemory:
    case W64_NtWriteVirtualMemory:
    {
        SIZE_T Bytes = 0;
        if (a[4]) PROBE(4);
        if (Number == W64_NtReadVirtualMemory)
            Status = NtReadVirtualMemory(H(0), P(1), P(2), a[3], &Bytes);
        else
            Status = NtWriteVirtualMemory(H(0), P(1), P(2), a[3], &Bytes);
        if (a[4]) U(4) = (ULONG)Bytes;
        return Status;
    }
    case W64_NtSignalAndWaitForSingleObject: return NtSignalAndWaitForSingleObject(H(0), H(1), a[2], P(3));
    case W64_NtPulseEvent: return NtPulseEvent(H(0), P(1));
    case W64_NtClearEvent: return NtClearEvent(H(0));
    case W64_NtQueryEvent: return NtQueryEvent(H(0), a[1], P(2), a[3], P(4));
    case W64_NtQuerySemaphore: return NtQuerySemaphore(H(0), a[1], P(2), a[3], P(4));
    case W64_NtQueryMutant: return NtQueryMutant(H(0), a[1], P(2), a[3], P(4));
    case W64_NtOpenSemaphore:
        PROBE(0); ATTR(2); Status = NtOpenSemaphore(&Cleanup->Handle, a[1], OA(2)); RETURN_HANDLE();
    case W64_NtOpenMutant:
        PROBE(0); ATTR(2); Status = NtOpenMutant(&Cleanup->Handle, a[1], OA(2)); RETURN_HANDLE();
    case W64_NtClose: return NtClose(H(0));
    case W64_NtContinue: return Wow64Continue(P(0), a[1]);
    case W64_NtQueryObject:
        if (a[1] == ObjectBasicInformation || a[1] == ObjectHandleFlagInformation)
            return NtQueryObject(H(0), a[1], P(2), a[3], P(4));
        if (a[1] == ObjectNameInformation)
        {
            OBJECT_NAME_INFORMATION *Native;
            WOW64_STRING *Guest = P(2);
            ULONG Needed = 0, GuestNeeded, Extra = sizeof(UNICODE_STRING) - sizeof(WOW64_STRING);
            if (a[3] > MAXULONG - Extra) return STATUS_INVALID_PARAMETER;
            if (a[4]) PROBE(4);
            Native = Cleanup->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, a[3] + Extra);
            if (!Native) return STATUS_NO_MEMORY;
            Status = NtQueryObject(H(0), a[1], Native, a[3] + Extra, &Needed);
            GuestNeeded = Needed >= sizeof(UNICODE_STRING) ? Needed - Extra : Needed;
            if (a[4]) U(4) = GuestNeeded;
            if (NT_SUCCESS(Status))
            {
                if (a[3] < sizeof(*Guest) ||
                    Native->Name.MaximumLength > a[3] - sizeof(*Guest))
                    return STATUS_INFO_LENGTH_MISMATCH;
                Guest->Length = Native->Name.Length;
                Guest->MaximumLength = Native->Name.MaximumLength;
                Guest->Buffer = Native->Name.Buffer ? PtrToUlong(Guest + 1) : 0;
                if (Native->Name.Buffer)
                    RtlCopyMemory(Guest + 1, Native->Name.Buffer, Native->Name.MaximumLength);
            }
            return Status;
        }
        return STATUS_NOT_SUPPORTED;
    case W64_NtSetInformationObject:
        if (a[1] == ObjectHandleFlagInformation)
            return NtSetInformationObject(H(0), a[1], P(2), a[3]);
        return STATUS_NOT_SUPPORTED;
    case W64_NtRaiseException: return Wow64RaiseException(P(0), P(1), a[2]);
    case W64_NtTestAlert: return NtTestAlert();
    case W64_NtYieldExecution: return NtYieldExecution();
    case W64_NtIsProcessInJob: return NtIsProcessInJob(H(0), H(1));
    case W64_NtTerminateProcess: return NtTerminateProcess(H(0), a[1]);
    case W64_NtTerminateThread: return NtTerminateThread(H(0), a[1]);
    case W64_NtQueryDebugFilterState: return NtQueryDebugFilterState(a[0], a[1]);
    case W64_NtQueryPerformanceCounter: return NtQueryPerformanceCounter(P(0), P(1));
    case W64_NtAllocateLocallyUniqueId: return NtAllocateLocallyUniqueId(P(0));
    case W64_NtQuerySystemTime: return NtQuerySystemTime(P(0));
    case W64_NtQueryDefaultLocale: return NtQueryDefaultLocale(a[0], P(1));
    case W64_NtQueryDefaultUILanguage: return NtQueryDefaultUILanguage(P(0));
    case W64_NtQueryInstallUILanguage: return NtQueryInstallUILanguage(P(0));
    case W64_NtDelayExecution: return NtDelayExecution(a[0], P(1));
    case W64_NtWaitForSingleObject: return NtWaitForSingleObject(H(0), a[1], P(2));
    case W64_NtWaitForMultipleObjects:
    {
        HANDLE Handles[MAXIMUM_WAIT_OBJECTS];
        ULONG i;
        if (!a[0] || a[0] > MAXIMUM_WAIT_OBJECTS) return STATUS_INVALID_PARAMETER;
        for (i = 0; i < a[0]; ++i) Handles[i] = HANDLE32(((ULONG *)P(1))[i]);
        return NtWaitForMultipleObjects(a[0], Handles, a[2], a[3], P(4));
    }
    case W64_NtAllocateVirtualMemory:
        PROBE(1); PROBE(3);
        Base = PTR32(U(1)); Size = U(3);
        /* MEM_COMMIT with a null base also creates a new reservation. Existing
         * reservations stay owned by the caller and must never be released. */
        Cleanup->Process = H(0);
        Cleanup->Mapped = FALSE;
        Length = !Base || (a[4] & MEM_RESERVE);
        Status = NtAllocateVirtualMemory(H(0), &Base, Wow64ZeroBits(a[2]),
                                         &Size, a[4], a[5]);
        if (NT_SUCCESS(Status))
        {
            if (Length) Cleanup->Base = Base;
            U(1) = PtrToUlong(Base); U(3) = Size;
            Cleanup->Base = NULL;
        }
        return Status;
    case W64_NtFreeVirtualMemory:
        PROBE(1); PROBE(2);
        Base = PTR32(U(1)); Size = U(2);
        Status = NtFreeVirtualMemory(H(0), &Base, &Size, a[3]);
        if (NT_SUCCESS(Status)) { U(1) = PtrToUlong(Base); U(2) = Size; }
        return Status;
    case W64_NtProtectVirtualMemory:
        PROBE(1); PROBE(2); PROBE(4);
        Base = PTR32(U(1)); Size = U(2);
        Status = NtProtectVirtualMemory(H(0), &Base, &Size, a[3], P(4));
        if (NT_SUCCESS(Status)) { U(1) = PtrToUlong(Base); U(2) = Size; }
        return Status;
    case W64_NtQueryVirtualMemory:
    {
        MEMORY_BASIC_INFORMATION Info;
        struct { ULONG Base, AllocationBase, AllocationProtect, Size, State, Protect, Type; } *Info32 = P(3);
        if (a[2] != MemoryBasicInformation) return STATUS_NOT_SUPPORTED;
        if (a[4] < sizeof(*Info32)) return STATUS_INFO_LENGTH_MISMATCH;
        Status = NtQueryVirtualMemory(H(0), P(1), a[2], &Info, sizeof(Info), NULL);
        if (NT_SUCCESS(Status))
        {
            Info32->Base = PtrToUlong(Info.BaseAddress);
            Info32->AllocationBase = PtrToUlong(Info.AllocationBase);
            Info32->AllocationProtect = Info.AllocationProtect;
            Info32->Size = min(Info.RegionSize, (SIZE_T)MAXULONG);
            Info32->State = Info.State; Info32->Protect = Info.Protect; Info32->Type = Info.Type;
            if (a[5]) U(5) = sizeof(*Info32);
        }
        return Status;
    }
    case W64_NtFlushInstructionCache: return NtFlushInstructionCache(H(0), P(1), a[2]);
    case W64_NtUnmapViewOfSection: return NtUnmapViewOfSection(H(0), P(1));
    case W64_NtMapViewOfSection:
        PROBE(2); PROBE(6);
        if (a[5]) ProbeOutput(a[5], sizeof(LARGE_INTEGER));
        Base = PTR32(U(2)); Size = U(6);
        Cleanup->Process = H(1);
        Cleanup->Mapped = TRUE;
        Status = NtMapViewOfSection(H(0), H(1), &Base, Wow64ZeroBits(a[3]),
                                    a[4], P(5), &Size, a[7], a[8], a[9]);
        if (NT_SUCCESS(Status))
        {
            Cleanup->Base = Base;
            U(2) = PtrToUlong(Base); U(6) = Size;
            Cleanup->Base = NULL;
        }
        return Status;
    case W64_NtCreateSection:
        PROBE(0);
        ATTR(2);
        Status = NtCreateSection(&Cleanup->Handle, a[1], OA(2), P(3), a[4], a[5], H(6));
        RETURN_HANDLE();
    case W64_NtQuerySection:
        if (a[1] == SectionImageInformation)
        {
            SECTION_IMAGE_INFORMATION From;
            struct { ULONG Transfer, ZeroBits, MaximumStack, CommitStack; UCHAR Tail[32]; } *To = P(2);
            if (a[3] < sizeof(*To)) return STATUS_INFO_LENGTH_MISMATCH;
            Status = NtQuerySection(H(0), a[1], &From, sizeof(From), NULL);
            if (NT_SUCCESS(Status))
            {
                To->Transfer = PtrToUlong(From.TransferAddress);
                To->ZeroBits = From.ZeroBits;
                To->MaximumStack = (ULONG)From.MaximumStackSize;
                To->CommitStack = (ULONG)From.CommittedStackSize;
                RtlCopyMemory(To->Tail, (PUCHAR)&From + FIELD_OFFSET(SECTION_IMAGE_INFORMATION, SubSystemType), sizeof(To->Tail));
                if (a[4]) U(4) = sizeof(*To);
            }
            return Status;
        }
        if (a[1] == SectionBasicInformation)
        {
            SECTION_BASIC_INFORMATION From;
            struct { ULONG Base, Attributes; LARGE_INTEGER Size; } *To = P(2);
            if (a[3] < sizeof(*To)) return STATUS_INFO_LENGTH_MISMATCH;
            Status = NtQuerySection(H(0), a[1], &From, sizeof(From), NULL);
            if (NT_SUCCESS(Status))
            {
                To->Base = PtrToUlong(From.BaseAddress); To->Attributes = From.Attributes;
                To->Size = From.Size;
                if (a[4]) U(4) = sizeof(*To);
            }
            return Status;
        }
        return STATUS_NOT_SUPPORTED;
    case W64_NtOpenSection:
        PROBE(0);
        ATTR(2); Status = NtOpenSection(&Cleanup->Handle, a[1], OA(2)); RETURN_HANDLE();
    case W64_NtOpenDirectoryObject:
        PROBE(0);
        ATTR(2); Status = NtOpenDirectoryObject(&Cleanup->Handle, a[1], OA(2)); RETURN_HANDLE();
    case W64_NtCreateDirectoryObject:
        PROBE(0);
        ATTR(2); Status = NtCreateDirectoryObject(&Cleanup->Handle, a[1], OA(2)); RETURN_HANDLE();
    case W64_NtCreateSymbolicLinkObject:
    {
        UNICODE_STRING Target;
        PROBE(0);
        ATTR(2);
        Status = NtCreateSymbolicLinkObject(&Cleanup->Handle, a[1], OA(2), String32(&Target, P(3)));
        RETURN_HANDLE();
    }
    case W64_NtOpenSymbolicLinkObject:
        PROBE(0);
        ATTR(2); Status = NtOpenSymbolicLinkObject(&Cleanup->Handle, a[1], OA(2)); RETURN_HANDLE();
    case W64_NtQuerySymbolicLinkObject:
        String32(&Name, P(1));
        Status = NtQuerySymbolicLinkObject(H(0), &Name, P(2));
        ((WOW64_STRING *)P(1))->Length = Name.Length;
        return Status;
    case W64_NtQueryDirectoryObject:
    {
        typedef struct { WOW64_STRING Name, TypeName; } DIRECTORY32;
        OBJECT_DIRECTORY_INFORMATION *From;
        DIRECTORY32 *To = P(1);
        ULONG Index, Bytes = 0, Count = 0, NativeLength, Required, i;
        PUCHAR Strings;
        const ULONG Extra = 2 * (sizeof(*From) - sizeof(*To));
        PROBE(5);
        if (a[6]) PROBE(6);
        ProbeOutput(a[1], a[2]);
        if (a[2] > MAXULONG - Extra) return STATUS_INVALID_PARAMETER;
        Index = a[4] ? 0 : U(5);
        NativeLength = a[2] + Extra;
        From = Cleanup->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, NativeLength);
        if (!From) return STATUS_NO_MEMORY;
        Status = NtQueryDirectoryObject(H(0), From, NativeLength, a[3], a[4], &Index, &Bytes);
        if (!NT_SUCCESS(Status))
        {
            if (a[6]) U(6) = Status == STATUS_BUFFER_TOO_SMALL && Bytes >= Extra ? Bytes - Extra :
                              Bytes >= sizeof(*From) ? Bytes - (sizeof(*From) - sizeof(*To)) : Bytes;
            return Status;
        }
        /* The kernel supplies an array terminated by an empty entry, with
         * both counted strings behind it. The extra space above pays for
         * widening one result and its terminator, even for a single query. */
        while ((Count + 1) * sizeof(*From) <= Bytes && From[Count].Name.Buffer)
            ++Count;
        if ((Count + 1) * sizeof(*From) > Bytes) return STATUS_INFO_LENGTH_MISMATCH;
        Required = Bytes - (Count + 1) * (sizeof(*From) - sizeof(*To));
        if (a[6]) U(6) = Required;
        if (Required > a[2]) return STATUS_BUFFER_TOO_SMALL;
        Strings = (PUCHAR)(To + Count + 1);
        for (i = 0; i < Count; ++i)
        {
            UNICODE_STRING *Source[2] = {&From[i].Name, &From[i].TypeName};
            WOW64_STRING *Destination[2] = {&To[i].Name, &To[i].TypeName};
            ULONG j;
            for (j = 0; j < 2; ++j)
            {
                Destination[j]->Length = Source[j]->Length;
                Destination[j]->MaximumLength = Source[j]->MaximumLength;
                Destination[j]->Buffer = PtrToUlong(Strings);
                RtlCopyMemory(Strings, Source[j]->Buffer, Source[j]->MaximumLength);
                Strings += Source[j]->MaximumLength;
            }
        }
        RtlZeroMemory(To + Count, sizeof(*To));
        U(5) = Index;
        return Status;
    }
    case W64_NtOpenKey:
        PROBE(0);
        ATTR(2);
        KeyAccess = a[1];
        if (!(KeyAccess & KEY_WOW64_RES)) KeyAccess |= KEY_WOW64_32KEY;
        Status = NtOpenKey(&Cleanup->Handle, KeyAccess, OA(2)); RETURN_HANDLE();
    case W64_NtCreateKey:
    {
        UNICODE_STRING Class;
        PROBE(0);
        ATTR(2);
        KeyAccess = a[1];
        if (!(KeyAccess & KEY_WOW64_RES)) KeyAccess |= KEY_WOW64_32KEY;
        Status = NtCreateKey(&Cleanup->Handle, KeyAccess, OA(2), a[3], String32(&Class, P(4)), a[5], P(6));
        RETURN_HANDLE();
    }
    case W64_NtQueryValueKey:
        return NtQueryValueKey(H(0), String32(&Name, P(1)), a[2], P(3), a[4], P(5));
    case W64_NtQueryKey: return NtQueryKey(H(0), a[1], P(2), a[3], P(4));
    case W64_NtEnumerateKey: return NtEnumerateKey(H(0), a[1], a[2], P(3), a[4], P(5));
    case W64_NtEnumerateValueKey: return NtEnumerateValueKey(H(0), a[1], a[2], P(3), a[4], P(5));
    case W64_NtSetValueKey:
        return NtSetValueKey(H(0), String32(&Name, P(1)), a[2], a[3], P(4), a[5]);
    case W64_NtDeleteKey: return NtDeleteKey(H(0));
    case W64_NtDeleteValueKey: return NtDeleteValueKey(H(0), String32(&Name, P(1)));
    case W64_NtFlushKey: return NtFlushKey(H(0));
    case W64_NtCreateJobObject:
        PROBE(0); ATTR(2);
        Status = NtCreateJobObject(&Cleanup->Handle, a[1], OA(2));
        RETURN_HANDLE();
    case W64_NtOpenJobObject:
        PROBE(0); ATTR(2);
        Status = NtOpenJobObject(&Cleanup->Handle, a[1], OA(2));
        RETURN_HANDLE();
    case W64_NtAssignProcessToJobObject: return NtAssignProcessToJobObject(H(0), H(1));
    case W64_NtTerminateJobObject: return NtTerminateJobObject(H(0), a[1]);
    case W64_NtSetInformationJobObject:
        if (a[1] == JobObjectBasicLimitInformation || a[1] == JobObjectExtendedLimitInformation)
        {
            const WOW64_JOB_EXTENDED_LIMIT *From = P(2);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION To = {0};
            BOOLEAN Extended = a[1] == JobObjectExtendedLimitInformation;
            if (a[3] != (Extended ? sizeof(*From) : sizeof(From->Basic)))
                return STATUS_INFO_LENGTH_MISMATCH;
            To.BasicLimitInformation.PerProcessUserTimeLimit = From->Basic.PerProcessUserTimeLimit;
            To.BasicLimitInformation.PerJobUserTimeLimit = From->Basic.PerJobUserTimeLimit;
            To.BasicLimitInformation.LimitFlags = From->Basic.LimitFlags;
            To.BasicLimitInformation.MinimumWorkingSetSize = From->Basic.MinimumWorkingSetSize;
            To.BasicLimitInformation.MaximumWorkingSetSize = From->Basic.MaximumWorkingSetSize;
            To.BasicLimitInformation.ActiveProcessLimit = From->Basic.ActiveProcessLimit;
            To.BasicLimitInformation.Affinity = From->Basic.Affinity;
            To.BasicLimitInformation.PriorityClass = From->Basic.PriorityClass;
            To.BasicLimitInformation.SchedulingClass = From->Basic.SchedulingClass;
            if (Extended)
            {
                To.IoInfo = From->IoInfo;
                To.ProcessMemoryLimit = From->ProcessMemoryLimit;
                To.JobMemoryLimit = From->JobMemoryLimit;
                To.PeakProcessMemoryUsed = From->PeakProcessMemoryUsed;
                To.PeakJobMemoryUsed = From->PeakJobMemoryUsed;
            }
            return NtSetInformationJobObject(H(0), a[1], &To,
                       Extended ? sizeof(To) : sizeof(To.BasicLimitInformation));
        }
        return STATUS_NOT_SUPPORTED;
    case W64_NtCreateEvent:
        PROBE(0);
        ATTR(2); Status = NtCreateEvent(&Cleanup->Handle, a[1], OA(2), a[3], a[4]); RETURN_HANDLE();
    case W64_NtOpenEvent:
        PROBE(0);
        ATTR(2); Status = NtOpenEvent(&Cleanup->Handle, a[1], OA(2)); RETURN_HANDLE();
    case W64_NtSetEvent: return NtSetEvent(H(0), P(1));
    case W64_NtResetEvent: return NtResetEvent(H(0), P(1));
    case W64_NtCreateSemaphore:
        PROBE(0);
        ATTR(2); Status = NtCreateSemaphore(&Cleanup->Handle, a[1], OA(2), a[3], a[4]); RETURN_HANDLE();
    case W64_NtReleaseSemaphore: return NtReleaseSemaphore(H(0), a[1], P(2));
    case W64_NtCreateMutant:
        PROBE(0);
        ATTR(2); Status = NtCreateMutant(&Cleanup->Handle, a[1], OA(2), a[3]); RETURN_HANDLE();
    case W64_NtReleaseMutant: return NtReleaseMutant(H(0), P(1));
    case W64_NtCreateTimer:
        PROBE(0);
        ATTR(2); Status = NtCreateTimer(&Cleanup->Handle, a[1], OA(2), a[3]); RETURN_HANDLE();
    case W64_NtOpenTimer:
        PROBE(0);
        ATTR(2); Status = NtOpenTimer(&Cleanup->Handle, a[1], OA(2)); RETURN_HANDLE();
    case W64_NtSetTimer:
        return NtSetTimer(H(0), P(1), a[2] ? (PTIMER_APC_ROUTINE)(ULONG_PTR)(-((LONG64)a[2] << 2)) : NULL,
                          P(3), a[4], a[5], P(6));
    case W64_NtCancelTimer: return NtCancelTimer(H(0), P(1));
    case W64_NtQueryTimer: return NtQueryTimer(H(0), a[1], P(2), a[3], P(4));
    case W64_NtCreateIoCompletion:
        PROBE(0);
        ATTR(2); Status = NtCreateIoCompletion(&Cleanup->Handle, a[1], OA(2), a[3]); RETURN_HANDLE();
    case W64_NtOpenIoCompletion:
        PROBE(0);
        ATTR(2); Status = NtOpenIoCompletion(&Cleanup->Handle, a[1], OA(2)); RETURN_HANDLE();
    case W64_NtQueryIoCompletion:
        return NtQueryIoCompletion(H(0), a[1], P(2), a[3], P(4));
    case W64_NtSetIoCompletion:
        return NtSetIoCompletion(H(0), P(1), P(2), a[3], a[4]);
    case W64_NtRemoveIoCompletion:
    {
        PVOID Key, Context;
        PROBE(1); PROBE(2); ProbeOutput(a[3], sizeof(WOW64_IOSB));
        Status = NtRemoveIoCompletion(H(0), &Key, &Context, &Io, P(4));
        if (Status == STATUS_SUCCESS)
        {
            U(1) = PtrToUlong(Key);
            U(2) = PtrToUlong(Context);
            StoreIo(a[3], &Io);
        }
        return Status;
    }
    case W64_NtCreateKeyedEvent:
        PROBE(0);
        ATTR(2); Status = NtCreateKeyedEvent(&Cleanup->Handle, a[1], OA(2), a[3]); RETURN_HANDLE();
    case W64_NtWaitForKeyedEvent: return NtWaitForKeyedEvent(H(0), P(1), a[2], P(3));
    case W64_NtReleaseKeyedEvent: return NtReleaseKeyedEvent(H(0), P(1), a[2], P(3));
    case W64_NtOpenFile:
        PROBE(0); ProbeOutput(a[3], sizeof(WOW64_IOSB));
        FILEATTR(2);
        Status = NtOpenFile(&Cleanup->Handle, a[1], OA(2), &Io, a[4], a[5]);
        /* Failed native calls do not transfer handle ownership. In particular,
         * an undefined failure output must not be closed if StoreIo faults. */
        if (!NT_SUCCESS(Status)) Cleanup->Handle = NULL;
        StoreIo(a[3], &Io); RETURN_HANDLE();
    case W64_NtCreateFile:
        PROBE(0); ProbeOutput(a[3], sizeof(WOW64_IOSB));
        FILEATTR(2);
        Status = NtCreateFile(&Cleanup->Handle, a[1], OA(2), &Io, P(4), a[5], a[6], a[7], a[8], P(9), a[10]);
        if (!NT_SUCCESS(Status)) Cleanup->Handle = NULL;
        StoreIo(a[3], &Io); RETURN_HANDLE();
    case W64_NtCreateNamedPipeFile:
        PROBE(0); ProbeOutput(a[3], sizeof(WOW64_IOSB));
        FILEATTR(2);
        Status = NtCreateNamedPipeFile(&Cleanup->Handle, a[1], OA(2), &Io,
                                       a[4], a[5], a[6], a[7], a[8], a[9],
                                       a[10], a[11], a[12], P(13));
        if (!NT_SUCCESS(Status)) Cleanup->Handle = NULL;
        StoreIo(a[3], &Io); RETURN_HANDLE();
    case W64_NtLockFile:
        return NtLockFile(H(0), H(1),
                         a[2] ? (PIO_APC_ROUTINE)(ULONG_PTR)(-((LONG64)a[2] << 2)) : NULL,
                         P(3), (PVOID)(0x8000000000000000ULL | a[4]),
                         P(5), P(6), a[7], a[8], a[9]);
    case W64_NtUnlockFile:
        ProbeOutput(a[1], sizeof(WOW64_IOSB));
        /* Native unlock waits for completion, including asynchronous handles. */
        Status = NtUnlockFile(H(0), &Io, P(2), P(3), a[4]);
        StoreIo(a[1], &Io);
        return Status;
    case W64_NtDeviceIoControlFile:
    {
        PIO_APC_ROUTINE Routine = a[2] ? (PIO_APC_ROUTINE)(ULONG_PTR)(-((LONG64)a[2] << 2)) : NULL;
        PIO_STATUS_BLOCK Iosb = (PVOID)(0x8000000000000000ULL | a[4]);
        PVOID Input = P(6);
        ULONG InputLength = a[7];
        AFD_RECV_INFO Transfer;
        AFD_ACCEPT_DATA Accept;
        /* These requests contain counted strings/addresses or opaque bytes.
         * AFD requests carrying handles or WSABUF arrays need translation. */
        switch (a[5])
        {
        case IOCTL_MOUNTMGR_QUERY_DOS_VOLUME_PATH:
        case IOCTL_TCP_QUERY_INFORMATION_EX: /* TCP captures the guest context offset. */
        case IOCTL_AFD_BIND:
        case IOCTL_AFD_CONNECT:
        case IOCTL_AFD_START_LISTEN:
        case IOCTL_AFD_WAIT_FOR_LISTEN:
        case IOCTL_AFD_DISCONNECT:
        case IOCTL_AFD_GET_INFO:
        case IOCTL_AFD_SET_INFO:
        case IOCTL_AFD_GET_SOCK_NAME:
        case IOCTL_AFD_GET_PEER_NAME:
        case IOCTL_AFD_SET_CONTEXT:
        case IOCTL_AFD_GET_CONTEXT_SIZE:
        case IOCTL_AFD_GET_TDI_HANDLES: /* AFD captures the 32-bit output handles. */
        case IOCTL_AFD_SELECT: /* AFD owns the translated output until completion. */
            break;
        case IOCTL_AFD_ACCEPT:
            if (InputLength < 3 * sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
            Accept.UseSAN = ((ULONG *)Input)[0];
            Accept.SequenceNumber = ((ULONG *)Input)[1];
            Accept.ListenHandle = HANDLE32(((ULONG *)Input)[2]);
            Input = &Accept;
            InputLength = sizeof(Accept);
            break;
        case IOCTL_AFD_SEND:
        case IOCTL_AFD_RECV:
        {
            const ULONG *From = Input;
            const ULONG *Buffers;
            ULONG i;
            if (InputLength < 4 * sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
            Transfer.BufferCount = From[1];
            Transfer.AfdFlags = From[2];
            Transfer.TdiFlags = From[3];
            Buffers = PTR32(From[0]);
            /* AFD's capture adds two pointer-sized mapping fields per buffer
             * and uses a ULONG byte count. Bound that allocation as well. */
            if (!Transfer.BufferCount ||
                Transfer.BufferCount > MAXULONG / (sizeof(AFD_WSABUF) + 2 * sizeof(PVOID)))
                return STATUS_INVALID_PARAMETER;
            Transfer.BufferArray = Cleanup->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0,
                                        Transfer.BufferCount * sizeof(AFD_WSABUF));
            if (!Transfer.BufferArray) return STATUS_NO_MEMORY;
            for (i = 0; i < Transfer.BufferCount; ++i)
            {
                Transfer.BufferArray[i].len = Buffers[2 * i];
                Transfer.BufferArray[i].buf = PTR32(Buffers[2 * i + 1]);
            }
            /* AFD captures both the request and the descriptors and locks the
             * actual user buffers before returning STATUS_PENDING. */
            Input = &Transfer;
            InputLength = sizeof(Transfer);
            break;
        }
        default:
            return STATUS_NOT_SUPPORTED;
        }
        ProbeOutput(a[4], sizeof(WOW64_IOSB));
        return NtDeviceIoControlFile(H(0), H(1), Routine, P(3), Iosb,
                                      a[5], Input, InputLength, P(8), a[9]);
    }
    case W64_NtFsControlFile:
        /* These pipe operations carry opaque bytes or fixed-width records.
         * Keep the guest IOSB alive through pending listen/transceive requests. */
        switch (a[5])
        {
        case FSCTL_PIPE_PEEK: case FSCTL_PIPE_LISTEN: case FSCTL_PIPE_DISCONNECT:
        case FSCTL_PIPE_WAIT: case FSCTL_PIPE_TRANSCEIVE:
            return NtFsControlFile(H(0), H(1),
                       a[2] ? (PIO_APC_ROUTINE)(ULONG_PTR)(-((LONG64)a[2] << 2)) : NULL,
                       P(3), (PVOID)(0x8000000000000000ULL | a[4]),
                       a[5], P(6), a[7], P(8), a[9]);
        default: return STATUS_NOT_SUPPORTED;
        }
    case W64_NtQueryAttributesFile:
        FILEATTR(0); return NtQueryAttributesFile(OA(0), P(1));
    case W64_NtQueryFullAttributesFile:
        FILEATTR(0); return NtQueryFullAttributesFile(OA(0), P(1));
    case W64_NtDeleteFile:
        FILEATTR(0); return NtDeleteFile(OA(0));
    case W64_NtFlushBuffersFile:
    {
        FILE_MODE_INFORMATION Mode;
        ProbeOutput(a[1], sizeof(WOW64_IOSB));
        Status = NtQueryInformationFile(H(0), &Io, &Mode, sizeof(Mode), FileModeInformation);
        if (!NT_SUCCESS(Status)) return Status;
        if (!(Mode.Mode & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)))
            return STATUS_NOT_SUPPORTED;
        Status = NtFlushBuffersFile(H(0), &Io);
        StoreIo(a[1], &Io);
        return Status;
    }
    case W64_NtSetInformationFile:
        switch (a[4])
        {
        case FileRenameInformation:
        case FileLinkInformation:
        {
            WOW64_RENAME_INFORMATION *From = P(2);
            FILE_RENAME_INFORMATION *To;
            PVOID Redirected = NULL;
            ULONG Bytes;
            BOOLEAN Replace;
            if (a[3] < FIELD_OFFSET(WOW64_RENAME_INFORMATION, Name))
                return STATUS_INFO_LENGTH_MISMATCH;
            Bytes = From->Length;
            if (Bytes > a[3] - FIELD_OFFSET(WOW64_RENAME_INFORMATION, Name) ||
                Bytes > MAXUSHORT || (Bytes & 1))
                return STATUS_INVALID_PARAMETER;
            ProbeOutput(a[1], sizeof(WOW64_IOSB));
            Replace = From->Replace;
            Name.Buffer = From->Name;
            Name.Length = Name.MaximumLength = Bytes;
            InitializeObjectAttributes(&Attributes, &Name, 0, HANDLE32(From->Root), NULL);
            _SEH2_TRY
            {
                Status = Wow64RedirectFile(&Attributes, &Name, &Redirected);
                if (NT_SUCCESS(Status))
                {
                    Bytes = sizeof(*To) + Name.Length;
                    To = Cleanup->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Bytes);
                    if (!To) Status = STATUS_NO_MEMORY;
                    else
                    {
                        To->ReplaceIfExists = Replace;
                        To->RootDirectory = Attributes.RootDirectory;
                        To->FileNameLength = Name.Length;
                        RtlCopyMemory(To->FileName, Name.Buffer, Name.Length);
                        Status = NtSetInformationFile(H(0), &Io, To, Bytes, a[4]);
                        StoreIo(a[1], &Io);
                    }
                }
            }
            _SEH2_FINALLY
            {
                if (Redirected) RtlFreeHeap(RtlGetProcessHeap(), 0, Redirected);
            }
            _SEH2_END;
            return Status;
        }
        case FileCompletionInformation:
        {
            FILE_COMPLETION_INFORMATION Completion;
            if (a[3] != 2 * sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
            ProbeOutput(a[1], sizeof(WOW64_IOSB));
            Completion.Port = HANDLE32(((ULONG *)P(2))[0]);
            Completion.Key = PTR32(((ULONG *)P(2))[1]);
            Status = NtSetInformationFile(H(0), &Io, &Completion, sizeof(Completion), a[4]);
            StoreIo(a[1], &Io);
            return Status;
        }
        case FileBasicInformation: case FileDispositionInformation:
        case FilePositionInformation: case FileEndOfFileInformation:
        case FileAllocationInformation: case FilePipeInformation:
            ProbeOutput(a[1], sizeof(WOW64_IOSB));
            Status = NtSetInformationFile(H(0), &Io, P(2), a[3], a[4]);
            StoreIo(a[1], &Io); return Status;
        default: return STATUS_NOT_SUPPORTED;
        }
    case W64_NtQueryInformationFile:
        /* Only fixed-width layouts used by the loader and console startup. */
        switch (a[4])
        {
        case FileBasicInformation: case FileStandardInformation: case FilePositionInformation:
        case FileNameInformation: case FileModeInformation: case FileInternalInformation:
        case FileAlignmentInformation: case FileNetworkOpenInformation:
        case FileAttributeTagInformation: case FileAllInformation:
        case FilePipeLocalInformation:
            ProbeOutput(a[1], sizeof(WOW64_IOSB));
            Status = NtQueryInformationFile(H(0), &Io, P(2), a[3], a[4]);
            StoreIo(a[1], &Io); return Status;
        default: return STATUS_NOT_SUPPORTED;
        }
    case W64_NtQueryDirectoryFile:
    {
        FILE_MODE_INFORMATION Mode;
        if (a[1] || a[2] || a[3]) return STATUS_NOT_SUPPORTED;
        Status = NtQueryInformationFile(H(0), &Io, &Mode, sizeof(Mode), FileModeInformation);
        if (!NT_SUCCESS(Status)) return Status;
        if (!(Mode.Mode & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)))
            return STATUS_NOT_SUPPORTED;
        switch (a[7])
        {
        case FileDirectoryInformation: case FileFullDirectoryInformation:
        case FileBothDirectoryInformation: case FileNamesInformation:
        case FileIdBothDirectoryInformation: case FileIdFullDirectoryInformation:
            ProbeOutput(a[4], sizeof(WOW64_IOSB));
            Status = NtQueryDirectoryFile(H(0), NULL, NULL, NULL, &Io, P(5), a[6], a[7],
                                           a[8], String32(&Name, P(9)), a[10]);
            StoreIo(a[4], &Io); return Status;
        default: return STATUS_NOT_SUPPORTED;
        }
    }
    case W64_NtQueryVolumeInformationFile:
        switch (a[4])
        {
        case FileFsDeviceInformation: case FileFsVolumeInformation: case FileFsSizeInformation:
        case FileFsAttributeInformation:
            ProbeOutput(a[1], sizeof(WOW64_IOSB));
            Status = NtQueryVolumeInformationFile(H(0), &Io, P(2), a[3], a[4]);
            StoreIo(a[1], &Io); return Status;
        default: return STATUS_NOT_SUPPORTED;
        }
    case W64_NtReadFile:
    case W64_NtWriteFile:
    {
        PIO_APC_ROUTINE Routine = a[2] ? (PIO_APC_ROUTINE)(ULONG_PTR)(-((LONG64)a[2] << 2)) : NULL;
        PIO_STATUS_BLOCK Iosb = (PVOID)(0x8000000000000000ULL | a[4]);
        ProbeOutput(a[4], sizeof(WOW64_IOSB));
        /* The kernel captures this explicit IOSB32 marker in the IRP. Its
         * completion writes the guest block before signaling or queuing APCs,
         * with no per-request user allocation to leak on thread termination. */
        if (Number == W64_NtReadFile)
            return NtReadFile(H(0), H(1), Routine, P(3), Iosb, P(5), a[6], P(7), P(8));
        return NtWriteFile(H(0), H(1), Routine, P(3), Iosb, P(5), a[6], P(7), P(8));
    }
    case W64_NtCancelIoFile:
        ProbeOutput(a[1], sizeof(WOW64_IOSB));
        Status = NtCancelIoFile(H(0), &Io);
        StoreIo(a[1], &Io); return Status;
    case W64_NtOpenProcess:
    case W64_NtOpenThread:
    {
        CLIENT_ID Id, *ClientId = NULL;
        PROBE(0);
        ATTR(2);
        if (a[3])
        {
            Id.UniqueProcess = HANDLE32(((ULONG *)P(3))[0]);
            Id.UniqueThread = HANDLE32(((ULONG *)P(3))[1]);
            ClientId = &Id;
        }
        if (Number == W64_NtOpenThread)
            Status = NtOpenThread(&Cleanup->Handle, a[1], OA(2), ClientId);
        else Status = NtOpenProcess(&Cleanup->Handle, a[1], OA(2), ClientId);
        RETURN_HANDLE();
    }
    case W64_NtAdjustPrivilegesToken:
        return NtAdjustPrivilegesToken(H(0), a[1], P(2), a[3], P(4), P(5));
    case W64_NtDuplicateToken:
        PROBE(5); ATTR(2);
        Status = NtDuplicateToken(H(0), a[1], OA(2), a[3], a[4], &Cleanup->Handle);
        RETURN_HANDLE_AT(5);
    case W64_NtAccessCheck:
        return NtAccessCheck(Security32(&Descriptor, P(0)), H(1), a[2], P(3), P(4), P(5), P(6), P(7));
    case W64_NtSetSecurityObject:
        return NtSetSecurityObject(H(0), a[1], Security32(&Descriptor, P(2)));
    case W64_NtQuerySecurityObject:
        return NtQuerySecurityObject(H(0), a[1], P(2), a[3], P(4));
    case W64_NtOpenProcessToken:
        PROBE(2);
        Status = NtOpenProcessToken(H(0), a[1], &Cleanup->Handle);
        RETURN_HANDLE_AT(2);
    case W64_NtOpenThreadToken:
        PROBE(3);
        Status = NtOpenThreadToken(H(0), a[1], a[2], &Cleanup->Handle);
        RETURN_HANDLE_AT(3);
    case W64_NtOpenProcessTokenEx:
        PROBE(3);
        Status = NtOpenProcessTokenEx(H(0), a[1], a[2], &Cleanup->Handle);
        RETURN_HANDLE_AT(3);
    case W64_NtOpenThreadTokenEx:
        PROBE(4);
        Status = NtOpenThreadTokenEx(H(0), a[1], a[2], a[3], &Cleanup->Handle);
        RETURN_HANDLE_AT(4);
    case W64_NtQueryInformationToken:
        if (a[1] == TokenUser || a[1] == TokenOwner || a[1] == TokenPrimaryGroup)
        {
            union { TOKEN_USER User; UCHAR Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]; } Info;
            ULONG Bytes, Prefix = a[1] == TokenUser ? 2 * sizeof(ULONG) : sizeof(ULONG);
            PROBE(4);
            Status = NtQueryInformationToken(H(0), a[1], &Info, sizeof(Info), &Bytes);
            if (!NT_SUCCESS(Status)) return Status;
            Bytes = RtlLengthSid(Info.User.User.Sid);
            U(4) = Prefix + Bytes;
            if (a[3] < Prefix + Bytes) return STATUS_BUFFER_TOO_SMALL;
            ProbeOutput(a[2], Prefix + Bytes);
            ((ULONG *)P(2))[0] = a[2] + Prefix;
            if (a[1] == TokenUser) ((ULONG *)P(2))[1] = Info.User.User.Attributes;
            RtlCopyMemory((BYTE *)P(2) + Prefix, Info.User.User.Sid, Bytes);
            return STATUS_SUCCESS;
        }
        switch (a[1])
        {
        case TokenType: case TokenImpersonationLevel: case TokenStatistics:
        case TokenPrivileges: case TokenSessionId: case TokenElevationType:
        case TokenElevation: case TokenIsAppContainer:
            return NtQueryInformationToken(H(0), a[1], P(2), a[3], P(4));
        default: return STATUS_NOT_SUPPORTED;
        }
    case W64_NtSetInformationToken:
        if (a[1] == TokenOwner || a[1] == TokenPrimaryGroup || a[1] == TokenDefaultDacl)
        {
            PVOID Pointer;
            if (a[3] < sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
            Pointer = PTR32(U(2));
            return NtSetInformationToken(H(0), a[1], &Pointer, sizeof(Pointer));
        }
        if (a[1] == TokenSessionId)
            return NtSetInformationToken(H(0), a[1], P(2), a[3]);
        return STATUS_NOT_SUPPORTED;
    case W64_NtQueryInformationProcess:
        if (a[1] == ProcessBasicInformation)
        {
            PROCESS_BASIC_INFORMATION Native;
            ULONG_PTR Peb;
            struct { LONG Exit; ULONG Peb, Affinity; LONG Priority; ULONG Id, Parent; } *Guest = P(2);
            if (a[3] != sizeof(*Guest)) return STATUS_INFO_LENGTH_MISMATCH;
            Status = NtQueryInformationProcess(H(0), ProcessBasicInformation, &Native, sizeof(Native), NULL);
            if (!NT_SUCCESS(Status)) return Status;
            Status = NtQueryInformationProcess(H(0), ProcessWow64Information, &Peb, sizeof(Peb), NULL);
            if (!NT_SUCCESS(Status)) return Status;
            Guest->Exit = Native.ExitStatus;
            Guest->Peb = (ULONG)Peb;
            Guest->Affinity = Native.AffinityMask;
            Guest->Priority = Native.BasePriority;
            Guest->Id = Native.UniqueProcessId;
            Guest->Parent = Native.InheritedFromUniqueProcessId;
            if (a[4]) U(4) = sizeof(*Guest);
            return STATUS_SUCCESS;
        }
        if (a[1] == ProcessWow64Information)
        {
            ULONG_PTR Peb;
            if (a[3] != sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
            Status = NtQueryInformationProcess(H(0), a[1], &Peb, sizeof(Peb), NULL);
            if (NT_SUCCESS(Status)) U(2) = (ULONG)Peb;
            if (a[4]) U(4) = sizeof(ULONG);
            return Status;
        }
        switch (a[1])
        {
        case ProcessDefaultHardErrorMode: case ProcessExecuteFlags: case ProcessPriorityClass:
        case ProcessSessionInformation: case ProcessBreakOnTermination: case ProcessCookie:
            return NtQueryInformationProcess(H(0), a[1], P(2), a[3], P(4));
        default: return STATUS_NOT_SUPPORTED;
        }
    case W64_NtSetInformationProcess:
        if (a[1] == ProcessAffinityMask)
        {
            ULONG_PTR Affinity;
            if (a[3] != sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
            Affinity = U(2);
            return NtSetInformationProcess(H(0), a[1], &Affinity, sizeof(Affinity));
        }
        switch (a[1])
        {
        case ProcessDefaultHardErrorMode: case ProcessExecuteFlags: case ProcessPriorityClass:
            return NtSetInformationProcess(H(0), a[1], P(2), a[3]);
        default: return STATUS_NOT_SUPPORTED;
        }
    case W64_NtSetInformationThread:
        switch (a[1])
        {
        case ThreadZeroTlsCell:
        {
            ULONG Index;
            if (a[3] != sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
            Index = U(2);
            if (Index >= TLS_MINIMUM_AVAILABLE + TLS_EXPANSION_SLOTS) return STATUS_INVALID_PARAMETER;
            Index |= 0x80000000;
            return NtSetInformationThread(H(0), a[1], &Index, sizeof(Index));
        }
        case ThreadQuerySetWin32StartAddress: case ThreadAffinityMask:
        case ThreadIdealProcessor: case ThreadPriorityBoost:
        {
            ULONG_PTR Value;
            if (a[3] != sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
            Value = U(2);
            return NtSetInformationThread(H(0), a[1], &Value, sizeof(Value));
        }
        case ThreadHideFromDebugger: case ThreadPriority: case ThreadBasePriority:
        case ThreadBreakOnTermination:
            return NtSetInformationThread(H(0), a[1], P(2), a[3]);
        default: return STATUS_NOT_SUPPORTED;
        }
    case W64_NtQuerySystemInformation:
        if (a[0] == SystemBasicInformation)
        {
            SYSTEM_BASIC_INFORMATION From;
            WOW64_SYSTEM_BASIC_INFORMATION *To = P(1);
            if (a[2] != sizeof(*To)) return STATUS_INFO_LENGTH_MISMATCH;
            Status = NtQuerySystemInformation(a[0], &From, sizeof(From), NULL);
            if (NT_SUCCESS(Status))
            {
                RtlCopyMemory(To, &From, 7 * sizeof(ULONG));
                To->MinimumUserModeAddress = 0x10000;
                To->MaximumUserModeAddress = NativeSystemInformation ? 0xfffeffff : Wow64HighestUserAddress;
                To->ActiveProcessorsAffinityMask = (ULONG)From.ActiveProcessorsAffinityMask;
                To->NumberOfProcessors = From.NumberOfProcessors;
                if (a[3]) U(3) = sizeof(*To);
            }
            return Status;
        }
        switch (a[0])
        {
        case SystemProcessorInformation:
            Status = NtQuerySystemInformation(a[0], P(1), a[2], P(3));
            if (NT_SUCCESS(Status) && !NativeSystemInformation)
                ((SYSTEM_PROCESSOR_INFORMATION *)P(1))->ProcessorArchitecture = PROCESSOR_ARCHITECTURE_INTEL;
            return Status;
        case SystemTimeOfDayInformation:
            return NtQuerySystemInformation(a[0], P(1), a[2], P(3));
        default: return STATUS_NOT_SUPPORTED;
        }
    default:
        Length = RTL_NUMBER_OF(ServiceNames);
        DbgPrint("WoW64: unsupported service %lx (%s)\n", Number,
                 Number < Length ? ServiceNames[Number] : "unknown");
        return STATUS_INVALID_SYSTEM_SERVICE;
    }
}

NTSTATUS NTAPI Wow64SystemService(ULONG Number, ULONG *Arguments)
{
    NTSTATUS Status;
    WOW64_CLEANUP Cleanup = {0};
    /* All pointer expansion occurs in user mode. Faulting captures become
     * NT errors; no untyped 32-bit argument list is sent to the kernel. */
    _SEH2_TRY
    {
        Status = (Number >= 0x1000 && Number < 0x2000) ?
            Wow64WinSystemService(Number, Arguments) : Dispatch(Number, Arguments, &Cleanup);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (Cleanup.Thread)
        Wow64AbortThread(Cleanup.Thread, Cleanup.ThreadProcess, Status);
    if (Cleanup.Handle)
    {
        if (Cleanup.HandleProcess)
            NtDuplicateObject(Cleanup.HandleProcess, Cleanup.Handle, NULL, NULL, 0, 0, DUPLICATE_CLOSE_SOURCE);
        else NtClose(Cleanup.Handle);
    }
    if (Cleanup.Buffer) RtlFreeHeap(RtlGetProcessHeap(), 0, Cleanup.Buffer);
    if (Cleanup.Base)
    {
        if (Cleanup.Mapped)
            NtUnmapViewOfSection(Cleanup.Process, Cleanup.Base);
        else
        {
            SIZE_T Size = 0;
            NtFreeVirtualMemory(Cleanup.Process, &Cleanup.Base, &Size, MEM_RELEASE);
        }
    }
    if (!NT_SUCCESS(Status) && Number != W64_NtQueryDebugFilterState &&
        !(Number >= 0x1000 && Number < 0x2000))
        DbgPrint("WoW64: service %lx returned %08lx\n", Number, Status);
    return Status;
}
