/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         AttachThreadInput away from a queue's last thread while the thread is
 *                  still dispatching messages on that queue
 */

#include "precomp.h"

#define WM_ATTACH_AWAY (WM_APP + 1)
#define WM_PROBE       (WM_APP + 2)

enum { MODE_SEND, MODE_POST, MODE_KEY };

typedef struct
{
    HANDLE ready, go, done, stop;
    DWORD tidMain;
    HWND hwnd;
    LONG fired;
    BOOL attached;
    HHOOK hook;
    int hookId;
} QUEUE_CTX;

static DWORD g_tls;

static LRESULT CALLBACK QueueWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    QUEUE_CTX *c = (QUEUE_CTX *)TlsGetValue(g_tls);

    if (c && msg == WM_ATTACH_AWAY && !c->fired++)
    {
        c->attached = AttachThreadInput(GetCurrentThreadId(), c->tidMain, TRUE);
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK QueueHook(int code, WPARAM wParam, LPARAM lParam)
{
    QUEUE_CTX *c = (QUEUE_CTX *)TlsGetValue(g_tls);

    if (c && code >= 0 && (c->hookId == WH_KEYBOARD || ((MSG *)lParam)->message == WM_PROBE) && !c->fired++)
        c->attached = AttachThreadInput(GetCurrentThreadId(), c->tidMain, TRUE);
    return CallNextHookEx(NULL, code, wParam, lParam);
}

static DWORD WINAPI QueueWorker(LPVOID arg)
{
    QUEUE_CTX *c = arg;
    MSG msg;

    TlsSetValue(g_tls, c);
    c->hwnd = CreateWindowExW(0, L"AttachQueueClass", L"worker", WS_POPUP | WS_VISIBLE,
                              300, 300, 100, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetForegroundWindow(c->hwnd);
    SetFocus(c->hwnd);
    c->hook = SetWindowsHookExW(c->hookId, QueueHook, NULL, GetCurrentThreadId());
    SetEvent(c->ready);
    WaitForSingleObject(c->go, 10000);

    for (;;)
    {
        DWORD r;

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
        r = MsgWaitForMultipleObjects(1, &c->stop, FALSE, 10000, QS_ALLINPUT);
        if (r != WAIT_OBJECT_0 + 1)
            break;
    }

    if (c->hook)
        UnhookWindowsHookEx(c->hook);
    if (c->hwnd)
        DestroyWindow(c->hwnd);
    SetEvent(c->done);
    return 0;
}

/* The worker attaches itself away from its own queue, which it holds alone, from inside a sent
   message, a WH_GETMESSAGE hook or a WH_KEYBOARD hook; the queue it joins must keep working. */
static void run_queue_case(int mode)
{
    QUEUE_CTX c;
    HANDLE th;
    DWORD_PTR res = 0;
    LRESULT ret;

    ZeroMemory(&c, sizeof(c));
    c.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    c.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    c.go = CreateEventW(NULL, TRUE, FALSE, NULL);
    c.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    c.tidMain = GetCurrentThreadId();
    c.hookId = mode == MODE_KEY ? WH_KEYBOARD : WH_GETMESSAGE;

    th = CreateThread(NULL, 0, QueueWorker, &c, 0, NULL);
    if (WaitForSingleObject(c.ready, 5000) != WAIT_OBJECT_0)
    {
        skip("worker did not start\n");
        SetEvent(c.go);
        SetEvent(c.stop);
        goto cleanup;
    }

    if (mode != MODE_KEY)
        SetEvent(c.go);

    if (mode == MODE_KEY)
    {
        int i;

        keybd_event('B', 0x30, 0, 0);
        for (i = 0; i < 4; i++)
            mouse_event(MOUSEEVENTF_MOVE, 1, 1, 0, 0);
        keybd_event('B', 0x30, KEYEVENTF_KEYUP, 0);
        Sleep(50);
        SetEvent(c.go);
        Sleep(500);
    }
    else if (mode == MODE_SEND)
    {
        ret = SendMessageTimeoutW(c.hwnd, WM_ATTACH_AWAY, 0, 0, SMTO_NORMAL, 5000, &res);
        ok(ret != 0, "SendMessageTimeout failed: %lu\n", GetLastError());
    }
    else
    {
        PostMessageW(c.hwnd, WM_PROBE, 0, 0);
        Sleep(500);
    }

    ok(c.fired == 1, "the attach never ran (%ld)\n", c.fired);
    ok(c.attached, "AttachThreadInput failed: %lu\n", GetLastError());

    /* the queue left behind must still be usable from both sides */
    ret = SendMessageTimeoutW(c.hwnd, WM_PROBE, 0, 0, SMTO_NORMAL, 5000, &res);
    ok(ret != 0, "SendMessageTimeout after the attach failed: %lu\n", GetLastError());

    SetEvent(c.stop);
    PostMessageW(c.hwnd, WM_NULL, 0, 0);
    ok(WaitForSingleObject(c.done, 5000) == WAIT_OBJECT_0, "worker did not finish\n");

cleanup:
    if (WaitForSingleObject(th, 5000) != WAIT_OBJECT_0)
        TerminateThread(th, 0);
    CloseHandle(th);
    CloseHandle(c.ready);
    CloseHandle(c.done);
    CloseHandle(c.go);
    CloseHandle(c.stop);
}

static void init_queue_class(void)
{
    WNDCLASSW wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = QueueWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"AttachQueueClass";
    RegisterClassW(&wc);
    g_tls = TlsAlloc();
}

START_TEST(AttachThreadInputQueue)
{
    init_queue_class();
    run_queue_case(MODE_SEND);
    run_queue_case(MODE_POST);
    run_queue_case(MODE_KEY);
}
