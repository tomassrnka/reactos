/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     wglMakeCurrent on a context that is already current binds it to the new DC
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windows.h>
#include <wingdi.h>
#include <GL/gl.h>

#include "wine/test.h"

static HWND create_window(int x)
{
    HWND hwnd = CreateWindowExA(0, "STATIC", "make_current", WS_POPUP | WS_VISIBLE, x, 100, 64, 64,
                                NULL, NULL, GetModuleHandleA(NULL), NULL);
    ok(hwnd != NULL, "CreateWindowEx failed, error %lu\n", GetLastError());
    return hwnd;
}

static BOOL set_format(HDC hdc, int format, PIXELFORMATDESCRIPTOR *pfd)
{
    BOOL ret = SetPixelFormat(hdc, format, pfd);
    ok(ret, "SetPixelFormat(%d) failed, error %lu\n", format, GetLastError());
    return ret;
}

static void test_rebind(BOOL release_first)
{
    PIXELFORMATDESCRIPTOR pfd;
    HWND wnd_a, wnd_b;
    HDC dc_a, dc_b;
    HGLRC rc;
    int format;
    BOOL ret;

    wnd_a = create_window(100);
    wnd_b = create_window(300);
    if (!wnd_a || !wnd_b)
        goto done;

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;

    dc_a = GetDC(wnd_a);
    format = ChoosePixelFormat(dc_a, &pfd);
    ok(format != 0, "ChoosePixelFormat failed, error %lu\n", GetLastError());
    if (!format || !set_format(dc_a, format, &pfd))
    {
        ReleaseDC(wnd_a, dc_a);
        goto done;
    }

    rc = wglCreateContext(dc_a);
    ok(rc != NULL, "wglCreateContext failed, error %lu\n", GetLastError());
    ret = rc && wglMakeCurrent(dc_a, rc);
    ok(ret, "wglMakeCurrent(A) failed, error %lu\n", GetLastError());
    if (!ret)
    {
        if (rc) wglDeleteContext(rc);
        ReleaseDC(wnd_a, dc_a);
        goto done;
    }
    ok(wglGetCurrentDC() == dc_a, "current DC is %p, expected %p\n", wglGetCurrentDC(), dc_a);

    /* wined3d releases the DC of the window and gets one of the window it presents to; cached DCs
     * reuse handles, so the new handle may be the old one */
    if (release_first)
        ReleaseDC(wnd_a, dc_a);

    dc_b = GetDC(wnd_b);
    if (!set_format(dc_b, format, &pfd))
    {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(rc);
        ReleaseDC(wnd_b, dc_b);
        if (!release_first) ReleaseDC(wnd_a, dc_a);
        goto done;
    }

    SetLastError(0xdeadbeef);
    ret = wglMakeCurrent(dc_b, rc);
    ok(ret, "wglMakeCurrent(B) of the current context failed, error %lu\n", GetLastError());
    ok(wglGetCurrentContext() == rc, "current context is %p, expected %p\n", wglGetCurrentContext(), rc);
    ok(wglGetCurrentDC() == dc_b, "current DC is %p, expected %p\n", wglGetCurrentDC(), dc_b);

    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SetLastError(0xdeadbeef);
    ret = SwapBuffers(dc_b);
    ok(ret, "SwapBuffers of the window the context was last made current on failed, error %lu\n", GetLastError());
    glClear(GL_COLOR_BUFFER_BIT);
    ret = SwapBuffers(dc_b);
    ok(ret, "second SwapBuffers failed, error %lu\n", GetLastError());
    glClear(GL_COLOR_BUFFER_BIT);

    /* and back */
    if (release_first)
    {
        ReleaseDC(wnd_b, dc_b);
        dc_a = GetDC(wnd_a);
        set_format(dc_a, format, &pfd);
        dc_b = NULL;
    }
    ret = wglMakeCurrent(dc_a, rc);
    ok(ret, "wglMakeCurrent(A) again failed, error %lu\n", GetLastError());
    ok(wglGetCurrentDC() == dc_a, "current DC is %p, expected %p\n", wglGetCurrentDC(), dc_a);
    glClear(GL_COLOR_BUFFER_BIT);
    ret = SwapBuffers(dc_a);
    ok(ret, "SwapBuffers(A) failed, error %lu\n", GetLastError());
    glClear(GL_COLOR_BUFFER_BIT);

    /* the same DC again changes nothing */
    ret = wglMakeCurrent(dc_a, rc);
    ok(ret, "wglMakeCurrent(A) of the same DC failed, error %lu\n", GetLastError());
    glClear(GL_COLOR_BUFFER_BIT);
    ret = SwapBuffers(dc_a);
    ok(ret, "SwapBuffers(A) failed, error %lu\n", GetLastError());

    ret = wglMakeCurrent(NULL, NULL);
    ok(ret, "wglMakeCurrent(NULL) failed, error %lu\n", GetLastError());
    wglDeleteContext(rc);
    ReleaseDC(wnd_a, dc_a);
    if (dc_b) ReleaseDC(wnd_b, dc_b);

done:
    if (wnd_a) DestroyWindow(wnd_a);
    if (wnd_b) DestroyWindow(wnd_b);
}

START_TEST(make_current)
{
    test_rebind(FALSE);
    test_rebind(TRUE);
}
