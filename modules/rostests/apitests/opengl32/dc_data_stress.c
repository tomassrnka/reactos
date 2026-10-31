/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Two threads create and destroy GL windows, make contexts current and query each other's
 *              windows: the pixel-format records of opengl32 must survive the sweep of the other thread
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windows.h>
#include <wingdi.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>

#include "wine/test.h"

#define DEFAULT_SECONDS 5

static volatile HDC shared_dc[2];
static volatile LONG stop;
static volatile LONG cycles[2];
static volatile LONG failures;

static void one_cycle(int me, unsigned int n)
{
    PIXELFORMATDESCRIPTOR pfd;
    HWND hwnd;
    HDC hdc;
    HGLRC rc;
    int format;

    hwnd = CreateWindowExA(0, "STATIC", "dc_data_stress", WS_POPUP, 0, 0, 32, 32,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd)
        return;
    hdc = GetDC(hwnd);

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;

    format = ChoosePixelFormat(hdc, &pfd);
    if (format && SetPixelFormat(hdc, format, &pfd))
    {
        rc = wglCreateContext(hdc);
        if (rc && wglMakeCurrent(hdc, rc))
        {
            shared_dc[me] = hdc;
            glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            SwapBuffers(hdc);
            if (GetPixelFormat(hdc) != format)
                InterlockedIncrement(&failures);
            /* every other time the window dies while its context is still current */
            if (n & 1)
                DestroyWindow(hwnd);
            wglMakeCurrent(NULL, NULL);
            shared_dc[me] = NULL;
        }
        else
            InterlockedIncrement(&failures);
        if (rc)
            wglDeleteContext(rc);
    }
    else
        InterlockedIncrement(&failures);

    ReleaseDC(hwnd, hdc);
    if (!(n & 1))
        DestroyWindow(hwnd);
}

static DWORD WINAPI worker(void *param)
{
    int me = (int)(INT_PTR)param;
    unsigned int n = 0;

    while (!stop)
    {
        HDC other = shared_dc[1 - me];

        one_cycle(me, n++);
        /* the other thread's window may be gone: the call has to fail cleanly, not crash */
        if (other)
        {
            GetPixelFormat(other);
            SwapBuffers(other);
        }
        InterlockedIncrement(&cycles[me]);
    }
    return 0;
}

START_TEST(dc_data_stress)
{
    HANDLE threads[2];
    const char *env = getenv("GL_STRESS_SECONDS");
    int seconds = env ? atoi(env) : DEFAULT_SECONDS;
    int i;

    if (seconds <= 0)
        seconds = DEFAULT_SECONDS;
    for (i = 0; i < 2; i++)
        threads[i] = CreateThread(NULL, 0, worker, (void *)(INT_PTR)i, 0, NULL);
    Sleep(seconds * 1000);
    InterlockedExchange(&stop, 1);
    WaitForMultipleObjects(2, threads, TRUE, 120000);
    trace("%ld + %ld cycles in %d s\n", cycles[0], cycles[1], seconds);
    ok(cycles[0] > 0 && cycles[1] > 0, "a thread made no progress\n");
    ok(!failures, "%ld operations failed\n", failures);
}
