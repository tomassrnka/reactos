/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Video memory DirectDraw surfaces on a display without 3D acceleration
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#define COBJMACROS
#include <wingdi.h>
#include <winuser.h>
#include <ddraw.h>
#include <wchar.h>

typedef HRESULT (WINAPI *PFN_DIRECTDRAWCREATE)(GUID *, IDirectDraw **, IUnknown *);

static HWND create_window(void)
{
    HWND hwnd = CreateWindowExA(0, "static", "ddraw_apitest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                0, 0, 320, 240, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "CreateWindowExA failed, error %lu\n", GetLastError());
    return hwnd;
}

static BOOL CALLBACK find_notice_proc(HWND hwnd, LPARAM lparam)
{
    WCHAR title[64];
    DWORD pid = 0;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && GetWindowTextW(hwnd, title, ARRAYSIZE(title))
        && !wcscmp(title, L"ReactOS - no 3D acceleration"))
    {
        *(HWND *)lparam = hwnd;
        return FALSE;
    }
    return TRUE;
}

/* ReactOS showed a notice about missing OpenGL even though DirectDraw falls back to 2D. */
static void test_no_notice(void)
{
    HWND notice = NULL;
    int i;

    for (i = 0; i < 20 && !notice; i++)
    {
        Sleep(100);
        EnumWindows(find_notice_proc, (LPARAM)&notice);
    }
    ok(notice == NULL, "DirectDrawCreate showed the no 3D acceleration notice\n");
    if (notice)
        PostMessageW(notice, WM_CLOSE, 0, 0);
}

static void init_desc(DDSURFACEDESC *desc, DWORD flags, DWORD caps)
{
    memset(desc, 0, sizeof(*desc));
    desc->dwSize = sizeof(*desc);
    desc->dwFlags = flags;
    desc->ddsCaps.dwCaps = caps;
}

/* Windows creates video memory surfaces when the driver caps report video memory
 * (Windows 10: 4 GB) and fails them with DDERR_NODIRECTDRAWHW when they report none
 * (Server 2008 R2 on a VGA driver without DirectDraw support). */
static BOOL driver_has_video_memory(IDirectDraw *ddraw)
{
    DDCAPS hal, hel;
    HRESULT hr;

    memset(&hal, 0, sizeof(hal));
    memset(&hel, 0, sizeof(hel));
    hal.dwSize = hel.dwSize = sizeof(DDCAPS);
    hr = IDirectDraw_GetCaps(ddraw, &hal, &hel);
    ok(hr == DD_OK, "GetCaps returned 0x%lx\n", hr);
    trace("driver caps 0x%08lx ddscaps 0x%08lx video memory %lu\n",
          hal.dwCaps, hal.ddsCaps.dwCaps, hal.dwVidMemTotal);
    return SUCCEEDED(hr) && hal.dwVidMemTotal != 0;
}

static void check_create(HRESULT hr, BOOL video_memory, const char *what)
{
    if (video_memory)
        ok(hr == DD_OK, "CreateSurface(%s, video memory) returned 0x%lx\n", what, hr);
    else
        ok(hr == DDERR_NODIRECTDRAWHW, "CreateSurface(%s, video memory) returned 0x%lx without video memory\n", what, hr);
}

/* What SDL 1.2's DirectX video driver asks for before it uses DirectDraw at all. */
static void test_primary(IDirectDraw *ddraw, BOOL video_memory)
{
    IDirectDrawSurface3 *surface3;
    IDirectDrawSurface *surface;
    DDSURFACEDESC desc;
    HRESULT hr;

    hr = IDirectDraw_SetCooperativeLevel(ddraw, NULL, DDSCL_NORMAL);
    ok(hr == DD_OK, "SetCooperativeLevel returned 0x%lx\n", hr);

    init_desc(&desc, DDSD_CAPS, DDSCAPS_PRIMARYSURFACE | DDSCAPS_VIDEOMEMORY);
    hr = IDirectDraw_CreateSurface(ddraw, &desc, &surface, NULL);
    check_create(hr, video_memory, "primary");
    if (FAILED(hr))
        return;

    init_desc(&desc, 0, 0);
    hr = IDirectDrawSurface_GetSurfaceDesc(surface, &desc);
    ok(hr == DD_OK, "GetSurfaceDesc returned 0x%lx\n", hr);
    trace("primary caps 0x%08lx\n", desc.ddsCaps.dwCaps);
    ok(desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE, "caps 0x%08lx lack DDSCAPS_PRIMARYSURFACE\n", desc.ddsCaps.dwCaps);
    ok(desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY, "caps 0x%08lx lack DDSCAPS_VIDEOMEMORY\n", desc.ddsCaps.dwCaps);
    ok(!(desc.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY), "caps 0x%08lx have DDSCAPS_SYSTEMMEMORY\n", desc.ddsCaps.dwCaps);

    hr = IDirectDrawSurface_QueryInterface(surface, &IID_IDirectDrawSurface3, (void **)&surface3);
    ok(hr == DD_OK, "QueryInterface(IDirectDrawSurface3) returned 0x%lx\n", hr);
    if (SUCCEEDED(hr))
        IDirectDrawSurface3_Release(surface3);

    IDirectDrawSurface_Release(surface);
}

static void test_offscreen(IDirectDraw *ddraw, HWND hwnd, BOOL video_memory)
{
    IDirectDrawSurface *vidmem, *sysmem;
    DDSURFACEDESC desc;
    DWORD x, y, bad;
    DDBLTFX fx;
    HRESULT hr;
    RECT rect;

    hr = IDirectDraw_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);
    ok(hr == DD_OK, "SetCooperativeLevel returned 0x%lx\n", hr);

    /* The display format, which a driver with video memory supports. */
    init_desc(&desc, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY);
    desc.dwWidth = 64;
    desc.dwHeight = 32;
    hr = IDirectDraw_CreateSurface(ddraw, &desc, &vidmem, NULL);
    check_create(hr, video_memory, "offscreen");
    if (FAILED(hr))
        return;

    init_desc(&desc, 0, 0);
    hr = IDirectDrawSurface_GetSurfaceDesc(vidmem, &desc);
    ok(hr == DD_OK, "GetSurfaceDesc returned 0x%lx\n", hr);
    trace("offscreen caps 0x%08lx\n", desc.ddsCaps.dwCaps);
    ok(desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY, "caps 0x%08lx lack DDSCAPS_VIDEOMEMORY\n", desc.ddsCaps.dwCaps);
    ok(!(desc.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY), "caps 0x%08lx have DDSCAPS_SYSTEMMEMORY\n", desc.ddsCaps.dwCaps);
    if (desc.ddpfPixelFormat.dwRGBBitCount != 32 || desc.ddpfPixelFormat.dwRBitMask != 0x00ff0000
        || desc.ddpfPixelFormat.dwGBitMask != 0x0000ff00 || desc.ddpfPixelFormat.dwBBitMask != 0x000000ff)
    {
        skip("The display format is not 32 bpp XRGB (%lu bpp)\n", desc.ddpfPixelFormat.dwRGBBitCount);
        IDirectDrawSurface_Release(vidmem);
        return;
    }

    init_desc(&desc, 0, 0);
    hr = IDirectDrawSurface_Lock(vidmem, NULL, &desc, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
    ok(hr == DD_OK, "Lock returned 0x%lx\n", hr);
    if (SUCCEEDED(hr))
    {
        for (y = 0; y < 32; ++y)
            for (x = 0; x < 64; ++x)
                ((DWORD *)((BYTE *)desc.lpSurface + y * desc.lPitch))[x] = (y << 16) | (x << 8) | 0x5a;
        hr = IDirectDrawSurface_Unlock(vidmem, NULL);
        ok(hr == DD_OK, "Unlock returned 0x%lx\n", hr);
    }

    init_desc(&desc, DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
    desc.dwWidth = 64;
    desc.dwHeight = 32;
    hr = IDirectDraw_CreateSurface(ddraw, &desc, &sysmem, NULL);
    ok(hr == DD_OK, "CreateSurface(offscreen, system memory) returned 0x%lx\n", hr);
    if (SUCCEEDED(hr))
    {
        SetRect(&rect, 0, 0, 64, 32);
        hr = IDirectDrawSurface_Blt(sysmem, &rect, vidmem, &rect, DDBLT_WAIT, NULL);
        ok(hr == DD_OK, "Blt returned 0x%lx\n", hr);

        init_desc(&desc, 0, 0);
        hr = IDirectDrawSurface_Lock(sysmem, NULL, &desc, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
        ok(hr == DD_OK, "Lock returned 0x%lx\n", hr);
        if (SUCCEEDED(hr))
        {
            for (y = 0, bad = 0; y < 32; ++y)
                for (x = 0; x < 64; ++x)
                    if ((((DWORD *)((BYTE *)desc.lpSurface + y * desc.lPitch))[x] & 0xffffff) != ((y << 16) | (x << 8) | 0x5a))
                        ++bad;
            ok(bad == 0, "%lu pixels differ after the blit\n", bad);
            IDirectDrawSurface_Unlock(sysmem, NULL);
        }
        IDirectDrawSurface_Release(sysmem);
    }

    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);
    fx.dwFillColor = 0x00123456;
    hr = IDirectDrawSurface_Blt(vidmem, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    ok(hr == DD_OK, "Blt(DDBLT_COLORFILL) returned 0x%lx\n", hr);
    init_desc(&desc, 0, 0);
    hr = IDirectDrawSurface_Lock(vidmem, NULL, &desc, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    ok(hr == DD_OK, "Lock returned 0x%lx\n", hr);
    if (SUCCEEDED(hr))
    {
        for (y = 0, bad = 0; y < 32; ++y)
            for (x = 0; x < 64; ++x)
                if ((((DWORD *)((BYTE *)desc.lpSurface + y * desc.lPitch))[x] & 0xffffff) != 0x123456)
                    ++bad;
        ok(bad == 0, "%lu pixels differ after the colour fill\n", bad);
        IDirectDrawSurface_Unlock(vidmem, NULL);
    }

    IDirectDrawSurface_Release(vidmem);
}

START_TEST(VideoMemory)
{
    IDirectDraw *ddraw;
    PFN_DIRECTDRAWCREATE pDirectDrawCreate;
    HMODULE hDDraw;
    BOOL video_memory;
    HRESULT hr;
    HWND hwnd;

    /* Loaded here, so that the NoDriverNotice child starts without the DirectX DLLs */
    hDDraw = LoadLibraryW(L"ddraw.dll");
    pDirectDrawCreate = hDDraw ? (PFN_DIRECTDRAWCREATE)GetProcAddress(hDDraw, "DirectDrawCreate") : NULL;
    ok(pDirectDrawCreate != NULL, "No DirectDrawCreate: %lu\n", GetLastError());
    if (!pDirectDrawCreate)
        return;
    hr = pDirectDrawCreate(NULL, &ddraw, NULL);
    ok(hr == DD_OK, "DirectDrawCreate returned 0x%lx\n", hr);
    if (FAILED(hr))
        return;

    test_no_notice();
    video_memory = driver_has_video_memory(ddraw);
    test_primary(ddraw, video_memory);

    hwnd = create_window();
    if (hwnd)
    {
        test_offscreen(ddraw, hwnd, video_memory);
        DestroyWindow(hwnd);
    }

    IDirectDraw_Release(ddraw);
}
