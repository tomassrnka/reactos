/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     opengl32 does not keep a pixel-format record for every window that ever used OpenGL
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windows.h>
#include <wingdi.h>
#include <GL/gl.h>

#include "wine/test.h"

#define WARMUP_WINDOWS 100
#define TEST_WINDOWS   1000
/* Everything allocated per window that stays behind shows up here. The process heap also moves for
 * reasons of its own (the loader, the ICD, user32 callbacks), hence the slack: a record kept for each of
 * the TEST_WINDOWS windows would add that many blocks. */
#define ALLOWED_GROWTH 100

static BOOL window_gl_cycle(void)
{
    PIXELFORMATDESCRIPTOR pfd;
    HWND hwnd;
    HDC hdc;
    int format;
    BOOL ret = FALSE;

    hwnd = CreateWindowExA(0, "STATIC", "dc_data_leak", WS_POPUP, 0, 0, 32, 32,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd)
        return FALSE;
    hdc = GetDC(hwnd);

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;

    format = ChoosePixelFormat(hdc, &pfd);
    if (format)
        ret = SetPixelFormat(hdc, format, &pfd);

    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    return ret;
}

static SIZE_T count_busy_blocks(void)
{
    PROCESS_HEAP_ENTRY entry;
    HANDLE heap = GetProcessHeap();
    SIZE_T count = 0;

    HeapLock(heap);
    memset(&entry, 0, sizeof(entry));
    while (HeapWalk(heap, &entry))
    {
        if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY)
            count++;
    }
    HeapUnlock(heap);
    return count;
}

START_TEST(dc_data_leak)
{
    SIZE_T before, after;
    int i, failed = 0;

    for (i = 0; i < WARMUP_WINDOWS; i++)
        failed += !window_gl_cycle();
    if (failed == WARMUP_WINDOWS)
    {
        skip("Cannot set a pixel format on a window\n");
        return;
    }

    before = count_busy_blocks();
    for (i = 0; i < TEST_WINDOWS; i++)
        window_gl_cycle();
    after = count_busy_blocks();

    ok(after < before + ALLOWED_GROWTH, "%d windows left %lu new heap blocks behind\n",
       TEST_WINDOWS, (unsigned long)(after - before));
    trace("heap blocks %lu -> %lu\n", (unsigned long)before, (unsigned long)after);
}
