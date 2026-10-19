/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GetBinaryTypeA and GetBinaryTypeW on generated images
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <ndk/rtlfuncs.h>

/*
 * The expected values are those measured on Windows Server 2008 R2 and Windows 10 22H2 (x64, and a
 * 32-bit process on it). Images are generated, so every architecture sees the same files. Cases that
 * Windows answers by header validation in the kernel (zero sections, unaligned PE header) and the
 * OS/2 NE heuristics are left out: a real OS/2 image would be needed to measure them.
 */
#define NO_RESULT 0xDEADBEEF

typedef struct _IMAGE_CASE
{
    PCWSTR Name;
    PCWSTR FileName;
    BYTE Kind;
    WORD Machine;
    BYTE NeTarget;
    BYTE Variant;
    /* Expected result; Error is only checked when Ret is FALSE */
    BOOL Ret;
    DWORD Type;
    DWORD Error;
} IMAGE_CASE;

enum { KIND_TEXT, KIND_EMPTY, KIND_DOS_MZ, KIND_NE, KIND_PE, KIND_PE_DLL, KIND_PE_NATIVE, KIND_PE_CUT,
       KIND_BYTES, KIND_MZ_BYTES, KIND_ELF };

/* KIND_PE variants; the length of a KIND_BYTES or KIND_MZ_BYTES file is its Variant */
enum { V_NONE, V_MAGIC32_ON_AMD64, V_MAGIC64_ON_I386, V_NO_OPTIONAL, V_NO_EXEC_FLAG,
       V_BAD_ALIGN, V_LFANEW_PAST_EOF, V_SHORT_OPTIONAL };

#define PE_I386 IMAGE_FILE_MACHINE_I386
#define PE_AMD64 IMAGE_FILE_MACHINE_AMD64

#define CASE(name, file, kind, machine, ne, variant, ret, type, err) \
    { name, file, kind, machine, ne, variant, ret, type, err }
#define OK_TYPE(name, file, kind, machine, ne, variant, type) \
    CASE(name, file, kind, machine, ne, variant, TRUE, type, 0)
#define BAD(name, file, kind, machine, ne, variant) \
    CASE(name, file, kind, machine, ne, variant, FALSE, NO_RESULT, ERROR_BAD_EXE_FORMAT)

static const IMAGE_CASE Cases[] =
{
    OK_TYPE(L"text file .exe", L"text.exe", KIND_TEXT, 0, 0, 0, SCS_DOS_BINARY),
    OK_TYPE(L"text file .com", L"textcom.com", KIND_TEXT, 0, 0, 0, SCS_DOS_BINARY),
    OK_TYPE(L"text file .pif", L"textpif.pif", KIND_TEXT, 0, 0, 0, SCS_PIF_BINARY),
    OK_TYPE(L"text file .EXE", L"TEXTUP.EXE", KIND_TEXT, 0, 0, 0, SCS_DOS_BINARY),
    BAD(L"text file .bat", L"textbat.bat", KIND_TEXT, 0, 0, 0),
    BAD(L"text file .cmd", L"textcmd.cmd", KIND_TEXT, 0, 0, 0),
    BAD(L"text file .txt", L"text.txt", KIND_TEXT, 0, 0, 0),
    BAD(L"text file .dll", L"textdll.dll", KIND_TEXT, 0, 0, 0),
    BAD(L"text file, no extension", L"textnoext", KIND_TEXT, 0, 0, 0),
    BAD(L"empty file .exe", L"empty.exe", KIND_EMPTY, 0, 0, 0),
    BAD(L"empty file .com", L"empty.com", KIND_EMPTY, 0, 0, 0),
    OK_TYPE(L"1 byte .exe", L"b1.exe", KIND_BYTES, 0, 0, 1, SCS_DOS_BINARY),
    OK_TYPE(L"2 bytes MZ .exe", L"b2mz.exe", KIND_MZ_BYTES, 0, 0, 2, SCS_DOS_BINARY),
    OK_TYPE(L"16 bytes .exe", L"b16.exe", KIND_BYTES, 0, 0, 16, SCS_DOS_BINARY),
    OK_TYPE(L"28 bytes MZ .exe", L"b28mz.exe", KIND_MZ_BYTES, 0, 0, 28, SCS_DOS_BINARY),
    OK_TYPE(L"ELF .exe", L"elf.exe", KIND_ELF, 0, 0, 0, SCS_DOS_BINARY),
    OK_TYPE(L"ELF .com", L"elf.com", KIND_ELF, 0, 0, 0, SCS_DOS_BINARY),
    BAD(L"ELF, no extension", L"elfnoext", KIND_ELF, 0, 0, 0),
    OK_TYPE(L"DOS MZ", L"dosmz.exe", KIND_DOS_MZ, 0, 0, 0, SCS_DOS_BINARY),
    OK_TYPE(L"DOS MZ named .dll", L"dosmz.dll", KIND_DOS_MZ, 0, 0, 0, SCS_DOS_BINARY),
    OK_TYPE(L"DOS MZ, no extension", L"dosmznoext", KIND_DOS_MZ, 0, 0, 0, SCS_DOS_BINARY),
    OK_TYPE(L"16-bit Windows NE", L"win16.exe", KIND_NE, 0, 2, 0, SCS_WOW_BINARY),
    OK_TYPE(L"NE, DOS target", L"nedos.exe", KIND_NE, 0, 5, 0, SCS_DOS_BINARY),
    OK_TYPE(L"PE32 i386 exe", L"pe32.exe", KIND_PE, PE_I386, 0, V_NONE, SCS_32BIT_BINARY),
    OK_TYPE(L"PE32+ x64 exe", L"pe64.exe", KIND_PE, PE_AMD64, 0, V_NONE, SCS_64BIT_BINARY),
    BAD(L"PE32 i386 dll", L"pe32.dll", KIND_PE_DLL, PE_I386, 0, V_NONE),
    BAD(L"PE32+ x64 dll", L"pe64.dll", KIND_PE_DLL, PE_AMD64, 0, V_NONE),
    BAD(L"PE32 i386 dll named .exe", L"pe32dll.exe", KIND_PE_DLL, PE_I386, 0, V_NONE),
    BAD(L"PE32+ x64 dll named .exe", L"pe64dll.exe", KIND_PE_DLL, PE_AMD64, 0, V_NONE),
    OK_TYPE(L"PE32 i386 exe named .dll", L"pe32exe.dll", KIND_PE, PE_I386, 0, V_NONE, SCS_32BIT_BINARY),
    OK_TYPE(L"PE32 i386 exe, no extension", L"pe32noext", KIND_PE, PE_I386, 0, V_NONE, SCS_32BIT_BINARY),
    OK_TYPE(L"PE32+ x64 exe, no extension", L"pe64noext", KIND_PE, PE_AMD64, 0, V_NONE, SCS_64BIT_BINARY),
    OK_TYPE(L"PE32 native subsystem", L"native32.exe", KIND_PE_NATIVE, PE_I386, 0, V_NONE, SCS_32BIT_BINARY),
    OK_TYPE(L"PE32+ native subsystem", L"native64.exe", KIND_PE_NATIVE, PE_AMD64, 0, V_NONE, SCS_64BIT_BINARY),
    BAD(L"PE32 ARMNT", L"armnt.exe", KIND_PE, IMAGE_FILE_MACHINE_ARMNT, 0, V_NONE),
    BAD(L"PE32+ ARM64", L"arm64.exe", KIND_PE, IMAGE_FILE_MACHINE_ARM64, 0, V_NONE),
    BAD(L"PE32 unknown machine", L"mach0.exe", KIND_PE, 0x1234, 0, V_NONE),
    OK_TYPE(L"PE header truncated", L"pecut.exe", KIND_PE_CUT, 0, 0, 0, SCS_DOS_BINARY),
    OK_TYPE(L"e_lfanew past the end", L"lfpast.exe", KIND_PE, PE_I386, 0, V_LFANEW_PAST_EOF, SCS_DOS_BINARY),
    BAD(L"i386 with a PE32+ optional header", L"magic64.exe", KIND_PE, PE_I386, 0, V_MAGIC64_ON_I386),
    BAD(L"x64 with a PE32 optional header", L"magic32.exe", KIND_PE, PE_AMD64, 0, V_MAGIC32_ON_AMD64),
    BAD(L"PE32 without optional header", L"noopt.exe", KIND_PE, PE_I386, 0, V_NO_OPTIONAL),
    BAD(L"PE32 with a short optional header", L"shortopt.exe", KIND_PE, PE_I386, 0, V_SHORT_OPTIONAL),
    BAD(L"PE32 without the executable flag", L"noexec.exe", KIND_PE, PE_I386, 0, V_NO_EXEC_FLAG),
    BAD(L"PE32 with a zero section alignment", L"badalign.exe", KIND_PE, PE_I386, 0, V_BAD_ALIGN),
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

static const BYTE DosCode[] =
{
    0x0E, 0x1F, 0xBA, 0x0E, 0x00, 0xB4, 0x09, 0xCD, 0x21,
    0xB8, 0x00, 0x4C, 0xCD, 0x21, 'D', 'O', 'S', '\r', '\n', '$'
};

static void BuildDosHeader(BYTE *Buf, ULONG FileSize, LONG NewHeader)
{
    PutWord(Buf, 0x00, IMAGE_DOS_SIGNATURE);
    PutWord(Buf, 0x02, (WORD)(FileSize % 512));
    PutWord(Buf, 0x04, (WORD)((FileSize + 511) / 512));
    PutWord(Buf, 0x08, 4);
    PutWord(Buf, 0x0A, 0x10);
    PutWord(Buf, 0x0C, 0xFFFF);
    PutWord(Buf, 0x10, 0x100);
    PutWord(Buf, 0x18, NewHeader ? 0x40 : 0x1C);
    PutDword(Buf, 0x3C, (DWORD)NewHeader);
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

        case KIND_EMPTY:
            return 0;

        case KIND_BYTES:
            CopyMemory(Buf, Text, Case->Variant);
            return Case->Variant;

        case KIND_MZ_BYTES:
            PutWord(Buf, 0, IMAGE_DOS_SIGNATURE);
            return Case->Variant;

        case KIND_ELF:
            CopyMemory(Buf, "\177ELF\002\001\001", 7);
            PutWord(Buf, 16, 2);
            return 0x40;

        case KIND_DOS_MZ:
            BuildDosHeader(Buf, 0x40 + sizeof(DosCode), 0);
            CopyMemory(Buf + 0x40, DosCode, sizeof(DosCode));
            return 0x40 + sizeof(DosCode);

        case KIND_NE:
            BuildDosHeader(Buf, 0x40 + sizeof(DosCode), 0x80);
            CopyMemory(Buf + 0x40, DosCode, sizeof(DosCode));
            Buf[0x80] = 'N';
            Buf[0x81] = 'E';
            Buf[0x82] = 5;
            Buf[0x83] = 10;
            PutWord(Buf, 0x80 + 0x04, 0x40);
            PutWord(Buf, 0x80 + 0x0C, 0x0302);
            PutWord(Buf, 0x80 + 0x0E, 1);
            PutWord(Buf, 0x80 + 0x10, 0x1000);
            PutWord(Buf, 0x80 + 0x12, 0x1000);
            PutDword(Buf, 0x80 + 0x14, 0x00010000);
            PutDword(Buf, 0x80 + 0x18, 0x00020000);
            PutWord(Buf, 0x80 + 0x22, 0x40);
            PutWord(Buf, 0x80 + 0x24, 0x40);
            PutWord(Buf, 0x80 + 0x26, 0x40);
            PutWord(Buf, 0x80 + 0x28, 0x40);
            PutWord(Buf, 0x80 + 0x2A, 0x40);
            PutWord(Buf, 0x80 + 0x32, 4);
            Buf[0x80 + 0x36] = Case->NeTarget;
            PutWord(Buf, 0x80 + 0x3E, 0x030A);
            return 0x200;

        case KIND_PE_CUT:
            BuildDosHeader(Buf, 0x40, 0x40);
            PutDword(Buf, 0x40, IMAGE_NT_SIGNATURE);
            return 0x44;

        case KIND_PE:
        case KIND_PE_DLL:
        case KIND_PE_NATIVE:
            IsDll = (Case->Kind == KIND_PE_DLL);
            Is64 = (Case->Machine == IMAGE_FILE_MACHINE_AMD64 ||
                    Case->Machine == IMAGE_FILE_MACHINE_ARM64);
            if (Case->Variant == V_MAGIC32_ON_AMD64)
                Is64 = FALSE;
            else if (Case->Variant == V_MAGIC64_ON_I386)
                Is64 = TRUE;
            Nt = 0x40;
            Opt = Nt + 4 + IMAGE_SIZEOF_FILE_HEADER;
            OptSize = Is64 ? 0xF0 : 0xE0;
            Sect = Opt + OptSize;
            BuildDosHeader(Buf, 0x400, (Case->Variant == V_LFANEW_PAST_EOF) ? 0x1000 : Nt);
            PutDword(Buf, Nt, IMAGE_NT_SIGNATURE);
            PutWord(Buf, Nt + 4, Case->Machine);
            PutWord(Buf, Nt + 6, 1);
            PutWord(Buf, Nt + 20, (WORD)(Case->Variant == V_NO_OPTIONAL ? 0 :
                                         Case->Variant == V_SHORT_OPTIONAL ? 0x20 : OptSize));
            PutWord(Buf, Nt + 22, (Case->Variant == V_NO_EXEC_FLAG ? 0 : IMAGE_FILE_EXECUTABLE_IMAGE) |
                                  (Is64 ? IMAGE_FILE_LARGE_ADDRESS_AWARE : IMAGE_FILE_32BIT_MACHINE) |
                                  (IsDll ? IMAGE_FILE_DLL : 0));
            PutWord(Buf, Opt + 0, Is64 ? IMAGE_NT_OPTIONAL_HDR64_MAGIC : IMAGE_NT_OPTIONAL_HDR32_MAGIC);
            Buf[Opt + 2] = 8;
            PutDword(Buf, Opt + 4, 0x200);
            PutDword(Buf, Opt + 16, 0x1000);
            PutDword(Buf, Opt + 20, 0x1000);
            if (Is64)
            {
                PutDword(Buf, Opt + 24, 0x40000000);
                PutDword(Buf, Opt + 28, 0x1);
            }
            else
            {
                PutDword(Buf, Opt + 28, 0x400000);
            }
            PutDword(Buf, Opt + 32, (Case->Variant == V_BAD_ALIGN) ? 0 : 0x1000);
            PutDword(Buf, Opt + 36, 0x200);
            PutWord(Buf, Opt + 40, 5);
            PutWord(Buf, Opt + 42, 1);
            PutWord(Buf, Opt + 48, 5);
            PutWord(Buf, Opt + 50, 1);
            PutDword(Buf, Opt + 56, 0x2000);
            PutDword(Buf, Opt + 60, 0x200);
            PutWord(Buf, Opt + 68, Case->Kind == KIND_PE_NATIVE ? IMAGE_SUBSYSTEM_NATIVE : IMAGE_SUBSYSTEM_WINDOWS_CUI);
            if (Is64)
            {
                PutDword(Buf, Opt + 72, 0x100000);
                PutDword(Buf, Opt + 80, 0x1000);
                PutDword(Buf, Opt + 88, 0x100000);
                PutDword(Buf, Opt + 96, 0x1000);
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
            PutDword(Buf, Sect + 8, 0x1000);
            PutDword(Buf, Sect + 12, 0x1000);
            PutDword(Buf, Sect + 16, 0x200);
            PutDword(Buf, Sect + 20, 0x200);
            PutDword(Buf, Sect + 36, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ);
            Buf[0x200] = 0xC3;
            return 0x400;
    }
    return 0;
}

static BOOL WriteFixture(PCWSTR Path, const IMAGE_CASE *Case)
{
    BYTE Buf[0x400];
    ULONG Length;
    DWORD Written;
    HANDLE hFile;
    BOOL Ok;

    Length = BuildImage(Case, Buf, sizeof(Buf));
    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = WriteFile(hFile, Buf, Length, &Written, NULL) && Written == Length;
    CloseHandle(hFile);
    return Ok;
}

static void CheckResult(PCWSTR Name, const char *Api, BOOL Ret, DWORD Type, DWORD Error,
                        const IMAGE_CASE *Case)
{
    trace("%ls: %s ret %d type %lu error %lu\n", Name, Api, Ret, Type, Error);
    if (Case->Ret)
    {
        ok(Ret == TRUE, "%ls: %s failed, error %lu\n", Name, Api, Error);
        ok(Type == Case->Type, "%ls: %s type %lu, expected %lu\n", Name, Api, Type, Case->Type);
    }
    else
    {
        ok(Ret == FALSE, "%ls: %s succeeded with type %lu\n", Name, Api, Type);
        ok(Error == Case->Error, "%ls: %s error %lu, expected %lu\n", Name, Api, Error, Case->Error);
        ok(Type == NO_RESULT, "%ls: %s wrote the type %lu on failure\n", Name, Api, Type);
    }
}

static void TestCase(PCWSTR Dir, const IMAGE_CASE *Case)
{
    WCHAR PathW[MAX_PATH];
    CHAR PathA[MAX_PATH];
    DWORD Type, Error;
    BOOL Ret, UsedDefault = FALSE;

    if (FAILED(StringCchPrintfW(PathW, ARRAYSIZE(PathW), L"%ls\\%ls", Dir, Case->FileName)))
    {
        skip("%ls: the fixture path is too long\n", Case->Name);
        return;
    }
    if (!WriteFixture(PathW, Case))
    {
        skip("%ls: cannot write %ls, error %lu\n", Case->Name, PathW, GetLastError());
        return;
    }

    Type = NO_RESULT;
    SetLastError(0xDEAD);
    Ret = GetBinaryTypeW(PathW, &Type);
    Error = GetLastError();
    CheckResult(Case->Name, "GetBinaryTypeW", Ret, Type, Error, Case);

    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, PathW, -1, PathA, sizeof(PathA), NULL, &UsedDefault) &&
        !UsedDefault)
    {
        Type = NO_RESULT;
        SetLastError(0xDEAD);
        Ret = GetBinaryTypeA(PathA, &Type);
        Error = GetLastError();
        CheckResult(Case->Name, "GetBinaryTypeA", Ret, Type, Error, Case);
    }
    else
    {
        skip("%ls: the path does not convert to the ANSI code page\n", Case->Name);
    }

    DeleteFileW(PathW);
}

static void TestInvalid(PCWSTR Dir)
{
    WCHAR PathW[MAX_PATH];
    DWORD Type;
    BOOL Ret;

    Type = NO_RESULT;
    SetLastError(0xDEAD);
    Ret = GetBinaryTypeW(NULL, &Type);
    ok(Ret == FALSE, "NULL path succeeded\n");
    ok(GetLastError() == ERROR_INVALID_NAME, "NULL path: error %lu\n", GetLastError());
    ok(Type == NO_RESULT, "NULL path: wrote the type %lu\n", Type);

    Type = NO_RESULT;
    SetLastError(0xDEAD);
    Ret = GetBinaryTypeA(NULL, &Type);
    ok(Ret == FALSE, "NULL ANSI path succeeded\n");
    ok(GetLastError() == ERROR_INVALID_NAME, "NULL ANSI path: error %lu\n", GetLastError());
    ok(Type == NO_RESULT, "NULL ANSI path: wrote the type %lu\n", Type);

    if (SUCCEEDED(StringCchPrintfW(PathW, ARRAYSIZE(PathW), L"%ls\\missing.exe", Dir)))
    {
        DeleteFileW(PathW);
        Type = NO_RESULT;
        SetLastError(0xDEAD);
        Ret = GetBinaryTypeW(PathW, &Type);
        ok(Ret == FALSE, "missing file succeeded\n");
        ok(GetLastError() == ERROR_FILE_NOT_FOUND, "missing file: error %lu\n", GetLastError());
        ok(Type == NO_RESULT, "missing file: wrote the type %lu\n", Type);
    }
    else
    {
        skip("The path of the missing file is too long\n");
    }

    SetLastError(0xDEAD);
    Type = NO_RESULT;
    Ret = GetBinaryTypeW(Dir, &Type);
    ok(Ret == FALSE, "directory succeeded with type %lu\n", Type);
    ok(Type == NO_RESULT, "directory: wrote the type %lu\n", Type);
}

static void TestHost(void)
{
    WCHAR Path[MAX_PATH];
    DWORD Type = NO_RESULT;
    BOOL Ret;

    GetModuleFileNameW(NULL, Path, ARRAYSIZE(Path));
    Ret = GetBinaryTypeW(Path, &Type);
    ok(Ret == TRUE, "own image: failed, error %lu\n", GetLastError());
    ok(Type == (sizeof(void*) == 8 ? SCS_64BIT_BINARY : SCS_32BIT_BINARY),
       "own image: type %lu\n", Type);

    GetSystemDirectoryW(Path, ARRAYSIZE(Path));
    StringCchCatW(Path, ARRAYSIZE(Path), L"\\kernel32.dll");
    Type = NO_RESULT;
    SetLastError(0xDEAD);
    Ret = GetBinaryTypeW(Path, &Type);
    ok(Ret == FALSE, "kernel32.dll: succeeded with type %lu\n", Type);
    ok(GetLastError() == ERROR_BAD_EXE_FORMAT, "kernel32.dll: error %lu\n", GetLastError());
}

START_TEST(GetBinaryType)
{
    WCHAR Dir[MAX_PATH];
    ULONG i;

    if (!GetTempPathW(ARRAYSIZE(Dir), Dir))
    {
        skip("GetTempPathW failed, error %lu\n", GetLastError());
        return;
    }
    if (FAILED(StringCchPrintfW(Dir + wcslen(Dir), ARRAYSIZE(Dir) - wcslen(Dir), L"gbt%lu", GetCurrentProcessId())) ||
        !CreateDirectoryW(Dir, NULL))
    {
        skip("Cannot create the fixture directory %ls, error %lu\n", Dir, GetLastError());
        return;
    }

    for (i = 0; i < ARRAYSIZE(Cases); i++)
        TestCase(Dir, &Cases[i]);

    TestInvalid(Dir);
    TestHost();

    RemoveDirectoryW(Dir);
}
