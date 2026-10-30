/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     A dynamic LoadLibrary of a DLL whose static import cannot be
 *              resolved fails with ERROR_MOD_NOT_FOUND / ERROR_PROC_NOT_FOUND
 *              and never raises a hard error (no "Unable To Locate Component"
 *              dialog that blocks the calling thread)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <pshpack1.h>
typedef struct _TESTIMG
{
    IMAGE_DOS_HEADER Dos;
#ifdef _WIN64
    IMAGE_NT_HEADERS64 Nt;
#else
    IMAGE_NT_HEADERS32 Nt;
#endif
    IMAGE_SECTION_HEADER Sect;
} TESTIMG;
#include <poppack.h>

#define IMG_FILE_ALIGN  0x200
#define IMG_SECT_ALIGN  0x1000

/* Section layout, offsets from the section start */
#define OFF_IMPORTS     0x000   /* IMAGE_IMPORT_DESCRIPTOR[2] */
#define OFF_ILT         0x040
#define OFF_IAT         0x060
#define OFF_HINTNAME    0x080
#define OFF_DLLNAME     0x0C0
#define OFF_CODE        0x100
#define OFF_EXPORT      0x200   /* IMAGE_EXPORT_DIRECTORY, one export "Foo" */
#define OFF_EXPFUNCS    0x240
#define OFF_EXPNAMES    0x250
#define OFF_EXPORDS     0x260
#define OFF_EXPSTR      0x270   /* "Foo", then the DLL name */
#define SECT_SIZE       0x400

#ifdef _WIN64
typedef ULONGLONG THUNK;
#define ORD_FLAG        IMAGE_ORDINAL_FLAG64
#else
typedef ULONG THUNK;
#define ORD_FLAG        IMAGE_ORDINAL_FLAG32
#endif

#define MISSING_ORDINAL 0x7ff0

/* A minimal image with one import: Name (or Ordinal when Name is NULL) of
 * ImportDll. A DLL has no entry point; an EXE returns 0 at once */
static BOOL WriteImage(PCWSTR Path, BOOL IsDll, PCSTR ImportDll, PCSTR Name, WORD Ordinal)
{
    BYTE Image[IMG_FILE_ALIGN + SECT_SIZE] = { 0 };
    TESTIMG *Img = (TESTIMG *)Image;
    PBYTE Sect = Image + IMG_FILE_ALIGN;
    PIMAGE_IMPORT_DESCRIPTOR Imp = (PIMAGE_IMPORT_DESCRIPTOR)(Sect + OFF_IMPORTS);
    THUNK *Ilt = (THUNK *)(Sect + OFF_ILT), *Iat = (THUNK *)(Sect + OFF_IAT);
    HANDLE hFile;
    DWORD Written;
    BOOL Ret;

    C_ASSERT(sizeof(TESTIMG) <= IMG_FILE_ALIGN);
    Img->Dos.e_magic = IMAGE_DOS_SIGNATURE;
    Img->Dos.e_lfanew = FIELD_OFFSET(TESTIMG, Nt);
    Img->Nt.Signature = IMAGE_NT_SIGNATURE;
#ifdef _WIN64
    Img->Nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    Img->Nt.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE;
    Img->Nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    Img->Nt.OptionalHeader.ImageBase = IsDll ? 0x180000000ULL : 0x140000000ULL;
#else
    Img->Nt.FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    Img->Nt.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE;
    Img->Nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    Img->Nt.OptionalHeader.ImageBase = IsDll ? 0x10000000 : 0x00400000;
#endif
    if (IsDll)
        Img->Nt.FileHeader.Characteristics |= IMAGE_FILE_DLL;
    Img->Nt.FileHeader.NumberOfSections = 1;
    Img->Nt.FileHeader.SizeOfOptionalHeader = sizeof(Img->Nt.OptionalHeader);
    Img->Nt.OptionalHeader.SizeOfCode = SECT_SIZE;
    Img->Nt.OptionalHeader.BaseOfCode = IMG_SECT_ALIGN;
    Img->Nt.OptionalHeader.AddressOfEntryPoint = IsDll ? 0 : IMG_SECT_ALIGN + OFF_CODE;
    Img->Nt.OptionalHeader.SectionAlignment = IMG_SECT_ALIGN;
    Img->Nt.OptionalHeader.FileAlignment = IMG_FILE_ALIGN;
    Img->Nt.OptionalHeader.MajorOperatingSystemVersion = 5;
    Img->Nt.OptionalHeader.MajorSubsystemVersion = 5;
    Img->Nt.OptionalHeader.SizeOfImage = 2 * IMG_SECT_ALIGN;
    Img->Nt.OptionalHeader.SizeOfHeaders = IMG_FILE_ALIGN;
    Img->Nt.OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    Img->Nt.OptionalHeader.SizeOfStackReserve = 0x100000;
    Img->Nt.OptionalHeader.SizeOfStackCommit = 0x1000;
    Img->Nt.OptionalHeader.SizeOfHeapReserve = 0x100000;
    Img->Nt.OptionalHeader.SizeOfHeapCommit = 0x1000;
    Img->Nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    Img->Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = IMG_SECT_ALIGN + OFF_IMPORTS;
    Img->Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = 2 * sizeof(*Imp);
    if (IsDll)
    {
        PIMAGE_EXPORT_DIRECTORY Exp = (PIMAGE_EXPORT_DIRECTORY)(Sect + OFF_EXPORT);

        Img->Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress = IMG_SECT_ALIGN + OFF_EXPORT;
        Img->Nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size = 0x80;
        Exp->Name = IMG_SECT_ALIGN + OFF_EXPSTR + 4;
        Exp->Base = 1;
        Exp->NumberOfFunctions = Exp->NumberOfNames = 1;
        Exp->AddressOfFunctions = IMG_SECT_ALIGN + OFF_EXPFUNCS;
        Exp->AddressOfNames = IMG_SECT_ALIGN + OFF_EXPNAMES;
        Exp->AddressOfNameOrdinals = IMG_SECT_ALIGN + OFF_EXPORDS;
        *(PULONG)(Sect + OFF_EXPFUNCS) = IMG_SECT_ALIGN + OFF_CODE;
        *(PULONG)(Sect + OFF_EXPNAMES) = IMG_SECT_ALIGN + OFF_EXPSTR;
        memcpy(Sect + OFF_EXPSTR, "Foo\0ldrmi.dll", 14);
    }
    memcpy(Img->Sect.Name, ".idata", 6);
    Img->Sect.Misc.VirtualSize = SECT_SIZE;
    Img->Sect.VirtualAddress = IMG_SECT_ALIGN;
    Img->Sect.SizeOfRawData = SECT_SIZE;
    Img->Sect.PointerToRawData = IMG_FILE_ALIGN;
    Img->Sect.Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_CNT_INITIALIZED_DATA |
                                IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;

    Imp[0].OriginalFirstThunk = IMG_SECT_ALIGN + OFF_ILT;
    Imp[0].Name = IMG_SECT_ALIGN + OFF_DLLNAME;
    Imp[0].FirstThunk = IMG_SECT_ALIGN + OFF_IAT;
    StringCchCopyA((char *)(Sect + OFF_DLLNAME), 0x40, ImportDll);
    if (Name)
    {
        StringCchCopyA((char *)(Sect + OFF_HINTNAME + 2), 0x3E, Name);
        Ilt[0] = Iat[0] = IMG_SECT_ALIGN + OFF_HINTNAME;
    }
    else
    {
        Ilt[0] = Iat[0] = ORD_FLAG | Ordinal;
    }

    /* xor eax, eax; ret (x86: ret 4, the thread start routine has a parameter) */
    Sect[OFF_CODE + 0] = 0x31;
    Sect[OFF_CODE + 1] = 0xC0;
#ifdef _WIN64
    Sect[OFF_CODE + 2] = 0xC3;
#else
    Sect[OFF_CODE + 2] = 0xC2;
    Sect[OFF_CODE + 3] = 0x04;
#endif

    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    Ret = WriteFile(hFile, Image, sizeof(Image), &Written, NULL) && Written == sizeof(Image);
    CloseHandle(hFile);
    return Ret;
}

typedef struct _LOADCASE
{
    PCWSTR File;
    BOOL IsDll;
    PCSTR ImportDll;
    PCSTR Name;
    WORD Ordinal;
    DWORD LoadError;        /* LoadLibrary */
    LONG RuntimeHardErrors; /* NtRaiseHardError calls during LoadLibrary */
    NTSTATUS StartStatus;   /* exit code of an EXE that imports the DLL */
} LOADCASE;

static const LOADCASE Cases[] =
{
    /* Control: every import resolves */
    { L"ldrmi_ok.dll",     TRUE, "kernel32.dll", "GetTickCount", 0, 0, 0, 0 },
    /* The static import is a DLL that does not exist */
    { L"ldrmi_nodll.dll",  TRUE, "ldrmi_nonexistent_dep.dll", "Foo",  0, ERROR_MOD_NOT_FOUND, 0, STATUS_DLL_NOT_FOUND },
    /* The DLL exists, the named export does not */
    { L"ldrmi_noname.dll", TRUE, "kernel32.dll", "LdrMiNoSuchExport", 0, ERROR_PROC_NOT_FOUND, 1, STATUS_ENTRYPOINT_NOT_FOUND },
    /* The DLL exists, the ordinal does not */
    { L"ldrmi_noord.dll",  TRUE, "kernel32.dll", NULL, MISSING_ORDINAL, ERROR_INVALID_ORDINAL, 1, STATUS_ORDINAL_NOT_FOUND },
    /* The failure is one level down: this DLL imports ldrmi_nodll.dll */
    { L"ldrmi_chain.dll",  TRUE, "ldrmi_nodll.dll", "Foo", 0,        ERROR_MOD_NOT_FOUND, 0, STATUS_DLL_NOT_FOUND },
};

#define LOAD_TIMEOUT    20000

typedef struct _LOADREQ
{
    WCHAR Path[MAX_PATH];
    DWORD Flags;
    HMODULE Module;
    DWORD Error;
} LOADREQ;

static
DWORD
WINAPI
LoadThread(LPVOID Param)
{
    LOADREQ *Req = Param;

    SetLastError(0xdeadbeef);
    Req->Module = LoadLibraryExW(Req->Path, NULL, Req->Flags);
    Req->Error = GetLastError();
    return 0;
}

/* The load runs in its own thread so that a hard error that never returns
 * costs a timeout instead of the whole test run. Returns FALSE on a hang */
static
BOOL
LoadGuarded(LOADREQ *Req)
{
    HANDLE Thread = CreateThread(NULL, 0, LoadThread, Req, 0, NULL);

    if (!Thread)
        return FALSE;
    if (WaitForSingleObject(Thread, LOAD_TIMEOUT) != WAIT_OBJECT_0)
    {
        /* Leak the thread and its request: it is stuck in the loader */
        return FALSE;
    }
    CloseHandle(Thread);
    return TRUE;
}


/*
 * Counting hook on NtRaiseHardError/ZwRaiseHardError of this process: the
 * error is answered "return to caller" instead of being sent to the system,
 * so a loader that raises it is caught without a dialog or a hang
 */
static LONG HardErrorCount;
static NTSTATUS HardErrorLast;

static
NTSTATUS
NTAPI
HookedRaiseHardError(NTSTATUS ErrorStatus,
                     ULONG NumberOfParameters,
                     ULONG UnicodeStringParameterMask,
                     PULONG_PTR Parameters,
                     ULONG ValidResponseOptions,
                     PULONG Response)
{
    InterlockedIncrement(&HardErrorCount);
    HardErrorLast = ErrorStatus;
    if (Response)
        *Response = 0; /* ResponseReturnToCaller */
    return 0;
}

#ifdef _WIN64
#define PATCH_SIZE 12
#else
#define PATCH_SIZE 5
#endif

typedef struct _PATCH
{
    PBYTE Target;
    BYTE Saved[PATCH_SIZE];
    BOOL Active;
} PATCH;

static
BOOL
PatchInstall(PATCH *Patch, PCSTR Name)
{
    BYTE Code[PATCH_SIZE];
    DWORD OldProtect;

    Patch->Active = FALSE;
    Patch->Target = (PBYTE)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), Name);
    if (!Patch->Target)
        return FALSE;
#ifdef _WIN64
    Code[0] = 0x48; Code[1] = 0xB8;                         /* mov rax, imm64 */
    *(PULONGLONG)&Code[2] = (ULONGLONG)(ULONG_PTR)HookedRaiseHardError;
    Code[10] = 0xFF; Code[11] = 0xE0;                       /* jmp rax */
#else
    Code[0] = 0xE9;                                         /* jmp rel32 */
    *(PLONG)&Code[1] = (LONG)((PBYTE)HookedRaiseHardError - (Patch->Target + 5));
#endif
    if (!VirtualProtect(Patch->Target, PATCH_SIZE, PAGE_EXECUTE_READWRITE, &OldProtect))
        return FALSE;
    memcpy(Patch->Saved, Patch->Target, PATCH_SIZE);
    memcpy(Patch->Target, Code, PATCH_SIZE);
    FlushInstructionCache(GetCurrentProcess(), Patch->Target, PATCH_SIZE);
    VirtualProtect(Patch->Target, PATCH_SIZE, OldProtect, &OldProtect);
    Patch->Active = TRUE;
    return TRUE;
}

static
VOID
PatchRemove(PATCH *Patch)
{
    DWORD OldProtect;

    if (!Patch->Active)
        return;
    VirtualProtect(Patch->Target, PATCH_SIZE, PAGE_EXECUTE_READWRITE, &OldProtect);
    memcpy(Patch->Target, Patch->Saved, PATCH_SIZE);
    FlushInstructionCache(GetCurrentProcess(), Patch->Target, PATCH_SIZE);
    VirtualProtect(Patch->Target, PATCH_SIZE, OldProtect, &OldProtect);
    Patch->Active = FALSE;
}

static BOOL Hung;

/* Loads every case with the given error mode. With Hooked, the hard errors
 * are counted and answered; without, a loader that raises one for a case that
 * must not shows up as a hang (cases that Windows reports to the system are
 * only loaded with SEM_FAILCRITICALERRORS, which makes the system ignore them) */
static
VOID
TestDynamic(PCWSTR Dir, UINT ErrorMode, BOOL Hooked)
{
    UINT OldMode;
    ULONG i;

    OldMode = SetErrorMode(ErrorMode);
    for (i = 0; i < _countof(Cases) && !Hung; i++)
    {
        static LOADREQ Req; /* static: outlives a stuck thread */
        LONG Before = HardErrorCount;
        BOOL Done;

        if (!Hooked && !ErrorMode && Cases[i].RuntimeHardErrors)
            continue;

        ZeroMemory(&Req, sizeof(Req));
        StringCchPrintfW(Req.Path, _countof(Req.Path), L"%ls\\%ls", Dir, Cases[i].File);
        /* The directory of the DLL is searched for its imports */
        Req.Flags = LOAD_WITH_ALTERED_SEARCH_PATH;
        Done = LoadGuarded(&Req);
        ok(Done, "[mode 0x%x] LoadLibraryW(%ls) did not return within %lu ms (hard error?)\n",
           ErrorMode, Cases[i].File, (ULONG)LOAD_TIMEOUT);
        if (!Done)
        {
            Hung = TRUE;
            break;
        }
        if (Hooked)
        {
            ok(HardErrorCount - Before == Cases[i].RuntimeHardErrors,
               "[mode 0x%x] LoadLibraryW(%ls) raised %ld hard errors (last 0x%lx), expected %ld\n",
               ErrorMode, Cases[i].File, HardErrorCount - Before, (ULONG)HardErrorLast, Cases[i].RuntimeHardErrors);
        }
        if (!Cases[i].LoadError)
        {
            ok(Req.Module != NULL, "[mode 0x%x] LoadLibraryW(%ls) failed: %lu\n", ErrorMode, Cases[i].File, Req.Error);
            if (Req.Module)
                FreeLibrary(Req.Module);
            continue;
        }
        ok(Req.Module == NULL, "[mode 0x%x] LoadLibraryW(%ls) succeeded: %p\n", ErrorMode, Cases[i].File, Req.Module);
        ok(Req.Error == Cases[i].LoadError, "[mode 0x%x] LoadLibraryW(%ls) error %lu, expected %lu\n",
           ErrorMode, Cases[i].File, Req.Error, Cases[i].LoadError);
        if (Req.Module)
            FreeLibrary(Req.Module);
        ok(GetModuleHandleW(Cases[i].File) == NULL, "[mode 0x%x] %ls stays loaded\n", ErrorMode, Cases[i].File);
    }
    SetErrorMode(OldMode);
}

/* Nothing is resolved with DONT_RESOLVE_DLL_REFERENCES, so these load */
static
VOID
TestNoResolve(PCWSTR Dir)
{
    ULONG i;

    for (i = 0; i < _countof(Cases); i++)
    {
        static LOADREQ Req;

        ZeroMemory(&Req, sizeof(Req));
        StringCchPrintfW(Req.Path, _countof(Req.Path), L"%ls\\%ls", Dir, Cases[i].File);
        Req.Flags = DONT_RESOLVE_DLL_REFERENCES | LOAD_WITH_ALTERED_SEARCH_PATH;
        if (!LoadGuarded(&Req))
        {
            ok(0, "LoadLibraryExW(%ls, DONT_RESOLVE_DLL_REFERENCES) hung\n", Cases[i].File);
            Hung = TRUE;
            return;
        }
        /* The chain DLL imports a DLL name only; nothing is mapped for it either */
        ok(Req.Module != NULL, "LoadLibraryExW(%ls, DONT_RESOLVE_DLL_REFERENCES) failed: %lu\n",
           Cases[i].File, Req.Error);
        if (Req.Module)
            FreeLibrary(Req.Module);
    }
}

/* A process whose static import fails ends with the loader's status.
 * The error mode of this process is inherited by the child */
static
VOID
TestStartup(PCWSTR Dir, UINT ErrorMode)
{
    WCHAR ExePath[MAX_PATH], CmdLine[MAX_PATH + 4];
    STARTUPINFOW Si;
    PROCESS_INFORMATION Pi;
    UINT OldMode;
    DWORD Code, Wait;
    ULONG i;

    OldMode = SetErrorMode(ErrorMode);
    for (i = 0; i < _countof(Cases); i++)
    {
        StringCchPrintfW(ExePath, _countof(ExePath), L"%ls\\ldrmi_exe%lu.exe", Dir, i);
        {
            CHAR Dll[64];
            WideCharToMultiByte(CP_ACP, 0, Cases[i].File, -1, Dll, sizeof(Dll), NULL, NULL);
            ok(WriteImage(ExePath, FALSE, Dll, "Foo", 0), "Cannot write %ls: %lu\n", ExePath, GetLastError());
        }
        ZeroMemory(&Si, sizeof(Si));
        Si.cb = sizeof(Si);
        StringCchPrintfW(CmdLine, _countof(CmdLine), L"\"%ls\"", ExePath);
        if (!CreateProcessW(NULL, CmdLine, NULL, NULL, FALSE, 0, NULL, Dir, &Si, &Pi))
        {
            ok(0, "CreateProcessW(%ls) failed: %lu\n", ExePath, GetLastError());
            continue;
        }
        Wait = WaitForSingleObject(Pi.hProcess, LOAD_TIMEOUT);
        ok(Wait == WAIT_OBJECT_0, "[mode 0x%x] %ls did not end within %lu ms\n", ErrorMode, ExePath, (ULONG)LOAD_TIMEOUT);
        if (Wait != WAIT_OBJECT_0)
        {
            TerminateProcess(Pi.hProcess, 0xdead);
            WaitForSingleObject(Pi.hProcess, 5000);
        }
        else
        {
            Code = 0xdeadbeef;
            GetExitCodeProcess(Pi.hProcess, &Code);
            ok(Code == (DWORD)Cases[i].StartStatus, "[mode 0x%x] %ls exit code 0x%lx, expected 0x%lx\n",
               ErrorMode, Cases[i].File, Code, (DWORD)Cases[i].StartStatus);
        }
        CloseHandle(Pi.hThread);
        CloseHandle(Pi.hProcess);
        DeleteFileW(ExePath);
    }
    SetErrorMode(OldMode);
}

START_TEST(LoadLibraryMissingImport)
{
    WCHAR Dir[MAX_PATH], Path[MAX_PATH];
    DWORD Length;
    ULONG i;
    BOOL Written = TRUE;

    Length = GetTempPathW(_countof(Dir), Dir);
    if (!Length || FAILED(StringCchPrintfW(Dir + Length, _countof(Dir) - Length, L"ldrmi-%lu", GetCurrentProcessId())) ||
        !CreateDirectoryW(Dir, NULL))
    {
        skip("Cannot create the test directory: %lu\n", GetLastError());
        return;
    }

    for (i = 0; i < _countof(Cases); i++)
    {
        StringCchPrintfW(Path, _countof(Path), L"%ls\\%ls", Dir, Cases[i].File);
        if (!WriteImage(Path, TRUE, Cases[i].ImportDll, Cases[i].Name, Cases[i].Ordinal))
            Written = FALSE;
    }
    ok(Written, "Cannot write the test DLLs: %lu\n", GetLastError());

    if (Written)
    {
        PATCH NtPatch, ZwPatch;
        BOOL Hooked;

        /*
         * Windows reports a missing import DLL to the system only while a
         * process starts, never for a LoadLibrary. A missing export is
         * reported in both cases, and the process error mode decides whether
         * the system shows it. First count what the loader raises
         */
        Hooked = PatchInstall(&NtPatch, "NtRaiseHardError");
        if (Hooked)
        {
            if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "ZwRaiseHardError") != (FARPROC)NtPatch.Target)
                PatchInstall(&ZwPatch, "ZwRaiseHardError");
            else
                ZwPatch.Active = FALSE;
            TestDynamic(Dir, 0, TRUE);
            TestDynamic(Dir, SEM_FAILCRITICALERRORS, TRUE);
            PatchRemove(&ZwPatch);
            PatchRemove(&NtPatch);
        }
        else
        {
            skip("Cannot hook NtRaiseHardError\n");
        }
        /* Then let the real thing happen: it must return, not wait for a click */
        TestDynamic(Dir, 0, FALSE);
        TestDynamic(Dir, SEM_FAILCRITICALERRORS, FALSE);
        if (!Hung)
            TestNoResolve(Dir);
        if (!Hung)
            TestStartup(Dir, SEM_FAILCRITICALERRORS);
    }

    for (i = 0; i < _countof(Cases); i++)
    {
        StringCchPrintfW(Path, _countof(Path), L"%ls\\%ls", Dir, Cases[i].File);
        DeleteFileW(Path);
    }
    RemoveDirectoryW(Dir);

    if (Hung)
    {
        /* The stuck thread owns the loader lock, so ExitProcess would wait for it too */
        TerminateProcess(GetCurrentProcess(), 1);
    }
}
