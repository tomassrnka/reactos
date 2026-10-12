/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Tests for the shcore DPI functions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include <apitest.h>
#include <wingdi.h>
#include <winuser.h>

typedef HRESULT (WINAPI *FN_GetDpiForMonitor)(HMONITOR, INT, UINT *, UINT *);
typedef UINT (WINAPI *FN_GetDpiForShellUIComponent)(INT);
typedef HRESULT (WINAPI *FN_GetProcessDpiAwareness)(HANDLE, INT *);
typedef HRESULT (WINAPI *FN_SetProcessDpiAwareness)(INT);

#define UNTOUCHED 0x5555

static HMODULE LoadShcore(void)
{
    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (!hShcore)
        skip("No shcore.dll\n");
    return hShcore;
}

START_TEST(GetDpiForMonitor)
{
    HMODULE hShcore = LoadShcore();
    FN_GetDpiForMonitor pGetDpiForMonitor;
    FN_GetDpiForShellUIComponent pGetDpiForShellUIComponent;
    POINT pt = { 0, 0 };
    HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    UINT x, y, uDpiX, uDpiY;
    HRESULT hr;
    HDC hdc;
    INT i;

    if (!hShcore)
        return;
    pGetDpiForMonitor = (FN_GetDpiForMonitor)GetProcAddress(hShcore, "GetDpiForMonitor");
    pGetDpiForShellUIComponent = (FN_GetDpiForShellUIComponent)GetProcAddress(hShcore, "GetDpiForShellUIComponent");
    if (!pGetDpiForMonitor || !pGetDpiForShellUIComponent)
    {
        skip("No DPI functions in shcore\n");
        return;
    }

    hdc = GetDC(NULL);
    uDpiX = GetDeviceCaps(hdc, LOGPIXELSX);
    uDpiY = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(NULL, hdc);

    /* Effective and angular DPI: the system DPI of an unaware process */
    for (i = 0; i <= 1; i++)
    {
        x = y = UNTOUCHED;
        hr = pGetDpiForMonitor(hMonitor, i, &x, &y);
        ok(hr == S_OK, "type %d: hr 0x%lx\n", i, hr);
        ok(x == uDpiX && y == uDpiY, "type %d: %u x %u, expected %u x %u\n", i, x, y, uDpiX, uDpiY);
    }

    /* Raw DPI needs the physical screen size; without it both are 0 */
    x = y = UNTOUCHED;
    hr = pGetDpiForMonitor(hMonitor, 2, &x, &y);
    ok(hr == S_OK || hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "raw: hr 0x%lx\n", hr);
    if (FAILED(hr))
        ok(x == 0 && y == 0, "raw: %u x %u\n", x, y);
    else
        ok(x != UNTOUCHED && y != UNTOUCHED, "raw: not written\n");

    for (i = -1; i <= 4; i += 4)
    {
        x = y = UNTOUCHED;
        hr = pGetDpiForMonitor(hMonitor, i, &x, &y);
        ok(hr == HRESULT_FROM_WIN32(ERROR_BAD_ARGUMENTS), "type %d: hr 0x%lx\n", i, hr);
        ok(x == UNTOUCHED && y == UNTOUCHED, "type %d: written\n", i);
    }

    x = y = UNTOUCHED;
    hr = pGetDpiForMonitor(NULL, 0, &x, &y);
    ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE), "NULL monitor: hr 0x%lx\n", hr);
    ok(x == UNTOUCHED && y == UNTOUCHED, "NULL monitor: written\n");

    x = UNTOUCHED;
    hr = pGetDpiForMonitor(hMonitor, 0, &x, NULL);
    ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS), "NULL y: hr 0x%lx\n", hr);
    ok(x == UNTOUCHED, "NULL y: written\n");
    y = UNTOUCHED;
    hr = pGetDpiForMonitor(hMonitor, 0, NULL, &y);
    ok(hr == HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS), "NULL x: hr 0x%lx\n", hr);
    ok(y == UNTOUCHED, "NULL x: written\n");

    for (i = 0; i <= 2; i++)
        ok(pGetDpiForShellUIComponent(i) == uDpiX, "component %d: %u, expected %u\n", i,
           pGetDpiForShellUIComponent(i), uDpiX);
}

START_TEST(ProcessDpiAwareness)
{
    HMODULE hShcore = LoadShcore();
    FN_GetProcessDpiAwareness pGetProcessDpiAwareness;
    FN_SetProcessDpiAwareness pSetProcessDpiAwareness;
    INT iAwareness;
    HRESULT hr;

    if (!hShcore)
        return;
    pGetProcessDpiAwareness = (FN_GetProcessDpiAwareness)GetProcAddress(hShcore, "GetProcessDpiAwareness");
    pSetProcessDpiAwareness = (FN_SetProcessDpiAwareness)GetProcAddress(hShcore, "SetProcessDpiAwareness");
    if (!pGetProcessDpiAwareness || !pSetProcessDpiAwareness)
    {
        skip("No DPI awareness functions in shcore\n");
        return;
    }

    /* A console program without a manifest is not DPI aware */
    iAwareness = UNTOUCHED;
    hr = pGetProcessDpiAwareness(NULL, &iAwareness);
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    ok(iAwareness == 0, "awareness %d\n", iAwareness);
    iAwareness = UNTOUCHED;
    hr = pGetProcessDpiAwareness(GetCurrentProcess(), &iAwareness);
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    ok(iAwareness == 0, "awareness %d\n", iAwareness);

    hr = pSetProcessDpiAwareness(3);
    ok(hr == E_INVALIDARG, "3: hr 0x%lx\n", hr);
    hr = pSetProcessDpiAwareness(-1);
    ok(hr == E_INVALIDARG, "-1: hr 0x%lx\n", hr);

    /* It can be set once */
    hr = pSetProcessDpiAwareness(1);
    ok(hr == S_OK, "1: hr 0x%lx\n", hr);
    iAwareness = UNTOUCHED;
    hr = pGetProcessDpiAwareness(NULL, &iAwareness);
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    ok(iAwareness == 1, "awareness %d after setting 1\n", iAwareness);
    hr = pSetProcessDpiAwareness(1);
    ok(hr == E_ACCESSDENIED, "1 again: hr 0x%lx\n", hr);
    hr = pSetProcessDpiAwareness(0);
    ok(hr == E_ACCESSDENIED, "0 after 1: hr 0x%lx\n", hr);
}
