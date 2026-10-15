/*
 * PROJECT:     ReactOS test VM agent
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Two kinds of misbehaving GUI applications for the shutdown reproducers in host/:
 *                (no argument)  a window whose thread never answers messages, like an application
 *                               that is hung: a forced shutdown must not wait for it
 *                churn          an application whose threads keep ending and starting and whose window
 *                               answers the end of the session slowly: the session manager walks the
 *                               thread list of such a process while it changes
 */

#include <windows.h>
#include <wchar.h>

static BOOL g_slow;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    /* a slow answer leaves the session manager time to see other threads of the process end */
    if (g_slow && (msg == WM_QUERYENDSESSION || msg == WM_ENDSESSION))
    {
        Sleep(1000);
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI ShortThread(LPVOID param)
{
    Sleep(1 + (GetCurrentThreadId() * 7) % 10);
    return 0;
}

/* starts a thread that ends within a few milliseconds, again and again, without owning a window */
static DWORD WINAPI SpawnerThread(LPVOID param)
{
    for (;;)
    {
        HANDLE h = CreateThread(NULL, 0, ShortThread, NULL, 0, NULL);

        if (h)
            CloseHandle(h);
        Sleep(0);
    }

    return 0;
}

static HINSTANCE g_instance;

static DWORD WINAPI WindowThread(LPVOID param)
{
    MSG msg;
    HWND hwnd = CreateWindowW(L"RosDevHungWindow", L"churn", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              50, 50, 300, 200, NULL, NULL, g_instance, NULL);

    if (!hwnd)
        return 1;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR cmdLine, int nShow)
{
    WNDCLASSW wc;
    HWND hwnd;
    BOOL churn = cmdLine && !wcscmp(cmdLine, L"churn");
    int i;

    g_instance = hInstance;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"RosDevHungWindow";
    RegisterClassW(&wc);

    if (churn)
    {
        /*
         * The threads of a process are walked in the order they were made. The window thread comes after
         * the thread that makes the short lived ones, so the entries that follow it keep ending.
         */
        g_slow = TRUE;
        for (i = 0; i < 4; i++)
            CreateThread(NULL, 0, SpawnerThread, NULL, 0, NULL);
        Sleep(100);
        CreateThread(NULL, 0, WindowThread, NULL, 0, NULL);
        Sleep(INFINITE);
        return 0;
    }

    hwnd = CreateWindowW(wc.lpszClassName, L"hung", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         50, 50, 300, 200, NULL, NULL, hInstance, NULL);
    if (!hwnd)
        return 1;

    /* the thread owns a window and never takes a message from its queue */
    Sleep(INFINITE);
    return 0;
}
