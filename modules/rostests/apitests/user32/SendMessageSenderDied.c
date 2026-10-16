/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         A sent message whose sender is terminated before the receiver dispatches it
 */

#include "precomp.h"

static DWORD WINAPI MoveWindowProc(LPVOID arg)
{
    /* the move sends WM_WINDOWPOSCHANGING with a pointer into this thread's kernel stack */
    SetWindowPos((HWND)arg, NULL, 20, 20, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
}

START_TEST(SendMessageSenderDied)
{
    WNDCLASSW wc;
    HWND hwnd, hwndLate;
    HANDLE th;
    MSG msg;
    DWORD ret;
    RECT rc;
    int i;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SenderDiedClass";
    RegisterClassW(&wc);

    hwnd = CreateWindowW(wc.lpszClassName, L"receiver", WS_POPUP | WS_VISIBLE,
                         100, 100, 100, 100, NULL, NULL, wc.hInstance, NULL);
    ok(hwnd != NULL, "CreateWindow failed: %lu\n", GetLastError());
    if (!hwnd)
        return;

    for (i = 0; i < 3; i++)
    {
        th = CreateThread(NULL, 0, MoveWindowProc, hwnd, 0, NULL);
        ok(th != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (!th)
            break;

        /* this thread does not pump, so the sender stays blocked until it is terminated */
        ret = WaitForSingleObject(th, 300);
        ok(ret == WAIT_TIMEOUT, "the cross-thread move completed without the receiver pumping: %lu\n", ret);
        TerminateThread(th, 0);
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
        Sleep(100);

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
    }

    ok(GetWindowRect(hwnd, &rc), "GetWindowRect failed: %lu\n", GetLastError());

    /* the receiver still takes messages from a sender that lives */
    hwndLate = CreateWindowW(wc.lpszClassName, L"late", WS_POPUP, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    ok(hwndLate != NULL, "second CreateWindow failed: %lu\n", GetLastError());
    th = CreateThread(NULL, 0, MoveWindowProc, hwnd, 0, NULL);
    ok(th != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (th)
    {
        for (i = 0; i < 100; i++)
        {
            if (MsgWaitForMultipleObjects(1, &th, FALSE, 50, QS_ALLINPUT) == WAIT_OBJECT_0)
                break;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
                DispatchMessageW(&msg);
        }
        ok(WaitForSingleObject(th, 0) == WAIT_OBJECT_0, "the move from a live thread did not complete\n");
        CloseHandle(th);
    }

    DestroyWindow(hwndLate);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
