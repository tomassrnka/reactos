/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     CreateActCtxW must reject a PE whose resource directory offsets
 *              point outside the file instead of reading past its buffer
 */

#include "precomp.h"

#include <winbase.h>

#define RSRC_RVA    0x1000
#define RSRC_RAW    0x200
#define RSRC_SIZE   0x200
#define FILE_SIZE   (RSRC_RAW + RSRC_SIZE)

static const char manifest[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
    "<assemblyIdentity version=\"1.0.0.0\" name=\"ActCtxResDirBound.test\" type=\"win32\"/>"
    "</assembly>";

enum corrupt
{
    CORRUPT_NONE,
    CORRUPT_TYPE_DIR,      /* root entry -> type directory far outside the file */
    CORRUPT_NAME_DIR,      /* name level -> language directory outside the file */
    CORRUPT_DATA_ENTRY,    /* language level -> data entry outside the file */
    CORRUPT_DATA_SIZE,     /* data entry Size runs past the file */
    CORRUPT_DATA_RVA,      /* data entry OffsetToData is outside the file */
    CORRUPT_ENTRY_COUNT    /* entry count makes the entry array run past the file */
};

static void build_pe(BYTE *f, enum corrupt c)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)f;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_SECTION_HEADER *sec;
    BYTE *r = f + RSRC_RAW;
    IMAGE_RESOURCE_DIRECTORY *d;
    IMAGE_RESOURCE_DIRECTORY_ENTRY *e;
    IMAGE_RESOURCE_DATA_ENTRY *de;

    memset(f, 0, FILE_SIZE);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x40;
    nt = (IMAGE_NT_HEADERS32 *)(f + 0x40);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
    nt->FileHeader.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.ImageBase = 0x10000000;
    nt->OptionalHeader.SectionAlignment = 0x1000;
    nt->OptionalHeader.FileAlignment = 0x200;
    nt->OptionalHeader.SizeOfImage = RSRC_RVA + 0x1000;
    nt->OptionalHeader.SizeOfHeaders = 0x200;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress = RSRC_RVA;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].Size = RSRC_SIZE;
    sec = IMAGE_FIRST_SECTION(nt);
    memcpy(sec->Name, ".rsrc", 5);
    sec->Misc.VirtualSize = RSRC_SIZE;
    sec->VirtualAddress = RSRC_RVA;
    sec->SizeOfRawData = RSRC_SIZE;
    sec->PointerToRawData = RSRC_RAW;
    sec->Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

    /* root (0x00) -> type RT_MANIFEST (0x18) -> name 1 (0x30) -> lang (0x48 data) */
    d = (IMAGE_RESOURCE_DIRECTORY *)(r + 0x00);
    d->NumberOfIdEntries = (c == CORRUPT_ENTRY_COUNT) ? 0xFFFF : 1;
    e = (IMAGE_RESOURCE_DIRECTORY_ENTRY *)(d + 1);
    e->Id = 24 /* RT_MANIFEST */;
    e->DataIsDirectory = 1;
    e->OffsetToDirectory = (c == CORRUPT_TYPE_DIR) ? 0x7FFF0000 : 0x18;

    d = (IMAGE_RESOURCE_DIRECTORY *)(r + 0x18);
    d->NumberOfIdEntries = 1;
    e = (IMAGE_RESOURCE_DIRECTORY_ENTRY *)(d + 1);
    e->Id = 1;
    e->DataIsDirectory = 1;
    e->OffsetToDirectory = (c == CORRUPT_NAME_DIR) ? 0x00F00000 : 0x30;

    d = (IMAGE_RESOURCE_DIRECTORY *)(r + 0x30);
    d->NumberOfIdEntries = 1;
    e = (IMAGE_RESOURCE_DIRECTORY_ENTRY *)(d + 1);
    e->Id = 0x409;
    e->DataIsDirectory = 0;
    e->OffsetToData = (c == CORRUPT_DATA_ENTRY) ? 0x7FFFFFF0 : 0x48;

    de = (IMAGE_RESOURCE_DATA_ENTRY *)(r + 0x48);
    de->OffsetToData = (c == CORRUPT_DATA_RVA) ? 0x70000000 : RSRC_RVA + 0x60;
    de->Size = (c == CORRUPT_DATA_SIZE) ? 0x10000000 : sizeof(manifest) - 1;
    memcpy(r + 0x60, manifest, sizeof(manifest) - 1);
}

static void try_create(const WCHAR *path, enum corrupt c, BOOL named)
{
    BYTE *f = HeapAlloc(GetProcessHeap(), 0, FILE_SIZE);
    HANDLE h, ctx;
    DWORD written;
    ACTCTXW act;

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(h != INVALID_HANDLE_VALUE, "CreateFile failed %lu\n", GetLastError());
    if (h == INVALID_HANDLE_VALUE) { HeapFree(GetProcessHeap(), 0, f); return; }
    build_pe(f, c);
    ok(WriteFile(h, f, FILE_SIZE, &written, NULL) && written == FILE_SIZE, "WriteFile failed\n");
    CloseHandle(h);
    HeapFree(GetProcessHeap(), 0, f);

    memset(&act, 0, sizeof(act));
    act.cbSize = sizeof(act);
    act.lpSource = path;
    if (named)
    {
        act.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID;
        act.lpResourceName = (LPCWSTR)(ULONG_PTR)1;
    }
    ctx = CreateActCtxW(&act);
    if (c == CORRUPT_NONE)
        ok(ctx != INVALID_HANDLE_VALUE, "valid manifest PE rejected, error %lu (named %d)\n",
           GetLastError(), named);
    else
        ok(ctx == INVALID_HANDLE_VALUE, "corrupt %d (named %d) was accepted\n", c, named);
    if (ctx != INVALID_HANDLE_VALUE) ReleaseActCtx(ctx);
    DeleteFileW(path);
}

/* ---- realistic tree: several resource types, several manifests, three levels ---- */

#define REAL_RSRC_SIZE 0x1000
#define REAL_FILE_SIZE (RSRC_RAW + REAL_RSRC_SIZE)

static const char real_manifest[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
    "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\r\n"
    "  <assemblyIdentity version=\"1.0.0.0\" processorArchitecture=\"x86\" name=\"%s\" type=\"win32\"/>\r\n"
    "  <description>Realistic test manifest</description>\r\n"
    "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">\r\n"
    "    <security><requestedPrivileges>\r\n"
    "      <requestedExecutionLevel level=\"asInvoker\" uiAccess=\"false\"/>\r\n"
    "    </requestedPrivileges></security>\r\n"
    "  </trustInfo>\r\n"
    "  <compatibility xmlns=\"urn:schemas-microsoft-com:compatibility.v1\">\r\n"
    "    <application>\r\n"
    "      <supportedOS Id=\"{e2011457-1546-43c5-a5fe-008deee3d3f0}\"/>\r\n"
    "      <supportedOS Id=\"{35138b9a-5d96-4fbd-8e2d-a2440225f93a}\"/>\r\n"
    "      <supportedOS Id=\"{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}\"/>\r\n"
    "      <supportedOS Id=\"{1f676c76-80e1-4239-95bb-83d0f6d0da78}\"/>\r\n"
    "      <supportedOS Id=\"{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}\"/>\r\n"
    "    </application>\r\n"
    "  </compatibility>\r\n"
    "  <application xmlns=\"urn:schemas-microsoft-com:asm.v3\">\r\n"
    "    <windowsSettings>\r\n"
    "      <dpiAware xmlns=\"http://schemas.microsoft.com/SMI/2005/WindowsSettings\">true</dpiAware>\r\n"
    "      <longPathAware xmlns=\"http://schemas.microsoft.com/SMI/2016/WindowsSettings\">true</longPathAware>\r\n"
    "    </windowsSettings>\r\n"
    "  </application>\r\n"
    "</assembly>\r\n";

static void put_dir(BYTE *r, ULONG off, WORD named, WORD ids)
{
    IMAGE_RESOURCE_DIRECTORY *d = (IMAGE_RESOURCE_DIRECTORY *)(r + off);
    d->NumberOfNamedEntries = named;
    d->NumberOfIdEntries = ids;
}

static void put_ent(BYTE *r, ULONG dir, ULONG idx, ULONG id, BOOL isdir, ULONG target)
{
    IMAGE_RESOURCE_DIRECTORY_ENTRY *e = (IMAGE_RESOURCE_DIRECTORY_ENTRY *)(r + dir + 16) + idx;
    e->Id = (WORD)id;
    if (isdir) { e->DataIsDirectory = 1; e->OffsetToDirectory = target; }
    else { e->DataIsDirectory = 0; e->OffsetToData = target; }
}

static void put_data(BYTE *r, ULONG off, ULONG blob, ULONG size)
{
    IMAGE_RESOURCE_DATA_ENTRY *de = (IMAGE_RESOURCE_DATA_ENTRY *)(r + off);
    de->OffsetToData = RSRC_RVA + blob;
    de->Size = size;
}

/* overlap: add a second section sharing the resource RVAs, so the compact reader
 * gives up and the full flat read is used */
static void build_real_pe(BYTE *f, const char *name, BOOL overlap)
{
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_SECTION_HEADER *sec;
    BYTE *r = f + RSRC_RAW;
    IMAGE_RESOURCE_DIR_STRING_U *str;
    char text[sizeof(real_manifest) + 64];
    int len;

    build_pe(f, CORRUPT_NONE);  /* headers; the rsrc contents are rewritten below */
    memset(f + RSRC_RAW, 0, REAL_RSRC_SIZE);
    nt = (IMAGE_NT_HEADERS32 *)(f + 0x40);
    nt->OptionalHeader.SizeOfImage = RSRC_RVA + 0x2000;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].Size = REAL_RSRC_SIZE;
    sec = IMAGE_FIRST_SECTION(nt);
    sec->Misc.VirtualSize = sec->SizeOfRawData = REAL_RSRC_SIZE;
    if (overlap)
    {
        nt->FileHeader.NumberOfSections = 2;
        memcpy(sec[1].Name, ".dup", 4);
        sec[1].Misc.VirtualSize = sec[1].SizeOfRawData = 0x100;
        sec[1].VirtualAddress = RSRC_RVA;
        sec[1].PointerToRawData = RSRC_RAW;
        sec[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
    }

    put_dir(r, 0x000, 1, 3);                 /* named CUSTOM, ids 3, 16, 24 */
    r[0x10 + 0] = 0;                         /* (entries are filled below) */
    {
        IMAGE_RESOURCE_DIRECTORY_ENTRY *e = (IMAGE_RESOURCE_DIRECTORY_ENTRY *)(r + 16);
        e->NameIsString = 1;
        e->NameOffset = 0x1A0;
    }
    put_ent(r, 0x000, 1, 3, TRUE, 0x080);
    put_ent(r, 0x000, 2, 16, TRUE, 0x0C0);
    put_ent(r, 0x000, 3, 24, TRUE, 0x100);
    {   /* the named entry points at 0x040; Id/NameOffset share storage, set the target only */
        IMAGE_RESOURCE_DIRECTORY_ENTRY *e = (IMAGE_RESOURCE_DIRECTORY_ENTRY *)(r + 16);
        e->DataIsDirectory = 1;
        e->OffsetToDirectory = 0x040;
    }
    str = (IMAGE_RESOURCE_DIR_STRING_U *)(r + 0x1A0);
    str->Length = 6;
    memcpy(str->NameString, L"CUSTOM", 12);

    put_dir(r, 0x040, 0, 1); put_ent(r, 0x040, 0, 7, TRUE, 0x058);
    put_dir(r, 0x058, 0, 1); put_ent(r, 0x058, 0, 0x409, FALSE, 0x070);
    put_data(r, 0x070, 0x400, 32);
    put_dir(r, 0x080, 0, 1); put_ent(r, 0x080, 0, 1, TRUE, 0x098);
    put_dir(r, 0x098, 0, 1); put_ent(r, 0x098, 0, 0x409, FALSE, 0x0B0);
    put_data(r, 0x0B0, 0x420, 32);
    put_dir(r, 0x0C0, 0, 1); put_ent(r, 0x0C0, 0, 1, TRUE, 0x0D8);
    put_dir(r, 0x0D8, 0, 1); put_ent(r, 0x0D8, 0, 0x409, FALSE, 0x0F0);
    put_data(r, 0x0F0, 0x440, 64);

    put_dir(r, 0x100, 0, 2);                 /* RT_MANIFEST: names 1 and 2 */
    put_ent(r, 0x100, 0, 1, TRUE, 0x120);
    put_ent(r, 0x100, 1, 2, TRUE, 0x140);
    put_dir(r, 0x120, 0, 2);                 /* name 1: two languages */
    put_ent(r, 0x120, 0, 0x409, FALSE, 0x160);
    put_ent(r, 0x120, 1, 0x804, FALSE, 0x170);
    put_dir(r, 0x140, 0, 1);
    put_ent(r, 0x140, 0, 0x409, FALSE, 0x180);

    len = sprintf(text, real_manifest, name);
    ok(len < 0xA00, "manifest too large\n");
    memset(r + 0x400, 0xA5, 0x100);
    memcpy(r + 0x500, text, len);
    put_data(r, 0x160, 0x500, len);
    put_data(r, 0x170, 0x500, len);
    put_data(r, 0x180, 0x500, len);
}

static void write_file(const WCHAR *path, const BYTE *buf, DWORD size)
{
    DWORD written;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    ok(h != INVALID_HANDLE_VALUE, "CreateFile failed %lu\n", GetLastError());
    if (h == INVALID_HANDLE_VALUE) return;
    ok(WriteFile(h, buf, size, &written, NULL) && written == size, "WriteFile failed\n");
    CloseHandle(h);
}

static void test_realistic(const WCHAR *path)
{
    static const char *ident = "ActCtxResDirBound.real";
    BYTE *f = HeapAlloc(GetProcessHeap(), 0, REAL_FILE_SIZE);
    ACTCTXW act;
    HANDLE ctx;
    int overlap, which;

    for (overlap = 0; overlap <= 1; overlap++)   /* compact reader, then flat reader */
    {
        build_real_pe(f, ident, overlap);
        write_file(path, f, REAL_FILE_SIZE);
        for (which = 0; which < 3; which++)       /* resource id 1, id 2, #1 string form */
        {
            memset(&act, 0, sizeof(act));
            act.cbSize = sizeof(act);
            act.lpSource = path;
            act.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID;
            act.lpResourceName = which == 2 ? L"#1" : (LPCWSTR)(ULONG_PTR)(which + 1);
            ctx = CreateActCtxW(&act);
            ok(ctx != INVALID_HANDLE_VALUE, "realistic PE (overlap %d, res %d) rejected, error %lu\n",
               overlap, which, GetLastError());
            if (ctx != INVALID_HANDLE_VALUE) ReleaseActCtx(ctx);
        }
    }
    HeapFree(GetProcessHeap(), 0, f);
    DeleteFileW(path);
}

/* dependency lookup: main manifest names an assembly, the loader finds <name>.dll next to it
 * and takes its first manifest resource (resname == NULL path) */
static void test_dependency(const WCHAR *dir)
{
    static const char depname[] = "ActCtxResDirBoundDep";
    static const char main_manifest[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
        "<assemblyIdentity version=\"1.0.0.0\" name=\"ActCtxResDirBound.main\" type=\"win32\"/>"
        "<dependency><dependentAssembly>"
        "<assemblyIdentity type=\"win32\" name=\"ActCtxResDirBoundDep\" version=\"1.0.0.0\" processorArchitecture=\"x86\"/>"
        "</dependentAssembly></dependency></assembly>";
    WCHAR sub[MAX_PATH], mainp[MAX_PATH], depp[MAX_PATH];
    BYTE *f = HeapAlloc(GetProcessHeap(), 0, REAL_FILE_SIZE);
    ACTCTXW act;
    HANDLE ctx;
    int mode;

    StringCchPrintfW(sub, MAX_PATH, L"%sActCtxResDirBoundDir", dir);
    CreateDirectoryW(sub, NULL);
    StringCchPrintfW(mainp, MAX_PATH, L"%s\\main.manifest", sub);
    StringCchPrintfW(depp, MAX_PATH, L"%s\\%S.dll", sub, depname);
    write_file(mainp, (const BYTE *)main_manifest, sizeof(main_manifest) - 1);

    for (mode = 0; mode < 3; mode++)   /* 0 valid compact, 1 valid flat, 2 corrupt */
    {
        if (mode == 2)
        {
            build_pe(f, CORRUPT_TYPE_DIR);
            write_file(depp, f, FILE_SIZE);
        }
        else
        {
            build_real_pe(f, depname, mode == 1);
            write_file(depp, f, REAL_FILE_SIZE);
        }
        memset(&act, 0, sizeof(act));
        act.cbSize = sizeof(act);
        act.lpSource = mainp;
        ctx = CreateActCtxW(&act);
        if (mode == 2)
            ok(ctx == INVALID_HANDLE_VALUE, "corrupt dependency PE was accepted\n");
        else
            ok(ctx != INVALID_HANDLE_VALUE, "dependency PE (mode %d) rejected, error %lu\n",
               mode, GetLastError());
        if (ctx != INVALID_HANDLE_VALUE) ReleaseActCtx(ctx);
    }
    DeleteFileW(depp);
    DeleteFileW(mainp);
    RemoveDirectoryW(sub);
    HeapFree(GetProcessHeap(), 0, f);
}

/* real manifested system binaries: whatever carries an RT_MANIFEST id 1 must still activate */
static void test_system_binaries(void)
{
    static const WCHAR *names[] = { L"cmd.exe", L"explorer.exe", L"notepad.exe", L"regedit.exe",
                                    L"calc.exe", L"mspaint.exe", L"taskmgr.exe", L"winver.exe",
                                    L"msiexec.exe", L"rundll32.exe", L"control.exe", L"wordpad.exe" };
    WCHAR path[MAX_PATH], sysdir[MAX_PATH];
    ULONG i, tested = 0;

    GetSystemDirectoryW(sysdir, MAX_PATH);
    for (i = 0; i < ARRAYSIZE(names); i++)
    {
        HMODULE mod;
        ACTCTXW act;
        HANDLE ctx;
        BOOL has;

        StringCchPrintfW(path, MAX_PATH, L"%s\\%s", sysdir, names[i]);
        mod = LoadLibraryExW(path, NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (!mod) continue;
        has = FindResourceW(mod, (LPCWSTR)(ULONG_PTR)1, (LPCWSTR)(ULONG_PTR)24) != NULL;
        FreeLibrary(mod);
        if (!has) continue;

        memset(&act, 0, sizeof(act));
        act.cbSize = sizeof(act);
        act.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID;
        act.lpSource = path;
        act.lpResourceName = (LPCWSTR)(ULONG_PTR)1;
        ctx = CreateActCtxW(&act);
        ok(ctx != INVALID_HANDLE_VALUE, "%S: manifest rejected, error %lu\n", names[i], GetLastError());
        if (ctx != INVALID_HANDLE_VALUE) ReleaseActCtx(ctx);
        tested++;
    }
    trace("system binaries with a manifest activated: %lu\n", tested);
}

START_TEST(ActCtxResDirBound)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    int c;

    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"acb", 0, path);

    for (c = CORRUPT_NONE; c <= CORRUPT_ENTRY_COUNT; c++)
    {
        /* only the resource-name form of CreateActCtxW reads the PE resource tree */
        try_create(path, c, TRUE);
    }
    DeleteFileW(path);
    test_realistic(path);
    test_dependency(dir);
    test_system_binaries();
}
