/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Rendering with the generic OpenGL implementation (no ICD)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windows.h>
#include <wingdi.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>

#include "wine/test.h"

static void check_strings(void)
{
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    int major = 0, minor = 0;

    ok(vendor && *vendor, "GL_VENDOR is empty\n");
    ok(renderer && *renderer, "GL_RENDERER is empty\n");
    ok(version != NULL, "GL_VERSION is NULL\n");
    ok(extensions != NULL, "GL_EXTENSIONS is NULL\n");
    if (version)
    {
        ok(sscanf(version, "%d.%d", &major, &minor) == 2, "GL_VERSION '%s' does not start with major.minor\n", version);
        ok(major > 1 || (major == 1 && minor >= 1), "GL_VERSION '%s' is below 1.1\n", version);
    }
    trace("vendor '%s' renderer '%s' version '%s'\n", vendor, renderer, version);
}

/* Green background, smooth red quad in the top left quadrant, flat blue quad in the bottom
 * right quadrant: rows and columns are both told apart, flat and smooth shading take
 * different span paths, and each quad touches a checked edge row. */
static void draw_scene(void)
{
    glViewport(0, 0, 16, 16);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_DITHER);

    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glShadeModel(GL_SMOOTH);
    glColor3f(1.0f, 0.0f, 0.0f);
    glBegin(GL_QUADS);
    glVertex2f(-1.0f, 0.0f);
    glVertex2f(0.0f, 0.0f);
    glVertex2f(0.0f, 1.0f);
    glVertex2f(-1.0f, 1.0f);
    glEnd();

    glShadeModel(GL_FLAT);
    glColor3f(0.0f, 0.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(0.0f, -1.0f);
    glVertex2f(1.0f, -1.0f);
    glVertex2f(1.0f, 0.0f);
    glVertex2f(0.0f, 0.0f);
    glEnd();
    glFinish();
    ok(glGetError() == GL_NO_ERROR, "GL error after drawing\n");
}

/* Expected 0xRRGGBB at GL pixel (x, y), y = 0 being the bottom row; 0 for pixels next to
 * a quad edge, which are not checked. */
static DWORD expected_color(int x, int y)
{
    if (x == 7 || x == 8 || y == 7 || y == 8)
        return 0;
    if (x < 8 && y > 8)
        return 0xff0000;
    if (x > 8 && y < 8)
        return 0x0000ff;
    return 0x00ff00;
}

static void check_readback(void)
{
    BYTE pixels[16 * 16 * 4];
    int x, y, bad = 0, first_x = -1, first_y = -1;
    DWORD first = 0;

    memset(pixels, 0x77, sizeof(pixels));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    ok(glGetError() == GL_NO_ERROR, "glReadPixels failed\n");

    for (y = 0; y < 16; y++)
    {
        for (x = 0; x < 16; x++)
        {
            const BYTE *p = &pixels[(y * 16 + x) * 4];
            DWORD color = (p[0] << 16) | (p[1] << 8) | p[2];
            DWORD expected = expected_color(x, y);
            if (expected && color != expected)
            {
                if (!bad++)
                {
                    first_x = x;
                    first_y = y;
                    first = color;
                }
            }
        }
    }
    ok(bad == 0, "glReadPixels: %d pixels differ, first at (%d, %d): 0x%06lx, expected 0x%06lx\n",
       bad, first_x, first_y, first, bad ? expected_color(first_x, first_y) : 0);
}

/* A generic (software) format: ChoosePixelFormat would prefer an installed ICD's formats. */
static int find_generic_format(HDC hdc, DWORD flags, BYTE color_bits)
{
    PIXELFORMATDESCRIPTOR pfd;
    int format, count;

    count = DescribePixelFormat(hdc, 1, sizeof(pfd), &pfd);
    for (format = 1; format <= count; format++)
    {
        if (!DescribePixelFormat(hdc, format, sizeof(pfd), &pfd))
            continue;
        if ((pfd.dwFlags & (flags | PFD_GENERIC_FORMAT | PFD_GENERIC_ACCELERATED)) != (flags | PFD_GENERIC_FORMAT))
            continue;
        if (pfd.iPixelType == PFD_TYPE_RGBA && pfd.cColorBits == color_bits)
            return format;
    }
    return 0;
}

static void test_window(void)
{
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC context;
    HWND hwnd;
    HDC hdc;
    int format;

    hwnd = CreateWindowA("static", "sw_render", WS_POPUP, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "CreateWindowA failed\n");
    if (!hwnd)
        return;
    hdc = GetDC(hwnd);

    format = find_generic_format(hdc, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                 GetDeviceCaps(hdc, BITSPIXEL));
    ok(format > 0, "no generic double-buffered window format for %d bpp\n", GetDeviceCaps(hdc, BITSPIXEL));
    if (format > 0)
    {
        DescribePixelFormat(hdc, format, sizeof(pfd), &pfd);
        ok(SetPixelFormat(hdc, format, &pfd), "SetPixelFormat failed, error %lu\n", GetLastError());
        context = wglCreateContext(hdc);
        ok(context != NULL, "wglCreateContext failed, error %lu\n", GetLastError());
        if (context)
        {
            ok(wglMakeCurrent(hdc, context), "wglMakeCurrent failed, error %lu\n", GetLastError());
            check_strings();
            draw_scene();
            glReadBuffer(GL_BACK);
            check_readback();
            wglMakeCurrent(NULL, NULL);
            ok(wglDeleteContext(context), "wglDeleteContext failed\n");
        }
    }

    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
}

static void test_bitmap(void)
{
    PIXELFORMATDESCRIPTOR pfd;
    BITMAPINFO bmi;
    HBITMAP bitmap, old;
    HGLRC context;
    DWORD *bits = NULL;
    int format, x, y, bad = 0, first_x = 0, first_y = 0;
    HDC hdc;

    hdc = CreateCompatibleDC(NULL);
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = 16;
    bmi.bmiHeader.biHeight = 16;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    ok(bitmap != NULL, "CreateDIBSection failed\n");
    if (!bitmap)
    {
        DeleteDC(hdc);
        return;
    }
    old = SelectObject(hdc, bitmap);

    format = find_generic_format(hdc, PFD_DRAW_TO_BITMAP | PFD_SUPPORT_OPENGL, 32);
    ok(format > 0, "no generic 32 bpp bitmap format\n");
    if (format > 0)
    {
        DescribePixelFormat(hdc, format, sizeof(pfd), &pfd);
        ok(!(pfd.dwFlags & PFD_DOUBLEBUFFER), "bitmap format %d is double-buffered\n", format);
        ok(SetPixelFormat(hdc, format, &pfd), "SetPixelFormat failed, error %lu\n", GetLastError());
        context = wglCreateContext(hdc);
        ok(context != NULL, "wglCreateContext failed, error %lu\n", GetLastError());
        if (context)
        {
            ok(wglMakeCurrent(hdc, context), "wglMakeCurrent failed, error %lu\n", GetLastError());
            draw_scene();
            check_readback();
            GdiFlush();
            /* Bottom-up DIB: memory row y is GL row y, independent of glReadPixels. */
            for (y = 0; y < 16; y++)
            {
                for (x = 0; x < 16; x++)
                {
                    DWORD expected = expected_color(x, y);
                    if (expected && (bits[y * 16 + x] & 0xffffff) != expected && !bad++)
                    {
                        first_x = x;
                        first_y = y;
                    }
                }
            }
            ok(bad == 0, "DIB: %d pixels differ, first at (%d, %d): 0x%06lx, expected 0x%06lx\n", bad, first_x, first_y,
               bad ? bits[first_y * 16 + first_x] & 0xffffff : 0, bad ? expected_color(first_x, first_y) : 0);
            wglMakeCurrent(NULL, NULL);
            ok(wglDeleteContext(context), "wglDeleteContext failed\n");
        }
    }

    SelectObject(hdc, old);
    DeleteObject(bitmap);
    DeleteDC(hdc);
}

START_TEST(sw_render)
{
    test_window();
    test_bitmap();
}
