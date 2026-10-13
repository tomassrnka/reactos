/* ReactOS WoW64 NT argument translation. SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64p.h"

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

static NTSTATUS Attributes32(OBJECT_ATTRIBUTES *To, UNICODE_STRING *Name, ULONG Address)
{
    const WOW64_OBJECT_ATTRIBUTES *From = PTR32(Address);
    RtlZeroMemory(To, sizeof(*To));
    if (!From) return STATUS_SUCCESS;
    if (From->Length != sizeof(*From)) return STATUS_INVALID_PARAMETER;
    InitializeObjectAttributes(To, String32(Name, PTR32(From->ObjectName)),
                               From->Attributes, HANDLE32(From->RootDirectory),
                               PTR32(From->SecurityDescriptor));
    To->SecurityQualityOfService = PTR32(From->SecurityQualityOfService);
    /* Self-relative descriptors have a fixed-width representation. */
    if (To->SecurityDescriptor &&
        !(((PISECURITY_DESCRIPTOR)To->SecurityDescriptor)->Control & SE_SELF_RELATIVE))
        return STATUS_NOT_SUPPORTED;
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
    HANDLE Process;
    PVOID Base;
    BOOLEAN Mapped;
} WOW64_CLEANUP;

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
    UNICODE_STRING Name;
    IO_STATUS_BLOCK Io = {{0}};
    PVOID Base;
    SIZE_T Size;
    ULONG Length;

#define H(i) HANDLE32(a[i])
#define P(i) PTR32(a[i])
#define U(i) (*(ULONG *)P(i))
#define ATTR(i) do { Status = Attributes32(&Attributes, &Name, a[i]); \
    if (!NT_SUCCESS(Status)) return Status; } while (0)
#define OA(i) (a[i] ? &Attributes : NULL)
#define PROBE(i) ProbeOutput(a[i], sizeof(ULONG))
#define RETURN_HANDLE_AT(i) do { \
    if (NT_SUCCESS(Status)) { U(i) = HandleToUlong(Cleanup->Handle); Cleanup->Handle = NULL; } \
    return Status; } while (0)
#define RETURN_HANDLE() RETURN_HANDLE_AT(0)
    if (Number == 0x10000) return Wow64CsrDispatch(a[0], P(1));
    switch (Number)
    {
    case W64_NtClose: return NtClose(H(0));
    case W64_NtRaiseException:
        /* Guest exception delivery is not implemented yet. Preserve the
         * exception status and fail closed instead of recursively raising
         * STATUS_INVALID_SYSTEM_SERVICE on the i386 stack. */
        DbgPrint("WoW64: guest exception %08lx at %08lx\n", U(0), ((ULONG *)P(0))[3]);
        return NtTerminateProcess(NtCurrentProcess(), U(0));
    case W64_NtTestAlert: return NtTestAlert();
    case W64_NtYieldExecution: return NtYieldExecution();
    case W64_NtTerminateProcess: return NtTerminateProcess(H(0), a[1]);
    case W64_NtTerminateThread: return NtTerminateThread(H(0), a[1]);
    case W64_NtQueryDebugFilterState: return NtQueryDebugFilterState(a[0], a[1]);
    case W64_NtQueryPerformanceCounter: return NtQueryPerformanceCounter(P(0), P(1));
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
        Status = NtAllocateVirtualMemory(H(0), &Base, a[2] ? a[2] : 0xFFFFFFFF,
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
        Status = NtMapViewOfSection(H(0), H(1), &Base, a[3] ? a[3] : 0xFFFFFFFF,
                                    a[4], P(5), &Size, a[7], a[8], a[9]);
        if (NT_SUCCESS(Status))
        {
            Cleanup->Process = H(1);
            Cleanup->Mapped = TRUE;
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
    case W64_NtOpenSymbolicLinkObject:
        PROBE(0);
        ATTR(2); Status = NtOpenSymbolicLinkObject(&Cleanup->Handle, a[1], OA(2)); RETURN_HANDLE();
    case W64_NtQuerySymbolicLinkObject:
        String32(&Name, P(1));
        Status = NtQuerySymbolicLinkObject(H(0), &Name, P(2));
        ((WOW64_STRING *)P(1))->Length = Name.Length;
        return Status;
    case W64_NtOpenKey:
        PROBE(0);
        ATTR(2); Status = NtOpenKey(&Cleanup->Handle, a[1], OA(2)); RETURN_HANDLE();
    case W64_NtCreateKey:
    {
        UNICODE_STRING Class;
        PROBE(0);
        ATTR(2);
        Status = NtCreateKey(&Cleanup->Handle, a[1], OA(2), a[3], String32(&Class, P(4)), a[5], P(6));
        RETURN_HANDLE();
    }
    case W64_NtQueryValueKey:
        return NtQueryValueKey(H(0), String32(&Name, P(1)), a[2], P(3), a[4], P(5));
    case W64_NtQueryKey: return NtQueryKey(H(0), a[1], P(2), a[3], P(4));
    case W64_NtEnumerateKey: return NtEnumerateKey(H(0), a[1], a[2], P(3), a[4], P(5));
    case W64_NtEnumerateValueKey: return NtEnumerateValueKey(H(0), a[1], a[2], P(3), a[4], P(5));
    case W64_NtSetValueKey:
        return NtSetValueKey(H(0), String32(&Name, P(1)), a[2], a[3], P(4), a[5]);
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
    case W64_NtCreateKeyedEvent:
        PROBE(0);
        ATTR(2); Status = NtCreateKeyedEvent(&Cleanup->Handle, a[1], OA(2), a[3]); RETURN_HANDLE();
    case W64_NtWaitForKeyedEvent: return NtWaitForKeyedEvent(H(0), P(1), a[2], P(3));
    case W64_NtReleaseKeyedEvent: return NtReleaseKeyedEvent(H(0), P(1), a[2], P(3));
    case W64_NtOpenFile:
        PROBE(0); ProbeOutput(a[3], sizeof(WOW64_IOSB));
        ATTR(2);
        Status = NtOpenFile(&Cleanup->Handle, a[1], OA(2), &Io, a[4], a[5]);
        StoreIo(a[3], &Io); RETURN_HANDLE();
    case W64_NtCreateFile:
        PROBE(0); ProbeOutput(a[3], sizeof(WOW64_IOSB));
        ATTR(2);
        Status = NtCreateFile(&Cleanup->Handle, a[1], OA(2), &Io, P(4), a[5], a[6], a[7], a[8], P(9), a[10]);
        StoreIo(a[3], &Io); RETURN_HANDLE();
    case W64_NtQueryAttributesFile:
        ATTR(0); return NtQueryAttributesFile(OA(0), P(1));
    case W64_NtQueryFullAttributesFile:
        ATTR(0); return NtQueryFullAttributesFile(OA(0), P(1));
    case W64_NtQueryInformationFile:
        /* Only fixed-width layouts used by the loader and console startup. */
        switch (a[4])
        {
        case FileBasicInformation: case FileStandardInformation: case FilePositionInformation:
        case FileNameInformation: case FileModeInformation: case FileInternalInformation:
        case FileAlignmentInformation: case FileNetworkOpenInformation:
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
        FILE_MODE_INFORMATION Mode;
        if (a[1] || a[2] || a[3]) return STATUS_NOT_SUPPORTED;
        Status = NtQueryInformationFile(H(0), &Io, &Mode, sizeof(Mode), FileModeInformation);
        if (!NT_SUCCESS(Status)) return Status;
        if (!(Mode.Mode & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)))
            return STATUS_NOT_SUPPORTED;
        ProbeOutput(a[4], sizeof(WOW64_IOSB));
        if (Number == W64_NtReadFile)
            Status = NtReadFile(H(0), NULL, NULL, NULL, &Io, P(5), a[6], P(7), P(8));
        else
            Status = NtWriteFile(H(0), NULL, NULL, NULL, &Io, P(5), a[6], P(7), P(8));
        StoreIo(a[4], &Io); return Status;
    }
    case W64_NtOpenProcessToken:
        PROBE(2);
        Status = NtOpenProcessToken(H(0), a[1], &Cleanup->Handle);
        RETURN_HANDLE_AT(2);
    case W64_NtOpenThreadToken:
        PROBE(3);
        Status = NtOpenThreadToken(H(0), a[1], a[2], &Cleanup->Handle);
        RETURN_HANDLE_AT(3);
    case W64_NtQueryInformationProcess:
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
        switch (a[1])
        {
        case ProcessDefaultHardErrorMode: case ProcessExecuteFlags: case ProcessPriorityClass:
            return NtSetInformationProcess(H(0), a[1], P(2), a[3]);
        default: return STATUS_NOT_SUPPORTED;
        }
    case W64_NtSetInformationThread:
        switch (a[1])
        {
        case ThreadHideFromDebugger: case ThreadPriority: case ThreadBasePriority:
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
                To->MaximumUserModeAddress = 0x7ffeffff;
                To->ActiveProcessorsAffinityMask = (ULONG)From.ActiveProcessorsAffinityMask;
                To->NumberOfProcessors = From.NumberOfProcessors;
                if (a[3]) U(3) = sizeof(*To);
            }
            return Status;
        }
        switch (a[0])
        {
        case SystemProcessorInformation: case SystemTimeOfDayInformation:
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
        Status = Dispatch(Number, Arguments, &Cleanup);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    if (Cleanup.Handle) NtClose(Cleanup.Handle);
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
    if (!NT_SUCCESS(Status)) DbgPrint("WoW64: service %lx returned %08lx\n", Number, Status);
    return Status;
}
