/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Wake and change bit semantics of MsgWaitForMultipleObjectsEx,
 *                  GetQueueStatus and PeekMessage (an idle wait loop must block)
 */

#include "precomp.h"

#ifndef QS_RAWINPUT
#define QS_RAWINPUT 0x0400
#endif
#ifndef QS_TOUCH
#define QS_TOUCH 0x0800
#endif
#ifndef QS_POINTER
#define QS_POINTER 0x1000
#endif

#define WAIT_MASK (QS_KEY | QS_MOUSEMOVE | QS_MOUSEBUTTON | QS_POSTMESSAGE | QS_TIMER | \
                   QS_PAINT | QS_SENDMESSAGE | QS_HOTKEY | QS_ALLPOSTMESSAGE | QS_RAWINPUT | \
                   QS_TOUCH | QS_POINTER)

#define WM_TEST (WM_APP + 1)

/* A loop that wakes this many times with nothing to read while the queue is idle is a busy loop */
#define MAX_SPURIOUS 10
#define IDLE_MS      300

static const WCHAR g_ClassName[] = L"MsgWaitTestClass";

static volatile LONG g_SentReceived;

static LRESULT CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_TEST)
    {
        InterlockedIncrement(&g_SentReceived);
        return 42;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

typedef void (*THREAD_FN)(void);

static DWORD WINAPI ThreadProc(LPVOID arg)
{
    ((THREAD_FN)arg)();
    return 0;
}

static void RunInThread(THREAD_FN fn)
{
    HANDLE hThread = CreateThread(NULL, 0, ThreadProc, (LPVOID)fn, 0, NULL);
    ok(hThread != NULL, "CreateThread failed %lu\n", GetLastError());
    if (!hThread)
        return;
    ok(WaitForSingleObject(hThread, 30000) == WAIT_OBJECT_0, "test thread did not finish\n");
    CloseHandle(hThread);
}

static void EnsureQueue(void)
{
    MSG msg;
    PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
}

static void DrainQueue(void)
{
    MSG msg;
    int i;

    /* WM_PAINT stays queued until its window is validated, so dispatch it */
    for (i = 0; i < 1000 && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE); i++)
    {
        if (msg.message == WM_PAINT)
            DispatchMessageW(&msg);
    }
}

static HWND CreateTestWindow(void)
{
    return CreateWindowExW(0, g_ClassName, L"", WS_POPUP, 0, 0, 10, 10,
                           HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
}

/*
 * The wait loop of Firefox's WinUtils::WaitForMessage. Returns the number of
 * wake-ups that found nothing to read and went round again.
 */
static ULONG WaitForMessageLoop(DWORD timeoutMs, BOOL *pFound)
{
    DWORD start = GetTickCount(), elapsed = 0, result;
    ULONG spurious = 0;
    MSG msg;
    BOOL sentPending;

    *pFound = FALSE;
    while (TRUE)
    {
        elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs)
            break;
        result = MsgWaitForMultipleObjectsEx(0, NULL, timeoutMs - elapsed, WAIT_MASK,
                                             MWMO_INPUTAVAILABLE);
        if (result == WAIT_TIMEOUT)
            break;
        sentPending = (HIWORD(GetQueueStatus(QS_SENDMESSAGE)) & QS_SENDMESSAGE) != 0;
        if (sentPending || PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE))
        {
            *pFound = TRUE;
            break;
        }
        spurious++;
        SwitchToThread();
    }
    return spurious;
}

/*
 * A signal left by a message that was already read may end one wait early;
 * a second wait must then block.
 */
static DWORD WaitTwice(DWORD ms, DWORD flags)
{
    DWORD result = MsgWaitForMultipleObjectsEx(0, NULL, ms, WAIT_MASK, flags);

    if (result != WAIT_TIMEOUT)
        result = MsgWaitForMultipleObjectsEx(0, NULL, ms, WAIT_MASK, flags);
    return result;
}

/* After posted messages are removed, the queue must look empty and a wait must block */
static void Test_DrainedPostedQueue(void)
{
    MSG msg;
    DWORD id = GetCurrentThreadId(), status, result, start;
    ULONG spurious;
    BOOL found;
    int i, count = 0;

    EnsureQueue();
    for (i = 0; i < 3; i++)
        ok(PostThreadMessageW(id, WM_TEST, i, 0), "PostThreadMessage failed %lu\n", GetLastError());

    status = GetQueueStatus(QS_ALLINPUT | QS_ALLPOSTMESSAGE);
    ok((HIWORD(status) & QS_POSTMESSAGE) != 0, "posted message not reported, status 0x%lx\n", status);

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        ok(msg.message == WM_TEST, "got message 0x%x\n", msg.message);
        count++;
    }
    ok(count == 3, "removed %d messages, expected 3\n", count);

    status = GetQueueStatus(QS_ALLINPUT | QS_ALLPOSTMESSAGE);
    ok(HIWORD(status) == 0, "wake bits after draining are 0x%x (status 0x%lx)\n", HIWORD(status), status);

    start = GetTickCount();
    result = WaitTwice(100, MWMO_INPUTAVAILABLE);
    ok(result == WAIT_TIMEOUT, "wait on an empty queue returned 0x%lx\n", result);
    ok(GetTickCount() - start >= 50, "wait on an empty queue returned after %lu ms\n", GetTickCount() - start);

    spurious = WaitForMessageLoop(IDLE_MS, &found);
    trace("drained posted queue: %lu spurious wake-ups in %d ms\n", spurious, IDLE_MS);
    ok(!found, "loop found a message on an empty queue\n");
    ok(spurious < MAX_SPURIOUS, "%lu wake-ups with nothing to read in %d ms\n", spurious, IDLE_MS);
}

/* Change bits (new since last check) against wake bits (still in the queue) */
static void Test_ChangeAndWakeBits(void)
{
    MSG msg;
    DWORD id = GetCurrentThreadId(), result, status;

    EnsureQueue();
    DrainQueue();
    ok(PostThreadMessageW(id, WM_TEST, 0, 0), "PostThreadMessage failed %lu\n", GetLastError());

    result = MsgWaitForMultipleObjectsEx(0, NULL, 1000, WAIT_MASK, 0);
    ok(result == WAIT_OBJECT_0, "wait for a new message returned 0x%lx\n", result);

    ok(PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE), "PeekMessage did not find the message\n");

    result = WaitTwice(100, 0);
    ok(result == WAIT_TIMEOUT, "wait without MWMO_INPUTAVAILABLE for a seen message returned 0x%lx\n", result);

    result = MsgWaitForMultipleObjectsEx(0, NULL, 100, WAIT_MASK, MWMO_INPUTAVAILABLE);
    ok(result == WAIT_OBJECT_0, "wait with MWMO_INPUTAVAILABLE for a queued message returned 0x%lx\n", result);

    ok(PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE), "PeekMessage did not remove the message\n");
    status = GetQueueStatus(QS_ALLINPUT | QS_ALLPOSTMESSAGE);
    ok(HIWORD(status) == 0, "wake bits after removing the message are 0x%x\n", HIWORD(status));

    result = MsgWaitForMultipleObjectsEx(0, NULL, 100, WAIT_MASK, MWMO_INPUTAVAILABLE);
    ok(result == WAIT_TIMEOUT, "wait with MWMO_INPUTAVAILABLE on a drained queue returned 0x%lx\n", result);
}

/* A message posted from another thread wakes the wait; afterwards the loop must idle again */
static DWORD g_PostTarget;

static DWORD WINAPI PostLater(LPVOID arg)
{
    Sleep(100);
    PostThreadMessageW(g_PostTarget, WM_TEST, 0, 0);
    return 0;
}

static void Test_PostFromOtherThread(void)
{
    HANDLE hThread;
    MSG msg;
    ULONG spurious;
    BOOL found;
    DWORD start;

    EnsureQueue();
    DrainQueue();
    g_PostTarget = GetCurrentThreadId();
    hThread = CreateThread(NULL, 0, PostLater, NULL, 0, NULL);
    ok(hThread != NULL, "CreateThread failed\n");

    start = GetTickCount();
    spurious = WaitForMessageLoop(5000, &found);
    ok(found, "loop did not see the posted message\n");
    ok(GetTickCount() - start < 3000, "message seen after %lu ms\n", GetTickCount() - start);
    ok(spurious < MAX_SPURIOUS, "%lu spurious wake-ups before the post\n", spurious);
    ok(PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE) && msg.message == WM_TEST, "message not removed\n");
    if (hThread)
    {
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }

    spurious = WaitForMessageLoop(IDLE_MS, &found);
    trace("after a cross-thread post: %lu spurious wake-ups in %d ms\n", spurious, IDLE_MS);
    ok(!found, "loop found a message on an empty queue\n");
    ok(spurious < MAX_SPURIOUS, "%lu wake-ups with nothing to read in %d ms\n", spurious, IDLE_MS);
}

/* Sent messages from another thread: delivered, QS_SENDMESSAGE cleared, no busy loop afterwards */
static HWND g_SendTarget;
static volatile LONG g_SendReplies;

static DWORD WINAPI SendLater(LPVOID arg)
{
    int i;

    for (i = 0; i < 3; i++)
    {
        Sleep(60);
        if (SendMessageW(g_SendTarget, WM_TEST, i, 0) == 42)
            InterlockedIncrement(&g_SendReplies);
    }
    return 0;
}

static void Test_SentFromOtherThread(void)
{
    HANDLE hThread;
    HWND hwnd;
    MSG msg;
    ULONG spurious, total = 0;
    BOOL found;
    DWORD start, status;

    EnsureQueue();
    hwnd = CreateTestWindow();
    ok(hwnd != NULL, "CreateWindow failed %lu\n", GetLastError());
    if (!hwnd)
        return;
    DrainQueue();

    g_SentReceived = 0;
    g_SendReplies = 0;
    g_SendTarget = hwnd;
    hThread = CreateThread(NULL, 0, SendLater, NULL, 0, NULL);
    ok(hThread != NULL, "CreateThread failed\n");

    start = GetTickCount();
    while (g_SentReceived < 3 && GetTickCount() - start < 5000)
    {
        spurious = WaitForMessageLoop(1000, &found);
        total += spurious;
        if (found)
            PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE);
    }
    ok(g_SentReceived == 3, "%ld of 3 sent messages delivered\n", g_SentReceived);
    if (hThread)
    {
        ok(WaitForSingleObject(hThread, 5000) == WAIT_OBJECT_0, "sender did not finish\n");
        CloseHandle(hThread);
    }
    ok(g_SendReplies == 3, "%ld of 3 sends returned the window procedure result\n", g_SendReplies);
    trace("sent messages: %lu spurious wake-ups while waiting for them\n", total);
    ok(total < MAX_SPURIOUS * 3, "%lu wake-ups with nothing to read while waiting for sent messages\n", total);

    status = GetQueueStatus(QS_ALLINPUT | QS_ALLPOSTMESSAGE);
    ok((HIWORD(status) & QS_SENDMESSAGE) == 0, "QS_SENDMESSAGE still set after delivery, status 0x%lx\n", status);

    spurious = WaitForMessageLoop(IDLE_MS, &found);
    trace("after sent messages: %lu spurious wake-ups in %d ms\n", spurious, IDLE_MS);
    ok(!found, "loop found a message on an empty queue\n");
    ok(spurious < MAX_SPURIOUS, "%lu wake-ups with nothing to read in %d ms\n", spurious, IDLE_MS);
    DestroyWindow(hwnd);
}


/*
 * Wake bits left over after a message is consumed keep MWMO_INPUTAVAILABLE waits from blocking.
 * allowSent tolerates real sent messages from other processes (the shell talks to visible windows).
 */
static void ExpectIdle(const char *what, BOOL allowSent)
{
    DWORD status, wake, start, elapsed;
    ULONG spurious = 0;
    BOOL found;

    status = GetQueueStatus(QS_ALLINPUT | QS_ALLPOSTMESSAGE);
    wake = HIWORD(status);
    if (allowSent)
        wake &= ~QS_SENDMESSAGE;
    ok(wake == 0, "%s: wake bits 0x%lx left after the message was consumed\n", what, wake);

    start = GetTickCount();
    do
    {
        elapsed = GetTickCount() - start;
        if (elapsed >= IDLE_MS)
            break;
        spurious += WaitForMessageLoop(IDLE_MS - elapsed, &found);
        if (found)
        {
            ok(allowSent, "%s: loop found a message on an empty queue\n", what);
            DrainQueue();
        }
    } while (found && allowSent);
    trace("%s: %lu spurious wake-ups in %d ms\n", what, spurious, IDLE_MS);
    ok(spurious < MAX_SPURIOUS, "%s: %lu wake-ups with nothing to read in %d ms\n", what, spurious, IDLE_MS);
}

static BOOL PumpUntil(UINT message, DWORD timeoutMs)
{
    DWORD start = GetTickCount();
    MSG msg;

    while (GetTickCount() - start < timeoutMs)
    {
        if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == message)
                return TRUE;
            DispatchMessageW(&msg);
        }
        else
        {
            MsgWaitForMultipleObjectsEx(0, NULL, 50, WAIT_MASK, 0);
        }
    }
    return FALSE;
}

static void Test_TimerMessage(void)
{
    UINT_PTR timer;

    EnsureQueue();
    timer = SetTimer(NULL, 0, 10, NULL);
    ok(timer != 0, "SetTimer failed %lu\n", GetLastError());
    ok(PumpUntil(WM_TIMER, 3000), "no WM_TIMER\n");
    KillTimer(NULL, timer);
    DrainQueue();
    ExpectIdle("thread timer", FALSE);
}

static void Test_WindowTimerMessage(void)
{
    HWND hwnd;
    UINT_PTR timer;

    EnsureQueue();
    hwnd = CreateTestWindow();
    ok(hwnd != NULL, "CreateWindow failed %lu\n", GetLastError());
    if (!hwnd)
        return;
    timer = SetTimer(hwnd, 7, 10, NULL);
    ok(timer != 0, "SetTimer failed %lu\n", GetLastError());
    ok(PumpUntil(WM_TIMER, 3000), "no WM_TIMER\n");
    KillTimer(hwnd, 7);
    DrainQueue();
    ExpectIdle("window timer", FALSE);
    DestroyWindow(hwnd);
}

static void Test_QuitMessage(void)
{
    EnsureQueue();
    PostQuitMessage(0);
    ok(PumpUntil(WM_QUIT, 1000), "no WM_QUIT\n");
    DrainQueue();
    ExpectIdle("quit message", FALSE);
}

static void Test_PostToWindow(void)
{
    HWND hwnd;

    EnsureQueue();
    hwnd = CreateTestWindow();
    ok(hwnd != NULL, "CreateWindow failed %lu\n", GetLastError());
    if (!hwnd)
        return;
    DrainQueue();
    ok(PostMessageW(hwnd, WM_TEST, 0, 0), "PostMessage failed %lu\n", GetLastError());
    ok(PumpUntil(WM_TEST, 1000), "no posted message\n");
    ExpectIdle("posted to a window", FALSE);
    DestroyWindow(hwnd);
}

static void Test_PaintMessage(void)
{
    HWND hwnd;

    EnsureQueue();
    hwnd = CreateWindowExW(0, g_ClassName, L"", WS_POPUP | WS_VISIBLE, 0, 0, 10, 10,
                           NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(hwnd != NULL, "CreateWindow failed %lu\n", GetLastError());
    if (!hwnd)
        return;
    InvalidateRect(hwnd, NULL, TRUE);
    ok(PumpUntil(WM_PAINT, 1000) || !GetUpdateRect(hwnd, NULL, FALSE), "paint not delivered\n");
    UpdateWindow(hwnd);
    Sleep(100);
    DrainQueue();
    ExpectIdle("paint message", TRUE);
    DestroyWindow(hwnd);
}

static DWORD WINAPI NotifyLater(LPVOID arg)
{
    Sleep(50);
    SendNotifyMessageW(g_SendTarget, WM_TEST, 0, 0);
    return 0;
}

static void Test_SendNotify(void)
{
    HWND hwnd;
    HANDLE hThread;

    EnsureQueue();
    hwnd = CreateTestWindow();
    ok(hwnd != NULL, "CreateWindow failed %lu\n", GetLastError());
    if (!hwnd)
        return;
    DrainQueue();
    g_SentReceived = 0;
    g_SendTarget = hwnd;
    hThread = CreateThread(NULL, 0, NotifyLater, NULL, 0, NULL);
    ok(hThread != NULL, "CreateThread failed\n");
    PumpUntil(WM_NULL, 300);
    ok(g_SentReceived == 1, "%ld notify messages delivered\n", g_SentReceived);
    if (hThread)
    {
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }
    ExpectIdle("send notify message", FALSE);
    DestroyWindow(hwnd);
}

/* A timer that fires and is removed before its message is read must not leave QS_TIMER behind */
static void Test_ReadyTimerKilled(void)
{
    UINT_PTR timer;
    DWORD start;

    EnsureQueue();
    timer = SetTimer(NULL, 0, 10, NULL);
    ok(timer != 0, "SetTimer failed %lu\n", GetLastError());
    start = GetTickCount();
    while (!(HIWORD(GetQueueStatus(QS_TIMER)) & QS_TIMER) && GetTickCount() - start < 2000)
        Sleep(10);
    ok(HIWORD(GetQueueStatus(QS_TIMER)) & QS_TIMER, "timer never became ready\n");
    KillTimer(NULL, timer);
    DrainQueue();
    ExpectIdle("ready timer killed", FALSE);
}

static void Test_ReadyWindowTimerDestroyed(void)
{
    HWND hwnd;
    DWORD start;

    EnsureQueue();
    hwnd = CreateTestWindow();
    ok(hwnd != NULL, "CreateWindow failed %lu\n", GetLastError());
    if (!hwnd)
        return;
    ok(SetTimer(hwnd, 7, 10, NULL) != 0, "SetTimer failed %lu\n", GetLastError());
    start = GetTickCount();
    while (!(HIWORD(GetQueueStatus(QS_TIMER)) & QS_TIMER) && GetTickCount() - start < 2000)
        Sleep(10);
    ok(HIWORD(GetQueueStatus(QS_TIMER)) & QS_TIMER, "timer never became ready\n");
    DestroyWindow(hwnd);
    DrainQueue();
    ExpectIdle("window with a ready timer destroyed", FALSE);
}

static void Test_PaintWindowDestroyed(void)
{
    HWND hwnd;

    EnsureQueue();
    hwnd = CreateWindowExW(0, g_ClassName, L"", WS_POPUP | WS_VISIBLE, 0, 0, 10, 10,
                           NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(hwnd != NULL, "CreateWindow failed %lu\n", GetLastError());
    if (!hwnd)
        return;
    InvalidateRect(hwnd, NULL, TRUE);
    DestroyWindow(hwnd);
    Sleep(100);
    DrainQueue();
    ExpectIdle("window with a pending paint destroyed", TRUE);
}

static void Test_PostedToDestroyedWindow(void)
{
    HWND hwnd;

    EnsureQueue();
    hwnd = CreateTestWindow();
    ok(hwnd != NULL, "CreateWindow failed %lu\n", GetLastError());
    if (!hwnd)
        return;
    DrainQueue();
    ok(PostMessageW(hwnd, WM_TEST, 0, 0), "PostMessage failed %lu\n", GetLastError());
    DestroyWindow(hwnd);
    DrainQueue();
    ExpectIdle("message posted to a window then destroyed", FALSE);
}

static void Test_QuitPostedTwice(void)
{
    EnsureQueue();
    PostQuitMessage(1);
    PostQuitMessage(2);
    ok(PumpUntil(WM_QUIT, 1000), "no WM_QUIT\n");
    DrainQueue();
    ExpectIdle("quit posted twice", FALSE);
}

static void Test_InternalPaint(void)
{
    HWND hwnd;

    EnsureQueue();
    hwnd = CreateTestWindow();
    ok(hwnd != NULL, "CreateWindow failed %lu\n", GetLastError());
    if (!hwnd)
        return;
    DrainQueue();
    RedrawWindow(hwnd, NULL, NULL, RDW_INTERNALPAINT);
    PumpUntil(WM_PAINT, 300);
    Sleep(50);
    DrainQueue();
    ExpectIdle("internal paint", TRUE);
    DestroyWindow(hwnd);
}

static void Test_ChildPaintDestroyed(void)
{
    HWND parent, child;

    EnsureQueue();
    parent = CreateWindowExW(0, g_ClassName, L"", WS_POPUP | WS_VISIBLE, 0, 0, 20, 20,
                             NULL, NULL, GetModuleHandleW(NULL), NULL);
    child = parent ? CreateWindowExW(0, g_ClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10,
                                     parent, NULL, GetModuleHandleW(NULL), NULL) : NULL;
    ok(parent && child, "CreateWindow failed %lu\n", GetLastError());
    if (!parent || !child)
        return;
    InvalidateRect(child, NULL, TRUE);
    DestroyWindow(parent);
    Sleep(100);
    DrainQueue();
    ExpectIdle("child with a pending paint destroyed with its parent", TRUE);
}

/* Raw input and mouse moves are counted per message; the bits must go once the messages are read */
static void Test_RawInputAndMouse(void)
{
    HWND hwnd;
    POINT pt;
    RAWINPUTDEVICE rid;
    BOOL gotInput, gotMove;

    EnsureQueue();
    if (!GetCursorPos(&pt))
    {
        skip("GetCursorPos failed\n");
        return;
    }
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, g_ClassName, L"", WS_POPUP, pt.x - 20, pt.y - 20, 40, 40,
                           NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(hwnd != NULL, "CreateWindow failed %lu\n", GetLastError());
    if (!hwnd)
        return;
    ShowWindow(hwnd, SW_SHOWNA);
    rid.usUsagePage = 1;
    rid.usUsage = 2;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
    {
        skip("RegisterRawInputDevices failed %lu\n", GetLastError());
        DestroyWindow(hwnd);
        return;
    }
    Sleep(100);
    DrainQueue();

    mouse_event(MOUSEEVENTF_MOVE, 1, 0, 0, 0);
    mouse_event(MOUSEEVENTF_MOVE, -1, 0, 0, 0);
    gotInput = PumpUntil(WM_INPUT, 500);
    gotMove = PumpUntil(WM_MOUSEMOVE, 100);
    trace("raw input %d, mouse move %d\n", gotInput, gotMove);
    if (!gotInput)
        skip("no WM_INPUT delivered for injected mouse input\n");
    Sleep(50);
    DrainQueue();
    ExpectIdle("raw input and mouse move", TRUE);

    rid.dwFlags = RIDEV_REMOVE;
    rid.hwndTarget = NULL;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    DestroyWindow(hwnd);
}

/* Two threads sharing an input queue (AttachThreadInput), each in the loop, both idle */
static volatile LONG g_AttachReady;
static volatile LONG g_AttachGo;
static volatile ULONG g_AttachSpurious[2];
static volatile DWORD g_AttachTid[2];

static DWORD WINAPI AttachedWaiter(LPVOID arg)
{
    LONG idx = (LONG)(LONG_PTR)arg;
    DWORD startWait = GetTickCount();
    HWND hwnd;
    BOOL found;
    MSG msg;
    ULONG n;

    EnsureQueue();
    hwnd = CreateTestWindow();
    DrainQueue();
    g_AttachTid[idx] = GetCurrentThreadId();
    InterlockedIncrement(&g_AttachReady);
    while (!g_AttachGo && GetTickCount() - startWait < 10000)
        Sleep(1);
    n = WaitForMessageLoop(IDLE_MS, &found);
    g_AttachSpurious[idx] = n;
    if (found)
        PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE);
    if (hwnd)
        DestroyWindow(hwnd);
    return 0;
}

static void Test_AttachedIdle(void)
{
    HANDLE hThreads[2];
    BOOL attached;
    DWORD startWait = GetTickCount();
    int i;

    g_AttachReady = 0;
    g_AttachGo = 0;
    for (i = 0; i < 2; i++)
    {
        g_AttachSpurious[i] = 0;
        hThreads[i] = CreateThread(NULL, 0, AttachedWaiter, (LPVOID)(LONG_PTR)i, 0, NULL);
        ok(hThreads[i] != NULL, "CreateThread failed\n");
        if (!hThreads[i])
            return;
    }
    while (g_AttachReady < 2 && GetTickCount() - startWait < 10000)
        Sleep(1);

    attached = AttachThreadInput(g_AttachTid[0], g_AttachTid[1], TRUE);
    ok(attached, "AttachThreadInput failed %lu\n", GetLastError());
    g_AttachGo = 1;
    ok(WaitForMultipleObjects(2, hThreads, TRUE, 15000) == WAIT_OBJECT_0, "attached threads did not finish\n");
    for (i = 0; i < 2; i++)
    {
        trace("attached thread %d: %lu spurious wake-ups in %d ms\n", i, g_AttachSpurious[i], IDLE_MS);
        ok(g_AttachSpurious[i] < MAX_SPURIOUS, "attached thread %d: %lu wake-ups with nothing to read\n",
           i, g_AttachSpurious[i]);
        CloseHandle(hThreads[i]);
    }
}

START_TEST(MsgWaitForMultipleObjectsEx)
{
    WNDCLASSW wc = {0};

    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = g_ClassName;
    ok(RegisterClassW(&wc) != 0, "RegisterClass failed %lu\n", GetLastError());

    RunInThread(Test_DrainedPostedQueue);
    RunInThread(Test_ChangeAndWakeBits);
    RunInThread(Test_PostFromOtherThread);
    RunInThread(Test_SentFromOtherThread);
    RunInThread(Test_TimerMessage);
    RunInThread(Test_WindowTimerMessage);
    RunInThread(Test_QuitMessage);
    RunInThread(Test_PostToWindow);
    RunInThread(Test_PaintMessage);
    RunInThread(Test_SendNotify);
    RunInThread(Test_ReadyTimerKilled);
    RunInThread(Test_ReadyWindowTimerDestroyed);
    RunInThread(Test_PaintWindowDestroyed);
    RunInThread(Test_PostedToDestroyedWindow);
    RunInThread(Test_QuitPostedTwice);
    RunInThread(Test_InternalPaint);
    RunInThread(Test_ChildPaintDestroyed);
    RunInThread(Test_RawInputAndMouse);
    RunInThread(Test_AttachedIdle);

    UnregisterClassW(g_ClassName, GetModuleHandleW(NULL));
}
