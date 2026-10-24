/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     COM registrations and DLLs that 32-bit installers need on the
 *              amd64 WoW64 image: browseui and windowscodecs classes through
 *              CoCreateInstance, a PNG decode, RICHEDIT50W from msftedit, and
 *              (at export level 0x600) the Direct3D 11 DLLs. The same source
 *              runs as a native and as a PE32 binary under WoW64.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define COBJMACROS
#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <ole2.h>
#include <shlwapi.h>
#include <initguid.h>
#include <wincodec.h>

#define DEFINE_CLSID(name, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    static const CLSID name = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }

DEFINE_CLSID(TEST_ACLMulti,        0x00BB2765, 0x6A77, 0x11D0, 0xA5, 0x35, 0x00, 0xC0, 0x4F, 0xD7, 0xD0, 0x62);
DEFINE_CLSID(TEST_ACListISF,       0x03C036F1, 0xA186, 0x11D0, 0x82, 0x4A, 0x00, 0xAA, 0x00, 0x5B, 0x43, 0x83);
DEFINE_CLSID(TEST_ACLHistory,      0x00BB2764, 0x6A77, 0x11D0, 0xA5, 0x35, 0x00, 0xC0, 0x4F, 0xD7, 0xD0, 0x62);
DEFINE_CLSID(TEST_CMultiLanguage,  0x275C23E2, 0x3747, 0x11D0, 0x9F, 0xEA, 0x00, 0xAA, 0x00, 0x3F, 0x86, 0x46);
DEFINE_CLSID(TEST_StdURLMoniker,   0x79EAC9E0, 0xBAF9, 0x11CE, 0x8C, 0x82, 0x00, 0xAA, 0x00, 0x4B, 0xA9, 0x0B);

static const struct
{
    const CLSID *clsid;
    const char *name;
} classes[] =
{
    { &TEST_ACLMulti, "browseui ACLMulti" },
    { &TEST_ACListISF, "browseui ACListISF" },
    { &TEST_ACLHistory, "browseui ACLHistory" },
    { &CLSID_WICImagingFactory, "windowscodecs WICImagingFactory" },
    { &TEST_CMultiLanguage, "mlang CMultiLanguage" },
    { &TEST_StdURLMoniker, "urlmon StdURLMoniker" },
};

static const BYTE TestPng[] =
{
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};

static void TestPngDecode(void)
{
    IWICImagingFactory *Factory = NULL;
    IWICBitmapDecoder *Decoder = NULL;
    IWICBitmapFrameDecode *Frame = NULL;
    IStream *Stream;
    UINT Width = 0, Height = 0;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&Factory);
    ok(hr == S_OK, "CoCreateInstance(WICImagingFactory) returned 0x%lx\n", hr);
    if (hr != S_OK)
        return;

    Stream = SHCreateMemStream(TestPng, sizeof(TestPng));
    ok(Stream != NULL, "SHCreateMemStream failed\n");
    if (Stream)
    {
        hr = IWICImagingFactory_CreateDecoderFromStream(Factory, Stream, NULL,
                                                        WICDecodeMetadataCacheOnDemand, &Decoder);
        ok(hr == S_OK, "CreateDecoderFromStream(PNG) returned 0x%lx\n", hr);
        if (hr == S_OK)
        {
            hr = IWICBitmapDecoder_GetFrame(Decoder, 0, &Frame);
            ok(hr == S_OK, "GetFrame returned 0x%lx\n", hr);
            if (hr == S_OK)
            {
                hr = IWICBitmapFrameDecode_GetSize(Frame, &Width, &Height);
                ok(hr == S_OK, "GetSize returned 0x%lx\n", hr);
                ok(Width == 1 && Height == 1, "PNG size %ux%u\n", Width, Height);
                IWICBitmapFrameDecode_Release(Frame);
            }
            IWICBitmapDecoder_Release(Decoder);
        }
        IStream_Release(Stream);
    }
    IWICImagingFactory_Release(Factory);
}

static void TestRichEdit50(void)
{
    HMODULE Lib;
    HWND Window;

    Lib = LoadLibraryW(L"msftedit.dll");
    ok(Lib != NULL, "LoadLibrary(msftedit.dll) failed: %lu\n", GetLastError());
    if (!Lib)
        return;
    SetLastError(0xdeadbeef);
    Window = CreateWindowExW(0, L"RICHEDIT50W", L"", WS_POPUP, 0, 0, 10, 10,
                             HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    ok(Window != NULL, "CreateWindow(RICHEDIT50W) failed: %lu\n", GetLastError());
    if (Window)
        DestroyWindow(Window);
    FreeLibrary(Lib);
}

#ifdef TEST_D3D11
static void TestD3D11Load(void)
{
    static const WCHAR *Names[] = { L"dxgi.dll", L"d3d11.dll" };
    ULONG i;

    for (i = 0; i < ARRAYSIZE(Names); i++)
    {
        HMODULE Lib = LoadLibraryW(Names[i]);
        ok(Lib != NULL, "LoadLibrary(%ls) failed: %lu\n", Names[i], GetLastError());
        if (Lib)
            FreeLibrary(Lib);
    }
}
#endif

START_TEST(Registrations)
{
    HRESULT hr;
    ULONG i;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok(SUCCEEDED(hr), "CoInitializeEx returned 0x%lx\n", hr);

    for (i = 0; i < ARRAYSIZE(classes); i++)
    {
        IUnknown *Unk = NULL;

        hr = CoCreateInstance(classes[i].clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&Unk);
        ok(hr == S_OK, "CoCreateInstance(%s) returned 0x%lx\n", classes[i].name, hr);
        if (Unk)
            IUnknown_Release(Unk);
    }

    TestPngDecode();
    TestRichEdit50();
#ifdef TEST_D3D11
    TestD3D11Load();
#endif
    CoUninitialize();
}
