/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Direct2D render targets reach the screen: window targets present through the
 *              swap chain, DC targets copy to the DC they are bound to
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define COBJMACROS
#include <windows.h>
#include <d2d1.h>

#include "wine/test.h"

#define WND_X 100
#define WND_Y 100
#define WND_SIZE 96

static ID2D1Factory *factory;

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG msg;

    do
    {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(10);
    } while ((LONG)(GetTickCount() - end) < 0);
}

static HWND create_window(void)
{
    HWND hwnd = CreateWindowExA(WS_EX_TOPMOST, "STATIC", "d2d1_present", WS_POPUP | WS_VISIBLE,
                                WND_X, WND_Y, WND_SIZE, WND_SIZE, NULL, NULL, GetModuleHandleA(NULL), NULL);
    ok(hwnd != NULL, "CreateWindowEx failed, error %lu\n", GetLastError());
    pump(300);
    return hwnd;
}

static void fill(ID2D1RenderTarget *rt, float r, float g, float b)
{
    D2D1_COLOR_F color;
    D2D1_RECT_F rect;
    ID2D1SolidColorBrush *brush;
    D2D1_COLOR_F white = {1.0f, 1.0f, 1.0f, 1.0f};
    HRESULT hr;

    color.r = r; color.g = g; color.b = b; color.a = 1.0f;
    rect.left = 16.0f; rect.top = 16.0f; rect.right = 48.0f; rect.bottom = 48.0f;

    ID2D1RenderTarget_BeginDraw(rt);
    ID2D1RenderTarget_Clear(rt, &white);
    hr = ID2D1RenderTarget_CreateSolidColorBrush(rt, &color, NULL, &brush);
    ok(hr == S_OK, "CreateSolidColorBrush failed, hr %#lx\n", hr);
    if (SUCCEEDED(hr))
    {
        ID2D1RenderTarget_FillRectangle(rt, &rect, (ID2D1Brush *)brush);
        ID2D1SolidColorBrush_Release(brush);
    }
    hr = ID2D1RenderTarget_EndDraw(rt, NULL, NULL);
    ok(hr == S_OK, "EndDraw failed, hr %#lx\n", hr);
}

static void check_screen(const char *what, BOOL todo)
{
    HDC screen = GetDC(NULL);
    COLORREF inside = GetPixel(screen, WND_X + 32, WND_Y + 32);
    COLORREF outside = GetPixel(screen, WND_X + 80, WND_Y + 80);

    ReleaseDC(NULL, screen);
    todo_if(todo)
    ok(inside == RGB(0, 0, 255), "%s: the rectangle is %06lx on the screen, expected 0000ff\n", what, inside);
    todo_if(todo)
    ok(outside == RGB(255, 255, 255), "%s: the cleared area is %06lx on the screen, expected ffffff\n", what, outside);
}

static void test_hwnd_target_present(void)
{
    D2D1_RENDER_TARGET_PROPERTIES desc;
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_desc;
    ID2D1HwndRenderTarget *rt;
    HWND hwnd;
    HRESULT hr;
    int frame;

    hwnd = create_window();
    if (!hwnd)
        return;

    memset(&desc, 0, sizeof(desc));
    desc.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    desc.pixelFormat.format = DXGI_FORMAT_UNKNOWN;
    desc.pixelFormat.alphaMode = D2D1_ALPHA_MODE_UNKNOWN;
    memset(&hwnd_desc, 0, sizeof(hwnd_desc));
    hwnd_desc.hwnd = hwnd;
    hwnd_desc.pixelSize.width = WND_SIZE;
    hwnd_desc.pixelSize.height = WND_SIZE;
    hwnd_desc.presentOptions = D2D1_PRESENT_OPTIONS_NONE;

    hr = ID2D1Factory_CreateHwndRenderTarget(factory, &desc, &hwnd_desc, &rt);
    if (FAILED(hr))
    {
        skip("Cannot create a window render target, hr %#lx\n", hr);
        DestroyWindow(hwnd);
        return;
    }

    for (frame = 0; frame < 2; ++frame)
    {
        fill((ID2D1RenderTarget *)rt, 0.0f, 0.0f, 1.0f);
        pump(300);
        check_screen("window target", FALSE);
    }

    ID2D1HwndRenderTarget_Release(rt);
    DestroyWindow(hwnd);
}

static void test_dc_target_screen(BOOL after_window_target)
{
    D2D1_RENDER_TARGET_PROPERTIES desc;
    ID2D1DCRenderTarget *rt;
    HWND hwnd;
    HDC dc;
    RECT rect;
    HRESULT hr;

    hwnd = create_window();
    if (!hwnd)
        return;

    memset(&desc, 0, sizeof(desc));
    desc.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    desc.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;

    hr = ID2D1Factory_CreateDCRenderTarget(factory, &desc, &rt);
    if (FAILED(hr))
    {
        skip("Cannot create a DC render target, hr %#lx\n", hr);
        DestroyWindow(hwnd);
        return;
    }

    dc = GetDC(hwnd);
    SetRect(&rect, 0, 0, WND_SIZE, WND_SIZE);
    hr = ID2D1DCRenderTarget_BindDC(rt, dc, &rect);
    ok(hr == S_OK, "BindDC failed, hr %#lx\n", hr);
    fill((ID2D1RenderTarget *)rt, 0.0f, 0.0f, 1.0f);
    GdiFlush();
    pump(100);
    check_screen("DC target on a window DC", after_window_target);

    ReleaseDC(hwnd, dc);
    ID2D1DCRenderTarget_Release(rt);
    DestroyWindow(hwnd);
}

START_TEST(d2d1_present)
{
    HRESULT hr;

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL, (void **)&factory);
    ok(hr == S_OK, "D2D1CreateFactory failed, hr %#lx\n", hr);
    if (FAILED(hr))
        return;

    test_dc_target_screen(FALSE);
    test_hwnd_target_present();
    /* Known failure, independent of the window target fix: once a window target has presented, every
     * DC target of the process reads back zeros from its surface (a memory DIB as well as a window DC) */
    test_dc_target_screen(TRUE);

    ID2D1Factory_Release(factory);
}
