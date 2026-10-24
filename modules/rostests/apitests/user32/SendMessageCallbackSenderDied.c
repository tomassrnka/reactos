/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     SendMessageCallback whose (callback) sender exits before the receiver dispatches it
 * COPYRIGHT:   Copyright Tomas Srnka
 */

#include "precomp.h"

static const WCHAR g_class[] = L"CbSenderDiedClass";
static LONG g_deadSenderMsgs;   /* WM_APP dispatched from a dead callback-sender */
static LONG g_liveSenderMsgs;   /* WM_APP dispatched from a live callback-sender */
static LONG g_liveCbFired;      /* completion callback invoked for the live send */

#define WPARAM_DEAD 0xDEAD
#define WPARAM_LIVE 0x11FE

static LRESULT CALLBACK ReceiverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_APP)
    {
        if (wParam == WPARAM_DEAD)
            g_deadSenderMsgs++;
        else if (wParam == WPARAM_LIVE)
            g_liveSenderMsgs++;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static VOID CALLBACK LiveCompletion(HWND hwnd, UINT msg, ULONG_PTR data, LRESULT result)
{
    InterlockedIncrement(&g_liveCbFired);
}

static VOID CALLBACK DeadCompletion(HWND hwnd, UINT msg, ULONG_PTR data, LRESULT result)
{
    /* must never run: the sender is gone before the receiver dispatches the message */
    ok(0, "the completion callback of a dead sender was invoked\n");
}

static void pump_all(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

/* the callback sender posts an inter-thread SendMessageCallback and then exits cleanly */
static DWORD WINAPI DeadSenderProc(LPVOID arg)
{
    SendMessageCallbackW((HWND)arg, WM_APP, WPARAM_DEAD, 0, DeadCompletion, 0);
    return 0; /* exit now: this thread's queue is torn down before the receiver pumps */
}

static void test_callback_sender_died(HWND hwnd)
{
    HANDLE th;
    int i;

    for (i = 0; i < 3; i++)
    {
        g_deadSenderMsgs = 0;

        th = CreateThread(NULL, 0, DeadSenderProc, hwnd, 0, NULL);
        ok(th != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (!th)
            break;

        /* Do not pump until the sender has fully exited: the callback message then sits on
           this thread's queue with a callback-sender whose THREADINFO and wake event are
           already gone. Dispatching it must not requeue/wake that queue -- that was the
           T131 use-after-free (KeSetEvent on a freed pEventQueueServer). */
        ok(WaitForSingleObject(th, 5000) == WAIT_OBJECT_0, "the callback sender did not exit\n");
        CloseHandle(th);
        Sleep(50);

        pump_all(); /* if the bug is present the machine faults in here */

        ok(g_deadSenderMsgs == 0,
           "a dead callback-sender's message was dispatched %ld times\n", g_deadSenderMsgs);
    }
}

/* a live callback sender must still be served normally (no regression) */
static DWORD WINAPI LiveSenderProc(LPVOID arg)
{
    MSG msg;
    int i;

    SendMessageCallbackW((HWND)arg, WM_APP, WPARAM_LIVE, 0, LiveCompletion, 0);

    /* pump so the completion callback can be delivered back to us */
    for (i = 0; i < 200 && !g_liveCbFired; i++)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
        Sleep(10);
    }
    return 0;
}

static void test_callback_sender_live(HWND hwnd)
{
    HANDLE th;
    int i;

    g_liveSenderMsgs = 0;
    g_liveCbFired = 0;

    th = CreateThread(NULL, 0, LiveSenderProc, hwnd, 0, NULL);
    ok(th != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!th)
        return;

    /* the receiver (this thread) must pump for the message to be dispatched */
    for (i = 0; i < 200; i++)
    {
        if (MsgWaitForMultipleObjects(1, &th, FALSE, 50, QS_ALLINPUT) == WAIT_OBJECT_0)
            break;
        pump_all();
    }
    pump_all();

    ok(WaitForSingleObject(th, 5000) == WAIT_OBJECT_0, "the live callback sender did not finish\n");
    CloseHandle(th);

    ok(g_liveSenderMsgs >= 1, "a live callback-sender's message was not dispatched\n");
    ok(g_liveCbFired >= 1, "the live completion callback did not fire\n");
}

START_TEST(SendMessageCallbackSenderDied)
{
    WNDCLASSW wc;
    HWND hwnd;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = ReceiverProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = g_class;
    RegisterClassW(&wc);

    hwnd = CreateWindowW(g_class, L"cb receiver", WS_POPUP | WS_VISIBLE,
                         100, 100, 100, 100, NULL, NULL, wc.hInstance, NULL);
    ok(hwnd != NULL, "CreateWindow failed: %lu\n", GetLastError());
    if (!hwnd)
        return;

    test_callback_sender_died(hwnd);
    test_callback_sender_live(hwnd);

    DestroyWindow(hwnd);
    UnregisterClassW(g_class, wc.hInstance);
}
