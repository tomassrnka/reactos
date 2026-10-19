/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Starting an EXE that cannot be mapped at its preferred base
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ndk/psfuncs.h>
#include <ndk/obfuncs.h>

#if defined(_M_AMD64) || defined(_M_IX86)

#include <pshpack1.h>
typedef struct _RELOC_EXE
{
    IMAGE_DOS_HEADER Dos;
    IMAGE_NT_HEADERS Nt;
    IMAGE_SECTION_HEADER Text;
} RELOC_EXE;
#include <poppack.h>

#define EXE_FILE_ALIGN  0x200
#define EXE_SECT_ALIGN  0x1000
#define TEXT_RVA        EXE_SECT_ALIGN

/* Layout of the only section */
#define CODE_OFF        0x00
#define DATA_OFF        0x40
#define IMPORT_OFF      0x80
#define ILT_OFF         0xB0
#define IAT_OFF         0xC0
#define HINTNAME_OFF    0xD0
#define DLLNAME_OFF     0xF0
#define RELOC_OFF       0x100

#define ENTRY_RVA       (TEXT_RVA + CODE_OFF)
#define EXIT_VALUE      0x42

/* WriteExe flags */
#define EXE_RELOCS          0x1
#define EXE_DYNAMIC_BASE    0x2
#define EXE_NATIVE          0x4
/* The entry point returns EXIT_VALUE without any absolute address (Win32 only) */
#define EXE_NO_ABSOLUTE     0x8

/* KUSER_SHARED_DATA lives here on every NT version, so no image can */
#define SHARED_DATA_BASE    0x7FFE0000

static WCHAR TempDir[MAX_PATH];
static WCHAR ExePath[MAX_PATH + 64];
static ULONG ExeCount;

/* A new name for every image: a cached image section must not be reused */
static BOOL NextExePath(void)
{
    HRESULT hr;

    hr = StringCchPrintfW(ExePath, _countof(ExePath), L"%srelocexe_%lu_%lu.exe",
                          TempDir, GetCurrentProcessId(), ++ExeCount);
    ok(hr == S_OK, "StringCchPrintfW failed: 0x%lx\n", hr);
    return hr == S_OK;
}

static void DeleteExes(void)
{
    ULONG i;

    for (i = 1; i <= ExeCount; i++)
    {
        if (FAILED(StringCchPrintfW(ExePath, _countof(ExePath), L"%srelocexe_%lu_%lu.exe",
                                    TempDir, GetCurrentProcessId(), i)))
        {
            continue;
        }
        if (!DeleteFileW(ExePath))
            trace("Cannot delete %S: %lu\n", ExePath, GetLastError());
    }
}

/* Unless EXE_NO_ABSOLUTE, the entry point reads EXIT_VALUE through an absolute
 * address, which only works when the base relocations were applied, and passes
 * it to ExitProcess (EXE_NATIVE: NtTerminateProcess), so the exit code tells
 * whether it ran with its relocations applied */
static BOOL WriteExe(PCWSTR Path, ULONG_PTR ImageBase, ULONG Flags)
{
    BYTE Image[2 * EXE_FILE_ALIGN] = { 0 };
    RELOC_EXE *Exe = (RELOC_EXE *)Image;
    PBYTE Text = Image + EXE_FILE_ALIGN;
    ULONG_PTR DataVa = ImageBase + TEXT_RVA + DATA_OFF;
    PIMAGE_IMPORT_DESCRIPTOR Import = (PIMAGE_IMPORT_DESCRIPTOR)(Text + IMPORT_OFF);
    PIMAGE_BASE_RELOCATION Reloc = (PIMAGE_BASE_RELOCATION)(Text + RELOC_OFF);
    PUSHORT TypeOffset = (PUSHORT)(Reloc + 1);
    ULONG_PTR Thunk = TEXT_RVA + HINTNAME_OFF;
    BOOL Native = !!(Flags & EXE_NATIVE);
    PCSTR DllName = Native ? "ntdll.dll" : "kernel32.dll";
    PCSTR FunctionName = Native ? "NtTerminateProcess" : "ExitProcess";
    ULONG Offset = 0, Count = 0;
    HANDLE hFile;
    DWORD Written;
    BOOL Ret;

    C_ASSERT(sizeof(RELOC_EXE) <= EXE_FILE_ALIGN);

    if (Flags & EXE_NO_ABSOLUTE)
    {
        /* mov eax, EXIT_VALUE; ret (kernel32 exits with the return value) */
        Text[Offset++] = 0xB8;
        *(PULONG)(Text + Offset) = EXIT_VALUE;
        Offset += sizeof(ULONG);
        Text[Offset++] = 0xC3;
    }
    else
    {
#if defined(_M_AMD64)
        /* sub rsp, 28h; mov rax, DataVa */
        static const BYTE Prologue[] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0xB8 };
        LONG Rel;

        memcpy(Text, Prologue, sizeof(Prologue));
        Offset = sizeof(Prologue);
        memcpy(Text + Offset, &DataVa, sizeof(DataVa));
        TypeOffset[Count++] = (IMAGE_REL_BASED_DIR64 << 12) | Offset;
        Offset += sizeof(DataVa);
        if (Native)
        {
            /* mov edx, [rax]; mov rcx, -1 */
            static const BYTE Args[] = { 0x8B, 0x10, 0x48, 0xC7, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF };
            memcpy(Text + Offset, Args, sizeof(Args));
            Offset += sizeof(Args);
        }
        else
        {
            /* mov ecx, [rax] */
            Text[Offset++] = 0x8B;
            Text[Offset++] = 0x08;
        }
        /* call [rip + IAT]; int 3 */
        Text[Offset++] = 0xFF;
        Text[Offset++] = 0x15;
        Rel = (LONG)(IAT_OFF - (Offset + sizeof(Rel)));
        memcpy(Text + Offset, &Rel, sizeof(Rel));
        Offset += sizeof(Rel);
        Text[Offset++] = 0xCC;
        /* Pad the block to a multiple of 4 bytes */
        TypeOffset[Count++] = IMAGE_REL_BASED_ABSOLUTE;
#else
        ULONG IatVa = (ULONG)ImageBase + TEXT_RVA + IAT_OFF;

        /* mov eax, [DataVa]; push eax */
        Text[Offset++] = 0xA1;
        memcpy(Text + Offset, &DataVa, sizeof(DataVa));
        TypeOffset[Count++] = (IMAGE_REL_BASED_HIGHLOW << 12) | Offset;
        Offset += sizeof(DataVa);
        Text[Offset++] = 0x50;
        if (Native)
        {
            /* push -1 */
            Text[Offset++] = 0x6A;
            Text[Offset++] = 0xFF;
        }
        /* call [IatVa]; int 3 */
        Text[Offset++] = 0xFF;
        Text[Offset++] = 0x15;
        memcpy(Text + Offset, &IatVa, sizeof(IatVa));
        TypeOffset[Count++] = (IMAGE_REL_BASED_HIGHLOW << 12) | Offset;
        Offset += sizeof(IatVa);
        Text[Offset++] = 0xCC;
#endif
    }
    ok(Offset <= DATA_OFF, "Code too large: %lu\n", Offset);
    *(PULONG)(Text + DATA_OFF) = EXIT_VALUE;

    Import->OriginalFirstThunk = TEXT_RVA + ILT_OFF;
    Import->FirstThunk = TEXT_RVA + IAT_OFF;
    Import->Name = TEXT_RVA + DLLNAME_OFF;
    memcpy(Text + ILT_OFF, &Thunk, sizeof(Thunk));
    memcpy(Text + IAT_OFF, &Thunk, sizeof(Thunk));
    strcpy((PCHAR)Text + HINTNAME_OFF + sizeof(WORD), FunctionName);
    strcpy((PCHAR)Text + DLLNAME_OFF, DllName);

    Exe->Dos.e_magic = IMAGE_DOS_SIGNATURE;
    Exe->Dos.e_lfanew = FIELD_OFFSET(RELOC_EXE, Nt);
    Exe->Nt.Signature = IMAGE_NT_SIGNATURE;
    Exe->Nt.FileHeader.Machine = IMAGE_FILE_MACHINE_NATIVE;
    Exe->Nt.FileHeader.NumberOfSections = 1;
    Exe->Nt.FileHeader.SizeOfOptionalHeader = sizeof(Exe->Nt.OptionalHeader);
    Exe->Nt.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE;
#ifdef _WIN64
    Exe->Nt.FileHeader.Characteristics |= IMAGE_FILE_LARGE_ADDRESS_AWARE;
#else
    Exe->Nt.FileHeader.Characteristics |= IMAGE_FILE_32BIT_MACHINE;
#endif
    if (!(Flags & EXE_RELOCS))
        Exe->Nt.FileHeader.Characteristics |= IMAGE_FILE_RELOCS_STRIPPED;
    Exe->Nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR_MAGIC;
    Exe->Nt.OptionalHeader.SizeOfCode = EXE_FILE_ALIGN;
    Exe->Nt.OptionalHeader.AddressOfEntryPoint = ENTRY_RVA;
    Exe->Nt.OptionalHeader.BaseOfCode = TEXT_RVA;
    Exe->Nt.OptionalHeader.ImageBase = ImageBase;
    Exe->Nt.OptionalHeader.SectionAlignment = EXE_SECT_ALIGN;
    Exe->Nt.OptionalHeader.FileAlignment = EXE_FILE_ALIGN;
    Exe->Nt.OptionalHeader.MajorOperatingSystemVersion = 5;
    Exe->Nt.OptionalHeader.MajorSubsystemVersion = 5;
    Exe->Nt.OptionalHeader.MinorSubsystemVersion = 2;
    Exe->Nt.OptionalHeader.SizeOfImage = 2 * EXE_SECT_ALIGN;
    Exe->Nt.OptionalHeader.SizeOfHeaders = EXE_FILE_ALIGN;
    Exe->Nt.OptionalHeader.Subsystem = Native ? IMAGE_SUBSYSTEM_NATIVE : IMAGE_SUBSYSTEM_WINDOWS_CUI;
    Exe->Nt.OptionalHeader.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
    if (Flags & EXE_DYNAMIC_BASE)
        Exe->Nt.OptionalHeader.DllCharacteristics |= IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;
    Exe->Nt.OptionalHeader.SizeOfStackReserve = 0x100000;
    Exe->Nt.OptionalHeader.SizeOfStackCommit = 0x1000;
    Exe->Nt.OptionalHeader.SizeOfHeapReserve = 0x100000;
    Exe->Nt.OptionalHeader.SizeOfHeapCommit = 0x1000;
    Exe->Nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    Exe->Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = TEXT_RVA + IMPORT_OFF;
    Exe->Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = 2 * sizeof(*Import);
    Exe->Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress = TEXT_RVA + IAT_OFF;
    Exe->Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size = 2 * sizeof(ULONG_PTR);
    if ((Flags & EXE_RELOCS) && Count)
    {
        Reloc->VirtualAddress = TEXT_RVA;
        Reloc->SizeOfBlock = sizeof(*Reloc) + Count * sizeof(USHORT);
        Exe->Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress = TEXT_RVA + RELOC_OFF;
        Exe->Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = Reloc->SizeOfBlock;
    }

    memcpy(Exe->Text.Name, ".text", 5);
    Exe->Text.Misc.VirtualSize = EXE_FILE_ALIGN;
    Exe->Text.VirtualAddress = TEXT_RVA;
    Exe->Text.SizeOfRawData = EXE_FILE_ALIGN;
    Exe->Text.PointerToRawData = EXE_FILE_ALIGN;
    Exe->Text.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    Ret = WriteFile(hFile, Image, sizeof(Image), &Written, NULL) && Written == sizeof(Image);
    CloseHandle(hFile);
    return Ret;
}

static BOOL PrepareExe(ULONG_PTR ImageBase, ULONG Flags)
{
    if (!NextExePath())
        return FALSE;
    if (!WriteExe(ExePath, ImageBase, Flags))
    {
        skip("Cannot write %S: %lu\n", ExePath, GetLastError());
        return FALSE;
    }
    return TRUE;
}

static PVOID GetRemoteImageBase(HANDLE hProcess)
{
    PROCESS_BASIC_INFORMATION Info;
    PVOID ImageBase = NULL;
    NTSTATUS Status;

    Status = NtQueryInformationProcess(hProcess, ProcessBasicInformation, &Info, sizeof(Info), NULL);
    ok(NT_SUCCESS(Status), "NtQueryInformationProcess failed: 0x%lx\n", Status);
    if (NT_SUCCESS(Status) &&
        !ReadProcessMemory(hProcess, &Info.PebBaseAddress->ImageBaseAddress, &ImageBase, sizeof(ImageBase), NULL))
    {
        ok(0, "ReadProcessMemory failed: %lu\n", GetLastError());
    }
    return ImageBase;
}

static DWORD WaitExit(HANDLE hProcess)
{
    DWORD ExitCode = 0xdeadbeef;

    if (WaitForSingleObject(hProcess, 20000) != WAIT_OBJECT_0)
    {
        ok(0, "The process did not exit\n");
        TerminateProcess(hProcess, 0xdead);
        WaitForSingleObject(hProcess, 5000);
    }
    ok(GetExitCodeProcess(hProcess, &ExitCode), "GetExitCodeProcess failed: %lu\n", GetLastError());
    return ExitCode;
}

/* MustMove: the preferred range is unusable on every Windows version, so the
 * image cannot be at its base, whether or not the system randomizes it */
static void Test_CreateProcess(ULONG_PTR ImageBase, ULONG Flags, BOOL MustMove)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    PVOID ActualBase;
    DWORD ExitCode;

    if (!PrepareExe(ImageBase, Flags))
        return;

    if (!CreateProcessW(ExePath, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
    {
        ok(0, "Base %p: CreateProcessW failed: %lu\n", (PVOID)ImageBase, GetLastError());
        return;
    }

    ActualBase = GetRemoteImageBase(pi.hProcess);
    ok(ActualBase != NULL, "Base %p: no image base in the PEB\n", (PVOID)ImageBase);
    if (MustMove)
        ok(ActualBase != (PVOID)ImageBase, "Base %p: the image is at its preferred base\n", (PVOID)ImageBase);

    ResumeThread(pi.hThread);
    ExitCode = WaitExit(pi.hProcess);
    if (Flags & EXE_RELOCS)
    {
        ok(ExitCode == EXIT_VALUE, "Base %p, relocated to %p: exit code 0x%lx\n", (PVOID)ImageBase, ActualBase, ExitCode);
    }
    else
    {
        /* The entry point would exit with EXIT_VALUE from anywhere */
        ok(ExitCode != EXIT_VALUE, "Base %p, no relocations, mapped at %p: the EXE ran\n", (PVOID)ImageBase, ActualBase);
        /* ReactOS: the loader raises STATUS_CONFLICTING_ADDRESSES, but CSRSS then
         * terminates a Win32 process with an unhandled exception with STATUS_ABANDONED */
        todo_if(is_reactos()) ok(ExitCode == (DWORD)STATUS_CONFLICTING_ADDRESSES,
                               "Base %p, no relocations, mapped at %p: exit code 0x%lx\n", (PVOID)ImageBase, ActualBase, ExitCode);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

/* What a debugger is told about the relocated EXE */
static void Test_Debugged(ULONG_PTR ImageBase)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DEBUG_EVENT Event;
    PVOID BaseOfImage = NULL, StartAddress = NULL, ActualBase = NULL;
    BOOL SeenBreakpoint = FALSE, Exited = FALSE;
    DWORD ExitCode = 0xdeadbeef;
    ULONG Timeouts = 0;

    if (!PrepareExe(ImageBase, EXE_RELOCS))
        return;

    if (!CreateProcessW(ExePath, NULL, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi))
    {
        ok(0, "CreateProcessW failed: %lu\n", GetLastError());
        return;
    }

    /* After a timeout, terminate the child and keep continuing its events
     * until it exits; after a second one, detach */
    while (!Exited)
    {
        DWORD Continue = DBG_CONTINUE;

        if (!WaitForDebugEvent(&Event, 20000))
        {
            ok(0, "Debugged: no debug event: %lu\n", GetLastError());
            if (++Timeouts > 1)
            {
                DebugActiveProcessStop(pi.dwProcessId);
                break;
            }
            TerminateProcess(pi.hProcess, 0xdead);
            continue;
        }

        switch (Event.dwDebugEventCode)
        {
            case CREATE_PROCESS_DEBUG_EVENT:
                BaseOfImage = Event.u.CreateProcessInfo.lpBaseOfImage;
                StartAddress = (PVOID)Event.u.CreateProcessInfo.lpStartAddress;
                ActualBase = GetRemoteImageBase(pi.hProcess);
                if (Event.u.CreateProcessInfo.hFile)
                    CloseHandle(Event.u.CreateProcessInfo.hFile);
                break;
            case LOAD_DLL_DEBUG_EVENT:
                if (Event.u.LoadDll.hFile)
                    CloseHandle(Event.u.LoadDll.hFile);
                break;
            case EXCEPTION_DEBUG_EVENT:
                if (!SeenBreakpoint && Event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT)
                    SeenBreakpoint = TRUE;
                else
                    Continue = DBG_EXCEPTION_NOT_HANDLED;
                break;
            case EXIT_PROCESS_DEBUG_EVENT:
                ExitCode = Event.u.ExitProcess.dwExitCode;
                Exited = TRUE;
                break;
        }
        ok(ContinueDebugEvent(Event.dwProcessId, Event.dwThreadId, Continue),
           "ContinueDebugEvent failed: %lu\n", GetLastError());
    }

    ok(ActualBase != NULL && ActualBase != (PVOID)ImageBase, "Debugged: image at %p\n", ActualBase);
    ok(BaseOfImage == ActualBase, "Debugged: image base %p reported, image at %p\n", BaseOfImage, ActualBase);
    /* Windows reports the entry point at the preferred base */
    ok(StartAddress == (PVOID)(ImageBase + ENTRY_RVA), "Debugged: start address %p, image at %p\n",
       StartAddress, BaseOfImage);
    ok(ExitCode == EXIT_VALUE, "Debugged: exit code 0x%lx\n", ExitCode);

    ok(WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0, "Debugged: the process is still running\n");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

static void Test_NativeProcess(ULONG_PTR ImageBase, BOOL MustMove)
{
    UNICODE_STRING NtPath;
    PRTL_USER_PROCESS_PARAMETERS Params;
    RTL_USER_PROCESS_INFORMATION Info;
    NTSTATUS Status;
    DWORD ExitCode;

    if (!PrepareExe(ImageBase, EXE_RELOCS | EXE_NATIVE))
        return;
    if (!RtlDosPathNameToNtPathName_U(ExePath, &NtPath, NULL, NULL))
    {
        skip("Cannot convert %S\n", ExePath);
        return;
    }

    Status = RtlCreateProcessParameters(&Params, &NtPath, NULL, NULL, &NtPath, NULL, NULL, NULL, NULL, NULL);
    ok(NT_SUCCESS(Status), "RtlCreateProcessParameters failed: 0x%lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        Status = RtlCreateUserProcess(&NtPath, OBJ_CASE_INSENSITIVE, Params, NULL, NULL, NULL, FALSE,
                                      NULL, NULL, &Info);
        ok(NT_SUCCESS(Status), "RtlCreateUserProcess failed: 0x%lx\n", Status);
        if (NT_SUCCESS(Status))
        {
            PVOID ActualBase = GetRemoteImageBase(Info.ProcessHandle);

            if (MustMove)
                ok(ActualBase != (PVOID)ImageBase, "Native: the image is at its preferred base %p\n", ActualBase);
            NtResumeThread(Info.ThreadHandle, NULL);
            ExitCode = WaitExit(Info.ProcessHandle);
            ok(ExitCode == EXIT_VALUE, "Native, base %p, mapped at %p: exit code 0x%lx\n",
               (PVOID)ImageBase, ActualBase, ExitCode);
            NtClose(Info.ThreadHandle);
            NtClose(Info.ProcessHandle);
        }
        RtlDestroyProcessParameters(Params);
    }
    RtlFreeUnicodeString(&NtPath);
}

START_TEST(ForcedRelocation)
{
    DWORD Length;
    UINT OldMode;

    Length = GetTempPathW(_countof(TempDir), TempDir);
    if (Length == 0 || Length >= _countof(TempDir))
    {
        skip("GetTempPathW failed: %lu, %lu\n", Length, GetLastError());
        return;
    }

    /* Children inherit this, so a failing start shows no message box */
    OldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    /* An ordinary base: the controls */
    Test_CreateProcess(0x00400000, EXE_RELOCS, FALSE);
    Test_NativeProcess(0x00400000, FALSE);

    /* The shared user data page is in the way: the EXE runs elsewhere */
    Test_CreateProcess(SHARED_DATA_BASE, EXE_RELOCS, TRUE);
    Test_CreateProcess(SHARED_DATA_BASE, EXE_RELOCS | EXE_DYNAMIC_BASE, TRUE);
    Test_NativeProcess(SHARED_DATA_BASE, TRUE);
    Test_Debugged(SHARED_DATA_BASE);

    /* Without relocations it must not run anywhere else */
    Test_CreateProcess(SHARED_DATA_BASE, EXE_NO_ABSOLUTE, TRUE);

#ifdef _WIN64
    /* Above the 8 TB user space of NT 6.1 (mapped there on newer Windows) */
    Test_CreateProcess(0x80000000000ULL, EXE_RELOCS | EXE_DYNAMIC_BASE, FALSE);
    Test_CreateProcess(0x7FF700000000ULL, EXE_RELOCS, FALSE);
#endif

    SetErrorMode(OldMode);
    DeleteExes();
}

#else

START_TEST(ForcedRelocation)
{
    skip("The test images exist only for x86 and amd64\n");
}

#endif
