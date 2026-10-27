/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for cross-process WriteProcessMemory into image sections
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * WriteProcessMemory / VirtualProtectEx must be able to patch a remote
 * process's image data. The Chromium/Electron sandbox broker, debuggers and
 * DLL injectors write into a suspended child's image (.data/.bss/.rdata/
 * .idata/.tls/.rsrc), which is copy-on-write: the write lands in a private
 * page and never reaches the backing file or other mappers.
 *
 * The section maximum-protection ceiling (W^X hardening) must NOT regress:
 * a user-mode attempt to gain EXECUTE on a non-executable image subsection
 * (turning a data page into RWX shellcode) must still be rejected. Both are
 * verified here against a real suspended child process.
 */

#include "precomp.h"

#include <ndk/pstypes.h>
#include <ndk/psfuncs.h>
#include <ndk/mmfuncs.h>

/* A recognisable pattern we stamp into each remote image section */
static const BYTE TestPattern[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };

static
HANDLE
StartSuspendedChild(VOID)
{
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    BOOL Success;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine,
                    sizeof(CommandLine),
                    L"\"%ls\" WriteProcessMemory child",
                    FileName);

    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;

    Success = CreateProcessW(FileName,
                             CommandLine,
                             NULL,
                             NULL,
                             FALSE,
                             CREATE_SUSPENDED,
                             NULL,
                             NULL,
                             &StartupInfo,
                             &ProcessInfo);
    if (!Success)
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
        return NULL;
    }
    /* Keep the child suspended; we never resume it, only patch and kill it. */
    CloseHandle(ProcessInfo.hThread);
    return ProcessInfo.hProcess;
}

static
PVOID
GetRemoteImageBase(
    _In_ HANDLE hProcess)
{
    PROCESS_BASIC_INFORMATION BasicInfo;
    NTSTATUS Status;
    PVOID ImageBase = NULL;
    SIZE_T BytesRead = 0;

    Status = NtQueryInformationProcess(hProcess,
                                       ProcessBasicInformation,
                                       &BasicInfo,
                                       sizeof(BasicInfo),
                                       NULL);
    if (!NT_SUCCESS(Status))
    {
        skip("NtQueryInformationProcess failed with 0x%08lx\n", Status);
        return NULL;
    }

    if (!ReadProcessMemory(hProcess,
                           (PUCHAR)BasicInfo.PebBaseAddress + FIELD_OFFSET(PEB, ImageBaseAddress),
                           &ImageBase,
                           sizeof(ImageBase),
                           &BytesRead) ||
        BytesRead != sizeof(ImageBase))
    {
        skip("Failed to read remote PEB ImageBaseAddress: %lu\n", GetLastError());
        return NULL;
    }
    return ImageBase;
}

static
PCSTR
ProtName(ULONG Chars)
{
    BOOLEAN r = !!(Chars & IMAGE_SCN_MEM_READ);
    BOOLEAN w = !!(Chars & IMAGE_SCN_MEM_WRITE);
    BOOLEAN x = !!(Chars & IMAGE_SCN_MEM_EXECUTE);

    if (x && w) return "RWX";
    if (x) return "RX";
    if (w) return "RW";
    if (r) return "RO";
    return "--";
}

/*
 * Write TestPattern into one remote image section and read it back. Every
 * mapped image subsection is copy-on-write, so this must succeed regardless of
 * the section's natural (read-only / read-write / executable) protection.
 */
static
VOID
TestWriteSection(
    _In_ HANDLE hProcess,
    _In_ PVOID RemoteAddress,
    _In_ PCSTR Name,
    _In_ ULONG Chars)
{
    BYTE Original[sizeof(TestPattern)];
    BYTE ReadBack[sizeof(TestPattern)];
    SIZE_T Bytes = 0;
    BOOL Success;

    /* The headers must be readable even before writing. */
    if (!ReadProcessMemory(hProcess, RemoteAddress, Original, sizeof(Original), &Bytes) ||
        Bytes != sizeof(Original))
    {
        skip("%s: ReadProcessMemory (pre) failed: %lu\n", Name, GetLastError());
        return;
    }

    /*
     * The regression: on the hardened kernel, kernel32!WriteProcessMemory used
     * to reprotect the page to PAGE_EXECUTE_READWRITE first, which the image
     * W^X ceiling rejects on a non-executable subsection -> ERROR_INVALID_
     * PARAMETER (87). Writing data must not require execute. (fail-before:
     * data/read-only sections fail with 87; pass-after: all succeed.)
     */
    SetLastError(0xdeadbeef);
    Bytes = 0;
    Success = WriteProcessMemory(hProcess, RemoteAddress, TestPattern, sizeof(TestPattern), &Bytes);
    ok(Success, "%s (%s): WriteProcessMemory failed, error %lu (expected success)\n",
       Name, ProtName(Chars), GetLastError());
    if (!Success)
        return;
    ok(Bytes == sizeof(TestPattern), "%s: wrote %Iu bytes, expected %Iu\n",
       Name, Bytes, sizeof(TestPattern));

    /* The copy-on-write write must be observable in the remote process. */
    Bytes = 0;
    Success = ReadProcessMemory(hProcess, RemoteAddress, ReadBack, sizeof(ReadBack), &Bytes);
    ok(Success, "%s: ReadProcessMemory (post) failed: %lu\n", Name, GetLastError());
    if (Success)
        ok(memcmp(ReadBack, TestPattern, sizeof(TestPattern)) == 0,
           "%s: remote image data not updated by WriteProcessMemory\n", Name);
}

/*
 * An in-process patcher (Chromium's chrome_elf patches ntdll syscall stubs this
 * way) calls WriteProcessMemory on code the calling thread is executing from.
 * The target here is NtProtectVirtualMemory's own syscall stub, which is on
 * the page WriteProcessMemory itself needs in order to restore the protection:
 * if the page loses PAGE_EXECUTE_* while it is writable, the thread faults on
 * the return from that very call. Writing back the byte that is already there
 * changes nothing, so this only exercises the protection handling.
 */
static
VOID
TestSelfPatchExecutedCode(VOID)
{
    MEMORY_BASIC_INFORMATION Before, After;
    PBYTE Stub;
    BYTE Original;
    SIZE_T Written = 0;
    BOOL Success;
    PBYTE Private;
    static const BYTE Patch[4] = { 0x90, 0x90, 0x90, 0xC3 };

    Stub = (PBYTE)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtProtectVirtualMemory");
    ok(Stub != NULL, "NtProtectVirtualMemory not found\n");
    if (Stub)
    {
        ok(VirtualQuery(Stub, &Before, sizeof(Before)) == sizeof(Before), "VirtualQuery failed\n");
        ok((Before.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0,
           "ntdll stub page is not executable: 0x%lx\n", Before.Protect);

        Original = *Stub;
        Success = WriteProcessMemory(GetCurrentProcess(), Stub, &Original, 1, &Written);
        ok(Success, "WriteProcessMemory into executed code failed: %lu\n", GetLastError());
        ok(Written == 1, "Written = %lu\n", (ULONG)Written);

        ok(VirtualQuery(Stub, &After, sizeof(After)) == sizeof(After), "VirtualQuery failed\n");
        ok(After.Protect == Before.Protect,
           "protection changed by WriteProcessMemory: 0x%lx -> 0x%lx\n", Before.Protect, After.Protect);
        ok(*Stub == Original, "stub byte changed\n");
    }

    /* A private executable, non-writable page is patched and left executable. */
    Private = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    ok(Private != NULL, "VirtualAlloc failed: %lu\n", GetLastError());
    if (Private)
    {
        ULONG Old;
        ok(VirtualProtect(Private, 0x1000, PAGE_EXECUTE_READ, &Old), "VirtualProtect failed\n");
        Written = 0;
        Success = WriteProcessMemory(GetCurrentProcess(), Private, Patch, sizeof(Patch), &Written);
        ok(Success, "WriteProcessMemory into PAGE_EXECUTE_READ failed: %lu\n", GetLastError());
        ok(Written == sizeof(Patch), "Written = %lu\n", (ULONG)Written);
        ok(memcmp(Private, Patch, sizeof(Patch)) == 0, "patch not applied\n");
        ok(VirtualQuery(Private, &After, sizeof(After)) == sizeof(After), "VirtualQuery failed\n");
        ok(After.Protect == PAGE_EXECUTE_READ, "protection not restored: 0x%lx\n", After.Protect);
        VirtualFree(Private, 0, MEM_RELEASE);
    }
}

START_TEST(WriteProcessMemory)
{
    HANDLE hProcess;
    PVOID RemoteBase;
    PIMAGE_DOS_HEADER DosHeader;
    PIMAGE_NT_HEADERS NtHeaders;
    PIMAGE_SECTION_HEADER Section;
    ULONG i;
    int argc;
    char **argv;
    ULONG OldProtect;
    BOOL Success;
    PVOID RoDataRemote = NULL;   /* a read-only, non-executable subsection */
    ULONG RoDataChars = 0;
    BOOLEAN FoundWritable = FALSE;
    BOOLEAN FoundReadOnly = FALSE;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "child"))
    {
        /* Safeguard: the child is kept suspended and never actually runs. */
        return;
    }

    TestSelfPatchExecutedCode();

    /* Parse our own (already loaded) image - the child is the same binary. */
    DosHeader = (PIMAGE_DOS_HEADER)GetModuleHandleW(NULL);
    if (DosHeader->e_magic != IMAGE_DOS_SIGNATURE)
    {
        ok(0, "Own module has no DOS signature\n");
        return;
    }
    NtHeaders = (PIMAGE_NT_HEADERS)((PUCHAR)DosHeader + DosHeader->e_lfanew);
    if (NtHeaders->Signature != IMAGE_NT_SIGNATURE)
    {
        ok(0, "Own module has no NT signature\n");
        return;
    }

    hProcess = StartSuspendedChild();
    if (!hProcess)
        return;

    RemoteBase = GetRemoteImageBase(hProcess);
    if (!RemoteBase)
    {
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
        return;
    }

    Section = IMAGE_FIRST_SECTION(NtHeaders);
    for (i = 0; i < NtHeaders->FileHeader.NumberOfSections; i++)
    {
        CHAR Name[IMAGE_SIZEOF_SHORT_NAME + 1];
        ULONG Chars = Section[i].Characteristics;
        PVOID RemoteAddress;

        RtlCopyMemory(Name, Section[i].Name, IMAGE_SIZEOF_SHORT_NAME);
        Name[IMAGE_SIZEOF_SHORT_NAME] = '\0';

        /* Only readable sections are mapped and interesting here. */
        if (!(Chars & IMAGE_SCN_MEM_READ))
            continue;
        if (Section[i].Misc.VirtualSize == 0 && Section[i].SizeOfRawData == 0)
            continue;

        RemoteAddress = (PUCHAR)RemoteBase + Section[i].VirtualAddress;

        /* P1 + P2: WriteProcessMemory must succeed into every image section. */
        TestWriteSection(hProcess, RemoteAddress, Name, Chars);

        if (Chars & IMAGE_SCN_MEM_WRITE)
            FoundWritable = TRUE;

        if ((Chars & IMAGE_SCN_MEM_READ) &&
            !(Chars & IMAGE_SCN_MEM_WRITE) &&
            !(Chars & IMAGE_SCN_MEM_EXECUTE))
        {
            FoundReadOnly = TRUE;
            if (RoDataRemote == NULL)
            {
                RoDataRemote = RemoteAddress;
                RoDataChars = Chars;
            }
        }
    }

    ok(FoundWritable, "No writable image section found to exercise P1\n");
    ok(FoundReadOnly, "No read-only image section found to exercise P2\n");

    /*
     * W^X hardening must NOT regress: gaining EXECUTE on a non-executable
     * read-only image subsection (RWX shellcode) must still be rejected with
     * ERROR_INVALID_PARAMETER (STATUS_SECTION_PROTECTION). This holds both on
     * the bare hardened kernel and after the WriteProcessMemory fix.
     */
    if (RoDataRemote != NULL)
    {
        SetLastError(0xdeadbeef);
        OldProtect = 0;
        Success = VirtualProtectEx(hProcess, RoDataRemote, sizeof(TestPattern),
                                   PAGE_EXECUTE_READWRITE, &OldProtect);
        ok(!Success,
           "VirtualProtectEx(RWX) on non-exec image section (%s) succeeded - W^X escalation not blocked!\n",
           ProtName(RoDataChars));
        if (!Success)
            ok(GetLastError() == ERROR_INVALID_PARAMETER,
               "escalation rejected with error %lu, expected ERROR_INVALID_PARAMETER (87)\n",
               GetLastError());

        /* PAGE_EXECUTE_READ (gaining execute without write) must also fail. */
        SetLastError(0xdeadbeef);
        OldProtect = 0;
        Success = VirtualProtectEx(hProcess, RoDataRemote, sizeof(TestPattern),
                                   PAGE_EXECUTE_READ, &OldProtect);
        ok(!Success,
           "VirtualProtectEx(RX) on non-exec image section (%s) succeeded - W^X escalation not blocked!\n",
           ProtName(RoDataChars));
    }
    else
    {
        skip("No read-only non-executable image section found for the escalation test\n");
    }

    TerminateProcess(hProcess, 0);
    CloseHandle(hProcess);
}
