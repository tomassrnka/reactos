/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for LoadLibraryExW of an IL-only .NET assembly built for any CPU (a PE32 file)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * A 64-bit runtime (CoreCLR) loads such an assembly with LoadLibraryExW and reads it as data. Windows loads it in a
 * 64-bit process with its headers rewritten to PE32+ (the machine stays i386), without relocating it, snapping its
 * imports or calling its entry point. An assembly that requires 32 bits does not load there.
 */

#include "precomp.h"

#define IL_IMAGE_BASE   0x10000000UL
#define IL_TEXT_RVA     0x1000
#define IL_RELOC_RVA    0x2000
#define IL_FILE_ALIGN   0x200

/* COMIMAGE_FLAGS_* */
#define IL_ILONLY           0x00000001
#define IL_32BITREQUIRED    0x00000002
#define IL_32BITPREFERRED   0x00020000

typedef struct _IL_COR20_HEADER
{
    DWORD cb;
    WORD MajorRuntimeVersion;
    WORD MinorRuntimeVersion;
    IMAGE_DATA_DIRECTORY MetaData;
    DWORD Flags;
    DWORD EntryPointToken;
    IMAGE_DATA_DIRECTORY Resources;
    IMAGE_DATA_DIRECTORY StrongNameSignature;
    IMAGE_DATA_DIRECTORY CodeManagerTable;
    IMAGE_DATA_DIRECTORY VTableFixups;
    IMAGE_DATA_DIRECTORY ExportAddressTableJumps;
    IMAGE_DATA_DIRECTORY ManagedNativeHeader;
} IL_COR20_HEADER;

C_ASSERT(sizeof(IL_COR20_HEADER) == 0x48);

/* A PE32 DLL with a COM descriptor, a metadata root and an empty relocation block; no code at all */
static
BOOL
WriteIlImage(
    _In_ PCWSTR Path,
    _In_ DWORD CorFlags,
    _In_ DWORD ComRva,
    _In_ DWORD SizeOfImage)
{
    BYTE Image[3 * IL_FILE_ALIGN];
    PIMAGE_DOS_HEADER Dos = (PIMAGE_DOS_HEADER)Image;
    PIMAGE_NT_HEADERS32 Nt = (PIMAGE_NT_HEADERS32)(Image + 0x80);
    PIMAGE_SECTION_HEADER Section;
    IL_COR20_HEADER *Cor = (IL_COR20_HEADER *)(Image + IL_FILE_ALIGN);
    PBYTE Metadata = (PBYTE)(Cor + 1);
    PIMAGE_BASE_RELOCATION Reloc = (PIMAGE_BASE_RELOCATION)(Image + 2 * IL_FILE_ALIGN);
    HANDLE File;
    DWORD Written;
    BOOL Ok;

    ZeroMemory(Image, sizeof(Image));
    Dos->e_magic = IMAGE_DOS_SIGNATURE;
    Dos->e_lfanew = 0x80;

    Nt->Signature = IMAGE_NT_SIGNATURE;
    Nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    Nt->FileHeader.NumberOfSections = 2;
    Nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
    Nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE | IMAGE_FILE_DLL;
    Nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    Nt->OptionalHeader.MajorLinkerVersion = 8;
    Nt->OptionalHeader.SizeOfCode = IL_FILE_ALIGN;
    Nt->OptionalHeader.SizeOfInitializedData = IL_FILE_ALIGN;
    Nt->OptionalHeader.BaseOfCode = IL_TEXT_RVA;
    Nt->OptionalHeader.BaseOfData = IL_RELOC_RVA;
    Nt->OptionalHeader.ImageBase = IL_IMAGE_BASE;
    Nt->OptionalHeader.SectionAlignment = 0x1000;
    Nt->OptionalHeader.FileAlignment = IL_FILE_ALIGN;
    Nt->OptionalHeader.MajorOperatingSystemVersion = 4;
    Nt->OptionalHeader.MajorSubsystemVersion = 4;
    Nt->OptionalHeader.SizeOfImage = SizeOfImage;
    Nt->OptionalHeader.SizeOfHeaders = IL_FILE_ALIGN;
    Nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
    Nt->OptionalHeader.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE | IMAGE_DLLCHARACTERISTICS_NX_COMPAT |
                                            IMAGE_DLLCHARACTERISTICS_NO_SEH;
    Nt->OptionalHeader.SizeOfStackReserve = 0x100000;
    Nt->OptionalHeader.SizeOfStackCommit = 0x1000;
    Nt->OptionalHeader.SizeOfHeapReserve = 0x100000;
    Nt->OptionalHeader.SizeOfHeapCommit = 0x1000;
    Nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress = IL_RELOC_RVA;
    Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = sizeof(IMAGE_BASE_RELOCATION);
    Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress = ComRva;
    Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].Size = sizeof(IL_COR20_HEADER);

    Section = IMAGE_FIRST_SECTION(Nt);
    memcpy(Section[0].Name, ".text", 5);
    Section[0].Misc.VirtualSize = 0x100;
    Section[0].VirtualAddress = IL_TEXT_RVA;
    Section[0].SizeOfRawData = IL_FILE_ALIGN;
    Section[0].PointerToRawData = IL_FILE_ALIGN;
    Section[0].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    memcpy(Section[1].Name, ".reloc", 6);
    Section[1].Misc.VirtualSize = sizeof(IMAGE_BASE_RELOCATION);
    Section[1].VirtualAddress = IL_RELOC_RVA;
    Section[1].SizeOfRawData = IL_FILE_ALIGN;
    Section[1].PointerToRawData = 2 * IL_FILE_ALIGN;
    Section[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_DISCARDABLE | IMAGE_SCN_MEM_READ;

    Cor->cb = sizeof(*Cor);
    Cor->MajorRuntimeVersion = 2;
    Cor->MinorRuntimeVersion = 5;
    Cor->MetaData.VirtualAddress = IL_TEXT_RVA + sizeof(*Cor);
    Cor->MetaData.Size = 0x20;
    Cor->Flags = CorFlags;
    /* "BSJB" metadata root signature, version 1.1 */
    Metadata[0] = 'B'; Metadata[1] = 'S'; Metadata[2] = 'J'; Metadata[3] = 'B';
    Metadata[4] = 1; Metadata[6] = 1;

    Reloc->VirtualAddress = IL_TEXT_RVA;
    Reloc->SizeOfBlock = sizeof(IMAGE_BASE_RELOCATION);

    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    Ok = WriteFile(File, Image, sizeof(Image), &Written, NULL) && Written == sizeof(Image);
    CloseHandle(File);
    return Ok;
}

static
void
test_IlOnlyAnyCpu(PCWSTR Path)
{
    HMODULE Module;
    PIMAGE_NT_HEADERS Nt;
    PIMAGE_SECTION_HEADER Section;
    MEMORY_BASIC_INFORMATION Info;

    ok(WriteIlImage(Path, IL_ILONLY, IL_TEXT_RVA, 0x3000), "cannot write %ls, error %lu\n", Path, GetLastError());

    SetLastError(0xdeadbeef);
    Module = LoadLibraryExW(Path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    ok(Module != NULL, "LoadLibraryExW failed, error %lu\n", GetLastError());
    if (!Module)
        return;

    Nt = (PIMAGE_NT_HEADERS)((PBYTE)Module + ((PIMAGE_DOS_HEADER)Module)->e_lfanew);
    ok(Nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386, "machine is 0x%x\n", Nt->FileHeader.Machine);
    ok(Nt->FileHeader.NumberOfSections == 2, "%u sections\n", Nt->FileHeader.NumberOfSections);
    /* The headers have the format of this process */
    ok(Nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR_MAGIC, "magic is 0x%x\n", Nt->OptionalHeader.Magic);
    ok(Nt->FileHeader.SizeOfOptionalHeader == sizeof(IMAGE_OPTIONAL_HEADER),
       "optional header size is 0x%x\n", Nt->FileHeader.SizeOfOptionalHeader);
    if (Nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR_MAGIC)
    {
        ok(Nt->OptionalHeader.SizeOfImage == 0x3000, "size of image 0x%lx\n", Nt->OptionalHeader.SizeOfImage);
        ok(Nt->OptionalHeader.SizeOfHeaders == IL_FILE_ALIGN, "size of headers 0x%lx\n", Nt->OptionalHeader.SizeOfHeaders);
        ok(Nt->OptionalHeader.SizeOfStackReserve == 0x100000, "stack reserve 0x%Ix\n",
           (SIZE_T)Nt->OptionalHeader.SizeOfStackReserve);
        ok(Nt->OptionalHeader.NumberOfRvaAndSizes == IMAGE_NUMBEROF_DIRECTORY_ENTRIES, "%lu directories\n",
           Nt->OptionalHeader.NumberOfRvaAndSizes);
        ok(Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress == IL_TEXT_RVA,
           "COM descriptor at 0x%lx\n", Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress);
        ok(Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress == IL_RELOC_RVA,
           "relocations at 0x%lx\n", Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);
#ifdef _WIN64
        /* The image base is the one of the file, not where the image is */
        ok(Nt->OptionalHeader.ImageBase == IL_IMAGE_BASE, "image base 0x%Ix\n", (SIZE_T)Nt->OptionalHeader.ImageBase);
#endif
        Section = IMAGE_FIRST_SECTION(Nt);
        ok(!memcmp(Section[0].Name, ".text", 6), "first section %.8s\n", Section[0].Name);
        ok(Section[0].VirtualAddress == IL_TEXT_RVA, "first section at 0x%lx\n", Section[0].VirtualAddress);
        ok(!memcmp(Section[1].Name, ".reloc", 7), "second section %.8s\n", Section[1].Name);
        ok(Section[1].VirtualAddress == IL_RELOC_RVA, "second section at 0x%lx\n", Section[1].VirtualAddress);
    }

    ok(VirtualQuery(Module, &Info, sizeof(Info)) == sizeof(Info), "VirtualQuery failed\n");
    ok(Info.Type == MEM_IMAGE, "type 0x%lx\n", Info.Type);
    ok(Info.Protect == PAGE_READONLY, "header protection 0x%lx\n", Info.Protect);

    ok(GetModuleHandleW(Path) == Module, "the module is not listed\n");
    ok(GetModuleHandleW(L"mscoree.dll") == NULL, "mscoree.dll was loaded\n");
    ok(FreeLibrary(Module), "FreeLibrary failed, error %lu\n", GetLastError());
}

static
void
test_Il32BitRequired(PCWSTR Path)
{
    HMODULE Module;

    ok(WriteIlImage(Path, IL_ILONLY | IL_32BITREQUIRED, IL_TEXT_RVA, 0x3000), "cannot write %ls, error %lu\n", Path, GetLastError());

    SetLastError(0xdeadbeef);
    Module = LoadLibraryExW(Path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#ifdef _WIN64
    ok(Module == NULL, "a 32-bit only assembly loaded\n");
    ok(GetLastError() == ERROR_BAD_EXE_FORMAT, "got error %lu\n", GetLastError());
#else
    ok(Module != NULL, "LoadLibraryExW failed, error %lu\n", GetLastError());
#endif
    if (Module)
        FreeLibrary(Module);
}

/* AnyCPU with Prefer32Bit sets both bits and requires 32 bits (measured on Windows 10 22H2); preferring alone does not */
static
void
test_Il32BitPreferred(PCWSTR Path)
{
    HMODULE Module;

    ok(WriteIlImage(Path, IL_ILONLY | IL_32BITREQUIRED | IL_32BITPREFERRED, IL_TEXT_RVA, 0x3000),
       "cannot write %ls, error %lu\n", Path, GetLastError());
    SetLastError(0xdeadbeef);
    Module = LoadLibraryExW(Path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#ifdef _WIN64
    ok(Module == NULL, "a Prefer32Bit assembly loaded\n");
    ok(GetLastError() == ERROR_BAD_EXE_FORMAT, "got error %lu\n", GetLastError());
#else
    ok(Module != NULL, "LoadLibraryExW failed, error %lu\n", GetLastError());
#endif
    if (Module)
        FreeLibrary(Module);
    ok(DeleteFileW(Path), "DeleteFileW failed, error %lu\n", GetLastError());

    ok(WriteIlImage(Path, IL_ILONLY | IL_32BITPREFERRED, IL_TEXT_RVA, 0x3000),
       "cannot write %ls, error %lu\n", Path, GetLastError());
    Module = LoadLibraryExW(Path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    ok(Module != NULL, "LoadLibraryExW failed, error %lu\n", GetLastError());
    if (Module)
        FreeLibrary(Module);
    ok(DeleteFileW(Path), "DeleteFileW failed, error %lu\n", GetLastError());
}

/* The COM descriptor points into a part of the image that no section covers */
static
void
test_IlUnmappedDescriptor(PCWSTR Path)
{
    HMODULE Module;

    ok(WriteIlImage(Path, IL_ILONLY, 0x4000, 0x5000), "cannot write %ls, error %lu\n", Path, GetLastError());
    SetLastError(0xdeadbeef);
    Module = LoadLibraryExW(Path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#ifdef _WIN64
    ok(Module == NULL, "an image with an unmapped COM descriptor loaded\n");
    ok(GetLastError() != 0xdeadbeef && GetLastError() != ERROR_SUCCESS, "no error\n");
#endif
    if (Module)
        FreeLibrary(Module);
    ok(DeleteFileW(Path), "DeleteFileW failed, error %lu\n", GetLastError());
}

START_TEST(LoadLibraryILOnly)
{
    WCHAR Dir[MAX_PATH], Path[MAX_PATH];

    GetTempPathW(_countof(Dir), Dir);
    StringCchPrintfW(Path, _countof(Path), L"%silonly_%lu.dll", Dir, GetCurrentProcessId());

    test_IlOnlyAnyCpu(Path);
    ok(DeleteFileW(Path), "DeleteFileW failed, error %lu\n", GetLastError());

    test_Il32BitRequired(Path);
    ok(DeleteFileW(Path), "DeleteFileW failed, error %lu\n", GetLastError());

    test_Il32BitPreferred(Path);
    test_IlUnmappedDescriptor(Path);
}
