/* System DLLs that 32-bit programs import statically must exist in the
 * process's own system directory (SysWOW64 under WoW64). SPDX-License-Identifier: LGPL-2.1-or-later */
#include "precomp.h"
#define COBJMACROS
#include <objbase.h>
#include <dwrite.h>

typedef BOOL (WINAPI *PROCESSPRNG)(PBYTE, SIZE_T);
typedef HRESULT (WINAPI *DWRITECREATEFACTORY)(DWRITE_FACTORY_TYPE, REFIID, IUnknown **);

/* IID_IDWriteFactory, b859ee5a-d838-4b5b-a2e8-1adc7d93db48 */
static const GUID FactoryIid = { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };

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
#ifdef _M_IX86
    {
        /* The PE32 build on a 64-bit system must be the WoW64 run this test is for */
        SYSTEM_INFO Info;
        GetNativeSystemInfo(&Info);
        if (Info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
            ok(Wow64, "A PE32 process on amd64 is not under WoW64\n");
    }
#endif

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
        DWRITECREATEFACTORY CreateFactory = (DWRITECREATEFACTORY)GetProcAddress(Module, "DWriteCreateFactory");
        IDWriteFactory *Factory = NULL;
        IDWriteFontCollection *Fonts = NULL;
        HRESULT hr;

        ok(CreateFactory != NULL, "DWriteCreateFactory missing: %lu\n", GetLastError());
        if (CreateFactory)
        {
            /* A factory and the installed fonts, which font resources are created from */
            hr = CreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, &FactoryIid, (IUnknown **)&Factory);
            ok(hr == S_OK && Factory, "DWriteCreateFactory: 0x%08lx\n", hr);
            if (Factory)
            {
                hr = IDWriteFactory_GetSystemFontCollection(Factory, &Fonts, FALSE);
                ok(hr == S_OK && Fonts, "GetSystemFontCollection: 0x%08lx\n", hr);
                if (Fonts)
                {
                    ok(IDWriteFontCollection_GetFontFamilyCount(Fonts) > 0, "No font families\n");
                    IDWriteFontCollection_Release(Fonts);
                }
                IDWriteFactory_Release(Factory);
            }
        }
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
