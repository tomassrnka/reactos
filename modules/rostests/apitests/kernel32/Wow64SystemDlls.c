/* System DLLs that 32-bit programs import statically must exist in the
 * process's own system directory (SysWOW64 under WoW64). SPDX-License-Identifier: LGPL-2.1-or-later */
#include "precomp.h"

typedef BOOL (WINAPI *PROCESSPRNG)(PBYTE, SIZE_T);

static HMODULE LoadSystemDll(PCWSTR Name)
{
    WCHAR Path[MAX_PATH];
    UINT Length = GetSystemDirectoryW(Path, ARRAYSIZE(Path));
    PIMAGE_NT_HEADERS Nt;
    HMODULE Module;

    ok(Length != 0 && Length + 1 + wcslen(Name) < ARRAYSIZE(Path), "GetSystemDirectoryW: %u\n", Length);
    if (Length == 0 || Length + 1 + wcslen(Name) >= ARRAYSIZE(Path))
        return NULL;
    Path[Length] = L'\\';
    wcscpy(Path + Length + 1, Name);
    Module = LoadLibraryW(Path);
    ok(Module != NULL, "LoadLibraryW(%S) failed: %lu\n", Path, GetLastError());
    if (!Module)
        return NULL;
    /* the copy that loaded must be the one built for this process's architecture */
    Nt = RtlImageNtHeader(Module);
    ok(Nt != NULL, "%S: no NT headers\n", Name);
    if (Nt)
    {
#ifdef _M_IX86
        ok(Nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386, "%S: machine %04x\n", Name, Nt->FileHeader.Machine);
#else
        ok(Nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64, "%S: machine %04x\n", Name, Nt->FileHeader.Machine);
#endif
    }
    return Module;
}

START_TEST(Wow64SystemDlls)
{
    BYTE Buffer[64], Zero[64] = { 0 };
    PROCESSPRNG ProcessPrng;
    HMODULE Module;
    BOOL Wow64 = FALSE;

    IsWow64Process(GetCurrentProcess(), &Wow64);
    trace("wow64=%d\n", Wow64);

    /* Rust's standard library (Firefox, among others) imports ProcessPrng from it */
    Module = LoadSystemDll(L"bcryptprimitives.dll");
    if (Module)
    {
        ProcessPrng = (PROCESSPRNG)GetProcAddress(Module, "ProcessPrng");
        ok(ProcessPrng != NULL, "ProcessPrng missing: %lu\n", GetLastError());
        if (ProcessPrng)
        {
            memset(Buffer, 0, sizeof(Buffer));
            ok(ProcessPrng(Buffer, sizeof(Buffer)), "ProcessPrng failed\n");
            ok(memcmp(Buffer, Zero, sizeof(Buffer)) != 0, "ProcessPrng left the buffer zero\n");
        }
        FreeLibrary(Module);
    }

    /* Firefox creates its font resources through DirectWrite */
    Module = LoadSystemDll(L"dwrite.dll");
    if (Module)
    {
        ok(GetProcAddress(Module, "DWriteCreateFactory") != NULL,
           "DWriteCreateFactory missing: %lu\n", GetLastError());
        FreeLibrary(Module);
    }

    Module = LoadSystemDll(L"ncrypt.dll");
    if (Module)
    {
        ok(GetProcAddress(Module, "NCryptOpenStorageProvider") != NULL,
           "NCryptOpenStorageProvider missing: %lu\n", GetLastError());
        FreeLibrary(Module);
    }
}
