/* ReactOS WoW64 startup. SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64p.h"

NTSTATUS Wow64Allocate(SIZE_T Size, PVOID *Base)
{
    *Base = NULL;
    return NtAllocateVirtualMemory(NtCurrentProcess(), Base, 0xFFFFFFFF,
                                   &Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

NTSTATUS Wow64CopyString(WOW64_STRING *To, const UNICODE_STRING *From)
{
    PVOID Buffer;
    NTSTATUS Status;
    if (!From->MaximumLength) return STATUS_SUCCESS;
    Status = Wow64Allocate(From->MaximumLength, &Buffer);
    if (!NT_SUCCESS(Status)) return Status;
    RtlCopyMemory(Buffer, From->Buffer, From->Length);
    To->Buffer = PtrToUlong(Buffer);
    To->Length = From->Length;
    To->MaximumLength = From->MaximumLength;
    return STATUS_SUCCESS;
}

static NTSTATUS CopyDllPath(WOW64_STRING *To, const UNICODE_STRING *From)
{
    WCHAR SystemBuffer[MAX_PATH];
    UNICODE_STRING SystemPath, Component;
    ULONG Start, End, Count = From->Length / sizeof(WCHAR);
    NTSTATUS Status;
    PWSTR Buffer;

    /* System32 and SysWOW64 have equal lengths. Preserve every other search
     * component (including application, current and PATH directories) and
     * their order; do not redirect similarly named application directories. */
    RtlInitEmptyUnicodeString(&SystemPath, SystemBuffer, sizeof(SystemBuffer));
    Status = RtlAppendUnicodeToString(&SystemPath, SharedUserData->NtSystemRoot);
    if (!NT_SUCCESS(Status)) return Status;
    Status = RtlAppendUnicodeToString(&SystemPath, L"\\System32");
    if (!NT_SUCCESS(Status)) return Status;
    Status = Wow64CopyString(To, From);
    if (!NT_SUCCESS(Status)) return Status;
    Buffer = PTR32(To->Buffer);
    for (Start = 0; Start < Count; Start = End + 1)
    {
        for (End = Start; End < Count && Buffer[End] != L';'; ++End) { }
        Component.Buffer = Buffer + Start;
        Component.Length = (End - Start) * sizeof(WCHAR);
        if (Component.Length && Component.Buffer[Component.Length / sizeof(WCHAR) - 1] == L'\\')
            Component.Length -= sizeof(WCHAR);
        Component.MaximumLength = Component.Length;
        if (RtlEqualUnicodeString(&Component, &SystemPath, TRUE))
            RtlCopyMemory(Component.Buffer + Component.Length / sizeof(WCHAR) - 8,
                          L"SysWOW64", 8 * sizeof(WCHAR));
    }
    return STATUS_SUCCESS;
}

static NTSTATUS InitializeParameters(PEB32 *Peb32)
{
    PRTL_USER_PROCESS_PARAMETERS From = RtlNormalizeProcessParams(NtCurrentPeb()->ProcessParameters);
    WOW64_PARAMETERS *To;
    NTSTATUS Status;
    PVOID Environment, NativeEnvironment = NULL;
    SIZE_T Size;
    ULONG i;

    Status = Wow64Allocate(sizeof(*To), (PVOID *)&To);
    if (!NT_SUCCESS(Status)) return Status;
    To->MaximumLength = To->Length = sizeof(*To);
    To->Flags = From->Flags | RTL_USER_PROCESS_PARAMETERS_NORMALIZED;
    To->DebugFlags = From->DebugFlags;
#define COPY_HANDLE(Field) To->Field = HandleToUlong(From->Field)
    COPY_HANDLE(ConsoleHandle);
    COPY_HANDLE(StandardInput);
    COPY_HANDLE(StandardOutput);
    COPY_HANDLE(StandardError);
    COPY_HANDLE(CurrentDirectory.Handle);
#undef COPY_HANDLE
    To->ConsoleFlags = From->ConsoleFlags;
    RtlCopyMemory(&To->StartingX, &From->StartingX, 9 * sizeof(ULONG));
#define COPY_STRING(Field) \
    Status = Wow64CopyString(&To->Field, &From->Field); \
    if (!NT_SUCCESS(Status)) return Status
    COPY_STRING(CurrentDirectory.DosPath);
    COPY_STRING(ImagePathName);
    COPY_STRING(CommandLine);
    COPY_STRING(WindowTitle);
    COPY_STRING(DesktopInfo);
    COPY_STRING(ShellInfo);
    COPY_STRING(RuntimeData);
    for (i = 0; i < 32; ++i)
    {
        To->CurrentDirectories[i].Flags = From->CurrentDirectories[i].Flags;
        To->CurrentDirectories[i].Length = From->CurrentDirectories[i].Length;
        To->CurrentDirectories[i].TimeStamp = From->CurrentDirectories[i].TimeStamp;
        COPY_STRING(CurrentDirectories[i].DosPath);
    }
#undef COPY_STRING
    Status = CopyDllPath(&To->DllPath, &From->DllPath);
    if (!NT_SUCCESS(Status)) return Status;
    Status = Wow64CreateEnvironment(&NativeEnvironment);
    if (NT_SUCCESS(Status))
    {
        PWSTR End = NativeEnvironment;
        while (*End) End += wcslen(End) + 1;
        Size = (End + 1 - (PWSTR)NativeEnvironment) * sizeof(WCHAR);
        Status = Wow64Allocate(Size, &Environment);
        if (NT_SUCCESS(Status))
        {
            RtlCopyMemory(Environment, NativeEnvironment, Size);
            To->Environment = PtrToUlong(Environment);
            To->EnvironmentSize = Size;
        }
    }
    if (NativeEnvironment) RtlDestroyEnvironment(NativeEnvironment);
    if (!NT_SUCCESS(Status)) return Status;
    Peb32->ProcessParameters = PtrToUlong(To);
    return STATUS_SUCCESS;
}

static NTSTATUS InitializePeb(PEB32 *To)
{
    PPEB From = NtCurrentPeb();
    MEMORY_BASIC_INFORMATION Info;
    PVOID Nls;
    NTSTATUS Status;

    RtlZeroMemory(To, sizeof(*To));
#define COPY(Field) To->Field = From->Field
    COPY(InheritedAddressSpace); COPY(ReadImageFileExecOptions); COPY(BeingDebugged);
    COPY(OSMajorVersion); COPY(OSMinorVersion); COPY(OSBuildNumber); COPY(OSPlatformId);
    COPY(OSCSDVersion); COPY(NumberOfProcessors); COPY(NtGlobalFlag); COPY(SessionId);
    COPY(HeapSegmentReserve); COPY(HeapSegmentCommit); COPY(HeapDeCommitTotalFreeThreshold);
    COPY(HeapDeCommitFreeBlockThreshold); COPY(CriticalSectionTimeout); COPY(MinimumStackCommit);
    COPY(ImageSubsystem); COPY(ImageSubsystemMajorVersion); COPY(ImageSubsystemMinorVersion);
#undef COPY
    To->Mutant = HandleToUlong(From->Mutant);
    To->ImageBaseAddress = PtrToUlong(From->ImageBaseAddress);
    To->ImageProcessAffinityMask = (ULONG)From->ImageProcessAffinityMask;
    To->MaximumNumberOfHeaps = (PAGE_SIZE - sizeof(*To)) / sizeof(ULONG);
    To->ProcessHeaps = PtrToUlong(To + 1);
    Status = NtQueryVirtualMemory(NtCurrentProcess(), From->AnsiCodePageData,
                                  MemoryBasicInformation, &Info, sizeof(Info), NULL);
    if (!NT_SUCCESS(Status)) return Status;
    Status = Wow64Allocate(Info.RegionSize, &Nls);
    if (!NT_SUCCESS(Status)) return Status;
    RtlCopyMemory(Nls, Info.BaseAddress, Info.RegionSize);
#define COPY_NLS(Field) To->Field = PtrToUlong((PUCHAR)Nls + ((PUCHAR)From->Field - (PUCHAR)Info.BaseAddress))
    COPY_NLS(AnsiCodePageData); COPY_NLS(OemCodePageData); COPY_NLS(UnicodeCaseTableData);
#undef COPY_NLS
    return InitializeParameters(To);
}

static NTSTATUS MapNtdll(PVOID *Base)
{
    WCHAR Buffer[MAX_PATH];
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    HANDLE File, Section;
    IO_STATUS_BLOCK Io;
    SIZE_T Size = 0;
    NTSTATUS Status;

    RtlInitEmptyUnicodeString(&Name, Buffer, sizeof(Buffer));
    RtlAppendUnicodeToString(&Name, L"\\??\\");
    RtlAppendUnicodeToString(&Name, SharedUserData->NtSystemRoot);
    RtlAppendUnicodeToString(&Name, L"\\SysWOW64\\ntdll.dll");
    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&File, FILE_READ_DATA | FILE_EXECUTE | SYNCHRONIZE,
                        &Attributes, &Io, FILE_SHARE_READ | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
    if (!NT_SUCCESS(Status)) return Status;
    Status = NtCreateSection(&Section, SECTION_ALL_ACCESS, NULL, NULL,
                             PAGE_EXECUTE, SEC_IMAGE, File);
    NtClose(File);
    if (!NT_SUCCESS(Status)) return Status;
    *Base = NULL;
    Status = NtMapViewOfSection(Section, NtCurrentProcess(), Base, 0xFFFFFFFF,
                                0, NULL, &Size, ViewUnmap, 0, PAGE_READONLY);
    NtClose(Section);
    if (NT_SUCCESS(Status))
    {
        PIMAGE_NT_HEADERS32 Header = RtlImageNtHeader(*Base);
        if (Header->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            Header->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            return STATUS_INVALID_IMAGE_FORMAT;
        if ((ULONG_PTR)*Base != Header->OptionalHeader.ImageBase)
            return STATUS_CONFLICTING_ADDRESSES;
    }
    return Status;
}

DECLSPEC_NORETURN VOID NTAPI Wow64LdrpInitialize(PCONTEXT Context)
{
    PPEB Peb = NtCurrentPeb();
    PTEB Teb = NtCurrentTeb();
    PEB32 *Peb32 = WOW64_PEB32(Peb);
    TEB32 *Teb32 = WOW64_TEB32(Teb);
    WOW64_CPU_STATE State = {0};
    PVOID Ntdll32, Stack, Entry, Gate;
    ANSI_STRING Name = RTL_CONSTANT_STRING("LdrInitializeWow64");
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Context);

    /* Thread creation and APC delivery need their own context conversion. */
    if (Peb32->Ldr) { Status = STATUS_NOT_SUPPORTED; goto Failed; }
    Status = Wow64InitializeRedirection();
    if (!NT_SUCCESS(Status)) goto Failed;
    Status = InitializePeb(Peb32);
    if (!NT_SUCCESS(Status)) goto Failed;
    Status = MapNtdll(&Ntdll32);
    if (!NT_SUCCESS(Status)) goto Failed;
    /* Export-directory entries have the same RVA representation for PE32. */
    {
        ULONG Bytes, i;
        PIMAGE_EXPORT_DIRECTORY Exports = RtlImageDirectoryEntryToData(Ntdll32, TRUE,
                                             IMAGE_DIRECTORY_ENTRY_EXPORT, &Bytes);
        ULONG *Names, *Functions;
        USHORT *Ordinals;
        Entry = NULL;
        if (!Exports) { Status = STATUS_ENTRYPOINT_NOT_FOUND; goto Failed; }
        Names = (PVOID)((PUCHAR)Ntdll32 + Exports->AddressOfNames);
        Functions = (PVOID)((PUCHAR)Ntdll32 + Exports->AddressOfFunctions);
        Ordinals = (PVOID)((PUCHAR)Ntdll32 + Exports->AddressOfNameOrdinals);
        for (i = 0; i < Exports->NumberOfNames; ++i)
        {
            if (!strcmp((PCHAR)Ntdll32 + Names[i], "KiUserExceptionDispatcher") &&
                Ordinals[i] < Exports->NumberOfFunctions)
                Wow64ExceptionDispatcher = (PUCHAR)Ntdll32 + Functions[Ordinals[i]];
            if (!strcmp((PCHAR)Ntdll32 + Names[i], Name.Buffer) && Ordinals[i] < Exports->NumberOfFunctions)
            {
                Entry = (PUCHAR)Ntdll32 + Functions[Ordinals[i]];
            }
        }
        if (!Entry || !Wow64ExceptionDispatcher) { Status = STATUS_ENTRYPOINT_NOT_FOUND; goto Failed; }
    }
    Status = Wow64CpuInitialize(Wow64SystemService, &Gate);
    if (!NT_SUCCESS(Status)) goto Failed;
    Status = Wow64Allocate(1024 * 1024, &Stack);
    if (!NT_SUCCESS(Status)) goto Failed;
    RtlZeroMemory(Teb32, sizeof(*Teb32));
    Teb32->NtTib.ExceptionList = MAXULONG;
    Teb32->NtTib.Self = PtrToUlong(Teb32);
    Teb32->NtTib.StackBase = PtrToUlong((PUCHAR)Stack + 1024 * 1024);
    Teb32->NtTib.StackLimit = PtrToUlong(Stack);
    Teb32->DeallocationStack = PtrToUlong(Stack);
    Teb32->ProcessEnvironmentBlock = PtrToUlong(Peb32);
    Teb32->ClientId.UniqueProcess = HandleToUlong(Teb->ClientId.UniqueProcess);
    Teb32->ClientId.UniqueThread = HandleToUlong(Teb->ClientId.UniqueThread);
    Teb32->RealClientId = Teb32->ClientId;
    Teb32->CurrentLocale = Teb->CurrentLocale;
    Teb32->WOW32Reserved = PtrToUlong(Gate);
    Teb32->StaticUnicodeString.Buffer = PtrToUlong(Teb32->StaticUnicodeBuffer);
    Teb32->StaticUnicodeString.MaximumLength = sizeof(Teb32->StaticUnicodeBuffer);
    State.Esp = Teb32->NtTib.StackBase - 8;
    ((ULONG *)(ULONG_PTR)State.Esp)[0] = 0;
    ((ULONG *)(ULONG_PTR)State.Esp)[1] = PtrToUlong(Ntdll32);
    State.Eip = PtrToUlong(Entry);
    State.EFlags = 0x202;
#if DBG
    DbgPrint("WoW64: entering i386 ntdll at %lx, PEB32 %p TEB32 %p\n", State.Eip, Peb32, Teb32);
#endif
    Wow64CpuRun(&State);
Failed:
    DbgPrint("WoW64: initialization failed: %08lx\n", Status);
    NtTerminateProcess(NtCurrentProcess(), Status);
    for (;;) NtTerminateThread(NtCurrentThread(), Status);
}
