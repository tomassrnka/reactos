/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Test for GetMessage/PeekMessage
 * PROGRAMMERS:     Thomas Faber <thomas.faber@reactos.org>
 */

#include "precomp.h"

void Test_GetMessage(HWND hWnd)
{
    MSG msg;

    SetLastError(DNS_ERROR_RCODE_NXRRSET);

    ok(GetMessage(&msg, hWnd, 0, 0) == -1, "\n");
    ok(GetLastError() == ERROR_INVALID_WINDOW_HANDLE, "GetLastError() = %lu\n", GetLastError());
}

void Test_PeekMessage(HWND hWnd)
{
    MSG msg;

    SetLastError(DNS_ERROR_RCODE_NXRRSET);

    ok(PeekMessage(&msg, hWnd, 0, 0, PM_NOREMOVE) == 0, "\n");
    ok(GetLastError() == ERROR_INVALID_WINDOW_HANDLE, "GetLastError() = %lu\n", GetLastError());
}

/*
 * A posted WM_TIMER must not leave QS_ALLPOSTMESSAGE set once the queue is
 * empty: MsgWaitForMultipleObjectsEx with MWMO_INPUTAVAILABLE would then
 * return at once forever, and a loop that waits like that (Firefox's) spins.
 */
static void Test_AllPostMessageCleared(void)
{
    HWND hWnd;
    MSG msg;
    DWORD ret, status, start;
    int timers = 0, i;

    hWnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 10, 10, NULL, NULL, GetModuleHandle(NULL), NULL);
    ok(hWnd != NULL, "CreateWindowExW failed: %lu\n", GetLastError());
    if (!hWnd)
        return;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);

    ok(SetTimer(hWnd, 1, 10, NULL) != 0, "SetTimer failed: %lu\n", GetLastError());
    start = GetTickCount();
    while (!timers && GetTickCount() - start < 2000)
    {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_TIMER);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_TIMER)
                timers++;
            DispatchMessageW(&msg);
        }
    }
    KillTimer(hWnd, 1);
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
    ok(timers > 0, "No WM_TIMER arrived\n");

    status = GetQueueStatus(QS_ALLPOSTMESSAGE);
    ok(status == 0, "GetQueueStatus(QS_ALLPOSTMESSAGE) after the timer = 0x%lx\n", status);
    /* The queue event may still hold one wake from the last post; a stuck wake bit returns every time */
    for (i = 0; i < 3; i++)
        ret = MsgWaitForMultipleObjectsEx(0, NULL, 50, QS_ALLPOSTMESSAGE, MWMO_INPUTAVAILABLE);
    ok(ret == WAIT_TIMEOUT, "MsgWaitForMultipleObjectsEx after the timer returned %lu\n", ret);

    /* The same once a posted message has been removed */
    ok(PostMessageW(hWnd, WM_USER, 0, 0), "PostMessageW failed: %lu\n", GetLastError());
    ok(PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE) && msg.message == WM_USER, "WM_USER not retrieved\n");
    status = GetQueueStatus(QS_ALLPOSTMESSAGE | QS_POSTMESSAGE);
    ok(HIWORD(status) == 0, "GetQueueStatus after the posted message = 0x%lx\n", status);
    for (i = 0; i < 3; i++)
        ret = MsgWaitForMultipleObjectsEx(0, NULL, 50, QS_ALLPOSTMESSAGE | QS_POSTMESSAGE, MWMO_INPUTAVAILABLE);
    ok(ret == WAIT_TIMEOUT, "MsgWaitForMultipleObjectsEx after the posted message returned %lu\n", ret);

    DestroyWindow(hWnd);
}

START_TEST(GetPeekMessage)
{
    HWND hWnd = CreateWindowExW(0, L"EDIT", L"miau", 0, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, GetModuleHandle(NULL), NULL);
    ok(hWnd != INVALID_HANDLE_VALUE, "\n");
    /* make sure we pass an invalid handle to GetMessage/PeekMessage */
    ok(DestroyWindow(hWnd), "\n");

    Test_GetMessage(hWnd);
    Test_PeekMessage(hWnd);
    Test_AllPostMessageCleared();
}
