/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the raw input API: registration, device list, WM_INPUT delivery
 *              of injected mouse and keyboard input, INPUTSINK, NOLEGACY, wake bits
 */

#include "precomp.h"
#include <winreg.h>

#ifndef WM_INPUT_DEVICE_CHANGE
#define WM_INPUT_DEVICE_CHANGE 0x00FE
#endif
#ifndef MOUSEEVENTF_MOVE_NOCOALESCE
#define MOUSEEVENTF_MOVE_NOCOALESCE 0x2000
#endif
#ifndef MOUSEEVENTF_HWHEEL
#define MOUSEEVENTF_HWHEEL 0x1000
#endif
#ifndef MOUSE_MOVE_NOCOALESCE
#define MOUSE_MOVE_NOCOALESCE 0x08
#endif
#ifndef RI_MOUSE_HWHEEL
#define RI_MOUSE_HWHEEL 0x0800
#endif

#define EXTRA_MARK 0x5AFE0000

typedef struct
{
    UINT message;
    int win;            /* 1 first window, 2 window of the second thread, 0 other */
    WPARAM wParam;
    LPARAM lParam;
    BOOL raw;
    UINT queryRet, querySize, dataRet, dataSize, headerRet, headerSize;
    DWORD dataError;
    RAWINPUT data;
} LOGENT;

#define MAXLOG 128
static LOGENT g_log[MAXLOG];
static LONG g_nlog;
static HWND g_hwndA, g_hwndB;
static HRAWINPUT g_lastHandle;
static BOOL g_wow64;
static const WCHAR g_class[] = L"RawInputTestWnd";

static LRESULT CALLBACK TestProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    BOOL logit = msg == WM_INPUT || msg == WM_INPUT_DEVICE_CHANGE ||
                 (msg >= WM_MOUSEFIRST && msg <= 0x020E) ||
                 (msg >= WM_KEYFIRST && msg <= WM_SYSCHAR);

    if (logit)
    {
        LONG i = InterlockedIncrement(&g_nlog) - 1;

        if (i < MAXLOG)
        {
            LOGENT *e = &g_log[i];

            ZeroMemory(e, sizeof(*e));
            e->message = msg;
            e->win = hwnd == g_hwndA ? 1 : (hwnd == g_hwndB && hwnd) ? 2 : 0;
            e->wParam = wParam;
            e->lParam = lParam;
            if (msg == WM_INPUT)
            {
                UINT size = 0;

                e->raw = TRUE;
                e->queryRet = GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));
                e->querySize = size;
                size = sizeof(e->data);
                SetLastError(0xdeadbeef);
                e->dataRet = GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &e->data, &size, sizeof(RAWINPUTHEADER));
                e->dataError = GetLastError();
                e->dataSize = size;
                size = 0;
                e->headerRet = GetRawInputData((HRAWINPUT)lParam, RID_HEADER, NULL, &size, sizeof(RAWINPUTHEADER));
                e->headerSize = size;
                g_lastHandle = (HRAWINPUT)lParam;
            }
        }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void log_reset(void)
{
    g_nlog = 0;
}

static LONG log_count(void)
{
    return g_nlog > MAXLOG ? MAXLOG : g_nlog;
}

static int count_msg(UINT message)
{
    int c = 0;
    LONG i;

    for (i = 0; i < log_count(); i++)
        if (g_log[i].message == message)
            c++;
    return c;
}

/* index of the n-th logged message of this kind, -1 if none */
static int find_msg(UINT message, int n)
{
    LONG i;

    for (i = 0; i < log_count(); i++)
    {
        if (g_log[i].message == message && n-- == 0)
            return i;
    }
    return -1;
}

static int count_legacy(void)
{
    int c = 0;
    LONG i;

    for (i = 0; i < log_count(); i++)
    {
        UINT m = g_log[i].message;

        if ((m >= WM_MOUSEFIRST && m <= 0x020E) || (m >= WM_KEYFIRST && m <= WM_SYSCHAR))
            c++;
    }
    return c;
}

static BOOL wait_pumping(HANDLE h, DWORD ms)
{
    DWORD end = GetTickCount() + ms;

    for (;;)
    {
        LONG left = (LONG)(end - GetTickCount());
        DWORD r;
        MSG msg;

        if (left <= 0)
            return FALSE;
        r = MsgWaitForMultipleObjects(1, &h, FALSE, left, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0)
            return TRUE;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG msg;

    for (;;)
    {
        LONG left;

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        left = (LONG)(end - GetTickCount());
        if (left <= 0)
            break;
        MsgWaitForMultipleObjects(0, NULL, FALSE, left > 20 ? 20 : left, QS_ALLINPUT);
    }
}

static HWND make_window(int x, int y)
{
    return CreateWindowExW(0, g_class, L"rawinput", WS_POPUP | WS_VISIBLE, x, y, 200, 200,
                           NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static void click_at(int x, int y)
{
    SetCursorPos(x, y);
    pump(50);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    pump(100);
}

static RAWINPUTDEVICE mkdev(USHORT page, USHORT usage, DWORD flags, HWND hwnd)
{
    RAWINPUTDEVICE d;

    d.usUsagePage = page;
    d.usUsage = usage;
    d.dwFlags = flags;
    d.hwndTarget = hwnd;
    return d;
}

static BOOL reg1(USHORT page, USHORT usage, DWORD flags, HWND hwnd)
{
    RAWINPUTDEVICE d = mkdev(page, usage, flags, hwnd);

    SetLastError(0xdeadbeef);
    return RegisterRawInputDevices(&d, 1, sizeof(d));
}

static void unreg_all(void)
{
    RAWINPUTDEVICE d[16];
    UINT n = 0, i, ret;

    GetRegisteredRawInputDevices(NULL, &n, sizeof(RAWINPUTDEVICE));
    if (!n || n > 16)
        return;
    ret = GetRegisteredRawInputDevices(d, &n, sizeof(RAWINPUTDEVICE));
    if (ret == (UINT)-1)
        return;
    for (i = 0; i < ret; i++)
    {
        RAWINPUTDEVICE r = d[i];

        r.dwFlags = RIDEV_REMOVE | (((d[i].dwFlags & 0x30) == RIDEV_PAGEONLY) ? RIDEV_PAGEONLY : 0);
        r.hwndTarget = NULL;
        RegisterRawInputDevices(&r, 1, sizeof(r));
    }
}

static UINT registered_count(void)
{
    UINT n = 0xdeadbeef;

    if (GetRegisteredRawInputDevices(NULL, &n, sizeof(RAWINPUTDEVICE)) != 0)
        return 0xffffffff;
    return n;
}

static const UINT RAWMOUSE_TOTAL = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE);
static const UINT RAWKBD_TOTAL = sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD);

static void test_device_list(void)
{
    RAWINPUTDEVICELIST list[32];
    UINT n = 0, small, ret, i, sz, mice = 0, kbds = 0;

    SetLastError(0xdeadbeef);
    ret = GetRawInputDeviceList(NULL, NULL, sizeof(RAWINPUTDEVICELIST));
    ok(ret == (UINT)-1, "GetRawInputDeviceList returned %u\n", ret);
    ok(GetLastError() == ERROR_NOACCESS, "got error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    ret = GetRawInputDeviceList(NULL, &n, 1);
    ok(ret == (UINT)-1, "GetRawInputDeviceList returned %u\n", ret);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());

    n = 0;
    ret = GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST));
    ok(ret == 0, "GetRawInputDeviceList returned %u\n", ret);
    ok(n >= 2 && n <= 32, "got %u devices\n", n);
    if (n < 2 || n > 32)
        return;

    small = n - 1;
    SetLastError(0xdeadbeef);
    ret = GetRawInputDeviceList(list, &small, sizeof(RAWINPUTDEVICELIST));
    ok(ret == (UINT)-1, "GetRawInputDeviceList returned %u\n", ret);
    ok(small == n, "got %u, expected %u\n", small, n);
    ok(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "got error %lu\n", GetLastError());

    ZeroMemory(list, sizeof(list));
    ret = GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST));
    ok(ret == n, "GetRawInputDeviceList returned %u, n %u\n", ret, n);

    for (i = 0; i < ret; i++)
    {
        RID_DEVICE_INFO info;
        UINT r2;

        ok(list[i].hDevice != NULL, "device %u has no handle\n", i);
        ok(list[i].dwType == RIM_TYPEMOUSE || list[i].dwType == RIM_TYPEKEYBOARD || list[i].dwType == RIM_TYPEHID,
           "device %u has type %lu\n", i, list[i].dwType);
        if (list[i].dwType == RIM_TYPEMOUSE) mice++;
        if (list[i].dwType == RIM_TYPEKEYBOARD) kbds++;

        info.cbSize = sizeof(info);
        sz = sizeof(info);
        r2 = GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz);
        ok(r2 == sizeof(info), "GetRawInputDeviceInfoW returned %u\n", r2);
        ok(info.dwType == list[i].dwType, "type %lu, expected %lu\n", info.dwType, list[i].dwType);
        if (info.dwType == RIM_TYPEMOUSE)
            ok(info.mouse.dwNumberOfButtons >= 2, "got %lu buttons\n", info.mouse.dwNumberOfButtons);
        if (info.dwType == RIM_TYPEKEYBOARD)
            ok(info.keyboard.dwNumberOfKeysTotal >= 1, "got %lu keys\n", info.keyboard.dwNumberOfKeysTotal);
    }
    ok(mice >= 1, "no mouse in the device list\n");
    ok(kbds >= 1, "no keyboard in the device list\n");
}

static void test_register_errors(void)
{
    RAWINPUTDEVICE d[4], out[8];
    HWND dead;
    UINT n, ret;
    BOOL ok1;
    DWORD badflags[] = { 0x2, 0x4, 0x8, 0x40, 0x80, 0x4000, 0x80000000 };
    UINT i;

    SetLastError(0xdeadbeef);
    ok(!RegisterRawInputDevices(NULL, 0, 0), "NULL, 0, 0 succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());

    d[0] = mkdev(1, 2, 0, NULL);
    SetLastError(0xdeadbeef);
    ok(!RegisterRawInputDevices(NULL, 1, sizeof(d[0])), "NULL array succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!RegisterRawInputDevices(d, 0, sizeof(d[0])), "zero count succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!RegisterRawInputDevices(d, 1, 0), "size 0 succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!RegisterRawInputDevices(d, 1, sizeof(d[0]) + 4), "size + 4 succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!RegisterRawInputDevices(d, 1, sizeof(d[0]) - 4), "size - 4 succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());

    /* usage rules */
    ok(!reg1(0, 0, 0, NULL) && GetLastError() == ERROR_INVALID_PARAMETER, "page 0 usage 0: %lu\n", GetLastError());
    ok(!reg1(1, 0, 0, NULL) && GetLastError() == ERROR_INVALID_PARAMETER, "usage 0 without PAGEONLY: %lu\n", GetLastError());
    ok(!reg1(1, 2, RIDEV_PAGEONLY, NULL) && GetLastError() == ERROR_INVALID_PARAMETER, "PAGEONLY with usage: %lu\n", GetLastError());
    ok(!reg1(1, 0, RIDEV_EXCLUDE, NULL) && GetLastError() == ERROR_INVALID_PARAMETER, "EXCLUDE without usage: %lu\n", GetLastError());
    ok(!reg1(1, 0, RIDEV_NOLEGACY, NULL) && GetLastError() == ERROR_INVALID_PARAMETER, "NOLEGACY without usage: %lu\n", GetLastError());
    ok(!reg1(1, 4, RIDEV_NOLEGACY, NULL) && GetLastError() == ERROR_INVALID_PARAMETER, "NOLEGACY for joystick: %lu\n", GetLastError());

    /* window handle and sink rules */
    ok(!reg1(1, 2, RIDEV_INPUTSINK, NULL) && GetLastError() == ERROR_INVALID_PARAMETER, "INPUTSINK without window: %lu\n", GetLastError());
    ok(!reg1(1, 6, RIDEV_EXINPUTSINK, NULL) && GetLastError() == ERROR_INVALID_PARAMETER, "EXINPUTSINK without window: %lu\n", GetLastError());
    ok(!reg1(1, 2, RIDEV_REMOVE, g_hwndA) && GetLastError() == ERROR_INVALID_PARAMETER, "REMOVE with window: %lu\n", GetLastError());
    ok(!reg1(1, 2, RIDEV_INPUTSINK, (HWND)(ULONG_PTR)0x12345678) && GetLastError() == ERROR_INVALID_PARAMETER, "bad window: %lu\n", GetLastError());
    ok(!reg1(1, 2, 0, (HWND)(ULONG_PTR)0x12345678) && GetLastError() == ERROR_INVALID_PARAMETER, "bad window without sink: %lu\n", GetLastError());
    ok(!reg1(1, 2, RIDEV_INPUTSINK, GetDesktopWindow()) && GetLastError() == ERROR_INVALID_PARAMETER, "desktop window: %lu\n", GetLastError());
    dead = make_window(700, 400);
    DestroyWindow(dead);
    ok(!reg1(1, 2, RIDEV_INPUTSINK, dead) && GetLastError() == ERROR_INVALID_PARAMETER, "destroyed window: %lu\n", GetLastError());
    ok(registered_count() == 0, "failed registrations left %u entries\n", registered_count());

    /* flag rules */
    ok(!reg1(1, 2, RIDEV_REMOVE | RIDEV_NOLEGACY, NULL) && GetLastError() == ERROR_INVALID_FLAGS, "REMOVE|NOLEGACY: %lu\n", GetLastError());
    ok(!reg1(1, 2, RIDEV_REMOVE | RIDEV_PAGEONLY, NULL) && GetLastError() == ERROR_INVALID_PARAMETER, "REMOVE|PAGEONLY with usage: %lu\n", GetLastError());
    for (i = 0; i < ARRAYSIZE(badflags); i++)
    {
        ok(!reg1(1, 2, badflags[i], NULL) && GetLastError() == ERROR_INVALID_FLAGS,
           "flags %lx: error %lu\n", badflags[i], GetLastError());
    }
    ok(!reg1(1, 2, RIDEV_CAPTUREMOUSE, g_hwndA) && GetLastError() == ERROR_INVALID_FLAGS, "CAPTUREMOUSE alone: %lu\n", GetLastError());
    ok(!reg1(1, 6, RIDEV_APPKEYS, g_hwndA) && GetLastError() == ERROR_INVALID_FLAGS, "APPKEYS alone: %lu\n", GetLastError());
    ok(registered_count() == 0, "failed registrations left %u entries\n", registered_count());

    /* successful forms and what GetRegisteredRawInputDevices keeps of them */
    ok(reg1(1, 2, RIDEV_REMOVE, NULL), "REMOVE of an unknown device failed: %lu\n", GetLastError());
    ok(reg1(1, 2, RIDEV_CAPTUREMOUSE | RIDEV_NOLEGACY, g_hwndA), "CAPTUREMOUSE|NOLEGACY failed: %lu\n", GetLastError());
    unreg_all();
    ok(reg1(1, 6, RIDEV_NOHOTKEYS, g_hwndA), "NOHOTKEYS alone failed: %lu\n", GetLastError());
    unreg_all();
    ok(reg1(1, 6, RIDEV_APPKEYS | RIDEV_NOLEGACY, g_hwndA), "APPKEYS|NOLEGACY failed: %lu\n", GetLastError());
    unreg_all();
    ok(reg1(0xFF00, 1, 0, NULL), "vendor page failed: %lu\n", GetLastError());
    ok(registered_count() == 1, "got %u registrations\n", registered_count());
    unreg_all();
    ok(registered_count() == 0, "got %u registrations\n", registered_count());

    d[0] = mkdev(1, 2, 0, NULL);
    d[1] = mkdev(1, 6, RIDEV_INPUTSINK, NULL);
    SetLastError(0xdeadbeef);
    ok(!RegisterRawInputDevices(d, 2, sizeof(d[0])), "array with a bad entry succeeded\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
    ok(registered_count() == 0, "a bad entry registered %u devices\n", registered_count());
    d[0] = mkdev(1, 2, RIDEV_INPUTSINK, NULL);
    d[1] = mkdev(1, 6, 0, NULL);
    ok(!RegisterRawInputDevices(d, 2, sizeof(d[0])), "array with a bad first entry succeeded\n");
    ok(registered_count() == 0, "a bad first entry registered %u devices\n", registered_count());

    d[0] = mkdev(1, 6, 0, NULL);
    d[1] = mkdev(1, 2, 0, NULL);
    d[2] = mkdev(1, 4, 0, NULL);
    d[3] = mkdev(1, 5, 0, NULL);
    ok(RegisterRawInputDevices(d, 4, sizeof(d[0])), "four devices failed: %lu\n", GetLastError());
    n = 8;
    ret = GetRegisteredRawInputDevices(out, &n, sizeof(RAWINPUTDEVICE));
    ok(ret == 4 && n == 8, "GetRegisteredRawInputDevices returned %u, n %u\n", ret, n);
    ok(out[0].usUsage == 2 && out[1].usUsage == 4 && out[2].usUsage == 5 && out[3].usUsage == 6,
       "usages %u %u %u %u, expected sorted\n", out[0].usUsage, out[1].usUsage, out[2].usUsage, out[3].usUsage);
    unreg_all();

    /* a later entry for the same device replaces the earlier one */
    d[0] = mkdev(1, 2, 0, NULL);
    d[1] = mkdev(1, 2, RIDEV_INPUTSINK, g_hwndA);
    ok(RegisterRawInputDevices(d, 2, sizeof(d[0])), "duplicate entries failed: %lu\n", GetLastError());
    n = 8;
    ret = GetRegisteredRawInputDevices(out, &n, sizeof(RAWINPUTDEVICE));
    ok(ret == 1, "GetRegisteredRawInputDevices returned %u\n", ret);
    ok(out[0].dwFlags == RIDEV_INPUTSINK && out[0].hwndTarget == g_hwndA, "flags %lx hwnd %p\n", out[0].dwFlags, out[0].hwndTarget);
    unreg_all();

    /* what is kept of the flags */
    ok(reg1(1, 6, RIDEV_INPUTSINK | RIDEV_EXINPUTSINK, g_hwndA), "both sinks failed: %lu\n", GetLastError());
    n = 8;
    ret = GetRegisteredRawInputDevices(out, &n, sizeof(RAWINPUTDEVICE));
    ok(ret == 1 && out[0].dwFlags == RIDEV_EXINPUTSINK, "ret %u flags %lx\n", ret, ret ? out[0].dwFlags : 0);
    unreg_all();
    ok(reg1(1, 6, RIDEV_DEVNOTIFY, NULL), "DEVNOTIFY failed: %lu\n", GetLastError());
    n = 8;
    ret = GetRegisteredRawInputDevices(out, &n, sizeof(RAWINPUTDEVICE));
    ok(ret == 1 && out[0].dwFlags == 0, "ret %u flags %lx\n", ret, ret ? out[0].dwFlags : 0);
    unreg_all();
    ok(reg1(1, 2, RIDEV_NOLEGACY, NULL), "NOLEGACY failed: %lu\n", GetLastError());
    n = 8;
    ret = GetRegisteredRawInputDevices(out, &n, sizeof(RAWINPUTDEVICE));
    ok(ret == 1 && out[0].dwFlags == RIDEV_NOLEGACY, "ret %u flags %lx\n", ret, ret ? out[0].dwFlags : 0);
    unreg_all();
    ok(reg1(1, 0, RIDEV_PAGEONLY, NULL), "PAGEONLY failed: %lu\n", GetLastError());
    ok(reg1(1, 2, RIDEV_EXCLUDE, NULL), "EXCLUDE failed: %lu\n", GetLastError());
    n = 8;
    ret = GetRegisteredRawInputDevices(out, &n, sizeof(RAWINPUTDEVICE));
    ok(ret == 2, "GetRegisteredRawInputDevices returned %u\n", ret);
    ok(out[0].usUsage == 0 && out[0].dwFlags == RIDEV_PAGEONLY, "usage %u flags %lx\n", out[0].usUsage, out[0].dwFlags);
    ok(out[1].usUsage == 2 && out[1].dwFlags == RIDEV_EXCLUDE, "usage %u flags %lx\n", out[1].usUsage, out[1].dwFlags);
    unreg_all();
    ok(registered_count() == 0, "got %u registrations\n", registered_count());

    /* GetRegisteredRawInputDevices errors */
    d[0] = mkdev(1, 2, 0, g_hwndA);
    d[1] = mkdev(1, 6, 0, g_hwndA);
    ok(RegisterRawInputDevices(d, 2, sizeof(d[0])), "register failed: %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ret = GetRegisteredRawInputDevices(NULL, NULL, sizeof(RAWINPUTDEVICE));
    ok(ret == (UINT)-1 && GetLastError() == ERROR_NOACCESS, "ret %u error %lu\n", ret, GetLastError());
    n = 0;
    SetLastError(0xdeadbeef);
    ret = GetRegisteredRawInputDevices(NULL, &n, 1);
    ok(ret == (UINT)-1 && GetLastError() == ERROR_INVALID_PARAMETER, "ret %u error %lu\n", ret, GetLastError());
    n = 1;
    SetLastError(0xdeadbeef);
    ret = GetRegisteredRawInputDevices(out, &n, sizeof(RAWINPUTDEVICE));
    ok(ret == (UINT)-1 && n == 2 && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "ret %u n %u error %lu\n", ret, n, GetLastError());
    n = 0;
    ret = GetRegisteredRawInputDevices(NULL, &n, sizeof(RAWINPUTDEVICE));
    ok(ret == 0 && n == 2, "ret %u n %u\n", ret, n);
    n = 8;
    ZeroMemory(out, sizeof(out));
    ret = GetRegisteredRawInputDevices(out, &n, sizeof(RAWINPUTDEVICE));
    ok(ret == 2, "ret %u\n", ret);
    ok(out[0].usUsage == 2 && out[0].hwndTarget == g_hwndA && out[1].usUsage == 6 && out[1].hwndTarget == g_hwndA,
       "usage %u hwnd %p, usage %u hwnd %p\n", out[0].usUsage, out[0].hwndTarget, out[1].usUsage, out[1].hwndTarget);
    unreg_all();

    /* a destroyed target window stays registered */
    dead = make_window(700, 400);
    ok1 = reg1(1, 2, RIDEV_INPUTSINK, dead);
    ok(ok1, "register on a new window failed: %lu\n", GetLastError());
    DestroyWindow(dead);
    n = 8;
    ret = GetRegisteredRawInputDevices(out, &n, sizeof(RAWINPUTDEVICE));
    ok(ret == 1 && out[0].hwndTarget == dead, "ret %u hwnd %p, expected %p\n", ret, ret ? out[0].hwndTarget : NULL, dead);
    unreg_all();
    ok(registered_count() == 0, "got %u registrations\n", registered_count());
}

static void check_header(int idx, DWORD type, UINT total, WPARAM code, const char *what)
{
    LOGENT *e = &g_log[idx];

    ok(e->dataRet == total, "%s: GetRawInputData returned %u (error %lx), expected %u\n", what, e->dataRet, e->dataError, total);
    ok(e->queryRet == 0 && e->querySize == total, "%s: size query returned %u, size %u\n", what, e->queryRet, e->querySize);
    ok(e->headerRet == 0 && e->headerSize == sizeof(RAWINPUTHEADER), "%s: header size query returned %u, size %u\n", what, e->headerRet, e->headerSize);
    ok(e->data.header.dwType == type, "%s: type %lu\n", what, e->data.header.dwType);
    ok(e->data.header.dwSize == total, "%s: dwSize %lu, expected %u\n", what, e->data.header.dwSize, total);
    ok(e->data.header.hDevice == NULL, "%s: injected input has device %p\n", what, e->data.header.hDevice);
    ok((e->wParam & 0xff) == code, "%s: message wParam %Ix, expected %Ix\n", what, e->wParam, code);
    ok((e->data.header.wParam & 0xff) == code, "%s: header wParam %Ix, expected %Ix\n", what, e->data.header.wParam, code);
}

static const RAWMOUSE *mouse_event_at(int idx)
{
    return &g_log[idx].data.data.mouse;
}

static const RAWKEYBOARD *kbd_event_at(int idx)
{
    return &g_log[idx].data.data.keyboard;
}

static void test_mouse_delivery(void)
{
    RAWINPUTDEVICE d;
    INPUT in;
    int i, j;

    ok(reg1(1, 2, 0, NULL), "register failed: %lu\n", GetLastError());
    SetCursorPos(150, 150);
    pump(50);

    /* a relative move: the raw packet comes before the legacy message */
    log_reset();
    mouse_event(MOUSEEVENTF_MOVE, 5, 3, 0, EXTRA_MARK + 1);
    pump(150);
    i = find_msg(WM_INPUT, 0);
    ok(i >= 0, "no WM_INPUT for a relative move\n");
    if (i >= 0)
    {
        check_header(i, RIM_TYPEMOUSE, RAWMOUSE_TOTAL, RIM_INPUT, "relative move");
        ok(mouse_event_at(i)->usFlags == MOUSE_MOVE_RELATIVE, "usFlags %x\n", mouse_event_at(i)->usFlags);
        ok(mouse_event_at(i)->lLastX == 5 && mouse_event_at(i)->lLastY == 3, "move %ld,%ld\n", mouse_event_at(i)->lLastX, mouse_event_at(i)->lLastY);
        ok(mouse_event_at(i)->usButtonFlags == 0 && mouse_event_at(i)->usButtonData == 0, "buttons %x data %x\n",
           mouse_event_at(i)->usButtonFlags, mouse_event_at(i)->usButtonData);
        ok(mouse_event_at(i)->ulExtraInformation == EXTRA_MARK + 1, "extra %lx\n", mouse_event_at(i)->ulExtraInformation);
        ok(g_log[i].win == 1, "WM_INPUT went to window %d\n", g_log[i].win);
        j = find_msg(WM_MOUSEMOVE, 0);
        ok(j > i, "WM_MOUSEMOVE at %d, WM_INPUT at %d\n", j, i);
    }
    ok(count_msg(WM_INPUT) == 1, "got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    ok(g_lastHandle != NULL, "no raw input handle\n");

    /* the handle is only good while its message is current */
    {
        RAWINPUT ri;
        UINT size = sizeof(ri);

        SetLastError(0xdeadbeef);
        ok(GetRawInputData(g_lastHandle, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1, "stale handle worked\n");
        ok(GetLastError() == ERROR_INVALID_HANDLE, "got error %lu\n", GetLastError());
    }

    /* absolute and virtual desktop moves, the no-coalesce flag, extra information */
    log_reset();
    mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, 30000, 20000, 0, 0);
    pump(150);
    i = find_msg(WM_INPUT, 0);
    ok(i >= 0, "no WM_INPUT for an absolute move\n");
    if (i >= 0)
    {
        ok(mouse_event_at(i)->usFlags == MOUSE_MOVE_ABSOLUTE, "usFlags %x\n", mouse_event_at(i)->usFlags);
        ok(mouse_event_at(i)->lLastX == 30000 && mouse_event_at(i)->lLastY == 20000, "move %ld,%ld\n", mouse_event_at(i)->lLastX, mouse_event_at(i)->lLastY);
    }
    SetCursorPos(150, 150);
    pump(50);

    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = 20000;
    in.mi.dy = 10000;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    log_reset();
    SendInput(1, &in, sizeof(in));
    pump(150);
    i = find_msg(WM_INPUT, 0);
    ok(i >= 0, "no WM_INPUT for a virtual desktop move\n");
    if (i >= 0)
        ok(mouse_event_at(i)->usFlags == (MOUSE_MOVE_ABSOLUTE | MOUSE_VIRTUAL_DESKTOP), "usFlags %x\n", mouse_event_at(i)->usFlags);
    SetCursorPos(150, 150);
    pump(50);

    in.mi.dx = 4;
    in.mi.dy = 0;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
    in.mi.dwExtraInfo = EXTRA_MARK + 2;
    log_reset();
    SendInput(1, &in, sizeof(in));
    pump(150);
    i = find_msg(WM_INPUT, 0);
    ok(i >= 0, "no WM_INPUT for SendInput\n");
    if (i >= 0)
    {
        ok(mouse_event_at(i)->usFlags == MOUSE_MOVE_NOCOALESCE, "usFlags %x\n", mouse_event_at(i)->usFlags);
        ok(mouse_event_at(i)->lLastX == 4, "x %ld\n", mouse_event_at(i)->lLastX);
        ok(mouse_event_at(i)->ulExtraInformation == EXTRA_MARK + 2, "extra %lx\n", mouse_event_at(i)->ulExtraInformation);
    }
    mouse_event(MOUSEEVENTF_MOVE, (DWORD)-9, (DWORD)-3, 0, 0);
    pump(100);

    /* buttons, the wheels */
    {
        struct { DWORD flags; DWORD data; USHORT expect; UINT n; } tests[] =
        {
            { MOUSEEVENTF_LEFTDOWN, 0, RI_MOUSE_LEFT_BUTTON_DOWN },
            { MOUSEEVENTF_LEFTUP, 0, RI_MOUSE_LEFT_BUTTON_UP },
            { MOUSEEVENTF_RIGHTDOWN, 0, RI_MOUSE_RIGHT_BUTTON_DOWN },
            { MOUSEEVENTF_RIGHTUP, 0, RI_MOUSE_RIGHT_BUTTON_UP },
            { MOUSEEVENTF_MIDDLEDOWN, 0, RI_MOUSE_MIDDLE_BUTTON_DOWN },
            { MOUSEEVENTF_MIDDLEUP, 0, RI_MOUSE_MIDDLE_BUTTON_UP },
            { MOUSEEVENTF_XDOWN, XBUTTON1, RI_MOUSE_BUTTON_4_DOWN },
            { MOUSEEVENTF_XUP, XBUTTON1, RI_MOUSE_BUTTON_4_UP },
            { MOUSEEVENTF_XDOWN, XBUTTON2, RI_MOUSE_BUTTON_5_DOWN },
            { MOUSEEVENTF_XUP, XBUTTON2, RI_MOUSE_BUTTON_5_UP },
        };
        UINT t;

        for (t = 0; t < ARRAYSIZE(tests); t++)
        {
            log_reset();
            mouse_event(tests[t].flags, 0, 0, tests[t].data, 0);
            pump(100);
            i = find_msg(WM_INPUT, 0);
            ok(i >= 0, "button test %u: no WM_INPUT\n", t);
            if (i >= 0)
            {
                ok(mouse_event_at(i)->usButtonFlags == tests[t].expect, "button test %u: flags %x, expected %x\n",
                   t, mouse_event_at(i)->usButtonFlags, tests[t].expect);
                ok(mouse_event_at(i)->usFlags == 0 && mouse_event_at(i)->lLastX == 0 && mouse_event_at(i)->lLastY == 0,
                   "button test %u: usFlags %x move %ld,%ld\n", t, mouse_event_at(i)->usFlags,
                   mouse_event_at(i)->lLastX, mouse_event_at(i)->lLastY);
            }
            ok(count_msg(WM_INPUT) == 1, "button test %u: %d WM_INPUT messages\n", t, count_msg(WM_INPUT));
        }
    }

    log_reset();
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0, 120, 0);
    pump(100);
    i = find_msg(WM_INPUT, 0);
    ok(i >= 0, "no WM_INPUT for the wheel\n");
    if (i >= 0)
    {
        ok(mouse_event_at(i)->usButtonFlags == RI_MOUSE_WHEEL && mouse_event_at(i)->usButtonData == 120,
           "flags %x data %x\n", mouse_event_at(i)->usButtonFlags, mouse_event_at(i)->usButtonData);
        j = find_msg(WM_MOUSEWHEEL, 0);
        ok(j > i, "WM_MOUSEWHEEL at %d, WM_INPUT at %d\n", j, i);
    }
    log_reset();
    mouse_event(MOUSEEVENTF_HWHEEL, 0, 0, (DWORD)-120, 0);
    pump(100);
    i = find_msg(WM_INPUT, 0);
    ok(i >= 0, "no WM_INPUT for the horizontal wheel\n");
    if (i >= 0)
        ok(mouse_event_at(i)->usButtonFlags == RI_MOUSE_HWHEEL && mouse_event_at(i)->usButtonData == 0xff88,
           "flags %x data %x\n", mouse_event_at(i)->usButtonFlags, mouse_event_at(i)->usButtonData);

    /* a move with a button is one packet, the legacy messages stay */
    log_reset();
    mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN, 2, 2, 0, 0);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    pump(150);
    ok(count_msg(WM_INPUT) == 2, "got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    i = find_msg(WM_INPUT, 0);
    if (i >= 0)
    {
        ok(mouse_event_at(i)->usButtonFlags == RI_MOUSE_LEFT_BUTTON_DOWN && mouse_event_at(i)->lLastX == 2 && mouse_event_at(i)->lLastY == 2,
           "flags %x move %ld,%ld\n", mouse_event_at(i)->usButtonFlags, mouse_event_at(i)->lLastX, mouse_event_at(i)->lLastY);
    }
    ok(count_msg(WM_LBUTTONDOWN) == 1 && count_msg(WM_LBUTTONUP) == 1, "legacy: down %d up %d\n", count_msg(WM_LBUTTONDOWN), count_msg(WM_LBUTTONUP));
    mouse_event(MOUSEEVENTF_MOVE, (DWORD)-2, (DWORD)-2, 0, 0);
    pump(100);

    /* the registration that is gone delivers nothing */
    d = mkdev(1, 2, RIDEV_REMOVE, NULL);
    ok(RegisterRawInputDevices(&d, 1, sizeof(d)), "remove failed: %lu\n", GetLastError());
    log_reset();
    mouse_event(MOUSEEVENTF_MOVE, 3, 3, 0, 0);
    pump(150);
    ok(count_msg(WM_INPUT) == 0, "got %d WM_INPUT messages after the removal\n", count_msg(WM_INPUT));
    mouse_event(MOUSEEVENTF_MOVE, (DWORD)-3, (DWORD)-3, 0, 0);
    pump(100);
    unreg_all();
}

static void test_keyboard_delivery(void)
{
    INPUT in[2];
    int i, j;

    ok(reg1(1, 6, 0, NULL), "register failed: %lu\n", GetLastError());

    log_reset();
    keybd_event('A', 0x1E, 0, EXTRA_MARK + 3);
    keybd_event('A', 0x1E, KEYEVENTF_KEYUP, EXTRA_MARK + 3);
    pump(150);
    ok(count_msg(WM_INPUT) == 2, "got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    i = find_msg(WM_INPUT, 0);
    if (i >= 0)
    {
        check_header(i, RIM_TYPEKEYBOARD, RAWKBD_TOTAL, RIM_INPUT, "key down");
        ok(kbd_event_at(i)->MakeCode == 0x1E && kbd_event_at(i)->Flags == RI_KEY_MAKE && kbd_event_at(i)->VKey == 'A',
           "make %x flags %x vkey %x\n", kbd_event_at(i)->MakeCode, kbd_event_at(i)->Flags, kbd_event_at(i)->VKey);
        ok(kbd_event_at(i)->Message == WM_KEYDOWN, "message %x\n", kbd_event_at(i)->Message);
        ok(kbd_event_at(i)->ExtraInformation == EXTRA_MARK + 3, "extra %lx\n", kbd_event_at(i)->ExtraInformation);
        j = find_msg(WM_KEYDOWN, 0);
        ok(j > i, "WM_KEYDOWN at %d, WM_INPUT at %d\n", j, i);
        ok(count_msg(WM_CHAR) == 1, "got %d WM_CHAR messages\n", count_msg(WM_CHAR));
    }
    i = find_msg(WM_INPUT, 1);
    if (i >= 0)
    {
        ok(kbd_event_at(i)->MakeCode == 0x1E && kbd_event_at(i)->Flags == RI_KEY_BREAK && kbd_event_at(i)->VKey == 'A',
           "make %x flags %x vkey %x\n", kbd_event_at(i)->MakeCode, kbd_event_at(i)->Flags, kbd_event_at(i)->VKey);
        ok(kbd_event_at(i)->Message == WM_KEYUP, "message %x\n", kbd_event_at(i)->Message);
    }

    /* scan code only, extended keys, Alt */
    log_reset();
    keybd_event(0, 0x1E, KEYEVENTF_SCANCODE, 0);
    keybd_event(0, 0x1E, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP, 0);
    pump(150);
    i = find_msg(WM_INPUT, 0);
    ok(count_msg(WM_INPUT) == 2 && i >= 0, "got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    if (i >= 0)
        ok(kbd_event_at(i)->MakeCode == 0x1E && kbd_event_at(i)->VKey == 'A', "make %x vkey %x\n", kbd_event_at(i)->MakeCode, kbd_event_at(i)->VKey);

    log_reset();
    keybd_event(VK_RIGHT, 0x4D, KEYEVENTF_EXTENDEDKEY, 0);
    keybd_event(VK_RIGHT, 0x4D, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
    pump(150);
    i = find_msg(WM_INPUT, 0);
    ok(count_msg(WM_INPUT) == 2 && i >= 0, "got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    if (i >= 0)
    {
        ok(kbd_event_at(i)->Flags == (RI_KEY_MAKE | RI_KEY_E0) && kbd_event_at(i)->VKey == VK_RIGHT && kbd_event_at(i)->MakeCode == 0x4D,
           "flags %x vkey %x make %x\n", kbd_event_at(i)->Flags, kbd_event_at(i)->VKey, kbd_event_at(i)->MakeCode);
        ok(g_log[find_msg(WM_INPUT, 1)].data.data.keyboard.Flags == (RI_KEY_BREAK | RI_KEY_E0), "break flags %x\n",
           g_log[find_msg(WM_INPUT, 1)].data.data.keyboard.Flags);
    }

    /* the message of a key follows the Alt state after it */
    log_reset();
    keybd_event(VK_MENU, 0x38, 0, 0);
    keybd_event('A', 0x1E, 0, 0);
    keybd_event('A', 0x1E, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_MENU, 0x38, KEYEVENTF_KEYUP, 0);
    pump(150);
    ok(count_msg(WM_INPUT) == 4, "got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    if (count_msg(WM_INPUT) == 4)
    {
        ok(kbd_event_at(find_msg(WM_INPUT, 0))->Message == WM_SYSKEYDOWN, "Alt down message %x\n", kbd_event_at(find_msg(WM_INPUT, 0))->Message);
        ok(kbd_event_at(find_msg(WM_INPUT, 1))->Message == WM_SYSKEYDOWN, "A down message %x\n", kbd_event_at(find_msg(WM_INPUT, 1))->Message);
        ok(kbd_event_at(find_msg(WM_INPUT, 2))->Message == WM_SYSKEYUP, "A up message %x\n", kbd_event_at(find_msg(WM_INPUT, 2))->Message);
        ok(kbd_event_at(find_msg(WM_INPUT, 3))->Message == WM_KEYUP, "Alt up message %x\n", kbd_event_at(find_msg(WM_INPUT, 3))->Message);
    }

    /* a unicode packet has no raw packet but still produces its character */
    ZeroMemory(in, sizeof(in));
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wScan = 0x263A;
    in[0].ki.dwFlags = KEYEVENTF_UNICODE;
    in[1] = in[0];
    in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    log_reset();
    SendInput(2, in, sizeof(INPUT));
    pump(150);
    ok(count_msg(WM_INPUT) == 0, "got %d WM_INPUT messages for a unicode packet\n", count_msg(WM_INPUT));
    ok(count_msg(WM_CHAR) == 1, "got %d WM_CHAR messages\n", count_msg(WM_CHAR));
    unreg_all();
}

static void test_nolegacy(void)
{
    RAWINPUTDEVICE d[2];
    POINT pt0, pt1;

    d[0] = mkdev(1, 2, RIDEV_NOLEGACY, NULL);
    d[1] = mkdev(1, 6, RIDEV_NOLEGACY, NULL);
    ok(RegisterRawInputDevices(d, 2, sizeof(d[0])), "register failed: %lu\n", GetLastError());
    SetCursorPos(150, 150);
    pump(100);
    GetCursorPos(&pt0);

    log_reset();
    mouse_event(MOUSEEVENTF_MOVE, 7, 0, 0, 0);
    pump(150);
    GetCursorPos(&pt1);
    ok(count_msg(WM_INPUT) == 1, "got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    ok(count_legacy() == 0, "got %d legacy messages for a NOLEGACY move\n", count_legacy());
    ok(pt1.x > pt0.x, "the cursor did not move (%ld, %ld)\n", pt0.x, pt1.x);

    log_reset();
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    ok(GetAsyncKeyState(VK_LBUTTON) & 0x8000, "the button is not down\n");
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    pump(150);
    ok(count_msg(WM_INPUT) == 2, "got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    ok(count_legacy() == 0, "got %d legacy messages for NOLEGACY buttons\n", count_legacy());
    mouse_event(MOUSEEVENTF_MOVE, (DWORD)-7, 0, 0, 0);
    pump(100);

    log_reset();
    keybd_event('A', 0x1E, 0, 0);
    ok(GetAsyncKeyState('A') & 0x8000, "the key is not down\n");
    keybd_event('A', 0x1E, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_F5, 0x3F, 0, 0);
    keybd_event(VK_F5, 0x3F, KEYEVENTF_KEYUP, 0);
    pump(150);
    ok(count_msg(WM_INPUT) == 4, "got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    ok(count_legacy() == 0, "got %d legacy messages for NOLEGACY keys\n", count_legacy());
    unreg_all();

    /* each device on its own */
    d[0] = mkdev(1, 2, RIDEV_NOLEGACY, NULL);
    d[1] = mkdev(1, 6, 0, NULL);
    ok(RegisterRawInputDevices(d, 2, sizeof(d[0])), "register failed: %lu\n", GetLastError());
    log_reset();
    mouse_event(MOUSEEVENTF_MOVE, 7, 0, 0, 0);
    keybd_event('A', 0x1E, 0, 0);
    keybd_event('A', 0x1E, KEYEVENTF_KEYUP, 0);
    pump(150);
    ok(count_msg(WM_INPUT) == 3, "got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    ok(count_msg(WM_MOUSEMOVE) == 0, "got %d WM_MOUSEMOVE messages\n", count_msg(WM_MOUSEMOVE));
    ok(count_msg(WM_KEYDOWN) == 1 && count_msg(WM_KEYUP) == 1, "keys: down %d up %d\n", count_msg(WM_KEYDOWN), count_msg(WM_KEYUP));
    mouse_event(MOUSEEVENTF_MOVE, (DWORD)-7, 0, 0, 0);
    pump(100);
    unreg_all();
}

static DWORD WINAPI ThreadB(LPVOID arg)
{
    HANDLE *h = arg; /* ready, stop */
    MSG msg;

    g_hwndB = make_window(400, 300);
    SetEvent(h[0]);
    while (WaitForSingleObject(h[1], 0) == WAIT_TIMEOUT)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(1, &h[1], FALSE, 20, QS_ALLINPUT);
    }
    DestroyWindow(g_hwndB);
    return 0;
}

static void test_second_thread_target(void)
{
    RAWINPUTDEVICE d[2];
    HANDLE h[2], th;
    int i;
    BOOL foreA;

    h[0] = CreateEventW(NULL, TRUE, FALSE, NULL);
    h[1] = CreateEventW(NULL, TRUE, FALSE, NULL);
    th = CreateThread(NULL, 0, ThreadB, h, 0, NULL);
    WaitForSingleObject(h[0], 5000);
    pump(100);
    click_at(150, 150);
    foreA = GetForegroundWindow() == g_hwndA;

    /* the foreground process gets the packets at the registered window, whatever thread owns it */
    d[0] = mkdev(1, 2, 0, g_hwndB);
    d[1] = mkdev(1, 6, 0, g_hwndB);
    ok(RegisterRawInputDevices(d, 2, sizeof(d[0])), "register failed: %lu\n", GetLastError());
    log_reset();
    mouse_event(MOUSEEVENTF_MOVE, 5, 3, 0, 0);
    keybd_event('A', 0x1E, 0, 0);
    keybd_event('A', 0x1E, KEYEVENTF_KEYUP, 0);
    pump(250);
    ok(count_msg(WM_INPUT) == 3, "got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    for (i = 0; i < count_msg(WM_INPUT); i++)
    {
        int idx = find_msg(WM_INPUT, i);

        ok(g_log[idx].win == 2, "WM_INPUT %d went to window %d\n", i, g_log[idx].win);
        ok((g_log[idx].wParam & 0xff) == RIM_INPUT, "WM_INPUT %d has code %Ix\n", i, g_log[idx].wParam);
    }
    if (foreA)
        ok(count_msg(WM_KEYDOWN) == 1 && g_log[find_msg(WM_KEYDOWN, 0)].win == 1, "legacy key went to the foreground window\n");
    mouse_event(MOUSEEVENTF_MOVE, (DWORD)-5, (DWORD)-3, 0, 0);
    pump(100);
    unreg_all();

    /* the registration goes away with the thread that owns its window */
    ok(reg1(1, 2, RIDEV_INPUTSINK, g_hwndB), "register failed: %lu\n", GetLastError());
    ok(registered_count() == 1, "got %u registrations\n", registered_count());
    SetEvent(h[1]);
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    ok(registered_count() == 0, "the registration survived the thread: %u\n", registered_count());
    log_reset();
    mouse_event(MOUSEEVENTF_MOVE, 5, 3, 0, 0);
    pump(150);
    ok(count_msg(WM_INPUT) == 0, "got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    mouse_event(MOUSEEVENTF_MOVE, (DWORD)-5, (DWORD)-3, 0, 0);
    pump(100);
    CloseHandle(h[0]);
    CloseHandle(h[1]);
    g_hwndB = NULL;
}

static BOOL running_on_reactos(void)
{
    HKEY key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ReactOS\\ReactOS", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return FALSE;
    RegCloseKey(key);
    return TRUE;
}

struct attach_ctx
{
    HANDLE ready, go1, ack1, go2, done;
    DWORD tidMain;
    HWND hwnd;
    int spurious1, spurious2;
    DWORD bits1, bits2;
    BOOL attached, detached;
};

static int idle_wake_count(void)
{
    DWORD t0 = GetTickCount(), r;
    int n = 0;
    MSG msg;

    while (GetTickCount() - t0 < 300)
    {
        r = MsgWaitForMultipleObjectsEx(0, NULL, 100, QS_RAWINPUT, MWMO_INPUTAVAILABLE);
        if (r != WAIT_TIMEOUT)
            n++;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
    }
    return n;
}

static DWORD WINAPI AttachThread(LPVOID arg)
{
    struct attach_ctx *c = arg;
    MSG msg;

    c->hwnd = make_window(700, 500);
    SetEvent(c->ready);

    /* raw input sits in the queue of this thread when it joins the queue of the main thread */
    WaitForSingleObject(c->go1, 10000);
    c->attached = AttachThreadInput(GetCurrentThreadId(), c->tidMain, TRUE);
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
    c->spurious1 = idle_wake_count();
    c->bits1 = HIWORD(GetQueueStatus(QS_RAWINPUT)) & QS_RAWINPUT;
    SetEvent(c->ack1);

    /* and when it leaves that queue again */
    WaitForSingleObject(c->go2, 10000);
    c->detached = AttachThreadInput(GetCurrentThreadId(), c->tidMain, FALSE);
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
    c->spurious2 = idle_wake_count();
    c->bits2 = HIWORD(GetQueueStatus(QS_RAWINPUT)) & QS_RAWINPUT;

    DestroyWindow(c->hwnd);
    SetEvent(c->done);
    return 0;
}

static void test_attach_wake_bits(void)
{
    struct attach_ctx c;
    HANDLE th;
    int i;

    ZeroMemory(&c, sizeof(c));
    c.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    c.go1 = CreateEventW(NULL, TRUE, FALSE, NULL);
    c.ack1 = CreateEventW(NULL, TRUE, FALSE, NULL);
    c.go2 = CreateEventW(NULL, TRUE, FALSE, NULL);
    c.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    c.tidMain = GetCurrentThreadId();
    th = CreateThread(NULL, 0, AttachThread, &c, 0, NULL);
    WaitForSingleObject(c.ready, 5000);
    pump(100);
    click_at(150, 150);

    ok(reg1(1, 2, RIDEV_INPUTSINK, c.hwnd), "register failed: %lu\n", GetLastError());
    for (i = 0; i < 3; i++)
        mouse_event(MOUSEEVENTF_MOVE, 1, 0, 0, 0);
    Sleep(100);
    SetEvent(c.go1);
    wait_pumping(c.ack1, 10000);
    for (i = 0; i < 3; i++)
        mouse_event(MOUSEEVENTF_MOVE, (DWORD)-1, 0, 0, 0);
    Sleep(100);
    SetEvent(c.go2);
    ok(wait_pumping(c.done, 10000), "the thread did not finish\n");
    WaitForSingleObject(th, 5000);
    CloseHandle(th);

    ok(c.attached, "AttachThreadInput failed\n");
    ok(c.detached, "detaching failed\n");
    if (!running_on_reactos())
    {
        /* Windows 7 keeps QS_RAWINPUT set for the thread that joined the queue, and
           Windows 10 wakes the thread for other traffic of the shared queue: no reference */
        trace("%d and %d wake-ups, bits %lx %lx\n", c.spurious1, c.spurious2, c.bits1, c.bits2);
    }
    else
    {
        ok(c.spurious1 <= 1, "got %d wake-ups after attaching\n", c.spurious1);
        ok(c.spurious2 <= 1, "got %d wake-ups after detaching\n", c.spurious2);
        ok(!c.bits1 && !c.bits2, "QS_RAWINPUT left set: %lx %lx\n", c.bits1, c.bits2);
    }
    unreg_all();
    pump(100);
    CloseHandle(c.ready);
    CloseHandle(c.go1);
    CloseHandle(c.ack1);
    CloseHandle(c.go2);
    CloseHandle(c.done);
}

static int child_main(const char *evReady, const char *evStop, BOOL registers)
{
    HANDLE hr = OpenEventA(EVENT_ALL_ACCESS, FALSE, evReady), hs = OpenEventA(EVENT_ALL_ACCESS, FALSE, evStop);
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"RawInputTestChild";
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"rawchild", WS_POPUP | WS_VISIBLE, registers ? 600 : 500, registers ? 400 : 100,
                           200, 200, NULL, NULL, wc.hInstance, NULL);
    if (registers)
    {
        RAWINPUTDEVICE d[2];

        d[0] = mkdev(1, 2, RIDEV_INPUTSINK, hwnd);
        d[1] = mkdev(1, 6, RIDEV_INPUTSINK, hwnd);
        RegisterRawInputDevices(d, 2, sizeof(d[0]));
    }
    else
    {
        SetForegroundWindow(hwnd);
    }
    SetEvent(hr);
    for (;;)
    {
        DWORD r;

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
        r = MsgWaitForMultipleObjects(1, &hs, FALSE, 20000, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT)
            break;
    }
    DestroyWindow(hwnd);
    return 0;
}

static HANDLE spawn_child(const char *mode, const char *evReady, const char *evStop)
{
    char **argv;
    char cmd[MAX_PATH * 2];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    winetest_get_mainargs(&argv);
    sprintf(cmd, "\"%s\" RawInput %s %s %s", argv[0], mode, evReady, evStop);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        skip("CreateProcess failed: %lu\n", GetLastError());
        return NULL;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static void stop_child(HANDLE hp, HANDLE hstop)
{
    SetEvent(hstop);
    if (WaitForSingleObject(hp, 5000) != WAIT_OBJECT_0)
        TerminateProcess(hp, 1);
    CloseHandle(hp);
}

static void test_background(void)
{
    char n1[64], n2[64];
    HANDLE er, es, hp;
    RAWINPUTDEVICE d[2];
    int i;

    sprintf(n1, "RawInputTestReady%lu", GetCurrentProcessId());
    sprintf(n2, "RawInputTestStop%lu", GetCurrentProcessId());
    er = CreateEventA(NULL, TRUE, FALSE, n1);
    es = CreateEventA(NULL, TRUE, FALSE, n2);

    hp = spawn_child("child", n1, n2);
    if (!hp)
        goto done;
    if (WaitForSingleObject(er, 10000) != WAIT_OBJECT_0)
    {
        ok(0, "the child did not start\n");
        TerminateProcess(hp, 1);
        CloseHandle(hp);
        goto done;
    }
    pump(100);
    click_at(600, 150);
    if (GetForegroundWindow() == g_hwndA)
    {
        skip("the child could not take the foreground\n");
        stop_child(hp, es);
        goto done;
    }

    /* a background process gets nothing unless it asks for it */
    d[0] = mkdev(1, 2, 0, NULL);
    d[1] = mkdev(1, 6, 0, NULL);
    ok(RegisterRawInputDevices(d, 2, sizeof(d[0])), "register failed: %lu\n", GetLastError());
    log_reset();
    mouse_event(MOUSEEVENTF_MOVE, 5, 3, 0, 0);
    keybd_event('A', 0x1E, 0, 0);
    keybd_event('A', 0x1E, KEYEVENTF_KEYUP, 0);
    pump(250);
    ok(count_msg(WM_INPUT) == 0, "no window: got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    unreg_all();

    d[0] = mkdev(1, 2, 0, g_hwndA);
    d[1] = mkdev(1, 6, 0, g_hwndA);
    ok(RegisterRawInputDevices(d, 2, sizeof(d[0])), "register failed: %lu\n", GetLastError());
    log_reset();
    mouse_event(MOUSEEVENTF_MOVE, 5, 3, 0, 0);
    keybd_event('A', 0x1E, 0, 0);
    keybd_event('A', 0x1E, KEYEVENTF_KEYUP, 0);
    pump(250);
    ok(count_msg(WM_INPUT) == 0, "no sink: got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    unreg_all();

    d[0] = mkdev(1, 2, RIDEV_INPUTSINK, g_hwndA);
    d[1] = mkdev(1, 6, RIDEV_INPUTSINK, g_hwndA);
    ok(RegisterRawInputDevices(d, 2, sizeof(d[0])), "register failed: %lu\n", GetLastError());
    log_reset();
    mouse_event(MOUSEEVENTF_MOVE, 5, 3, 0, 0);
    keybd_event('A', 0x1E, 0, 0);
    keybd_event('A', 0x1E, KEYEVENTF_KEYUP, 0);
    pump(250);
    ok(count_msg(WM_INPUT) == 3, "INPUTSINK: got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    for (i = 0; i < count_msg(WM_INPUT); i++)
    {
        int idx = find_msg(WM_INPUT, i);

        ok((g_log[idx].wParam & 0xff) == RIM_INPUTSINK, "INPUTSINK: message %d has code %Ix\n", i, g_log[idx].wParam);
        ok((g_log[idx].data.header.wParam & 0xff) == RIM_INPUTSINK, "INPUTSINK: header %d has code %Ix\n", i, g_log[idx].data.header.wParam);
    }
    ok(count_legacy() == 0, "INPUTSINK: got %d legacy messages\n", count_legacy());
    unreg_all();

    d[0] = mkdev(1, 2, RIDEV_EXINPUTSINK, g_hwndA);
    d[1] = mkdev(1, 6, RIDEV_EXINPUTSINK, g_hwndA);
    ok(RegisterRawInputDevices(d, 2, sizeof(d[0])), "register failed: %lu\n", GetLastError());
    log_reset();
    mouse_event(MOUSEEVENTF_MOVE, 5, 3, 0, 0);
    pump(250);
    ok(count_msg(WM_INPUT) == 1, "EXINPUTSINK: got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    if (count_msg(WM_INPUT))
        ok((g_log[find_msg(WM_INPUT, 0)].wParam & 0xff) == RIM_INPUTSINK, "EXINPUTSINK: code %Ix\n", g_log[find_msg(WM_INPUT, 0)].wParam);
    unreg_all();

    d[0] = mkdev(1, 2, RIDEV_NOLEGACY | RIDEV_INPUTSINK, g_hwndA);
    ok(RegisterRawInputDevices(d, 1, sizeof(d[0])), "register failed: %lu\n", GetLastError());
    log_reset();
    mouse_event(MOUSEEVENTF_MOVE, 5, 3, 0, 0);
    pump(250);
    ok(count_msg(WM_INPUT) == 1, "NOLEGACY sink: got %d WM_INPUT messages\n", count_msg(WM_INPUT));
    unreg_all();
    mouse_event(MOUSEEVENTF_MOVE, (DWORD)-5, (DWORD)-3, 0, 0);
    pump(100);

    stop_child(hp, es);
    ResetEvent(es);
    ResetEvent(er);

    /* registrations belong to the process */
    hp = spawn_child("reg", n1, n2);
    if (hp)
    {
        UINT n = 99, ret;

        WaitForSingleObject(er, 10000);
        pump(100);
        ret = GetRegisteredRawInputDevices(NULL, &n, sizeof(RAWINPUTDEVICE));
        ok(ret == 0 && n == 0, "the child's registrations show up here: ret %u n %u\n", ret, n);
        stop_child(hp, es);
    }

done:
    CloseHandle(er);
    CloseHandle(es);
}

static void test_device_notify(void)
{
    RAWINPUTDEVICELIST list[32];
    RAWINPUTDEVICE d[2];
    UINT n = 0, i, expect = 0;
    int j, c;

    GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST));
    if (!n || n > 32)
        return;
    GetRawInputDeviceList(list, &n, sizeof(RAWINPUTDEVICELIST));
    for (i = 0; i < n; i++)
        if (list[i].dwType == RIM_TYPEMOUSE || list[i].dwType == RIM_TYPEKEYBOARD)
            expect++;

    /* without a window nothing is sent */
    d[0] = mkdev(1, 2, RIDEV_DEVNOTIFY, NULL);
    d[1] = mkdev(1, 6, RIDEV_DEVNOTIFY, NULL);
    log_reset();
    ok(RegisterRawInputDevices(d, 2, sizeof(d[0])), "register failed: %lu\n", GetLastError());
    pump(300);
    ok(count_msg(WM_INPUT_DEVICE_CHANGE) == 0, "got %d notifications without a window\n", count_msg(WM_INPUT_DEVICE_CHANGE));
    unreg_all();

    /* with one, the devices that exist already arrive */
    d[0] = mkdev(1, 2, RIDEV_DEVNOTIFY, g_hwndA);
    d[1] = mkdev(1, 6, RIDEV_DEVNOTIFY, g_hwndA);
    log_reset();
    ok(RegisterRawInputDevices(d, 2, sizeof(d[0])), "register failed: %lu\n", GetLastError());
    pump(400);
    c = count_msg(WM_INPUT_DEVICE_CHANGE);
    ok(c == (int)expect, "got %d notifications, expected %u\n", c, expect);
    for (j = 0; j < c; j++)
    {
        LOGENT *e = &g_log[find_msg(WM_INPUT_DEVICE_CHANGE, j)];
        BOOL known = FALSE;

        ok(e->wParam == GIDC_ARRIVAL, "notification %d has wParam %Ix\n", j, e->wParam);
        for (i = 0; i < n; i++)
            if ((LPARAM)list[i].hDevice == e->lParam)
                known = TRUE;
        ok(known, "notification %d names an unknown device %Ix\n", j, e->lParam);
        ok(e->win == 1, "notification %d went to window %d\n", j, e->win);
    }
    unreg_all();
    pump(100);
}

static void test_wake_bits(void)
{
    RAWINPUTDEVICE d;
    BYTE buf[1024];
    UINT size, ret;
    MSG msg;
    DWORD r, t0;
    int spurious;

    d = mkdev(1, 2, 0, g_hwndA);
    ok(RegisterRawInputDevices(&d, 1, sizeof(d)), "register failed: %lu\n", GetLastError());
    pump(50);
    ok(!(HIWORD(GetQueueStatus(QS_RAWINPUT)) & QS_RAWINPUT), "QS_RAWINPUT set before any input\n");

    mouse_event(MOUSEEVENTF_MOVE, 3, 0, 0, 0);
    Sleep(100);
    ok(GetQueueStatus(QS_RAWINPUT) & (QS_RAWINPUT << 16), "QS_RAWINPUT not set after input\n");
    ok(MsgWaitForMultipleObjectsEx(0, NULL, 0, QS_RAWINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0,
       "the wait did not see the raw input\n");
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
    ok(!(HIWORD(GetQueueStatus(QS_RAWINPUT)) & QS_RAWINPUT), "QS_RAWINPUT still set after the message loop\n");

    /* an idle wait for raw input must block */
    spurious = 0;
    t0 = GetTickCount();
    while (GetTickCount() - t0 < 300)
    {
        r = MsgWaitForMultipleObjectsEx(0, NULL, 100, QS_RAWINPUT, MWMO_INPUTAVAILABLE);
        if (r != WAIT_TIMEOUT)
            spurious++;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
    }
    /* a signal left over from a message that was read without a wait may end one wait early, no more */
    ok(spurious <= 1, "got %d wake-ups from an idle raw input wait\n", spurious);

    /* GetRawInputBuffer takes the packets and the wake bit with them */
    mouse_event(MOUSEEVENTF_MOVE, 1, 0, 0, 0);
    mouse_event(MOUSEEVENTF_MOVE, 1, 0, 0, 0);
    mouse_event(MOUSEEVENTF_MOVE, 1, 0, 0, 0);
    Sleep(100);
    ok(GetQueueStatus(QS_RAWINPUT) & (QS_RAWINPUT << 16), "QS_RAWINPUT not set after input\n");
    size = 0;
    ret = GetRawInputBuffer(NULL, &size, sizeof(RAWINPUTHEADER));
    ok(ret == 0, "GetRawInputBuffer returned %u\n", ret);
    /* a 32-bit process under WOW64 gets the 64-bit layout from this function */
    ok(size == (g_wow64 ? 48 : RAWMOUSE_TOTAL), "size %u\n", size);
    size = sizeof(buf);
    ret = GetRawInputBuffer((PRAWINPUT)buf, &size, sizeof(RAWINPUTHEADER));
    ok(ret == 3, "GetRawInputBuffer returned %u\n", ret);
    ok(!(HIWORD(GetQueueStatus(QS_RAWINPUT)) & QS_RAWINPUT), "QS_RAWINPUT still set after GetRawInputBuffer\n");
    size = sizeof(buf);
    ret = GetRawInputBuffer((PRAWINPUT)buf, &size, sizeof(RAWINPUTHEADER));
    ok(ret == 0 && size == 0, "second GetRawInputBuffer returned %u, size %u\n", ret, size);
    pump(100); /* the legacy mouse messages of the moves */
    spurious = 0;
    t0 = GetTickCount();
    while (GetTickCount() - t0 < 300)
    {
        r = MsgWaitForMultipleObjectsEx(0, NULL, 100, QS_RAWINPUT, MWMO_INPUTAVAILABLE);
        if (r != WAIT_TIMEOUT)
            spurious++;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
    }
    ok(spurious <= 1, "got %d wake-ups after GetRawInputBuffer\n", spurious);
    mouse_event(MOUSEEVENTF_MOVE, (DWORD)-6, 0, 0, 0);
    pump(100);
    unreg_all();
}

static void test_buffer_errors(void)
{
    RAWINPUT ri;
    UINT size = sizeof(ri), ret;

    SetLastError(0xdeadbeef);
    ret = GetRawInputData(NULL, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER));
    ok(ret == (UINT)-1 && GetLastError() == ERROR_INVALID_HANDLE, "NULL handle: ret %u error %lu\n", ret, GetLastError());
    SetLastError(0xdeadbeef);
    ret = GetRawInputData((HRAWINPUT)(ULONG_PTR)0x1234, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER));
    ok(ret == (UINT)-1 && GetLastError() == ERROR_INVALID_HANDLE, "bad handle: ret %u error %lu\n", ret, GetLastError());
    SetLastError(0xdeadbeef);
    ret = GetRawInputData((HRAWINPUT)(ULONG_PTR)0x1234, RID_INPUT, &ri, &size, 4);
    ok(ret == (UINT)-1 && GetLastError() == ERROR_INVALID_PARAMETER, "bad header size: ret %u error %lu\n", ret, GetLastError());
    SetLastError(0xdeadbeef);
    ret = GetRawInputBuffer(NULL, NULL, sizeof(RAWINPUTHEADER));
    ok(ret == (UINT)-1 && GetLastError() == ERROR_INVALID_PARAMETER, "NULL size: ret %u error %lu\n", ret, GetLastError());
    SetLastError(0xdeadbeef);
    ret = GetRawInputBuffer(&ri, &size, 4);
    ok(ret == (UINT)-1 && GetLastError() == ERROR_INVALID_PARAMETER, "bad header size: ret %u error %lu\n", ret, GetLastError());
    ok(DefRawInputProc(NULL, 0, sizeof(RAWINPUTHEADER)) == 0, "DefRawInputProc failed with the right header size\n");
    ok(DefRawInputProc(NULL, 0, 4) == -1, "DefRawInputProc accepted a bad header size\n");
}

static void test_handle_edges(void)
{
    RAWINPUTDEVICE d;
    RAWINPUT ri;
    UINT size, ret;
    BYTE tmp[256];
    MSG msg;

    d = mkdev(1, 2, 0, NULL);
    ok(RegisterRawInputDevices(&d, 1, sizeof(d)), "register failed: %lu\n", GetLastError());
    mouse_event(MOUSEEVENTF_MOVE, 1, 0, 0, 0);
    Sleep(50);
    if (PeekMessageW(&msg, g_hwndA, WM_INPUT, WM_INPUT, PM_REMOVE))
    {
        HRAWINPUT h = (HRAWINPUT)msg.lParam;

        SetLastError(0xdeadbeef);
        size = sizeof(tmp);
        ret = GetRawInputData(h, RID_INPUT, tmp, NULL, sizeof(RAWINPUTHEADER));
        ok(ret == (UINT)-1 && GetLastError() == ERROR_NOACCESS, "NULL size: ret %u error %lu\n", ret, GetLastError());
        if (!g_wow64) /* WOW64 takes a header-sized buffer of 8 bytes */
        {
            SetLastError(0xdeadbeef);
            size = 8;
            ret = GetRawInputData(h, RID_HEADER, tmp, &size, sizeof(RAWINPUTHEADER));
            /* Windows 7 leaves the size alone, Windows 10 reports the size it needs */
            ok(ret == (UINT)-1 && GetLastError() == ERROR_INSUFFICIENT_BUFFER && (size == 8 || size == sizeof(RAWINPUTHEADER)),
               "small buffer: ret %u error %lu size %u\n", ret, GetLastError(), size);
        }
        SetLastError(0xdeadbeef);
        size = sizeof(tmp);
        ret = GetRawInputData(h, 0x12345, tmp, &size, sizeof(RAWINPUTHEADER));
        ok(ret == (UINT)-1 && GetLastError() == ERROR_INVALID_PARAMETER, "bad command: ret %u error %lu\n", ret, GetLastError());
        SetLastError(0xdeadbeef);
        size = sizeof(tmp);
        ret = GetRawInputData((HRAWINPUT)((ULONG_PTR)h + 8), RID_INPUT, tmp, &size, sizeof(RAWINPUTHEADER));
        ok(ret == (UINT)-1 && GetLastError() == ERROR_INVALID_HANDLE, "moved handle: ret %u error %lu\n", ret, GetLastError());
        size = sizeof(tmp);
        ret = GetRawInputData(h, RID_HEADER, tmp, &size, sizeof(RAWINPUTHEADER));
        /* WOW64 rewrites the size to what it copied */
        ok(ret == sizeof(RAWINPUTHEADER) && (size == sizeof(tmp) || (g_wow64 && size == ret)), "header: ret %u size %u\n", ret, size);
        size = 1000;
        ret = GetRawInputData(h, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER));
        ok(ret == RAWMOUSE_TOTAL && size == 1000, "input: ret %u size %u\n", ret, size);
        DispatchMessageW(&msg);
    }
    else
    {
        ok(0, "no WM_INPUT message\n");
    }
    mouse_event(MOUSEEVENTF_MOVE, (DWORD)-1, 0, 0, 0);
    pump(100);
    unreg_all();
}

START_TEST(RawInput)
{
    WNDCLASSW wc;
    char **argv;
    int argc;
    typedef BOOL (WINAPI *PIW64)(HANDLE, PBOOL);
    PIW64 pIsWow64Process;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 5 && !strcmp(argv[2], "child"))
    {
        child_main(argv[3], argv[4], FALSE);
        return;
    }
    if (argc >= 5 && !strcmp(argv[2], "reg"))
    {
        child_main(argv[3], argv[4], TRUE);
        return;
    }

    pIsWow64Process = (PIW64)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
    if (pIsWow64Process)
        pIsWow64Process(GetCurrentProcess(), &g_wow64);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = TestProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = g_class;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);

    g_hwndA = make_window(100, 100);
    pump(100);
    click_at(150, 150);
    if (GetForegroundWindow() != g_hwndA)
    {
        skip("could not take the foreground\n");
        DestroyWindow(g_hwndA);
        return;
    }

    test_device_list();
    test_register_errors();
    test_buffer_errors();
    test_mouse_delivery();
    test_keyboard_delivery();
    test_handle_edges();
    test_wake_bits();
    test_device_notify();
    test_nolegacy();
    test_second_thread_target();
    click_at(150, 150);
    test_attach_wake_bits();
    click_at(150, 150);
    test_background();

    unreg_all();
    DestroyWindow(g_hwndA);
}
