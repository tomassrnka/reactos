/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL - See COPYING in the top level directory
 * PURPOSE:     Contract test: the kernel-internal WM_ASYNC_* message range
 *              (bit 31 set, see enum internal_event_message in
 *              win32ss/user/ntuser/msgqueue.h) must be rejected when it arrives
 *              from user mode, and must keep working for kernel-originated
 *              dispatch (e.g. SetWindowPos, which posts WM_ASYNC_SETWINDOWPOS
 *              internally to another threads window).
 *
 *              Security background: handle_internal_message() treats lParam of
 *              WM_ASYNC_SETWINDOWPOS as a kernel-built PWINDOWPOS, dereferences
 *              it and ExFreePoolWithTag()s it. If user mode could route such a
 *              message it would be an arbitrary kernel-pointer free / EoP. The
 *              fix gates bit-31 messages at NtUserMessageCall / NtUserPostMessage
 *              / NtUserPostThreadMessage.
 */

#include "../win32nt.h"

/* Mirror of enum internal_event_message (kernel-private, not exported). */
#define WM_ASYNC_SHOWWINDOW     0x80000000
#define WM_ASYNC_SETWINDOWPOS   0x80000001
#define WM_ASYNC_DESTROYWINDOW  0x80000003

/* A bogus "kernel pointer". If the gate were missing, the unfixed kernel would
   dereference and ExFreePoolWithTag() this value -> bugcheck. With the fix the
   calls are rejected before handle_internal_message() ever runs. */
#define BOGUS_KERNEL_PTR        ((LPARAM)(ULONG_PTR)0xDEADBEEFDEADBEEFULL)

static HWND g_hWndWorker = NULL;
static HANDLE g_hWorkerReady = NULL;
static DWORD g_WorkerTid = 0;

static DWORD WINAPI WorkerThreadProc(LPVOID param)
{
    MSG msg;
    HINSTANCE hinst = GetModuleHandle(NULL);

    g_hWndWorker = CreateWindowA("BUTTON", "AsyncWorker",
                                 BS_PUSHBUTTON | WS_OVERLAPPEDWINDOW,
                                 0, 0, 40, 40, NULL, NULL, hinst, 0);
    /* Force the message queue to exist. */
    PeekMessageA(&msg, NULL, 0, 0, PM_NOREMOVE);
    SetEvent(g_hWorkerReady);

    while (GetMessageA(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

START_TEST(NtUserMessageCallAsync)
{
    HINSTANCE hinst = GetModuleHandle(NULL);
    HWND hWnd;
    HANDLE hThread;
    BOOL bRet;
    ULONG_PTR Result = 0;
    RECT rc;

    hWnd = CreateWindowA("BUTTON", "AsyncTest",
                         BS_PUSHBUTTON | WS_VISIBLE,
                         0, 0, 50, 30, NULL, NULL, hinst, 0);
    ASSERT(hWnd != NULL);

    /* --- Negative contract: user-mode must NOT be able to route an internal
       WM_ASYNC_* message. All three entry points must reject bit-31 messages.
       The real proof is that the system does not bugcheck on BOGUS_KERNEL_PTR. */

    /* NtUserMessageCall / FNID_SENDMESSAGE (same-thread window -> would reach
       handle_internal_message -> ExFreePoolWithTag(BOGUS_KERNEL_PTR)). */
    SetLastError(0xdeadf00d);
    bRet = NtUserMessageCall(hWnd, WM_ASYNC_SETWINDOWPOS, 0, BOGUS_KERNEL_PTR,
                             (ULONG_PTR)&Result, FNID_SENDMESSAGE, FALSE);
    ok(bRet == FALSE,
       "NtUserMessageCall(WM_ASYNC_SETWINDOWPOS) should be rejected, got %d\n", bRet);

    /* WM_ASYNC_SHOWWINDOW and WM_ASYNC_DESTROYWINDOW act cross-thread/cross-process
       with no sender check; they too must be refused from user mode. */
    bRet = NtUserMessageCall(hWnd, WM_ASYNC_SHOWWINDOW, SW_HIDE, 0,
                             (ULONG_PTR)&Result, FNID_SENDMESSAGE, FALSE);
    ok(bRet == FALSE,
       "NtUserMessageCall(WM_ASYNC_SHOWWINDOW) should be rejected, got %d\n", bRet);
    /* The window must still be visible: the rejected message must not have run. */
    ok(IsWindowVisible(hWnd) == TRUE,
       "Window was acted on by a rejected WM_ASYNC_SHOWWINDOW\n");

    bRet = NtUserMessageCall(hWnd, WM_ASYNC_DESTROYWINDOW, 0, 0,
                             (ULONG_PTR)&Result, FNID_SENDMESSAGE, FALSE);
    ok(bRet == FALSE,
       "NtUserMessageCall(WM_ASYNC_DESTROYWINDOW) should be rejected, got %d\n", bRet);
    ok(IsWindow(hWnd) == TRUE,
       "Window was destroyed by a rejected WM_ASYNC_DESTROYWINDOW\n");

    /* Post paths. */
    bRet = NtUserPostMessage(hWnd, WM_ASYNC_SETWINDOWPOS, 0, BOGUS_KERNEL_PTR);
    ok(bRet == FALSE,
       "NtUserPostMessage(WM_ASYNC_SETWINDOWPOS) should be rejected, got %d\n", bRet);

    bRet = NtUserPostThreadMessage(GetCurrentThreadId(), WM_ASYNC_SETWINDOWPOS,
                                   0, BOGUS_KERNEL_PTR);
    ok(bRet == FALSE,
       "NtUserPostThreadMessage(WM_ASYNC_SETWINDOWPOS) should be rejected, got %d\n", bRet);

    /* --- Positive control 1: ordinary window positioning on a same-thread
       window still works (the gate rejects only bit-31 messages). */
    bRet = SetWindowPos(hWnd, NULL, 11, 22, 60, 40, SWP_NOZORDER | SWP_NOACTIVATE);
    ok(bRet == TRUE, "SetWindowPos (same thread) failed: %lu\n", GetLastError());
    ok(GetWindowRect(hWnd, &rc) == TRUE, "GetWindowRect failed\n");
    ok(rc.left == 11 && rc.top == 22,
       "Window not moved: left=%ld top=%ld\n", rc.left, rc.top);

    /* --- Positive control 2: cross-thread SetWindowPos with SWP_ASYNCWINDOWPOS.
       co_WinPosSetWindowPos only takes the async branch when SWP_ASYNCWINDOWPOS
       is set: it allocates a kernel PWINDOWPOS and posts WM_ASYNC_SETWINDOWPOS
       (via co_IntSendMessageNoWait) to the owning thread. This exercises the
       legitimate kernel WM_ASYNC_SETWINDOWPOS dispatch that the fix must leave
       intact; without SWP_ASYNCWINDOWPOS the async path is never driven and this
       control would pass even if the gate broke it. */
    g_hWorkerReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    ASSERT(g_hWorkerReady != NULL);
    hThread = CreateThread(NULL, 0, WorkerThreadProc, NULL, 0, &g_WorkerTid);
    ASSERT(hThread != NULL);
    WaitForSingleObject(g_hWorkerReady, 5000);
    ok(g_hWndWorker != NULL, "worker window not created\n");

    if (g_hWndWorker)
    {
        bRet = SetWindowPos(g_hWndWorker, NULL, 33, 44, 70, 50,
                            SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        ok(bRet == TRUE, "SetWindowPos (cross thread) failed: %lu\n", GetLastError());
        /* Give the worker time to process the posted WM_ASYNC_SETWINDOWPOS. */
        Sleep(200);
        ok(GetWindowRect(g_hWndWorker, &rc) == TRUE, "GetWindowRect (worker) failed\n");
        ok(rc.left == 33 && rc.top == 44,
           "Cross-thread window not moved (kernel WM_ASYNC_SETWINDOWPOS broken?): left=%ld top=%ld\n",
           rc.left, rc.top);

        /* --- Positive control 3: ShowWindowAsync drives the legitimate kernel
           WM_ASYNC_SHOWWINDOW dispatch (NtUserShowWindowAsync ->
           co_IntSendMessageNoWait(hWnd, WM_ASYNC_SHOWWINDOW, ...)). The worker
           window was created without WS_VISIBLE, so making it visible through the
           async path proves the gate left the kernel-originated
           WM_ASYNC_SHOWWINDOW path intact. */
        ok(IsWindowVisible(g_hWndWorker) == FALSE,
           "worker window unexpectedly visible before ShowWindowAsync\n");
        bRet = ShowWindowAsync(g_hWndWorker, SW_SHOW);
        ok(bRet == TRUE, "ShowWindowAsync failed: %lu\n", GetLastError());
        /* Give the worker time to process the posted WM_ASYNC_SHOWWINDOW. */
        Sleep(200);
        ok(IsWindowVisible(g_hWndWorker) == TRUE,
           "Worker window not shown (kernel WM_ASYNC_SHOWWINDOW broken?)\n");
    }

    /* Teardown. */
    PostThreadMessageA(g_WorkerTid, WM_QUIT, 0, 0);
    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    CloseHandle(g_hWorkerReady);
    DestroyWindow(hWnd);
}
