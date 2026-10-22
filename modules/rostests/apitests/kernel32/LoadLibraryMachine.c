/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Loading a DLL built for another machine type
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ndk/mmfuncs.h>
#include <ndk/obfuncs.h>

#include <pshpack1.h>
typedef struct _FOREIGN_DLL32
{
    IMAGE_DOS_HEADER Dos;
    IMAGE_NT_HEADERS32 Nt;
    IMAGE_SECTION_HEADER Text;
} FOREIGN_DLL32;

typedef struct _FOREIGN_DLL64
{
    IMAGE_DOS_HEADER Dos;
    IMAGE_NT_HEADERS64 Nt;
    IMAGE_SECTION_HEADER Text;
} FOREIGN_DLL64;
#include <poppack.h>

#define FOREIGN_FILE_ALIGN  0x200
#define FOREIGN_SECT_ALIGN  0x1000

static void InitSection(PIMAGE_SECTION_HEADER Text, BOOL HasCode)
{
    memcpy(Text->Name, HasCode ? ".text" : ".rsrc", 5);
    Text->Misc.VirtualSize = FOREIGN_FILE_ALIGN;
    Text->VirtualAddress = FOREIGN_SECT_ALIGN;
    Text->SizeOfRawData = FOREIGN_FILE_ALIGN;
    Text->PointerToRawData = FOREIGN_FILE_ALIGN;
    Text->Characteristics = HasCode ? (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ)
                                    : (IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ);
}

/* A DLL without imports and without an entry point, so that nothing of it
 * would run even if it were loaded. With HasCode it declares a code size,
 * which is what makes Windows treat it as an image with code */
static BOOL WriteForeignDll(PCWSTR Path, BOOL HasCode, WORD *Machine)
{
    BYTE Image[2 * FOREIGN_FILE_ALIGN] = { 0 };
    PIMAGE_DOS_HEADER Dos = (PIMAGE_DOS_HEADER)Image;
    HANDLE hFile;
    DWORD Written;
    BOOL Ret;

    Dos->e_magic = IMAGE_DOS_SIGNATURE;
    Dos->e_lfanew = FIELD_OFFSET(FOREIGN_DLL32, Nt);
#ifdef _WIN64
    {
        FOREIGN_DLL32 *Dll = (FOREIGN_DLL32 *)Image;
        C_ASSERT(sizeof(FOREIGN_DLL32) <= FOREIGN_FILE_ALIGN);
        Dll->Nt.Signature = IMAGE_NT_SIGNATURE;
        Dll->Nt.FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
        Dll->Nt.FileHeader.NumberOfSections = 1;
        Dll->Nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
        Dll->Nt.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE |
                                             IMAGE_FILE_DLL;
        Dll->Nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
        Dll->Nt.OptionalHeader.SizeOfCode = HasCode ? FOREIGN_FILE_ALIGN : 0;
        Dll->Nt.OptionalHeader.BaseOfCode = HasCode ? FOREIGN_SECT_ALIGN : 0;
        Dll->Nt.OptionalHeader.ImageBase = 0x10000000;
        Dll->Nt.OptionalHeader.SectionAlignment = FOREIGN_SECT_ALIGN;
        Dll->Nt.OptionalHeader.FileAlignment = FOREIGN_FILE_ALIGN;
        Dll->Nt.OptionalHeader.MajorOperatingSystemVersion = 5;
        Dll->Nt.OptionalHeader.MajorSubsystemVersion = 5;
        Dll->Nt.OptionalHeader.SizeOfImage = 2 * FOREIGN_SECT_ALIGN;
        Dll->Nt.OptionalHeader.SizeOfHeaders = FOREIGN_FILE_ALIGN;
        Dll->Nt.OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
        Dll->Nt.OptionalHeader.SizeOfStackReserve = 0x100000;
        Dll->Nt.OptionalHeader.SizeOfStackCommit = 0x1000;
        Dll->Nt.OptionalHeader.SizeOfHeapReserve = 0x100000;
        Dll->Nt.OptionalHeader.SizeOfHeapCommit = 0x1000;
        Dll->Nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        InitSection(&Dll->Text, HasCode);
        *Machine = IMAGE_FILE_MACHINE_I386;
    }
#else
    {
        FOREIGN_DLL64 *Dll = (FOREIGN_DLL64 *)Image;
        C_ASSERT(sizeof(FOREIGN_DLL64) <= FOREIGN_FILE_ALIGN);
        Dll->Nt.Signature = IMAGE_NT_SIGNATURE;
        Dll->Nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        Dll->Nt.FileHeader.NumberOfSections = 1;
        Dll->Nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        Dll->Nt.FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE |
                                             IMAGE_FILE_DLL;
        Dll->Nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        Dll->Nt.OptionalHeader.SizeOfCode = HasCode ? FOREIGN_FILE_ALIGN : 0;
        Dll->Nt.OptionalHeader.BaseOfCode = HasCode ? FOREIGN_SECT_ALIGN : 0;
        Dll->Nt.OptionalHeader.ImageBase = 0x180000000ULL;
        Dll->Nt.OptionalHeader.SectionAlignment = FOREIGN_SECT_ALIGN;
        Dll->Nt.OptionalHeader.FileAlignment = FOREIGN_FILE_ALIGN;
        Dll->Nt.OptionalHeader.MajorOperatingSystemVersion = 5;
        Dll->Nt.OptionalHeader.MajorSubsystemVersion = 5;
        Dll->Nt.OptionalHeader.SizeOfImage = 2 * FOREIGN_SECT_ALIGN;
        Dll->Nt.OptionalHeader.SizeOfHeaders = FOREIGN_FILE_ALIGN;
        Dll->Nt.OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI;
        Dll->Nt.OptionalHeader.SizeOfStackReserve = 0x100000;
        Dll->Nt.OptionalHeader.SizeOfStackCommit = 0x1000;
        Dll->Nt.OptionalHeader.SizeOfHeapReserve = 0x100000;
        Dll->Nt.OptionalHeader.SizeOfHeapCommit = 0x1000;
        Dll->Nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        InitSection(&Dll->Text, HasCode);
        *Machine = IMAGE_FILE_MACHINE_AMD64;
    }
#endif
    Image[FOREIGN_FILE_ALIGN] = 0xC3;

    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    Ret = WriteFile(hFile, Image, sizeof(Image), &Written, NULL) && Written == sizeof(Image);
    CloseHandle(hFile);
    return Ret;
}

static void Test_ForeignDll(PCWSTR Path, BOOL HasCode)
{
    HMODULE hMod;
    DWORD Flags[] = { 0, DONT_RESOLVE_DLL_REFERENCES };
    ULONG i;

    for (i = 0; i < _countof(Flags); i++)
    {
        SetLastError(0xdeadbeef);
        hMod = LoadLibraryExW(Path, NULL, Flags[i]);
#ifdef _WIN64
        if (!HasCode)
        {
            /* Without code a 32-bit DLL loads, as a resource DLL does */
            ok(hMod != NULL, "Flags 0x%lx: a 32-bit DLL without code failed: %lu\n", Flags[i], GetLastError());
            if (hMod)
                FreeLibrary(hMod);
            continue;
        }
#endif
        ok(hMod == NULL, "Flags 0x%lx: a DLL for another machine was loaded at %p\n", Flags[i], hMod);
        ok(GetLastError() == ERROR_BAD_EXE_FORMAT, "Flags 0x%lx: error %lu\n", Flags[i], GetLastError());
        ok(GetModuleHandleW(Path) == NULL, "Flags 0x%lx: the DLL stays listed\n", Flags[i]);
        if (hMod)
            FreeLibrary(hMod);
    }

    /* Resources of such a DLL stay readable */
    hMod = LoadLibraryExW(Path, NULL, LOAD_LIBRARY_AS_DATAFILE);
    ok(hMod != NULL, "LOAD_LIBRARY_AS_DATAFILE failed: %lu\n", GetLastError());
    if (hMod)
        FreeLibrary(hMod);
}

static void Test_ForeignSection(PCWSTR Path, WORD Machine, BOOL HasCode)
{
    HANDLE hFile, hSection;
    PVOID Base = NULL, Base2 = NULL;
    SIZE_T ViewSize = 0;
    NTSTATUS Status, Expected;
    SECTION_IMAGE_INFORMATION ImageInfo;
#ifndef _WIN64
    BOOL bWow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &bWow64);
#endif

    hFile = CreateFileW(Path, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed: %lu\n", GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    Status = NtCreateSection(&hSection, SECTION_ALL_ACCESS, NULL, NULL, PAGE_EXECUTE_READ, SEC_IMAGE, hFile);
    CloseHandle(hFile);
#ifndef _WIN64
    /* A 32-bit system refuses a 64-bit image here; WOW64 does not */
    if (!bWow64)
    {
        ok_ntstatus(Status, STATUS_INVALID_IMAGE_WIN_64);
        if (NT_SUCCESS(Status))
            NtClose(hSection);
        return;
    }
#endif
    ok(NT_SUCCESS(Status), "NtCreateSection failed: 0x%lx\n", Status);
    if (!NT_SUCCESS(Status))
        return;

    Status = NtQuerySection(hSection, SectionImageInformation, &ImageInfo, sizeof(ImageInfo), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        ok_hex(ImageInfo.Machine, Machine);
        ok(!ImageInfo.ImageContainsCode == !HasCode, "ImageContainsCode %u\n", ImageInfo.ImageContainsCode);
    }

#ifdef _WIN64
    /* An image with code is mapped, but the status says it is foreign */
    Expected = HasCode ? STATUS_IMAGE_MACHINE_TYPE_MISMATCH : STATUS_SUCCESS;
#else
    /* WOW64 maps the 64-bit image as a native one, above its 4 GB base */
    Expected = STATUS_IMAGE_NOT_AT_BASE;
#endif
    Status = NtMapViewOfSection(hSection, NtCurrentProcess(), &Base, 0, 0, NULL, &ViewSize,
                                ViewShare, 0, PAGE_READONLY);
#ifdef _WIN64
    /* Windows 10 relocates the image without code even when its base is free */
    if (!HasCode && Status == STATUS_IMAGE_NOT_AT_BASE)
        Expected = Status;
#endif
    ok_ntstatus(Status, Expected);
    if (NT_SUCCESS(Status))
    {
        ok(Base != NULL, "No view\n");
        ok(*(PUSHORT)Base == IMAGE_DOS_SIGNATURE, "Header not mapped\n");

        /* A second view cannot use the image base; a mismatch still wins */
#ifdef _WIN64
        Expected = HasCode ? STATUS_IMAGE_MACHINE_TYPE_MISMATCH : STATUS_IMAGE_NOT_AT_BASE;
#endif
        ViewSize = 0;
        Status = NtMapViewOfSection(hSection, NtCurrentProcess(), &Base2, 0, 0, NULL, &ViewSize,
                                    ViewShare, 0, PAGE_READONLY);
        ok_ntstatus(Status, Expected);
        if (NT_SUCCESS(Status))
            NtUnmapViewOfSection(NtCurrentProcess(), Base2);
        NtUnmapViewOfSection(NtCurrentProcess(), Base);
    }
    NtClose(hSection);
}

START_TEST(LoadLibraryMachine)
{
    WCHAR TempDir[MAX_PATH], Path[MAX_PATH];
    WORD Machine;
    BOOL HasCode;
    ULONG i;

    GetTempPathW(_countof(TempDir), TempDir);
    for (i = 0; i < 2; i++)
    {
        HasCode = (i == 0);
        StringCchPrintfW(Path, _countof(Path), L"%sforeign_machine_%lu_%d.dll", TempDir, GetCurrentProcessId(), HasCode);
        if (!WriteForeignDll(Path, HasCode, &Machine))
        {
            skip("Cannot write %S: %lu\n", Path, GetLastError());
            continue;
        }

        trace("DLL %s code\n", HasCode ? "with" : "without");
        Test_ForeignSection(Path, Machine, HasCode);
        Test_ForeignDll(Path, HasCode);

        ok(DeleteFileW(Path), "DeleteFileW failed: %lu\n", GetLastError());
    }
}
