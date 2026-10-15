/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the mouse wheel messages that SendInput and mouse_event produce,
 *              in particular WM_MOUSEHWHEEL for MOUSEEVENTF_HWHEEL
 */

#include "precomp.h"

#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif
#ifndef MOUSEEVENTF_HWHEEL
#define MOUSEEVENTF_HWHEEL 0x1000
#endif

#define EXTRA_MARK 0x57EE1000

typedef struct
{
    UINT message;
    int win;            /* 1 top level window, 2 child window, 3 other */
    WPARAM wParam;
    LPARAM lParam;
} LOGENT;

typedef struct
{
    UINT message;
    DWORD mouseData;
    DWORD flags;
} HOOKENT;

typedef struct
{
    POINT pt;
    HWND hwnd;
    UINT wHitTestCode;
    ULONG_PTR dwExtraInfo;
    DWORD mouseData;
} MOUSEHOOKSTRUCTEX_;

#define MAXLOG 64
static LOGENT g_log[MAXLOG];
static LONG g_nlog;
static HOOKENT g_llhook[MAXLOG];
static LONG g_nllhook;
static DWORD g_wheelhook[MAXLOG];
static UINT g_wheelhookmsg[MAXLOG];
static LONG g_nwheelhook;
static HWND g_hwndTop, g_hwndChild;

static LRESULT CALLBACK TestProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
    {
        LONG i = InterlockedIncrement(&g_nlog) - 1;

        if (i < MAXLOG)
        {
            g_log[i].message = msg;
            g_log[i].win = hwnd == g_hwndTop ? 1 : (hwnd == g_hwndChild && hwnd) ? 2 : 3;
            g_log[i].wParam = wParam;
            g_log[i].lParam = lParam;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK LowLevelProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL))
    {
        LONG i = InterlockedIncrement(&g_nllhook) - 1;
        MSLLHOOKSTRUCT *s = (MSLLHOOKSTRUCT *)lParam;

        if (i < MAXLOG)
        {
            g_llhook[i].message = (UINT)wParam;
            g_llhook[i].mouseData = s->mouseData;
            g_llhook[i].flags = s->flags;
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL))
    {
        LONG i = InterlockedIncrement(&g_nwheelhook) - 1;

        if (i < MAXLOG)
        {
            g_wheelhookmsg[i] = (UINT)wParam;
            g_wheelhook[i] = ((MOUSEHOOKSTRUCTEX_ *)lParam)->mouseData;
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

static void Pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG msg;

    for (;;)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if ((LONG)(end - GetTickCount()) <= 0)
            break;
        MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);
    }
}

static void Reset(void)
{
    Pump(60);
    g_nlog = 0;
    g_nllhook = 0;
    g_nwheelhook = 0;
}

static UINT Send(DWORD flags, LONG dx, LONG dy, DWORD data)
{
    INPUT input;

    ZeroMemory(&input, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = flags;
    input.mi.mouseData = data;
    input.mi.dwExtraInfo = EXTRA_MARK;
    return SendInput(1, &input, sizeof(input));
}

static void Key(WORD vk, BOOL down)
{
    INPUT input;

    ZeroMemory(&input, sizeof(input));
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(input));
}

static POINT ClientCentre(HWND hwnd)
{
    RECT rc;
    POINT pt;

    GetClientRect(hwnd, &rc);
    pt.x = (rc.left + rc.right) / 2;
    pt.y = (rc.top + rc.bottom) / 2;
    ClientToScreen(hwnd, &pt);
    return pt;
}

static void ExpectWheel(const char *what, UINT message, int index, int win, int keys, int delta, POINT pt)
{
    if (index >= g_nlog)
    {
        ok(0, "%s: no message %d (got %ld)\n", what, index, g_nlog);
        return;
    }
    ok(g_log[index].message == message, "%s: got message %#x, expected %#x\n", what, g_log[index].message, message);
    ok(g_log[index].win == win, "%s: delivered to window %d, expected %d\n", what, g_log[index].win, win);
    /* the upper half of a 64 bit wParam is zero on Windows 10 and the sign of the distance on Windows 7 */
    ok((DWORD)g_log[index].wParam == MAKELONG(keys, (WORD)delta),
       "%s: got wParam %#Ix, expected %#lx\n", what, g_log[index].wParam, MAKELONG(keys, (WORD)delta));
    ok(g_log[index].wParam == (WPARAM)(DWORD)g_log[index].wParam ||
       g_log[index].wParam == (WPARAM)(LONG_PTR)(LONG)g_log[index].wParam,
       "%s: got wParam %#Ix\n", what, g_log[index].wParam);
    ok(g_log[index].lParam == MAKELPARAM(pt.x, pt.y), "%s: got lParam %#Ix, expected %#lx\n",
       what, g_log[index].lParam, MAKELPARAM(pt.x, pt.y));
}

static void TestHWheelValues(POINT pt)
{
    static const struct
    {
        DWORD data;
        int delta;      /* 0: no message */
    } values[] =
    {
        { 120, 120 }, { (DWORD)-120, -120 }, { 60, 60 }, { 1, 1 }, { 240, 240 }, { (DWORD)-1, -1 },
        { 0, 0 },
        /* mouseData is clamped to the range of a signed 16 bit number */
        { 0x7FFF, 0x7FFF }, { 0x8000, 0x7FFF }, { 0x00010078, 0x7FFF }, { 0x7FFFFFFF, 0x7FFF },
        { 0xFFFF8000, -0x8000 }, { 0x80000000, -0x8000 },
    };
    char what[64];
    int i;

    for (i = 0; i < ARRAYSIZE(values); i++)
    {
        UINT ret;

        sprintf(what, "SendInput HWHEEL %#lx", values[i].data);
        Reset();
        ret = Send(MOUSEEVENTF_HWHEEL, 0, 0, values[i].data);
        ok(ret == 1, "%s: returned %u\n", what, ret);
        Pump(150);
        if (values[i].delta == 0)
        {
            ok(g_nlog == 0, "%s: got %ld messages\n", what, g_nlog);
            continue;
        }
        ok(g_nlog == 1, "%s: got %ld messages\n", what, g_nlog);
        ExpectWheel(what, WM_MOUSEHWHEEL, 0, 1, 0, values[i].delta, pt);
    }

    Reset();
    mouse_event(MOUSEEVENTF_HWHEEL, 0, 0, 120, EXTRA_MARK);
    Pump(150);
    ok(g_nlog == 1, "mouse_event HWHEEL: got %ld messages\n", g_nlog);
    ExpectWheel("mouse_event HWHEEL 120", WM_MOUSEHWHEEL, 0, 1, 0, 120, pt);

    Reset();
    mouse_event(MOUSEEVENTF_HWHEEL, 0, 0, (DWORD)-240, 0);
    Pump(150);
    ok(g_nlog == 1, "mouse_event HWHEEL -240: got %ld messages\n", g_nlog);
    ExpectWheel("mouse_event HWHEEL -240", WM_MOUSEHWHEEL, 0, 1, 0, -240, pt);

    Reset();
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0, 120, 0);
    Pump(150);
    ok(g_nlog == 1, "mouse_event WHEEL: got %ld messages\n", g_nlog);
    ExpectWheel("mouse_event WHEEL 120", WM_MOUSEWHEEL, 0, 1, 0, 120, pt);
}

static void TestKeys(POINT pt)
{
    Reset();
    Key(VK_CONTROL, TRUE);
    Pump(40);
    Send(MOUSEEVENTF_HWHEEL, 0, 0, 120);
    Pump(150);
    Key(VK_CONTROL, FALSE);
    ExpectWheel("control held", WM_MOUSEHWHEEL, 0, 1, MK_CONTROL, 120, pt);

    Reset();
    Key(VK_SHIFT, TRUE);
    Pump(40);
    Send(MOUSEEVENTF_HWHEEL, 0, 0, 120);
    Pump(150);
    Key(VK_SHIFT, FALSE);
    ExpectWheel("shift held", WM_MOUSEHWHEEL, 0, 1, MK_SHIFT, 120, pt);

    Reset();
    Key(VK_CONTROL, TRUE);
    Key(VK_SHIFT, TRUE);
    Pump(40);
    Send(MOUSEEVENTF_WHEEL, 0, 0, 120);
    Pump(150);
    Key(VK_SHIFT, FALSE);
    Key(VK_CONTROL, FALSE);
    ExpectWheel("control and shift held, WHEEL", WM_MOUSEWHEEL, 0, 1, MK_CONTROL | MK_SHIFT, 120, pt);

    Reset();
    Send(MOUSEEVENTF_LEFTDOWN, 0, 0, 0);
    Pump(60);
    g_nlog = 0;
    Send(MOUSEEVENTF_HWHEEL, 0, 0, 120);
    Pump(150);
    Send(MOUSEEVENTF_LEFTUP, 0, 0, 0);
    Pump(60);
    ExpectWheel("left button down", WM_MOUSEHWHEEL, 0, 1, MK_LBUTTON, 120, pt);

    Reset();
    Send(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0);
    Pump(60);
    g_nlog = 0;
    Send(MOUSEEVENTF_HWHEEL, 0, 0, 120);
    Pump(150);
    Send(MOUSEEVENTF_RIGHTUP, 0, 0, 0);
    Pump(60);
    ExpectWheel("right button down", WM_MOUSEHWHEEL, 0, 1, MK_RBUTTON, 120, pt);

    Reset();
    Send(MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0);
    Pump(60);
    g_nlog = 0;
    Send(MOUSEEVENTF_HWHEEL, 0, 0, 120);
    Pump(150);
    Send(MOUSEEVENTF_MIDDLEUP, 0, 0, 0);
    Pump(60);
    ExpectWheel("middle button down", WM_MOUSEHWHEEL, 0, 1, MK_MBUTTON, 120, pt);
}

static void TestCombinations(POINT pt)
{
    INPUT input[3];
    UINT ret;
    int i;

    /* wheels and buttons that need mouseData can not be asked for together: nothing happens */
    Reset();
    ret = Send(MOUSEEVENTF_WHEEL | MOUSEEVENTF_HWHEEL, 0, 0, 120);
    ok(ret == 1, "WHEEL|HWHEEL: returned %u\n", ret);
    Pump(150);
    ok(g_nlog == 0, "WHEEL|HWHEEL: got %ld messages\n", g_nlog);

    Reset();
    ret = Send(MOUSEEVENTF_XDOWN | MOUSEEVENTF_HWHEEL, 0, 0, XBUTTON1);
    ok(ret == 1, "XDOWN|HWHEEL: returned %u\n", ret);
    Pump(150);
    ok(g_nlog == 0, "XDOWN|HWHEEL: got %ld messages\n", g_nlog);
    Send(MOUSEEVENTF_XUP, 0, 0, XBUTTON1);
    Pump(60);

    /* a button together with a wheel */
    Reset();
    Send(MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_HWHEEL, 0, 0, 120);
    Pump(150);
    Send(MOUSEEVENTF_LEFTUP, 0, 0, 0);
    Pump(60);
    ok(g_nlog == 1, "LEFTDOWN|HWHEEL: got %ld messages\n", g_nlog);
    ExpectWheel("LEFTDOWN|HWHEEL", WM_MOUSEHWHEEL, 0, 1, MK_LBUTTON, 120, pt);

    /* horizontal and vertical wheel keep their order */
    Reset();
    ZeroMemory(input, sizeof(input));
    for (i = 0; i < 3; i++)
    {
        input[i].type = INPUT_MOUSE;
        input[i].mi.dwFlags = i == 1 ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL;
        input[i].mi.mouseData = 120 * (i + 1);
    }
    ret = SendInput(3, input, sizeof(INPUT));
    ok(ret == 3, "three wheel events: returned %u\n", ret);
    Pump(150);
    ok(g_nlog == 3, "three wheel events: got %ld messages\n", g_nlog);
    ExpectWheel("first of three", WM_MOUSEHWHEEL, 0, 1, 0, 120, pt);
    ExpectWheel("second of three", WM_MOUSEWHEEL, 1, 1, 0, 240, pt);
    ExpectWheel("third of three", WM_MOUSEHWHEEL, 2, 1, 0, 360, pt);
}

static void TestHooks(POINT pt)
{
    Reset();
    Send(MOUSEEVENTF_HWHEEL, 0, 0, 120);
    Pump(150);
    ok(g_nllhook == 1, "low level hook: %ld calls\n", g_nllhook);
    if (g_nllhook >= 1)
    {
        ok(g_llhook[0].message == WM_MOUSEHWHEEL, "low level hook: got message %#x\n", g_llhook[0].message);
        ok(g_llhook[0].mouseData == MAKELONG(0, 120), "low level hook: got mouseData %#lx\n", g_llhook[0].mouseData);
        ok(g_llhook[0].flags & LLMHF_INJECTED, "low level hook: got flags %#lx\n", g_llhook[0].flags);
    }
    ok(g_nwheelhook == 1, "mouse hook: %ld calls\n", g_nwheelhook);
    if (g_nwheelhook >= 1)
    {
        ok(g_wheelhookmsg[0] == WM_MOUSEHWHEEL, "mouse hook: got message %#x\n", g_wheelhookmsg[0]);
        ok(g_wheelhook[0] == MAKELONG(0, 120), "mouse hook: got mouseData %#lx\n", g_wheelhook[0]);
    }

    Reset();
    Send(MOUSEEVENTF_HWHEEL, 0, 0, (DWORD)-120);
    Pump(150);
    ok(g_nllhook == 1 && g_llhook[0].mouseData == MAKELONG(0, (WORD)-120),
       "low level hook: got %ld calls, mouseData %#lx\n", g_nllhook, g_llhook[0].mouseData);
    ok(g_nwheelhook == 1 && g_wheelhook[0] == MAKELONG(0, (WORD)-120),
       "mouse hook: got %ld calls, mouseData %#lx\n", g_nwheelhook, g_wheelhook[0]);

    /* a clamped distance is what the hooks see */
    Reset();
    Send(MOUSEEVENTF_HWHEEL, 0, 0, 0x8000);
    Pump(150);
    ok(g_nllhook == 1 && g_llhook[0].mouseData == MAKELONG(0, 0x7FFF),
       "low level hook: got %ld calls, mouseData %#lx\n", g_nllhook, g_llhook[0].mouseData);
}

static void TestPeek(void)
{
    static const struct
    {
        UINT low, high;
        BOOL found;
    } filters[] =
    {
        { WM_MOUSEHWHEEL, WM_MOUSEHWHEEL, TRUE },
        { WM_MOUSEFIRST, WM_MOUSEHWHEEL, TRUE },
        { 0, 0, TRUE },
        { WM_MOUSEHWHEEL - 1, WM_MOUSEHWHEEL + 1, TRUE },
        { WM_MOUSEFIRST, WM_MOUSEWHEEL, FALSE },
        { WM_MOUSEWHEEL, WM_MOUSEWHEEL, FALSE },
        { WM_MOUSEFIRST, WM_MOUSEHWHEEL - 1, FALSE },
    };
    MSG msg;
    int i;

    for (i = 0; i < ARRAYSIZE(filters); i++)
    {
        BOOL found;

        Reset();
        Send(MOUSEEVENTF_HWHEEL, 0, 0, 120);
        Sleep(120);
        ZeroMemory(&msg, sizeof(msg));
        found = PeekMessageW(&msg, NULL, filters[i].low, filters[i].high, PM_REMOVE);
        ok(found == filters[i].found, "filter %#x..%#x: found %d\n", filters[i].low, filters[i].high, found);
        if (found)
        {
            ok(msg.message == WM_MOUSEHWHEEL, "filter %#x..%#x: got message %#x\n", filters[i].low, filters[i].high, msg.message);
            ok(msg.hwnd == g_hwndTop, "filter %#x..%#x: got window %p\n", filters[i].low, filters[i].high, msg.hwnd);
            ok(HIWORD(msg.wParam) == 120, "filter %#x..%#x: got wParam %#Ix\n", filters[i].low, filters[i].high, msg.wParam);
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            ;
    }
}

static void TestChild(void)
{
    POINT pt = ClientCentre(g_hwndChild);

    SetCursorPos(pt.x, pt.y);
    Pump(60);

    /* a child that does not handle the message passes it to its parent */
    Reset();
    Send(MOUSEEVENTF_HWHEEL, 0, 0, 120);
    Pump(150);
    ok(g_nlog == 2, "child: got %ld messages\n", g_nlog);
    ExpectWheel("child", WM_MOUSEHWHEEL, 0, 2, 0, 120, pt);
    ExpectWheel("parent of the child", WM_MOUSEHWHEEL, 1, 1, 0, 120, pt);

    Reset();
    Send(MOUSEEVENTF_WHEEL, 0, 0, 120);
    Pump(150);
    ok(g_nlog == 2, "child, WHEEL: got %ld messages\n", g_nlog);
    ExpectWheel("child, WHEEL", WM_MOUSEWHEEL, 0, 2, 0, 120, pt);
    ExpectWheel("parent of the child, WHEEL", WM_MOUSEWHEEL, 1, 1, 0, 120, pt);

    /* the cursor stays where it is */
    {
        POINT now;

        GetCursorPos(&now);
        ok(now.x == pt.x && now.y == pt.y, "cursor moved to %ld,%ld\n", now.x, now.y);
    }
}

START_TEST(MouseWheel)
{
    WNDCLASSW wc;
    POINT pt, saved;
    HHOOK llhook, mhook;
    int i;

    GetCursorPos(&saved);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = TestProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"MouseWheelTestWnd";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);

    g_hwndTop = CreateWindowW(wc.lpszClassName, L"top", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 300, 300,
                              NULL, NULL, wc.hInstance, NULL);
    g_hwndChild = CreateWindowW(wc.lpszClassName, L"child", WS_CHILD | WS_VISIBLE | WS_BORDER, 20, 20, 100, 100,
                                g_hwndTop, NULL, wc.hInstance, NULL);
    if (!g_hwndTop || !g_hwndChild)
    {
        skip("could not create the windows\n");
        goto done;
    }

    for (i = 0; i < 10 && GetForegroundWindow() != g_hwndTop; i++)
    {
        /* a tap of Alt lets a process that is not in the foreground take it */
        if (i > 0)
        {
            Key(VK_MENU, TRUE);
            Key(VK_MENU, FALSE);
        }
        SetForegroundWindow(g_hwndTop);
        Pump(100);
    }
    SetFocus(g_hwndTop);
    Pump(100);
    if (GetForegroundWindow() != g_hwndTop || GetFocus() != g_hwndTop)
    {
        skip("the window could not take the focus\n");
        goto done;
    }

    llhook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelProc, GetModuleHandleW(NULL), 0);
    mhook = SetWindowsHookExW(WH_MOUSE, MouseProc, NULL, GetCurrentThreadId());
    ok(llhook != NULL, "WH_MOUSE_LL hook failed, error %lu\n", GetLastError());
    ok(mhook != NULL, "WH_MOUSE hook failed, error %lu\n", GetLastError());

    /* the pointer is over the top level window (outside of the child), which has the focus */
    pt = ClientCentre(g_hwndTop);
    pt.x += 60;
    pt.y += 60;
    SetCursorPos(pt.x, pt.y);
    Pump(100);
    Reset();

    TestHWheelValues(pt);
    TestKeys(pt);
    TestCombinations(pt);
    TestHooks(pt);
    TestPeek();

    SetFocus(g_hwndChild);
    Pump(100);
    if (GetFocus() == g_hwndChild)
        TestChild();
    else
        skip("the child window could not take the focus\n");

    if (llhook)
        UnhookWindowsHookEx(llhook);
    if (mhook)
        UnhookWindowsHookEx(mhook);

done:
    if (g_hwndTop)
        DestroyWindow(g_hwndTop);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    SetCursorPos(saved.x, saved.y);
}
