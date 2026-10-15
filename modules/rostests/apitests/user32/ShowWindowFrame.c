/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Tests the frame messages of ShowWindow and a custom frame through minimize, maximize and restore
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef SWP_STATECHANGED
#define SWP_STATECHANGED 0x8000
#endif

#define MAX_EVENTS 64

typedef struct FRAME_EVENT
{
    UINT msg;
    UINT value; /* WINDOWPOS flags, or the WM_SIZE type */
} FRAME_EVENT;

static FRAME_EVENT s_events[MAX_EVENTS];
static UINT s_cEvents;

/* A window that draws its own caption: it keeps the side frame but moves the client area to the top
   of the window, unless the window is minimized. It sets this when it minimizes the window itself and
   learns about every later state change from SWP_FRAMECHANGED, as browsers do. */
static BOOL s_bMinimizedFrame;

static void Record(UINT msg, UINT value)
{
    if (s_cEvents < MAX_EVENTS)
    {
        s_events[s_cEvents].msg = msg;
        s_events[s_cEvents].value = value;
        s_cEvents++;
    }
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    LRESULT ret;
    RECT *prc;
    LONG top;

    switch (uMsg)
    {
        case WM_WINDOWPOSCHANGING:
        {
            WINDOWPOS *pwp = (WINDOWPOS *)lParam;
            Record(uMsg, pwp->flags);
            if ((pwp->flags & SWP_FRAMECHANGED) && !(pwp->flags & SWP_NOSIZE))
                s_bMinimizedFrame = IsIconic(hwnd);
            break;
        }

        case WM_WINDOWPOSCHANGED:
            Record(uMsg, ((WINDOWPOS *)lParam)->flags);
            break;

        case WM_SIZE:
            Record(uMsg, (UINT)wParam);
            break;

        case WM_NCCALCSIZE:
            if (wParam)
                Record(uMsg, 0);
            prc = wParam ? &((NCCALCSIZE_PARAMS *)lParam)->rgrc[0] : (RECT *)lParam;
            top = prc->top;
            ret = DefWindowProcA(hwnd, uMsg, wParam, lParam);
            if (!s_bMinimizedFrame)
                prc->top = top;
            return ret;
    }

    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

typedef struct STATE
{
    RECT rcWindow;
    POINT ptClient;
} STATE;

static void GetState(HWND hwnd, STATE *pState)
{
    GetWindowRect(hwnd, &pState->rcWindow);
    pState->ptClient.x = pState->ptClient.y = 0;
    ClientToScreen(hwnd, &pState->ptClient);
}

/* Look for the WM_WINDOWPOSCHANGING that carries the state change and check what follows it */
static void CheckStateChange(const char *step, UINT sizeType, UINT requiredFlags)
{
    UINT i;
    INT iChanging = -1, iCalc = -1, iChanged = -1, iSize = -1;

    for (i = 0; i < s_cEvents; i++)
    {
        switch (s_events[i].msg)
        {
            case WM_WINDOWPOSCHANGING:
                if ((s_events[i].value & SWP_STATECHANGED) && iChanging < 0)
                    iChanging = i;
                break;
            case WM_NCCALCSIZE:
                if (iChanging >= 0 && iCalc < 0)
                    iCalc = i;
                break;
            case WM_WINDOWPOSCHANGED:
                if ((s_events[i].value & SWP_STATECHANGED) && iChanged < 0)
                    iChanged = i;
                break;
            case WM_SIZE:
                if (iSize < 0)
                    iSize = i;
                break;
        }
    }

    ok(iChanging >= 0, "%s: no WM_WINDOWPOSCHANGING with SWP_STATECHANGED\n", step);
    ok(iChanged >= 0, "%s: no WM_WINDOWPOSCHANGED with SWP_STATECHANGED\n", step);
    if (iChanging >= 0)
    {
        ok((s_events[iChanging].value & requiredFlags) == requiredFlags,
           "%s: WM_WINDOWPOSCHANGING flags 0x%x, expected 0x%x set\n",
           step, s_events[iChanging].value, requiredFlags);
    }
    if (iChanged >= 0)
    {
        ok((s_events[iChanged].value & requiredFlags) == requiredFlags,
           "%s: WM_WINDOWPOSCHANGED flags 0x%x, expected 0x%x set\n",
           step, s_events[iChanged].value, requiredFlags);
    }
    ok(iCalc >= 0 && iCalc < iChanged, "%s: WM_NCCALCSIZE(TRUE) at %d, changed at %d\n", step, iCalc, iChanged);
    ok(iSize > iChanged, "%s: WM_SIZE at %d, changed at %d\n", step, iSize, iChanged);
    if (iSize >= 0)
        ok(s_events[iSize].value == sizeType, "%s: WM_SIZE type %u, expected %u\n", step, s_events[iSize].value, sizeType);
}

static void CheckRestored(const char *step, HWND hwnd, const STATE *pNormal)
{
    STATE state;
    GetState(hwnd, &state);
    ok(EqualRect(&state.rcWindow, &pNormal->rcWindow),
       "%s: window rect (%ld,%ld,%ld,%ld), expected (%ld,%ld,%ld,%ld)\n", step,
       state.rcWindow.left, state.rcWindow.top, state.rcWindow.right, state.rcWindow.bottom,
       pNormal->rcWindow.left, pNormal->rcWindow.top, pNormal->rcWindow.right, pNormal->rcWindow.bottom);
    ok(state.ptClient.x == pNormal->ptClient.x && state.ptClient.y == pNormal->ptClient.y,
       "%s: client origin (%ld,%ld), expected (%ld,%ld)\n", step,
       state.ptClient.x, state.ptClient.y, pNormal->ptClient.x, pNormal->ptClient.y);
}

/* The restore rectangle of a window is in screen coordinates, WINDOWPLACEMENT gives it relative to the work area.
   With a work area that does not start at the corner of the screen (a taskbar on the left or at the top) the
   window must still come back where it was. */
static void test_work_area_offset(HINSTANCE hInst)
{
    WNDCLASSA wc;
    RECT orig, wa, got, before, after;
    HWND hwnd;
    BOOL set;

    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &orig, 0))
    {
        skip("SPI_GETWORKAREA failed\n");
        return;
    }
    wa = orig;
    wa.left += 100;
    wa.top += 60;
    set = SystemParametersInfoA(SPI_SETWORKAREA, 0, &wa, 0);
    if (!set || !SystemParametersInfoA(SPI_GETWORKAREA, 0, &got, 0) || !EqualRect(&got, &wa))
    {
        skip("The work area cannot be moved (%ld,%ld)-(%ld,%ld)\n", got.left, got.top, got.right, got.bottom);
        if (set)
            SystemParametersInfoA(SPI_SETWORKAREA, 0, &orig, 0);
        return;
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = hInst;
    wc.lpszClassName = "ShowWindowFrameWorkArea";
    RegisterClassA(&wc);
    hwnd = CreateWindowA(wc.lpszClassName, "work area", WS_OVERLAPPEDWINDOW, 320, 220, 400, 260, NULL, NULL, hInst, NULL);
    ok(hwnd != NULL, "CreateWindowA failed: %lu\n", GetLastError());
    if (hwnd)
    {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        GetWindowRect(hwnd, &before);

        ShowWindow(hwnd, SW_MINIMIZE);
        ShowWindow(hwnd, SW_RESTORE);
        GetWindowRect(hwnd, &after);
        ok(EqualRect(&before, &after), "minimize and restore: (%ld,%ld)-(%ld,%ld), expected (%ld,%ld)-(%ld,%ld)\n",
           after.left, after.top, after.right, after.bottom, before.left, before.top, before.right, before.bottom);

        ShowWindow(hwnd, SW_MAXIMIZE);
        ShowWindow(hwnd, SW_RESTORE);
        GetWindowRect(hwnd, &after);
        ok(EqualRect(&before, &after), "maximize and restore: (%ld,%ld)-(%ld,%ld), expected (%ld,%ld)-(%ld,%ld)\n",
           after.left, after.top, after.right, after.bottom, before.left, before.top, before.right, before.bottom);

        SendMessageA(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        SendMessageA(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        GetWindowRect(hwnd, &after);
        ok(EqualRect(&before, &after), "SC_MINIMIZE and SC_RESTORE: (%ld,%ld)-(%ld,%ld), expected (%ld,%ld)-(%ld,%ld)\n",
           after.left, after.top, after.right, after.bottom, before.left, before.top, before.right, before.bottom);

        ShowWindow(hwnd, SW_MAXIMIZE);
        ShowWindow(hwnd, SW_MINIMIZE);
        ShowWindow(hwnd, SW_RESTORE);
        ShowWindow(hwnd, SW_RESTORE);
        GetWindowRect(hwnd, &after);
        ok(EqualRect(&before, &after), "maximize, minimize and restore twice: (%ld,%ld)-(%ld,%ld), expected (%ld,%ld)-(%ld,%ld)\n",
           after.left, after.top, after.right, after.bottom, before.left, before.top, before.right, before.bottom);

        DestroyWindow(hwnd);
    }
    UnregisterClassA(wc.lpszClassName, hInst);
    SystemParametersInfoA(SPI_SETWORKAREA, 0, &orig, 0);
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &got, 0);
    ok(EqualRect(&got, &orig), "the work area was not restored: (%ld,%ld)-(%ld,%ld)\n", got.left, got.top, got.right, got.bottom);
}

#define FRAME (SWP_FRAMECHANGED | SWP_STATECHANGED)
#define STEP(call) do { s_cEvents = 0; call; } while (0)

START_TEST(ShowWindowFrame)
{
    WNDCLASSA wc;
    HWND hwnd;
    STATE normal;
    HINSTANCE hInst = GetModuleHandleA(NULL);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "ShowWindowFrameTest";
    if (!RegisterClassA(&wc))
    {
        skip("RegisterClassA failed\n");
        return;
    }

    hwnd = CreateWindowA(wc.lpszClassName, "ShowWindowFrame", WS_OVERLAPPEDWINDOW,
                         120, 90, 400, 260, NULL, NULL, hInst, NULL);
    if (!hwnd)
    {
        skip("CreateWindowA failed\n");
        UnregisterClassA(wc.lpszClassName, hInst);
        return;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    /* The application applies its frame once the window exists */
    s_bMinimizedFrame = FALSE;
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    GetState(hwnd, &normal);
    ok(normal.ptClient.y == normal.rcWindow.top, "client starts at y=%ld, window at y=%ld\n", normal.ptClient.y, normal.rcWindow.top);
    ok(normal.ptClient.x > normal.rcWindow.left, "client starts at x=%ld, window at x=%ld\n", normal.ptClient.x, normal.rcWindow.left);

    /* The taskbar minimizes with ShowWindow, the title bar button with SC_MINIMIZE, and the taskbar restores
       with SW_RESTORE or SC_RESTORE */
    STEP(ShowWindow(hwnd, SW_MINIMIZE));
    s_bMinimizedFrame = TRUE;
    ok(IsIconic(hwnd), "window is not minimized\n");
    CheckStateChange("ShowWindow(SW_MINIMIZE)", SIZE_MINIMIZED, FRAME | SWP_NOCOPYBITS);

    STEP(ShowWindow(hwnd, SW_RESTORE));
    ok(!IsIconic(hwnd) && !IsZoomed(hwnd), "window is not restored\n");
    CheckStateChange("ShowWindow(SW_RESTORE) from minimized", SIZE_RESTORED, FRAME | SWP_NOCOPYBITS);
    CheckRestored("ShowWindow(SW_RESTORE) from minimized", hwnd, &normal);

    STEP(SendMessageA(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0));
    s_bMinimizedFrame = TRUE;
    ok(IsIconic(hwnd), "window is not minimized by SC_MINIMIZE\n");
    CheckStateChange("SC_MINIMIZE", SIZE_MINIMIZED, FRAME | SWP_NOCOPYBITS);

    STEP(SendMessageA(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0));
    ok(!IsIconic(hwnd) && !IsZoomed(hwnd), "window is not restored by SC_RESTORE\n");
    CheckStateChange("SC_RESTORE from minimized", SIZE_RESTORED, FRAME | SWP_NOCOPYBITS);
    CheckRestored("SC_RESTORE from minimized", hwnd, &normal);

    STEP(ShowWindow(hwnd, SW_MAXIMIZE));
    ok(IsZoomed(hwnd), "window is not maximized\n");
    CheckStateChange("ShowWindow(SW_MAXIMIZE)", SIZE_MAXIMIZED, FRAME);

    STEP(ShowWindow(hwnd, SW_RESTORE));
    ok(!IsIconic(hwnd) && !IsZoomed(hwnd), "window is not restored from maximized\n");
    CheckStateChange("ShowWindow(SW_RESTORE) from maximized", SIZE_RESTORED, FRAME);
    CheckRestored("ShowWindow(SW_RESTORE) from maximized", hwnd, &normal);

    /* Minimize a maximized window, which restores to maximized, then restore twice */
    ShowWindow(hwnd, SW_MAXIMIZE);
    STEP(ShowWindow(hwnd, SW_MINIMIZE));
    s_bMinimizedFrame = TRUE;
    ok(IsIconic(hwnd), "maximized window is not minimized\n");
    CheckStateChange("ShowWindow(SW_MINIMIZE) from maximized", SIZE_MINIMIZED, FRAME | SWP_NOCOPYBITS);

    STEP(ShowWindow(hwnd, SW_RESTORE));
    ok(IsZoomed(hwnd) && !IsIconic(hwnd), "window did not return to maximized\n");
    CheckStateChange("ShowWindow(SW_RESTORE) to maximized", SIZE_MAXIMIZED, FRAME | SWP_NOCOPYBITS);

    STEP(ShowWindow(hwnd, SW_RESTORE));
    ok(!IsIconic(hwnd) && !IsZoomed(hwnd), "window is not restored\n");
    CheckStateChange("ShowWindow(SW_RESTORE) from maximized again", SIZE_RESTORED, FRAME);
    CheckRestored("ShowWindow(SW_RESTORE) from maximized again", hwnd, &normal);

    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, hInst);

    test_work_area_offset(hInst);
}
