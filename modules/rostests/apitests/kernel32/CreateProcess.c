/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Test spoiling of StaticUnicodeString by CreateProcessA,
 *                  CreateProcessW on DOS, 16-bit, foreign-machine and invalid images
 * PROGRAMMERS:     Mark Jansen
 *                  Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <ndk/rtlfuncs.h>

/*
 * DOS and 16-bit images (and non-MZ files named .exe or .com) start in NTVDM where the system
 * has one: 32-bit Windows, and ReactOS on every architecture. 64-bit Windows has no NTVDM and
 * fails them with ERROR_EXE_MACHINE_TYPE_MISMATCH; ReactOS deliberately keeps NTVDM on amd64,
 * so these cases are skipped on a system without ntvdm.exe. Images for another machine fail
 * with ERROR_BAD_EXE_FORMAT on 64-bit systems.
 */
#define EXPECT_NTVDM ((DWORD)-1)
#define EXPECT_UNTESTED ((DWORD)-2)

typedef struct _IMAGE_CASE
{
    PCWSTR Name;
    PCWSTR FileName;
    BYTE Kind;
    WORD Machine;
    DWORD Expect32;
    DWORD Expect64;
} IMAGE_CASE;

enum { KIND_TEXT, KIND_COM, KIND_DOS_MZ, KIND_NE, KIND_PE, KIND_PE_DLL };

static const IMAGE_CASE Cases[] =
{
    { L"text file", L"text.exe", KIND_TEXT, 0, EXPECT_NTVDM, EXPECT_NTVDM },
    { L"text file .com", L"textcom.com", KIND_TEXT, 0, EXPECT_NTVDM, EXPECT_NTVDM },
    { L"DOS .com", L"dosprog.com", KIND_COM, 0, EXPECT_NTVDM, EXPECT_NTVDM },
    { L"DOS MZ", L"dosmz.exe", KIND_DOS_MZ, 0, EXPECT_NTVDM, EXPECT_NTVDM },
    { L"16-bit Windows NE", L"win16.exe", KIND_NE, 0, EXPECT_NTVDM, EXPECT_NTVDM },
    { L"batch file", L"batch.bat", KIND_TEXT, 0, ERROR_SUCCESS, ERROR_SUCCESS },
    { L"native DLL", L"nativedll.exe", KIND_PE_DLL, 0, ERROR_BAD_EXE_FORMAT, ERROR_BAD_EXE_FORMAT },
    { L"PE32+ x64", L"pe64.exe", KIND_PE, IMAGE_FILE_MACHINE_AMD64, ERROR_EXE_MACHINE_TYPE_MISMATCH, ERROR_SUCCESS },
    { L"PE32 i386", L"pe32.exe", KIND_PE, IMAGE_FILE_MACHINE_I386, ERROR_SUCCESS, ERROR_SUCCESS },
    { L"PE32 ARMNT", L"armnt.exe", KIND_PE, IMAGE_FILE_MACHINE_ARMNT, EXPECT_UNTESTED, ERROR_BAD_EXE_FORMAT },
    { L"PE32+ ARM64", L"arm64.exe", KIND_PE, IMAGE_FILE_MACHINE_ARM64, EXPECT_UNTESTED, ERROR_BAD_EXE_FORMAT },
};

static void PutWord(BYTE *Buf, ULONG Offset, WORD Value)
{
    Buf[Offset] = (BYTE)Value;
    Buf[Offset + 1] = (BYTE)(Value >> 8);
}

static void PutDword(BYTE *Buf, ULONG Offset, DWORD Value)
{
    PutWord(Buf, Offset, (WORD)Value);
    PutWord(Buf, Offset + 2, (WORD)(Value >> 16));
}

/* A real-mode program: print a line through INT 21h/09h, then exit through INT 21h/4Ch */
static const BYTE DosCode[] =
{
    0x0E, 0x1F,             /* push cs; pop ds */
    0xBA, 0x0E, 0x00,       /* mov dx, 000Eh (offset of the text in this block) */
    0xB4, 0x09,             /* mov ah, 09h */
    0xCD, 0x21,             /* int 21h */
    0xB8, 0x00, 0x4C,       /* mov ax, 4C00h */
    0xCD, 0x21,             /* int 21h */
    'D', 'O', 'S', '\r', '\n', '$'
};

static ULONG BuildDosHeader(BYTE *Buf, ULONG FileSize, LONG NewHeader)
{
    PutWord(Buf, 0x00, IMAGE_DOS_SIGNATURE);
    PutWord(Buf, 0x02, (WORD)(FileSize % 512));
    PutWord(Buf, 0x04, (WORD)((FileSize + 511) / 512));
    PutWord(Buf, 0x08, 4);                      /* header paragraphs */
    PutWord(Buf, 0x0A, 0x10);                   /* minimum extra paragraphs */
    PutWord(Buf, 0x0C, 0xFFFF);
    PutWord(Buf, 0x10, 0x100);                  /* SP */
    PutWord(Buf, 0x18, NewHeader ? 0x40 : 0x1C); /* relocation table offset */
    PutDword(Buf, 0x3C, (DWORD)NewHeader);
    return 0x40;
}

static ULONG BuildImage(const IMAGE_CASE *Case, BYTE *Buf, ULONG Size)
{
    static const char Text[] = "This is a plain text file, not a program.\r\n";
    BOOL Is64, IsDll;
    ULONG Nt, Opt, Sect, OptSize;

    ZeroMemory(Buf, Size);
    switch (Case->Kind)
    {
        case KIND_TEXT:
            CopyMemory(Buf, Text, sizeof(Text) - 1);
            return sizeof(Text) - 1;

        case KIND_COM:
            /* A .com file is raw code loaded at offset 100h: the text then sits at 10Eh */
            CopyMemory(Buf, DosCode + 2, sizeof(DosCode) - 2);
            Buf[1] = 0x0E - 2;
            Buf[2] = 0x01;
            return sizeof(DosCode) - 2;

        case KIND_DOS_MZ:
            BuildDosHeader(Buf, 0x40 + sizeof(DosCode), 0);
            CopyMemory(Buf + 0x40, DosCode, sizeof(DosCode));
            return 0x40 + sizeof(DosCode);

        case KIND_NE:
            BuildDosHeader(Buf, 0x40 + sizeof(DosCode), 0x80);
            CopyMemory(Buf + 0x40, DosCode, sizeof(DosCode));
            Buf[0x80] = 'N';
            Buf[0x81] = 'E';
            Buf[0x82] = 5;                      /* linker version 5.10 */
            Buf[0x83] = 10;
            PutWord(Buf, 0x80 + 0x04, 0x40);    /* entry table offset */
            PutWord(Buf, 0x80 + 0x0C, 0x0302);  /* flags: multiple data, protected mode */
            PutWord(Buf, 0x80 + 0x0E, 1);       /* automatic data segment */
            PutWord(Buf, 0x80 + 0x10, 0x1000);  /* heap */
            PutWord(Buf, 0x80 + 0x12, 0x1000);  /* stack */
            PutDword(Buf, 0x80 + 0x14, 0x00010000); /* CS:IP = 1:0000 */
            PutDword(Buf, 0x80 + 0x18, 0x00020000); /* SS:SP = 2:0000 */
            PutWord(Buf, 0x80 + 0x22, 0x40);    /* segment table offset */
            PutWord(Buf, 0x80 + 0x24, 0x40);    /* resource table offset */
            PutWord(Buf, 0x80 + 0x26, 0x40);    /* resident name table offset */
            PutWord(Buf, 0x80 + 0x28, 0x40);    /* module reference table offset */
            PutWord(Buf, 0x80 + 0x2A, 0x40);    /* imported names table offset */
            PutWord(Buf, 0x80 + 0x32, 4);       /* segment alignment shift */
            Buf[0x80 + 0x36] = 2;               /* target OS: Windows */
            PutWord(Buf, 0x80 + 0x3E, 0x030A);  /* expected Windows version 3.10 */
            return 0x200;

        case KIND_PE:
        case KIND_PE_DLL:
            IsDll = (Case->Kind == KIND_PE_DLL);
            Is64 = IsDll ? (sizeof(void*) == 8) : (Case->Machine == IMAGE_FILE_MACHINE_AMD64 ||
                                                   Case->Machine == IMAGE_FILE_MACHINE_ARM64 ||
                                                   Case->Machine == IMAGE_FILE_MACHINE_IA64);
            Nt = 0x40;
            Opt = Nt + 4 + IMAGE_SIZEOF_FILE_HEADER;
            OptSize = Is64 ? 0xF0 : 0xE0;
            Sect = Opt + OptSize;
            BuildDosHeader(Buf, 0x400, Nt);
            PutDword(Buf, Nt, IMAGE_NT_SIGNATURE);
            PutWord(Buf, Nt + 4, IsDll ? (Is64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386) : Case->Machine);
            PutWord(Buf, Nt + 6, 1);            /* one section */
            PutWord(Buf, Nt + 20, (WORD)OptSize);
            PutWord(Buf, Nt + 22, IMAGE_FILE_EXECUTABLE_IMAGE |
                                  (Is64 ? IMAGE_FILE_LARGE_ADDRESS_AWARE : IMAGE_FILE_32BIT_MACHINE) |
                                  (IsDll ? IMAGE_FILE_DLL : 0));
            PutWord(Buf, Opt + 0, Is64 ? IMAGE_NT_OPTIONAL_HDR64_MAGIC : IMAGE_NT_OPTIONAL_HDR32_MAGIC);
            Buf[Opt + 2] = 8;                   /* linker version */
            PutDword(Buf, Opt + 4, 0x200);      /* size of code */
            PutDword(Buf, Opt + 16, 0x1000);    /* entry point */
            PutDword(Buf, Opt + 20, 0x1000);    /* base of code */
            if (Is64)
            {
                PutDword(Buf, Opt + 24, 0x40000000);
                PutDword(Buf, Opt + 28, 0x1);   /* image base 0x140000000 */
            }
            else
            {
                PutDword(Buf, Opt + 28, 0x400000);
            }
            PutDword(Buf, Opt + 32, 0x1000);    /* section alignment */
            PutDword(Buf, Opt + 36, 0x200);     /* file alignment */
            PutWord(Buf, Opt + 40, 5);          /* OS version 5.1 */
            PutWord(Buf, Opt + 42, 1);
            PutWord(Buf, Opt + 48, 5);          /* subsystem version 5.1 */
            PutWord(Buf, Opt + 50, 1);
            PutDword(Buf, Opt + 56, 0x2000);    /* size of image */
            PutDword(Buf, Opt + 60, 0x200);     /* size of headers */
            PutWord(Buf, Opt + 68, IMAGE_SUBSYSTEM_WINDOWS_CUI);
            if (Is64)
            {
                PutDword(Buf, Opt + 72, 0x100000);  /* stack reserve */
                PutDword(Buf, Opt + 80, 0x1000);    /* stack commit */
                PutDword(Buf, Opt + 88, 0x100000);  /* heap reserve */
                PutDword(Buf, Opt + 96, 0x1000);    /* heap commit */
                PutDword(Buf, Opt + 108, IMAGE_NUMBEROF_DIRECTORY_ENTRIES);
            }
            else
            {
                PutDword(Buf, Opt + 72, 0x100000);
                PutDword(Buf, Opt + 76, 0x1000);
                PutDword(Buf, Opt + 80, 0x100000);
                PutDword(Buf, Opt + 84, 0x1000);
                PutDword(Buf, Opt + 92, IMAGE_NUMBEROF_DIRECTORY_ENTRIES);
            }
            CopyMemory(Buf + Sect, ".text", 5);
            PutDword(Buf, Sect + 8, 0x1000);    /* virtual size */
            PutDword(Buf, Sect + 12, 0x1000);   /* virtual address */
            PutDword(Buf, Sect + 16, 0x200);    /* raw size */
            PutDword(Buf, Sect + 20, 0x200);    /* raw offset */
            PutDword(Buf, Sect + 36, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ);
            Buf[0x200] = 0xC3;                  /* ret */
            return 0x400;
    }
    return 0;
}

static BOOL IsWow64Available(void)
{
    WCHAR Path[MAX_PATH];
    UINT Length;

    Length = GetSystemWow64DirectoryW(Path, ARRAYSIZE(Path));
    if (!Length || Length >= ARRAYSIZE(Path) - 12)
        return FALSE;
    StringCchCatW(Path, ARRAYSIZE(Path), L"\\ntdll.dll");
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL IsNtvdmPresent(void)
{
    WCHAR Path[MAX_PATH];
    UINT Length;

    Length = GetSystemDirectoryW(Path, ARRAYSIZE(Path));
    if (!Length || Length >= ARRAYSIZE(Path) - 12)
        return FALSE;
    StringCchCatW(Path, ARRAYSIZE(Path), L"\\ntvdm.exe");
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

/* Exit codes of the child modes; a failed CreateProcess gives CHILD_CREATE_FAILED + error */
#define CHILD_OK                0
#define CHILD_NO_NEW_VDM        1
#define CHILD_TERMINATE_FAILED  2
#define CHILD_WAIT_FAILED       3
#define CHILD_CREATE_FAILED     0x10000

/* Child mode "vdmcase": start the image from this process's own new console, so that no
 * existing VDM can take the task, then end the new NTVDM through its process id
 * (for an NTVDM start, hProcess is a tagged VDM wait handle). */
static DWORD VdmCaseChild(PCSTR Path)
{
    CHAR CmdLine[MAX_PATH + 2];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    HANDLE Process;
    DWORD Result = CHILD_OK;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    StringCchPrintfA(CmdLine, ARRAYSIZE(CmdLine), "\"%s\"", Path);
    if (!CreateProcessA(Path, CmdLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
        return CHILD_CREATE_FAILED + (GetLastError() & 0xFFFF);

    Process = pi.hProcess;
    if ((ULONG_PTR)pi.hProcess & 3)
        Process = pi.dwProcessId ? OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pi.dwProcessId) : NULL;
    if (!Process)
        Result = CHILD_NO_NEW_VDM;
    else if (!TerminateProcess(Process, 0))
        Result = CHILD_TERMINATE_FAILED;
    else if (WaitForSingleObject(Process, 10000) != WAIT_OBJECT_0)
        Result = CHILD_WAIT_FAILED;

    if (Process && Process != pi.hProcess)
        CloseHandle(Process);
    if (pi.hThread)
        CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return Result;
}

/* Child mode "hardcase": CreateProcess with the default error mode, so a hard error would block it */
static DWORD HardErrorCaseChild(PCSTR Path)
{
    CHAR CmdLine[MAX_PATH + 2];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    SetErrorMode(0);
    StringCchPrintfA(CmdLine, ARRAYSIZE(CmdLine), "\"%s\"", Path);
    if (!CreateProcessA(Path, CmdLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
        return CHILD_CREATE_FAILED + (GetLastError() & 0xFFFF);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return CHILD_OK;
}

/* Run this test in a child mode with its own new console; FALSE if it did not end in time */
static BOOL RunChild(PCSTR Mode, PCWSTR Path, DWORD Timeout, DWORD *ExitCode)
{
    WCHAR Self[MAX_PATH], CmdLine[2 * MAX_PATH + 64];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD Wait, Length;

    *ExitCode = (DWORD)-1;
    Length = GetModuleFileNameW(NULL, Self, ARRAYSIZE(Self));
    ok(Length && Length < ARRAYSIZE(Self), "GetModuleFileNameW failed with %lu\n", GetLastError());
    if (!Length || Length >= ARRAYSIZE(Self) ||
        FAILED(StringCchPrintfW(CmdLine, ARRAYSIZE(CmdLine), L"\"%s\" CreateProcess %S \"%s\"", Self, Mode, Path)))
    {
        return TRUE;
    }
    if (!CreateProcessW(Self, CmdLine, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi))
    {
        ok(0, "CreateProcessW for the %s child failed with %lu\n", Mode, GetLastError());
        return TRUE;
    }
    Wait = WaitForSingleObject(pi.hProcess, Timeout);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    GetExitCodeProcess(pi.hProcess, ExitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return Wait == WAIT_OBJECT_0;
}

static void TestCase(const IMAGE_CASE *Case, PCWSTR Dir, BOOL Os64, BOOL Wow64, BOOL HasNtvdm)
{
    WCHAR Path[MAX_PATH], CmdLine[MAX_PATH + 2];
    BYTE Image[0x400];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    HANDLE File;
    DWORD Size, Written, Error, Expect, ExitCode;
    BOOL Ret;

    Expect = Os64 ? Case->Expect64 : Case->Expect32;
    if (Expect == EXPECT_NTVDM && !HasNtvdm)
    {
        skip("%S: no NTVDM on this system (64-bit Windows fails it with ERROR_EXE_MACHINE_TYPE_MISMATCH)\n",
             Case->Name);
        return;
    }
    /* A 64-bit system without WoW64 cannot start a 32-bit image either */
    if (Os64 && Case->Kind == KIND_PE && Case->Machine == IMAGE_FILE_MACHINE_I386 && !Wow64)
        Expect = ERROR_BAD_EXE_FORMAT;

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\%s", Dir, Case->FileName);
    StringCchPrintfW(CmdLine, ARRAYSIZE(CmdLine), L"\"%s\"", Path);
    Size = BuildImage(Case, Image, sizeof(Image));
    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(File != INVALID_HANDLE_VALUE, "%S: CreateFileW failed with %lu\n", Case->Name, GetLastError());
    if (File == INVALID_HANDLE_VALUE)
        return;
    Ret = WriteFile(File, Image, Size, &Written, NULL);
    ok(Ret && Written == Size, "%S: WriteFile failed with %lu\n", Case->Name, GetLastError());
    CloseHandle(File);

    if (Expect == EXPECT_NTVDM)
    {
        Ret = RunChild("vdmcase", Path, 60000, &ExitCode);
        ok(Ret, "%S: the NTVDM start did not finish\n", Case->Name);
        ok(ExitCode == CHILD_OK, "%S: NTVDM start result %#lx, expected a new NTVDM process ended by its id\n",
           Case->Name, ExitCode);
        DeleteFileW(Path);
        return;
    }

    SetLastError(0xdeadbeef);
    Ret = CreateProcessW(Path, CmdLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, Dir, &si, &pi);
    Error = Ret ? ERROR_SUCCESS : GetLastError();
    if (Ret)
    {
        ok(TerminateProcess(pi.hProcess, 0), "%S: TerminateProcess failed with %lu\n", Case->Name, GetLastError());
        ok(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0, "%S: the process did not end\n", Case->Name);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    if (Expect == EXPECT_UNTESTED)
        trace("%S: CreateProcessW returned %d, error %lu\n", Case->Name, Ret, Error);
    else
        ok(Error == Expect, "%S: CreateProcessW returned %d, error %lu, expected %lu\n",
           Case->Name, Ret, Error, Expect);

    /* 64-bit Windows raises no hard error for an image it cannot run */
    if (Os64 && Expect == ERROR_BAD_EXE_FORMAT)
    {
        Ret = RunChild("hardcase", Path, 15000, &ExitCode);
        ok(Ret, "%S: CreateProcess with the default error mode blocked (hard error?)\n", Case->Name);
        ok(ExitCode == CHILD_CREATE_FAILED + ERROR_BAD_EXE_FORMAT,
           "%S: CreateProcess with the default error mode gave %#lx\n", Case->Name, ExitCode);
    }

    DeleteFileW(Path);
}

static void TestImageTypes(void)
{
    WCHAR Dir[MAX_PATH];
    BOOL Os64 = (sizeof(void*) == 8), Wow64 = FALSE, HasNtvdm;
    UINT OldMode, i;
    DWORD Length;

    if (!Os64 && IsWow64Process(GetCurrentProcess(), &Wow64) && Wow64)
    {
        skip("32-bit test on a 64-bit system: run the 64-bit test\n");
        return;
    }
    Wow64 = Os64 && IsWow64Available();
    HasNtvdm = IsNtvdmPresent();
    trace("64-bit system %d, WoW64 %d, NTVDM %d\n", Os64, Wow64, HasNtvdm);

    Length = GetTempPathW(ARRAYSIZE(Dir), Dir);
    if (!Length || Length >= ARRAYSIZE(Dir) - 32)
    {
        skip("GetTempPathW failed with %lu\n", GetLastError());
        return;
    }
    StringCchPrintfW(Dir + Length, ARRAYSIZE(Dir) - Length, L"cpimgtype%lu", GetCurrentProcessId());
    if (!CreateDirectoryW(Dir, NULL))
    {
        skip("CreateDirectoryW(%S) failed with %lu\n", Dir, GetLastError());
        return;
    }

    /* A 32-bit machine mismatch raises a hard error; keep it off the screen */
    OldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    for (i = 0; i < ARRAYSIZE(Cases); i++)
        TestCase(&Cases[i], Dir, Os64, Wow64, HasNtvdm);
    SetErrorMode(OldMode);

    RemoveDirectoryW(Dir);
}

START_TEST(CreateProcess)
{
    PUNICODE_STRING StaticString;
    UNICODE_STRING CompareString;
    BOOL Process;
    STARTUPINFO si = {0};
    PROCESS_INFORMATION pi = {0};
    LONG Result;
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 4 && !strcmp(argv[2], "vdmcase"))
        ExitProcess(VdmCaseChild(argv[3]));
    if (argc >= 4 && !strcmp(argv[2], "hardcase"))
        ExitProcess(HardErrorCaseChild(argv[3]));

    StaticString = &NtCurrentTeb()->StaticUnicodeString;
    RtlInitUnicodeString(&CompareString, L"--sentinel--");
    RtlCopyUnicodeString(StaticString, &CompareString);

    si.cb = sizeof(si);
    Process = CreateProcessA("ApplicationName", "CommandLine", NULL, NULL, FALSE, 0, NULL, "CurrentDir", &si, &pi);
    ok_int(Process, 0);

    Result = RtlCompareUnicodeString(StaticString, &CompareString, TRUE);
    ok(!Result, "Expected %s to equal %s\n",
       wine_dbgstr_wn(StaticString->Buffer, StaticString->Length / sizeof(WCHAR)),
       wine_dbgstr_wn(CompareString.Buffer, CompareString.Length / sizeof(WCHAR)));

    TestImageTypes();
}
