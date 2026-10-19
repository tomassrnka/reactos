/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         A sent message whose sender is terminated before the receiver dispatches it
 */

#include "precomp.h"

static const WCHAR g_class[] = L"SenderDiedClass";
static HANDLE g_inCallback, g_stuckReady, g_never;
static HWND g_hwndStuck;
static LONG g_deadMoves, g_liveMoves;

/* the dead senders move to (20,20), the live one to (40,40) */
static LRESULT CALLBACK ReceiverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_WINDOWPOSCHANGING)
    {
        const WINDOWPOS *wp = (const WINDOWPOS *)lParam;

        if (wp->x == 20)
            g_deadMoves++;
        else if (wp->x == 40)
            g_liveMoves++;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static BOOL send_pending(void)
{
    return (LOWORD(GetQueueStatus(QS_SENDMESSAGE)) & QS_SENDMESSAGE) != 0;
}

static DWORD WINAPI MoveWindowProc(LPVOID arg)
{
    /* the move sends WM_WINDOWPOSCHANGING with a pointer into this thread's kernel stack */
    SetWindowPos((HWND)arg, NULL, 20, 20, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
}

static DWORD WINAPI LiveMoveProc(LPVOID arg)
{
    return SetWindowPos((HWND)arg, NULL, 40, 40, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void kill_thread(HANDLE th, const char *what)
{
    ok(TerminateThread(th, 0), "TerminateThread (%s) failed: %lu\n", what, GetLastError());
    ok(WaitForSingleObject(th, 5000) == WAIT_OBJECT_0, "the %s did not exit\n", what);
    CloseHandle(th);
}

static LRESULT CALLBACK StuckProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_APP)
    {
        SetEvent(g_inCallback);
        WaitForSingleObject(g_never, INFINITE);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI StuckMoveProc(LPVOID arg)
{
    g_hwndStuck = CreateWindowW(L"SenderDiedStuckClass", L"stuck", WS_POPUP, 0, 0, 10, 10,
                                NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetEvent(g_stuckReady);
    return MoveWindowProc(arg);
}

static DWORD WINAPI PokeProc(LPVOID arg)
{
    SendMessageW((HWND)arg, WM_APP, 0, 0);
    return 0;
}

static void pump_all(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

/* the sender is terminated while it is blocked in the send */
static void test_terminated_in_send(HWND hwnd)
{
    HANDLE th;
    DWORD ret;
    int i;

    for (i = 0; i < 3; i++)
    {
        th = CreateThread(NULL, 0, MoveWindowProc, hwnd, 0, NULL);
        ok(th != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (!th)
            break;

        /* this thread does not pump, so the sender stays blocked until it is terminated */
        ret = WaitForSingleObject(th, 300);
        ok(ret == WAIT_TIMEOUT, "the cross-thread move completed without the receiver pumping: %lu\n", ret);
        ok(send_pending(), "the move is not queued on this thread\n");
        kill_thread(th, "blocked sender");
        Sleep(100);

        /* the dead sender's message must be gone: its lParam pointed into the dead stack.
           Other threads may send to this one meanwhile, so count the move, not the queue bit */
        pump_all();
        ok(g_deadMoves == 0, "a dead sender's move was dispatched %ld times\n", g_deadMoves);
    }
}

/* the sender is terminated inside a window procedure it entered while it waited for its own send */
static void test_terminated_in_callback(HWND hwnd)
{
    HANDLE thSender, thPoke;

    g_inCallback = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_stuckReady = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_never = CreateEventW(NULL, TRUE, FALSE, NULL);
    thSender = CreateThread(NULL, 0, StuckMoveProc, hwnd, 0, NULL);
    ok(thSender != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!thSender)
        goto done;
    ok(WaitForSingleObject(g_stuckReady, 5000) == WAIT_OBJECT_0, "the sender did not start\n");

    /* by now the sender waits for this thread to take its move */
    Sleep(300);
    ok(send_pending(), "the move is not queued on this thread\n");
    thPoke = CreateThread(NULL, 0, PokeProc, g_hwndStuck, 0, NULL);
    ok(thPoke != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!thPoke)
    {
        kill_thread(thSender, "sender");
        goto done;
    }
    ok(WaitForSingleObject(g_inCallback, 5000) == WAIT_OBJECT_0, "the sender never entered its window procedure\n");

    kill_thread(thSender, "sender in a window procedure");
    ok(WaitForSingleObject(thPoke, 5000) == WAIT_OBJECT_0, "the sender to the dead thread did not return\n");
    CloseHandle(thPoke);
    Sleep(100);

    pump_all();
    ok(g_deadMoves == 0, "a dead sender's move was dispatched %ld times\n", g_deadMoves);

done:
    CloseHandle(g_inCallback);
    CloseHandle(g_stuckReady);
    CloseHandle(g_never);
}

START_TEST(SendMessageSenderDied)
{
    WNDCLASSW wc;
    HWND hwnd;
    HANDLE th;
    DWORD code = 0;
    RECT rc;
    int i;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = ReceiverProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = g_class;
    RegisterClassW(&wc);
    wc.lpfnWndProc = StuckProc;
    wc.lpszClassName = L"SenderDiedStuckClass";
    RegisterClassW(&wc);

    hwnd = CreateWindowW(g_class, L"receiver", WS_POPUP | WS_VISIBLE,
                         100, 100, 100, 100, NULL, NULL, wc.hInstance, NULL);
    ok(hwnd != NULL, "CreateWindow failed: %lu\n", GetLastError());
    if (!hwnd)
        return;

    test_terminated_in_send(hwnd);
    test_terminated_in_callback(hwnd);

    /* the receiver still takes messages from a sender that lives */
    th = CreateThread(NULL, 0, LiveMoveProc, hwnd, 0, NULL);
    ok(th != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (th)
    {
        for (i = 0; i < 100; i++)
        {
            if (MsgWaitForMultipleObjects(1, &th, FALSE, 50, QS_ALLINPUT) == WAIT_OBJECT_0)
                break;
            pump_all();
        }
        ok(WaitForSingleObject(th, 0) == WAIT_OBJECT_0, "the move from a live thread did not complete\n");
        ok(GetExitCodeThread(th, &code) && code, "SetWindowPos from a live thread failed (%lu)\n", code);
        CloseHandle(th);
    }
    ok(GetWindowRect(hwnd, &rc), "GetWindowRect failed: %lu\n", GetLastError());
    ok(rc.left == 40 && rc.top == 40, "the live move did not land: (%ld,%ld)\n", rc.left, rc.top);
    ok(g_liveMoves >= 1, "the live move was not dispatched\n");

    DestroyWindow(hwnd);
    UnregisterClassW(g_class, wc.hInstance);
    UnregisterClassW(L"SenderDiedStuckClass", wc.hInstance);
}
