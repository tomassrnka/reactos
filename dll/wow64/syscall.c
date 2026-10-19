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

static const char *ServiceName(ULONG Service)
{
    if (Service < RTL_NUMBER_OF(ServiceNames)) return ServiceNames[Service];
    if (Service == WOW64_SERVICE_CSR) return "CsrClientCallServer";
    if (Service == WOW64_SERVICE_CREATE_PROCESS) return "CreateProcess";
    return "unknown";
}

/* Keys of the reported requests, never 0. When the table is full, further new
 * items are summarized by a single line instead of being printed on every call. */
static volatile LONG64 ReportedKeys[512];
static volatile LONG ReportedFull;

BOOLEAN Wow64FirstReport(ULONG Service, ULONG Kind, ULONG Value)
{
    LONG64 Key;
    ULONG i;
    /* A guest can pass any service number: one bucket for those outside the known
     * ranges, keyed by the number itself */
    if (Service >= RTL_NUMBER_OF(ServiceNames) && Service != WOW64_SERVICE_CSR &&
        Service != WOW64_SERVICE_CREATE_PROCESS)
    {
        Value = Service;
        Service = 0xfffff;
    }
    Key = (LONG64)(0x8000000000000000ULL | ((ULONG64)Service << 36) | ((ULONG64)(Kind & 0xf) << 32) | Value);
    for (i = 0; i < RTL_NUMBER_OF(ReportedKeys); ++i)
    {
        LONG64 Old = ReportedKeys[i];
        if (!Old) Old = InterlockedCompareExchange64(&ReportedKeys[i], Key, 0);
        if (!Old) return TRUE;
        if (Old == Key) return FALSE;
    }
    if (!InterlockedExchange(&ReportedFull, 1))
        DbgPrint("WoW64: more than %lu unsupported items, no further ones are reported, pid %lx\n",
                 (ULONG)RTL_NUMBER_OF(ReportedKeys), HandleToUlong(NtCurrentTeb()->ClientId.UniqueProcess));
    return FALSE;
}

NTSTATUS Wow64Unsupported(ULONG Service, ULONG Kind, ULONG Value, BOOLEAN *Reported)
{
    static const char *Kinds[] = {"class", "class", "ioctl", "fsctl", "mode", "attribute", "api", "server", "operation"};
    *Reported = TRUE;
    if (Wow64FirstReport(Service, Kind, Value))
    {
        if (Kind == WOW64_UNSUPPORTED_SERVICE)
            DbgPrint("WoW64: unsupported service %lx (%s) class %lu, pid %lx\n", Service,
                     ServiceName(Service), Value, HandleToUlong(NtCurrentTeb()->ClientId.UniqueProcess));
        else
            DbgPrint(Kind == WOW64_UNSUPPORTED_CLASS || Kind == WOW64_UNSUPPORTED_CSR_SERVER ||
                     Kind == WOW64_UNSUPPORTED_OPERATION ? "WoW64: unsupported %s (%lx) %s %lu, pid %lx\n" :
                     "WoW64: unsupported %s (%lx) %s 0x%08lx, pid %lx\n", ServiceName(Service), Service,
                     Kind < RTL_NUMBER_OF(Kinds) ? Kinds[Kind] : "value", Value,
                     HandleToUlong(NtCurrentTeb()->ClientId.UniqueProcess));
    }
    return Kind == WOW64_UNSUPPORTED_SERVICE || Kind == WOW64_UNSUPPORTED_OPERATION ?
           STATUS_INVALID_SYSTEM_SERVICE : STATUS_NOT_SUPPORTED;
}

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
    BOOLEAN Reported; /* an unsupported request, already reported once */
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
/* NtPowerInformation records passed through unchanged */
C_ASSERT(sizeof(SYSTEM_BATTERY_STATE) == 32);
C_ASSERT(sizeof(SYSTEM_POWER_CAPABILITIES) == 76);
C_ASSERT(sizeof(PROCESSOR_POWER_INFORMATION) == 24);
C_ASSERT(sizeof(EXECUTION_STATE) == 4);
C_ASSERT(sizeof(WOW64_JOB_EXTENDED_LIMIT) == 112);

/* The 32-bit loader's PROCESS_TLS_INFORMATION (16-byte entries) as a native
 * request with PROCESS_TLS_WOW64, so the kernel changes the TEB32 vectors of
 * the threads. The request is checked as writable guest memory and read once;
 * results go back per entry. The loader holds its lock and owns the request,
 * so only the process unmapping it meanwhile can still lose the results (as
 * for the native caller, see PspSetProcessTlsInformation). */
typedef struct { ULONG Flags, NewTlsData, OldTlsData, ThreadId; } WOW64_THREAD_TLS_INFORMATION;
C_ASSERT(sizeof(WOW64_THREAD_TLS_INFORMATION) == 16);
C_ASSERT(FIELD_OFFSET(PROCESS_TLS_INFORMATION, ThreadData) == 16);

static VOID ProbeOutput(ULONG Address, ULONG Size);

/* Size bytes at a guest address lie below the guest's highest address */
static BOOLEAN GuestRange32(ULONG Address, ULONG64 Size)
{
    return (ULONG64)Address + Size <= (ULONG64)Wow64HighestUserAddress + 1;
}

static NTSTATUS SetProcessTls32(HANDLE Process, ULONG Address, ULONG Length, WOW64_CLEANUP *Cleanup)
{
    PROCESS_TLS_INFORMATION *Guest = PTR32(Address), *Native;
    WOW64_THREAD_TLS_INFORMATION *Entries = (PVOID)((PUCHAR)Guest + FIELD_OFFSET(PROCESS_TLS_INFORMATION, ThreadData));
    ULONG Flags, Count, i;
    SIZE_T Size;
    NTSTATUS Status;

    if (Length < FIELD_OFFSET(PROCESS_TLS_INFORMATION, ThreadData)) return STATUS_INFO_LENGTH_MISMATCH;
    if ((ULONG64)Address + Length > (ULONG64)Wow64HighestUserAddress + 1) return STATUS_INVALID_PARAMETER;
    Flags = Guest->Flags;
    Count = Guest->ThreadDataCount;
    /* Only this layer asks for the 32-bit vectors */
    if (Flags & PROCESS_TLS_WOW64) return STATUS_INVALID_PARAMETER;
    if (Count > (Length - FIELD_OFFSET(PROCESS_TLS_INFORMATION, ThreadData)) / sizeof(*Entries) ||
        Count > (MAXULONG - FIELD_OFFSET(PROCESS_TLS_INFORMATION, ThreadData)) / sizeof(THREAD_TLS_INFORMATION))
        return STATUS_INVALID_PARAMETER;
    /* The whole guest request must be guest memory and writable before any
     * thread's TLS changes: the results are written back afterwards */
    ProbeOutput(Address, FIELD_OFFSET(PROCESS_TLS_INFORMATION, ThreadData) + Count * sizeof(*Entries));
    Size = FIELD_OFFSET(PROCESS_TLS_INFORMATION, ThreadData) + (SIZE_T)Count * sizeof(THREAD_TLS_INFORMATION);
    Native = Cleanup->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (!Native) return STATUS_NO_MEMORY;
    Native->Flags = Flags | PROCESS_TLS_WOW64;
    Native->OperationType = Guest->OperationType;
    Native->ThreadDataCount = Count;
    Native->TlsIndex = Guest->TlsIndex;
    for (i = 0; i < Count; ++i)
        Native->ThreadData[i].NewTlsData = UlongToPtr(Entries[i].NewTlsData);
    Status = NtSetInformationProcess(Process, ProcessTlsInformation, Native, (ULONG)Size);
    for (i = 0; i < Count; ++i)
    {
        Entries[i].Flags = Native->ThreadData[i].Flags;
        if (Native->ThreadData[i].Flags & THREAD_TLS_INFORMATION_ASSIGNED)
        {
            Entries[i].OldTlsData = PtrToUlong(Native->ThreadData[i].OldTlsData);
            Entries[i].ThreadId = HandleToUlong(Native->ThreadData[i].ThreadId);
        }
    }
    Guest->ThreadDataCount = Native->ThreadDataCount;
    return Status;
}

static VOID ProbeOutput(ULONG Address, ULONG Size)
{
    volatile UCHAR *Bytes = PTR32(Address);
    ULONG i;
    for (i = 0; i < Size; ++i) Bytes[i] = Bytes[i];
}

/* Size bytes at a guest address lie below the guest's highest address */
static BOOLEAN GuestSpan(ULONG Address, ULONG64 Size)
{
    return (ULONG64)Address + Size <= (ULONG64)Wow64HighestUserAddress + 1;
}

/* MemorySectionName: the guest's counted-string header is 8 bytes. Query the native form into a
 * scratch buffer that holds any name, then apply the guest's buffer size as Windows WoW64 does
 * (measured on Server 2008 R2 and Windows 10): an address above the guest's range is invalid; a
 * buffer smaller than the header fails with STATUS_INFO_LENGTH_MISMATCH and one smaller than
 * header, name and terminator with STATUS_BUFFER_OVERFLOW, both with only the required length
 * stored; otherwise the header, the name and its terminator. The buffer need not be aligned. */
static NTSTATUS SectionName32(HANDLE Process, ULONG Address, ULONG Buffer, ULONG Length,
                              ULONG ReturnLength, WOW64_CLEANUP *Cleanup)
{
    /* The native MaximumLength is a USHORT: keep it even and below 64 KiB */
    const ULONG NativeLength = sizeof(MEMORY_SECTION_NAME) + MAXUSHORT - 1;
    MEMORY_SECTION_NAME *Native;
    WOW64_STRING UNALIGNED *Guest = PTR32(Buffer);
    SIZE_T Bytes = 0;
    ULONG Needed;
    NTSTATUS Status;

    if (Address > Wow64HighestUserAddress) return STATUS_INVALID_PARAMETER;
    if (ReturnLength)
    {
        if (!GuestSpan(ReturnLength, sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
        ProbeOutput(ReturnLength, sizeof(ULONG));
    }
    Native = Cleanup->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, NativeLength);
    if (!Native) return STATUS_NO_MEMORY;
    Status = NtQueryVirtualMemory(Process, PTR32(Address), MemorySectionName, Native, NativeLength, &Bytes);
    if (!NT_SUCCESS(Status)) return Status;
    Needed = sizeof(WOW64_STRING) + Native->SectionFileName.Length + sizeof(WCHAR);
    if (Length < Needed)
    {
        if (ReturnLength) *(ULONG UNALIGNED *)PTR32(ReturnLength) = Needed;
        return Length < sizeof(WOW64_STRING) ? STATUS_INFO_LENGTH_MISMATCH : STATUS_BUFFER_OVERFLOW;
    }
    /* Writable before anything is stored, so a fault leaves no partial header */
    if (!Buffer || !GuestSpan(Buffer, Needed)) return STATUS_ACCESS_VIOLATION;
    ProbeOutput(Buffer, Needed);
    Guest->Length = Native->SectionFileName.Length;
    Guest->MaximumLength = Native->SectionFileName.Length + sizeof(WCHAR);
    Guest->Buffer = Buffer + sizeof(WOW64_STRING);
    RtlCopyMemory((PUCHAR)Guest + sizeof(WOW64_STRING), Native->SectionFileName.Buffer, Native->SectionFileName.Length);
    *(WCHAR UNALIGNED *)((PUCHAR)Guest + Needed - sizeof(WCHAR)) = UNICODE_NULL;
    /* Last, as the native service does, in case it lies inside the buffer */
    if (ReturnLength) *(ULONG UNALIGNED *)PTR32(ReturnLength) = Needed;
    return STATUS_SUCCESS;
}

/* TOKEN_GROUPS classes: the 32-bit layout has 4-byte SID pointers, so query the
 * native form into a scratch buffer and rebuild it in the guest buffer. Measured on
 * Windows WOW64: the token is checked before the buffer, the buffer need not be
 * aligned, and the length is stored before the data. */
static NTSTATUS TokenGroups32(HANDLE Token, TOKEN_INFORMATION_CLASS Class, ULONG Buffer,
                              ULONG Length, ULONG ReturnLength, WOW64_CLEANUP *Cleanup)
{
    PTOKEN_GROUPS Native;
    ULONG Bytes = 0, Needed, SidBytes = 0, Offset, Sid, i;
    UCHAR *Guest;
    NTSTATUS Status;

    if (!ReturnLength || !GuestSpan(ReturnLength, sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    Status = NtQueryInformationToken(Token, Class, NULL, 0, &Bytes);
    if (Status != STATUS_BUFFER_TOO_SMALL) return Status;
    if (Bytes < sizeof(TOKEN_GROUPS)) Bytes = sizeof(TOKEN_GROUPS);
    Native = Cleanup->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Bytes);
    if (!Native) return STATUS_NO_MEMORY;
    Status = NtQueryInformationToken(Token, Class, Native, Bytes, &Bytes);
    if (!NT_SUCCESS(Status)) return Status;
    /* Bounded by the native result, which is far below 4 GiB */
    for (i = 0; i < Native->GroupCount; ++i)
        SidBytes += RtlLengthSid(Native->Groups[i].Sid);
    Needed = sizeof(ULONG) + Native->GroupCount * 2 * sizeof(ULONG) + SidBytes;
    *(ULONG *)PTR32(ReturnLength) = Needed;
    if (Length < Needed) return STATUS_BUFFER_TOO_SMALL;
    if (!Buffer || !GuestSpan(Buffer, Needed)) return STATUS_ACCESS_VIOLATION;
    /* Unaligned stores: the guest buffer may be misaligned */
    Guest = PTR32(Buffer);
    *(ULONG UNALIGNED *)Guest = Native->GroupCount;
    Offset = sizeof(ULONG) + Native->GroupCount * 2 * sizeof(ULONG);
    for (i = 0; i < Native->GroupCount; ++i)
    {
        Sid = RtlLengthSid(Native->Groups[i].Sid);
        *(ULONG UNALIGNED *)(Guest + 4 + 8 * i) = Buffer + Offset;
        *(ULONG UNALIGNED *)(Guest + 8 + 8 * i) = Native->Groups[i].Attributes;
        RtlCopyMemory(Guest + Offset, Native->Groups[i].Sid, Sid);
        Offset += Sid;
    }
    return STATUS_SUCCESS;
}

/* TokenDefaultDacl: TOKEN_DEFAULT_DACL is one ACL pointer followed by the ACL, which has
 * no pointers. Same order as TokenGroups32. */
static NTSTATUS TokenDefaultDacl32(HANDLE Token, ULONG Buffer, ULONG Length, ULONG ReturnLength,
                                   WOW64_CLEANUP *Cleanup)
{
    PTOKEN_DEFAULT_DACL Native;
    ULONG Bytes = 0, Needed, AclBytes;
    UCHAR *Guest;
    NTSTATUS Status;

    if (!ReturnLength || !GuestSpan(ReturnLength, sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    Status = NtQueryInformationToken(Token, TokenDefaultDacl, NULL, 0, &Bytes);
    if (Status != STATUS_BUFFER_TOO_SMALL) return Status;
    if (Bytes < sizeof(TOKEN_DEFAULT_DACL)) Bytes = sizeof(TOKEN_DEFAULT_DACL);
    Native = Cleanup->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Bytes);
    if (!Native) return STATUS_NO_MEMORY;
    Status = NtQueryInformationToken(Token, TokenDefaultDacl, Native, Bytes, &Bytes);
    if (Status == STATUS_BUFFER_TOO_SMALL && Bytes >= sizeof(TOKEN_DEFAULT_DACL))
    {
        /* The DACL grew meanwhile: report what the 32-bit form now needs */
        *(ULONG *)PTR32(ReturnLength) = Bytes - sizeof(TOKEN_DEFAULT_DACL) + sizeof(ULONG);
        return Status;
    }
    if (!NT_SUCCESS(Status)) return Status;
    AclBytes = Native->DefaultDacl ? Native->DefaultDacl->AclSize : 0;
    Needed = sizeof(ULONG) + AclBytes;
    *(ULONG *)PTR32(ReturnLength) = Needed;
    if (Length < Needed) return STATUS_BUFFER_TOO_SMALL;
    if (!Buffer || !GuestSpan(Buffer, Needed)) return STATUS_ACCESS_VIOLATION;
    Guest = PTR32(Buffer);
    *(ULONG UNALIGNED *)Guest = AclBytes ? Buffer + sizeof(ULONG) : 0;
    if (AclBytes) RtlCopyMemory(Guest + sizeof(ULONG), Native->DefaultDacl, AclBytes);
    return STATUS_SUCCESS;
}

/* A 32-bit TOKEN_GROUPS argument in the native layout. The SIDs stay in guest memory,
 * where the kernel captures them as caller data; Count was read and range-checked once. */
static PTOKEN_GROUPS GroupsFrom32(ULONG Guest, ULONG Count, PUCHAR *Next)
{
    ULONG *Source = PTR32(Guest), i;
    PTOKEN_GROUPS Groups;

    if (!Guest) return NULL;
    Groups = (PTOKEN_GROUPS)*Next;
    Groups->GroupCount = Count;
    for (i = 0; i < Count; ++i)
    {
        ULONG Sid = Source[1 + 2 * i];
        /* The SID must lie in the guest range too. The kernel reads it again; a count raised
         * meanwhile reaches at most 60 bytes further, into the reserved page past the limit. */
        if (Sid && (!GuestSpan(Sid, FIELD_OFFSET(SID, SubAuthority)) ||
                    !GuestSpan(Sid, FIELD_OFFSET(SID, SubAuthority) +
                               (ULONG64)((SID *)PTR32(Sid))->SubAuthorityCount * sizeof(ULONG))))
            return (PTOKEN_GROUPS)-1;
        Groups->Groups[i].Sid = PTR32(Sid);
        Groups->Groups[i].Attributes = Source[2 + 2 * i];
    }
    *Next += FIELD_OFFSET(TOKEN_GROUPS, Groups) + (SIZE_T)Count * sizeof(SID_AND_ATTRIBUTES);
    return Groups;
}

/* The element count of a guest group or privilege array, after checking that the
 * whole array lies in the guest's range */
static NTSTATUS GuestArrayCount(ULONG Address, ULONG ElementSize, PULONG Count)
{
    *Count = 0;
    if (!Address) return STATUS_SUCCESS;
    if (!GuestSpan(Address, sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    *Count = *(ULONG *)PTR32(Address);
    if (!GuestSpan(Address, sizeof(ULONG) + (ULONG64)*Count * ElementSize)) return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* NtFilterToken: SidsToDisable and RestrictedSids hold SID pointers; TOKEN_PRIVILEGES
 * has the same layout for both sizes and goes through as is. */
static NTSTATUS FilterToken32(ULONG *a, WOW64_CLEANUP *Cleanup)
{
    ULONG DisableCount, RestrictCount, PrivilegeCount;
    SIZE_T Bytes = 1;
    PUCHAR Next;
    PTOKEN_GROUPS Disable, Restrict;
    NTSTATUS Status;

    if (!a[5] || !GuestSpan(a[5], sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
    ProbeOutput(a[5], sizeof(ULONG));
    Status = GuestArrayCount(a[2], 2 * sizeof(ULONG), &DisableCount);
    if (!NT_SUCCESS(Status)) return Status;
    Status = GuestArrayCount(a[3], sizeof(LUID_AND_ATTRIBUTES), &PrivilegeCount);
    if (!NT_SUCCESS(Status)) return Status;
    Status = GuestArrayCount(a[4], 2 * sizeof(ULONG), &RestrictCount);
    if (!NT_SUCCESS(Status)) return Status;
    if (a[2]) Bytes += FIELD_OFFSET(TOKEN_GROUPS, Groups) + (SIZE_T)DisableCount * sizeof(SID_AND_ATTRIBUTES);
    if (a[4]) Bytes += FIELD_OFFSET(TOKEN_GROUPS, Groups) + (SIZE_T)RestrictCount * sizeof(SID_AND_ATTRIBUTES);
    Next = Cleanup->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, Bytes);
    if (!Next) return STATUS_NO_MEMORY;
    Disable = GroupsFrom32(a[2], DisableCount, &Next);
    if (Disable == (PTOKEN_GROUPS)-1) return STATUS_ACCESS_VIOLATION;
    Restrict = GroupsFrom32(a[4], RestrictCount, &Next);
    if (Restrict == (PTOKEN_GROUPS)-1) return STATUS_ACCESS_VIOLATION;
    Status = NtFilterToken(HANDLE32(a[0]), a[1], Disable, PTR32(a[3]), Restrict, &Cleanup->Handle);
    if (NT_SUCCESS(Status)) *(ULONG *)PTR32(a[5]) = HandleToUlong(Cleanup->Handle);
    Cleanup->Handle = NULL;
    return Status;
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
#define UNSUPPORTED(Kind, Value) return Wow64Unsupported(Number, WOW64_UNSUPPORTED_##Kind, Value, &Cleanup->Reported)
    if (Number == WOW64_SERVICE_CSR) return Wow64CsrDispatch(a[0], P(1), &Cleanup->Reported);
    if (Number == WOW64_SERVICE_CREATE_PROCESS) return Wow64CreateProcess(P(0), &Cleanup->Reported);
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
    case W64_NtQueryInformationThread:
        /* The native service does not fail with STATUS_NOT_SUPPORTED for the classes
         * Wow64QueryThread forwards, so this status means an untranslated class */
        Status = Wow64QueryThread(H(0), a[1], P(2), a[3], P(4));
        if (Status == STATUS_NOT_SUPPORTED) UNSUPPORTED(CLASS, a[1]);
        return Status;
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
        UNSUPPORTED(CLASS, a[1]);
    case W64_NtSetInformationObject:
        if (a[1] == ObjectHandleFlagInformation)
            return NtSetInformationObject(H(0), a[1], P(2), a[3]);
        UNSUPPORTED(CLASS, a[1]);
    case W64_NtRaiseException: return Wow64RaiseException(P(0), P(1), a[2]);
    case W64_NtRaiseHardError:
    {
        /* kernel32 reports an unhandled exception this way when no crash
         * reporter runs. Widen the ULONG_PTR array; string parameters are
         * UNICODE_STRING pointers. The service must still reject a bad
         * record in its own order (count, response options, response
         * pointer, then the parameters), with the status i386 returns:
         * a misaligned guest pointer is passed on as it is, and one that
         * cannot be read is replaced by the reserved, inaccessible page
         * above the guest address limit (never by the guest record read
         * at the native width). */
        ULONG_PTR Parameters[MAXIMUM_HARDERROR_PARAMETERS];
        UNICODE_STRING Strings[MAXIMUM_HARDERROR_PARAMETERS];
        ULONG_PTR Inaccessible = (ULONG_PTR)Wow64HighestUserAddress + 1;
        const ULONG *From = P(3);
        PVOID Native = P(3);
        ULONG i;
        if (a[1] > MAXIMUM_HARDERROR_PARAMETERS) return STATUS_INVALID_PARAMETER_2;
        if (From && !(a[3] & (sizeof(ULONG) - 1)))
        {
            _SEH2_TRY
            {
                for (i = 0; i < a[1]; ++i)
                {
                    Parameters[i] = From[i];
                    if (!(a[2] & (1 << i)) || !Parameters[i] || (Parameters[i] & (sizeof(ULONG) - 1)))
                        continue;
                    _SEH2_TRY
                    {
                        String32(&Strings[i], PTR32(Parameters[i]));
                        Parameters[i] = (ULONG_PTR)&Strings[i];
                    }
                    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
                    {
                        Parameters[i] = Inaccessible;
                    }
                    _SEH2_END;
                }
                Native = Parameters;
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Native = (PVOID)Inaccessible;
            }
            _SEH2_END;
        }
        return NtRaiseHardError(a[0], a[1], a[2], Native, a[4], P(5));
    }
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
        if (a[2] == MemorySectionName) return SectionName32(H(0), a[1], a[3], a[4], a[5], Cleanup);
        if (a[2] != MemoryBasicInformation) UNSUPPORTED(CLASS, a[2]);
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
        UNSUPPORTED(CLASS, a[1]);
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
        if (a[1] == JobObjectBasicUIRestrictions || a[1] == JobObjectAssociateCompletionPortInformation)
        {
            /* The arguments live on the guest stack: read each once */
            HANDLE Job = H(0);
            ULONG Class = a[1], Buffer = a[2], Length = a[3];
            if (Class == JobObjectBasicUIRestrictions)
            {
                ULONG Restrictions;
                if (Length != sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
                if (!GuestRange32(Buffer, sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
                Restrictions = *(ULONG *)PTR32(Buffer);
                return NtSetInformationJobObject(Job, Class, &Restrictions, sizeof(Restrictions));
            }
            else
            {
                /* { PVOID CompletionKey; HANDLE CompletionPort; } with 4-byte members. The key
                 * comes back zero-extended through NtRemoveIoCompletion, which truncates it. */
                JOBOBJECT_ASSOCIATE_COMPLETION_PORT Port;
                ULONG Guest[2];
                if (Length != sizeof(Guest)) return STATUS_INFO_LENGTH_MISMATCH;
                if (!GuestRange32(Buffer, sizeof(Guest))) return STATUS_ACCESS_VIOLATION;
                RtlCopyMemory(Guest, PTR32(Buffer), sizeof(Guest));
                Port.CompletionKey = PTR32(Guest[0]);
                Port.CompletionPort = HANDLE32(Guest[1]);
                return NtSetInformationJobObject(Job, Class, &Port, sizeof(Port));
            }
        }
        UNSUPPORTED(CLASS, a[1]);
    case W64_NtQueryInformationJobObject:
    {
        /* The arguments live on the guest stack: read each once */
        HANDLE Job = H(0);
        ULONG Class = a[1], Buffer = a[2], Length = a[3], ReturnLength = a[4];
        if (ReturnLength && !GuestRange32(ReturnLength, sizeof(ULONG))) return STATUS_ACCESS_VIOLATION;
        switch (Class)
        {
        case JobObjectBasicUIRestrictions:            /* one DWORD */
        case JobObjectBasicAccountingInformation:     /* LARGE_INTEGERs and ULONGs */
        case JobObjectBasicAndIoAccountingInformation:
            if (!GuestRange32(Buffer, Length)) return STATUS_ACCESS_VIOLATION;
            return NtQueryInformationJobObject(Job, Class, PTR32(Buffer), Length, PTR32(ReturnLength));
        case JobObjectBasicLimitInformation:
        case JobObjectExtendedLimitInformation:
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION From;
            WOW64_JOB_EXTENDED_LIMIT *To = PTR32(Buffer);
            BOOLEAN Extended = Class == JobObjectExtendedLimitInformation;
            ULONG Size = Extended ? sizeof(*To) : sizeof(To->Basic);
            if (Length != Size) return STATUS_INFO_LENGTH_MISMATCH;
            if (!GuestRange32(Buffer, Size)) return STATUS_ACCESS_VIOLATION;
            if (ReturnLength) ProbeOutput(ReturnLength, sizeof(ULONG));
            Status = NtQueryInformationJobObject(Job, Class, &From,
                         Extended ? sizeof(From) : sizeof(From.BasicLimitInformation), NULL);
            if (!NT_SUCCESS(Status)) return Status;
            ProbeOutput(Buffer, Size);
            To->Basic.PerProcessUserTimeLimit = From.BasicLimitInformation.PerProcessUserTimeLimit;
            To->Basic.PerJobUserTimeLimit = From.BasicLimitInformation.PerJobUserTimeLimit;
            To->Basic.LimitFlags = From.BasicLimitInformation.LimitFlags;
            To->Basic.MinimumWorkingSetSize = (ULONG)min(From.BasicLimitInformation.MinimumWorkingSetSize, (SIZE_T)MAXULONG);
            To->Basic.MaximumWorkingSetSize = (ULONG)min(From.BasicLimitInformation.MaximumWorkingSetSize, (SIZE_T)MAXULONG);
            To->Basic.ActiveProcessLimit = From.BasicLimitInformation.ActiveProcessLimit;
            To->Basic.Affinity = (ULONG)From.BasicLimitInformation.Affinity;
            To->Basic.PriorityClass = From.BasicLimitInformation.PriorityClass;
            To->Basic.SchedulingClass = From.BasicLimitInformation.SchedulingClass;
            if (Extended)
            {
                To->IoInfo = From.IoInfo;
                To->ProcessMemoryLimit = (ULONG)min(From.ProcessMemoryLimit, (SIZE_T)MAXULONG);
                To->JobMemoryLimit = (ULONG)min(From.JobMemoryLimit, (SIZE_T)MAXULONG);
                To->PeakProcessMemoryUsed = (ULONG)min(From.PeakProcessMemoryUsed, (SIZE_T)MAXULONG);
                To->PeakJobMemoryUsed = (ULONG)min(From.PeakJobMemoryUsed, (SIZE_T)MAXULONG);
            }
            if (ReturnLength) *(ULONG *)PTR32(ReturnLength) = Size;
            return STATUS_SUCCESS;
        }
        default: UNSUPPORTED(CLASS, Class);
        }
    }
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
        case IOCTL_AFD_EVENT_SELECT: /* AFD captures event handles and the in/out record. */
        case IOCTL_AFD_ENUM_NETWORK_EVENTS:
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
            UNSUPPORTED(IOCTL, a[5]);
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
        default: UNSUPPORTED(FSCTL, a[5]);
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
            UNSUPPORTED(MODE, 2);
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
        default: UNSUPPORTED(CLASS, a[4]);
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
        default: UNSUPPORTED(CLASS, a[4]);
        }
    case W64_NtQueryDirectoryFile:
    {
        FILE_MODE_INFORMATION Mode;
        if (a[1] || a[2] || a[3]) UNSUPPORTED(MODE, 1);
        Status = NtQueryInformationFile(H(0), &Io, &Mode, sizeof(Mode), FileModeInformation);
        if (!NT_SUCCESS(Status)) return Status;
        if (!(Mode.Mode & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)))
            UNSUPPORTED(MODE, 2);
        switch (a[7])
        {
        case FileDirectoryInformation: case FileFullDirectoryInformation:
        case FileBothDirectoryInformation: case FileNamesInformation:
        case FileIdBothDirectoryInformation: case FileIdFullDirectoryInformation:
            ProbeOutput(a[4], sizeof(WOW64_IOSB));
            Status = NtQueryDirectoryFile(H(0), NULL, NULL, NULL, &Io, P(5), a[6], a[7],
                                           a[8], String32(&Name, P(9)), a[10]);
            StoreIo(a[4], &Io); return Status;
        default: UNSUPPORTED(CLASS, a[7]);
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
        default: UNSUPPORTED(CLASS, a[4]);
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
    case W64_NtFilterToken:
        return FilterToken32(a, Cleanup);
    case W64_NtImpersonateAnonymousToken:
        return NtImpersonateAnonymousToken(H(0));
    case W64_NtQueryInformationToken:
        /* TOKEN_MANDATORY_LABEL has the layout of TOKEN_USER: a SID_AND_ATTRIBUTES and the SID */
        if (a[1] == TokenUser || a[1] == TokenOwner || a[1] == TokenPrimaryGroup || a[1] == TokenIntegrityLevel)
        {
            union { TOKEN_USER User; UCHAR Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]; } Info;
            ULONG Bytes, Prefix = (a[1] == TokenUser || a[1] == TokenIntegrityLevel) ? 2 * sizeof(ULONG) : sizeof(ULONG);
            PROBE(4);
            Status = NtQueryInformationToken(H(0), a[1], &Info, sizeof(Info), &Bytes);
            if (!NT_SUCCESS(Status)) return Status;
            Bytes = RtlLengthSid(Info.User.User.Sid);
            U(4) = Prefix + Bytes;
            if (a[3] < Prefix + Bytes) return STATUS_BUFFER_TOO_SMALL;
            ProbeOutput(a[2], Prefix + Bytes);
            ((ULONG *)P(2))[0] = a[2] + Prefix;
            if (Prefix == 2 * sizeof(ULONG)) ((ULONG *)P(2))[1] = Info.User.User.Attributes;
            RtlCopyMemory((BYTE *)P(2) + Prefix, Info.User.User.Sid, Bytes);
            return STATUS_SUCCESS;
        }
        switch (a[1])
        {
        case TokenType: case TokenImpersonationLevel: case TokenStatistics:
        case TokenPrivileges: case TokenSessionId: case TokenElevationType:
        case TokenElevation: case TokenIsAppContainer:
            return NtQueryInformationToken(H(0), a[1], P(2), a[3], P(4));
        case TokenGroups: case TokenRestrictedSids: case TokenLogonSid:
            return TokenGroups32(H(0), a[1], a[2], a[3], a[4], Cleanup);
        case TokenDefaultDacl:
            return TokenDefaultDacl32(H(0), a[2], a[3], a[4], Cleanup);
        default: UNSUPPORTED(CLASS, a[1]);
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
        if (a[1] == TokenIntegrityLevel)
        {
            /* TOKEN_MANDATORY_LABEL: a 4-byte SID pointer and the attributes; the kernel
             * captures the SID from guest memory */
            TOKEN_MANDATORY_LABEL Label;
            UCHAR Sid[SECURITY_MAX_SID_SIZE];
            ULONG Header[2], Buffer = a[2], Length = a[3], SidBytes;
            UCHAR Count;
            /* The arguments live on the guest stack and the label in guest memory: read once */
            if (Length < sizeof(Header)) return STATUS_INFO_LENGTH_MISMATCH;
            if (!GuestSpan(Buffer, sizeof(Header))) return STATUS_ACCESS_VIOLATION;
            RtlCopyMemory(Header, PTR32(Buffer), sizeof(Header));
            Label.Label.Attributes = Header[1];
            Label.Label.Sid = NULL;
            if (Header[0])
            {
                if (!GuestSpan(Header[0], FIELD_OFFSET(SID, SubAuthority))) return STATUS_ACCESS_VIOLATION;
                Count = ((SID *)PTR32(Header[0]))->SubAuthorityCount;
                if (Count > SID_MAX_SUB_AUTHORITIES) return STATUS_INVALID_PARAMETER;
                SidBytes = FIELD_OFFSET(SID, SubAuthority) + Count * sizeof(ULONG);
                if (!GuestSpan(Header[0], SidBytes)) return STATUS_ACCESS_VIOLATION;
                RtlCopyMemory(Sid, PTR32(Header[0]), SidBytes);
                ((SID *)Sid)->SubAuthorityCount = Count;
                Label.Label.Sid = Sid;
            }
            return NtSetInformationToken(H(0), TokenIntegrityLevel, &Label, sizeof(Label));
        }
        UNSUPPORTED(CLASS, a[1]);
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
        if (a[1] == ProcessImageFileName || a[1] == ProcessImageFileNameWin32)
        {
            /* A UNICODE_STRING followed by its characters: the guest header is
             * eight bytes, the native one sixteen, and the buffer pointer is
             * rewritten to the characters behind the guest header. */
            UNICODE_STRING *Native;
            WOW64_STRING *Guest = P(2);
            ULONG Needed = 0, Extra = sizeof(UNICODE_STRING) - sizeof(WOW64_STRING);
            if (a[3] > MAXULONG - Extra) return STATUS_INVALID_PARAMETER;
            if (a[4]) PROBE(4);
            Native = Cleanup->Buffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, a[3] + Extra);
            if (!Native) return STATUS_NO_MEMORY;
            Status = NtQueryInformationProcess(H(0), a[1], Native, a[3] + Extra, &Needed);
            if (a[4] && Needed) U(4) = Needed >= sizeof(UNICODE_STRING) ? Needed - Extra : Needed;
            if (NT_SUCCESS(Status))
            {
                Guest->Length = Native->Length;
                Guest->MaximumLength = Native->MaximumLength;
                Guest->Buffer = Native->Buffer ? PtrToUlong(Guest + 1) : 0;
                if (Native->Buffer)
                    RtlCopyMemory(Guest + 1, Native->Buffer, Native->MaximumLength);
            }
            return Status;
        }
        switch (a[1])
        {
        case ProcessDefaultHardErrorMode: case ProcessExecuteFlags: case ProcessPriorityClass:
        case ProcessSessionInformation: case ProcessBreakOnTermination: case ProcessCookie:
            return NtQueryInformationProcess(H(0), a[1], P(2), a[3], P(4));
        default: UNSUPPORTED(CLASS, a[1]);
        }
    case W64_NtSetInformationProcess:
        if (a[1] == ProcessTlsInformation) return SetProcessTls32(H(0), a[2], a[3], Cleanup);
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
        default: UNSUPPORTED(CLASS, a[1]);
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
        case ThreadImpersonationToken:
        {
            /* SetThreadToken and RevertToSelf: a token handle, or NULL */
            HANDLE Token;
            if (a[3] != sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
            Token = HANDLE32(U(2));
            return NtSetInformationThread(H(0), a[1], &Token, sizeof(Token));
        }
        case ThreadHideFromDebugger: case ThreadPriority: case ThreadBasePriority:
        case ThreadBreakOnTermination:
            return NtSetInformationThread(H(0), a[1], P(2), a[3]);
        default: UNSUPPORTED(CLASS, a[1]);
        }
    case W64_NtPowerInformation:
        switch (a[0])
        {
        case SystemBatteryState: case SystemPowerCapabilities:
        case ProcessorInformation: case SystemExecutionState:
            /* Fixed-size records of ULONG, BOOLEAN and enumeration fields, the same for
             * both sizes; the native service checks lengths and probes the buffers. They
             * must lie in the guest's range, which the native probe cannot tell. */
            if ((a[1] && !GuestSpan(a[1], a[2])) || (a[3] && !GuestSpan(a[3], a[4])))
                return STATUS_ACCESS_VIOLATION;
            return NtPowerInformation(a[0], P(1), a[2], P(3), a[4]);
        default: UNSUPPORTED(CLASS, a[0]);
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
        case SystemRangeStartInformation:
            /* The end of the guest's user range, as on Windows WoW64 (measured on Server 2008 R2
             * and Windows 10: 0x7fff0000 for an image that is not large address aware) */
            if (NativeSystemInformation) UNSUPPORTED(CLASS, a[0]);
            /* Windows WoW64 checks the length before the buffers. Both buffers must lie in the
             * guest's range and be writable before anything is stored. */
            if (a[2] != sizeof(ULONG)) return STATUS_INFO_LENGTH_MISMATCH;
            if (!GuestSpan(a[1], sizeof(ULONG)) || (a[3] && !GuestSpan(a[3], sizeof(ULONG))))
                return STATUS_ACCESS_VIOLATION;
            PROBE(1);
            if (a[3]) PROBE(3);
            U(1) = Wow64HighestUserAddress + 1;
            if (a[3]) U(3) = sizeof(ULONG);
            return STATUS_SUCCESS;
        case SystemCurrentTimeZoneInformation: /* RTL_TIME_ZONE_INFORMATION has no pointers. */
        case SystemTimeOfDayInformation:
            return NtQuerySystemInformation(a[0], P(1), a[2], P(3));
        default: UNSUPPORTED(CLASS, a[0]);
        }
    default:
        /* Name the information class of the class-based services as well */
        switch (Number)
        {
        case W64_NtSetSystemInformation: Length = a[0]; break;
        case W64_NtQueryInformationJobObject: case W64_NtQueryInformationPort:
        case W64_NtSetInformationKey: case W64_NtSetInformationDebugObject: Length = a[1]; break;
        case W64_NtSetVolumeInformationFile: Length = a[4]; break;
        default: Length = 0; break;
        }
        UNSUPPORTED(SERVICE, Length);
    }
}

NTSTATUS NTAPI Wow64SystemService(ULONG Number, ULONG *Arguments)
{
    NTSTATUS Status;
    WOW64_CLEANUP Cleanup = {0};
    WOW64_CPU_STATE *Cpu = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU];
    /* Assembly has finished every save, including EAX and FXSAVE. Keep the
     * flag set outside this routine: that also covers the low-address gate,
     * the instructions following CALL and the final IRET itself. */
    Cpu->ContextEaxChanged = FALSE;
    InterlockedExchange(&Cpu->InTransition, 0);
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
#if DBG
    if (!NT_SUCCESS(Status) && !Cleanup.Reported && Number != W64_NtQueryDebugFilterState &&
        !(Number >= 0x1000 && Number < 0x2000))
        DbgPrint("WoW64: service %lx returned %08lx\n", Number, Status);
#endif
    InterlockedExchange(&Cpu->InTransition, 1);
    if (!Cpu->ContextEaxChanged) Cpu->Eax = Status;
    return Status;
}
