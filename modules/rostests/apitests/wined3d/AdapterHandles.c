/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Repeated DirectDraw and Direct3D 9 creation in one process
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#define COBJMACROS
#include <wingdi.h>
#include <winuser.h>
#include <ddraw.h>
#include <d3d9.h>

#define CYCLES 64

typedef HRESULT (WINAPI *PFN_DIRECTDRAWCREATE)(GUID *, IDirectDraw **, IUnknown *);
typedef HRESULT (WINAPI *PFN_DIRECTDRAWENUMERATEEXA)(LPDDENUMCALLBACKEXA, void *, DWORD);
typedef IDirect3D9 *(WINAPI *PFN_DIRECT3DCREATE9)(UINT);

static PFN_DIRECTDRAWCREATE pDirectDrawCreate;
static PFN_DIRECTDRAWENUMERATEEXA pDirectDrawEnumerateExA;
static PFN_DIRECT3DCREATE9 pDirect3DCreate9;

/* One open of a DirectDraw video output as a media player does it:
 * create, cooperative level, primary surface, release. */
static HRESULT ddraw_open_close(void)
{
    IDirectDrawSurface *primary;
    DDSURFACEDESC desc;
    IDirectDraw *ddraw;
    HRESULT hr;

    hr = pDirectDrawCreate(NULL, &ddraw, NULL);
    if (FAILED(hr))
        return hr;

    hr = IDirectDraw_SetCooperativeLevel(ddraw, NULL, DDSCL_NORMAL);
    if (SUCCEEDED(hr))
    {
        memset(&desc, 0, sizeof(desc));
        desc.dwSize = sizeof(desc);
        desc.dwFlags = DDSD_CAPS;
        desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        hr = IDirectDraw_CreateSurface(ddraw, &desc, &primary, NULL);
        if (SUCCEEDED(hr))
            IDirectDrawSurface_Release(primary);
    }

    IDirectDraw_Release(ddraw);
    return hr;
}

static BOOL WINAPI enum_cb(GUID *guid, char *desc, char *name, void *context, HMONITOR monitor)
{
    ++*(int *)context;
    return TRUE;
}

static BOOL ddraw_baseline(void)
{
    int devices = 0;
    HRESULT hr;

    hr = pDirectDrawEnumerateExA(enum_cb, &devices, DDENUM_ATTACHEDSECONDARYDEVICES);
    if (FAILED(hr) || !devices)
    {
        skip("DirectDrawEnumerateExA returned 0x%08lx with %d devices\n", hr, devices);
        return FALSE;
    }
    hr = ddraw_open_close();
    if (FAILED(hr))
    {
        skip("DirectDraw is not available here (0x%08lx)\n", hr);
        return FALSE;
    }
    return TRUE;
}

static void test_ddraw_cycles(void)
{
    int i, failed = 0, first = -1, devices;
    HRESULT hr, first_hr = S_OK;

    for (i = 0; i < CYCLES; i++)
    {
        devices = 0;
        hr = pDirectDrawEnumerateExA(enum_cb, &devices, DDENUM_ATTACHEDSECONDARYDEVICES);
        if (SUCCEEDED(hr) && devices > 0)
            hr = ddraw_open_close();
        else if (SUCCEEDED(hr))
            hr = E_FAIL;

        if (FAILED(hr) && !failed++)
        {
            first = i;
            first_hr = hr;
        }
    }
    ok(failed == 0, "%d of %d DirectDraw open/close cycles failed, first at cycle %d with 0x%08lx\n",
       failed, CYCLES, first, first_hr);
}

/* Without an OpenGL 2.0 driver Direct3DCreate9 fails on ReactOS; failing must not use up
 * what a later DirectDraw object needs. On Windows it succeeds. */
static void test_d3d9_then_ddraw(void)
{
    IDirect3D9 *d3d9;
    HMODULE module;
    int i, created = 0;
    HRESULT hr;

    module = LoadLibraryW(L"d3d9.dll");
    ok(module != NULL, "d3d9.dll not loaded, error %lu\n", GetLastError());
    if (!module)
        return;
    pDirect3DCreate9 = (PFN_DIRECT3DCREATE9)GetProcAddress(module, "Direct3DCreate9");
    ok(pDirect3DCreate9 != NULL, "Direct3DCreate9 not exported\n");
    if (!pDirect3DCreate9)
    {
        FreeLibrary(module);
        return;
    }

    for (i = 0; i < CYCLES; i++)
    {
        d3d9 = pDirect3DCreate9(D3D_SDK_VERSION);
        if (d3d9)
        {
            created++;
            IDirect3D9_Release(d3d9);
        }
    }
    trace("Direct3DCreate9 succeeded %d of %d times\n", created, CYCLES);

    for (i = 0; i < 4; i++)
    {
        hr = ddraw_open_close();
        ok(hr == DD_OK, "DirectDraw open %d after %d Direct3DCreate9 calls returned 0x%08lx\n",
           i, CYCLES, hr);
    }
    FreeLibrary(module);
}

/* Each case runs in a fresh process, so one cannot use up handles for the other. */
static void run_child(char *exe, const char *mode)
{
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    char cmdline[MAX_PATH + 64];

    sprintf(cmdline, "\"%s\" AdapterHandles %s", exe, mode);
    ok(CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi),
       "CreateProcessA failed, error %lu\n", GetLastError());
    if (!pi.hProcess)
        return;
    wait_child_process(pi.hProcess);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

START_TEST(AdapterHandles)
{
    HMODULE ddraw;
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc < 3)
    {
        run_child(argv[0], "ddraw");
        run_child(argv[0], "d3d9");
        return;
    }

    ddraw = LoadLibraryW(L"ddraw.dll");
    ok(ddraw != NULL, "ddraw.dll not loaded, error %lu\n", GetLastError());
    if (!ddraw)
        return;
    pDirectDrawCreate = (PFN_DIRECTDRAWCREATE)GetProcAddress(ddraw, "DirectDrawCreate");
    pDirectDrawEnumerateExA = (PFN_DIRECTDRAWENUMERATEEXA)GetProcAddress(ddraw, "DirectDrawEnumerateExA");
    ok(pDirectDrawCreate && pDirectDrawEnumerateExA, "ddraw exports missing\n");
    if (!pDirectDrawCreate || !pDirectDrawEnumerateExA || !ddraw_baseline())
        return;

    if (!strcmp(argv[2], "ddraw"))
        test_ddraw_cycles();
    else if (!strcmp(argv[2], "d3d9"))
        test_d3d9_then_ddraw();
    else
        ok(0, "unknown mode %s\n", argv[2]);
}
