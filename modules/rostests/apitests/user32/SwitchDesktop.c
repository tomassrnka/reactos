/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test activation of a window whose desktop stopped being the input desktop
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static const WCHAR s_szClassName[] = L"SwitchDesktopTest";
static const WCHAR s_szDesktopName[] = L"SwitchDesktopTest";

static HDESK s_hdeskTest;
static HANDLE s_hReady, s_hGo, s_hReady2, s_hCheck2;
static volatile LONG s_bAbort;

typedef struct _THREAD_RESULTS
{
    BOOL bSetThreadDesktop;
    BOOL bGo;
    HWND hwnd;
    HWND hwnd2;
    HWND hwndForegroundBefore;
    HWND hwndActiveBefore;
    BOOL bMove;
    HWND hwndActiveAfterMove;
    BOOL bTop;
    HWND hwndActiveAfterTop;
    BOOL bForeground;
    HWND hwndActiveAfterForeground;
    HWND hwndActiveAfterCreate2;
    HWND hwndActiveAfterSetActive;
    BOOL bMove2;
    HWND hwndActiveAfterMove2;
    BOOL bForeground2;
    HWND hwndActiveAfterForeground2;
    BOOL bThread2Created;
    BOOL bThread2;
    HWND hwnd3;
    HWND hwnd4;
    HWND hwndActive2Before;
    BOOL bMove3;
    HWND hwndActive2After;
} THREAD_RESULTS;

static THREAD_RESULTS s_Results;

static void PumpMessages(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* TRUE only when hEvent was signalled within dwTimeout */
static BOOL WaitPumping(HANDLE hEvent, DWORD dwTimeout)
{
    DWORD dwStart = GetTickCount(), dwElapsed, dwRet;

    for (;;)
    {
        dwElapsed = GetTickCount() - dwStart;
        if (dwElapsed >= dwTimeout)
            return FALSE;
        dwRet = MsgWaitForMultipleObjects(1, &hEvent, FALSE, dwTimeout - dwElapsed, QS_ALLINPUT);
        if (dwRet == WAIT_OBJECT_0)
            return TRUE;
        if (dwRet != WAIT_OBJECT_0 + 1)
            return FALSE;
        PumpMessages();
    }
}

static HWND CreateTestWindow(int x)
{
    return CreateWindowExW(0, s_szClassName, L"SwitchDesktop", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           x, 0, 200, 200, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

/* Second thread on the test desktop: owns two windows, the second one active */
static DWORD WINAPI Thread2Proc(LPVOID Param)
{
    THREAD_RESULTS *Res = &s_Results;
    DWORD dwStart;

    if (SetThreadDesktop(s_hdeskTest))
    {
        Res->hwnd3 = CreateTestWindow(100);
        Res->hwnd4 = CreateTestWindow(350);
        PumpMessages();
        Res->hwndActive2Before = GetActiveWindow();
    }
    SetEvent(s_hReady2);
    if (Res->hwnd3 && Res->hwnd4 && WaitPumping(s_hCheck2, 30000))
    {
        /* Let a posted activation arrive */
        dwStart = GetTickCount();
        while (GetTickCount() - dwStart < 500)
        {
            PumpMessages();
            Sleep(10);
        }
        Res->hwndActive2After = GetActiveWindow();
    }
    if (Res->hwnd4)
        DestroyWindow(Res->hwnd4);
    if (Res->hwnd3)
        DestroyWindow(Res->hwnd3);
    PumpMessages();
    return 0;
}

static DWORD WINAPI ThreadProc(LPVOID Param)
{
    THREAD_RESULTS *Res = &s_Results;
    HWND hwndOther;
    HANDLE hThread2;

    Res->bSetThreadDesktop = SetThreadDesktop(s_hdeskTest);
    if (Res->bSetThreadDesktop)
    {
        /* The test desktop is the input desktop */
        Res->hwnd = CreateTestWindow(0);
        PumpMessages();
        Res->hwndForegroundBefore = GetForegroundWindow();
        Res->hwndActiveBefore = GetActiveWindow();
    }

    SetEvent(s_hReady);
    Res->bGo = WaitPumping(s_hGo, 60000) && !s_bAbort;

    if (Res->hwnd && Res->bGo)
    {
        /* The main thread made its own desktop the input desktop again and took the foreground */
        Res->bMove = SetWindowPos(Res->hwnd, NULL, 10, 0, 0, 0, SWP_NOSIZE);
        PumpMessages();
        Res->hwndActiveAfterMove = GetActiveWindow();

        Res->bTop = SetWindowPos(Res->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
        PumpMessages();
        Res->hwndActiveAfterTop = GetActiveWindow();

        Res->bForeground = SetForegroundWindow(Res->hwnd);
        PumpMessages();
        Res->hwndActiveAfterForeground = GetActiveWindow();

        Res->hwnd2 = CreateTestWindow(250);
        PumpMessages();
        Res->hwndActiveAfterCreate2 = GetActiveWindow();

        if (Res->hwnd2)
        {
            SetActiveWindow(Res->hwnd);
            PumpMessages();
            Res->hwndActiveAfterSetActive = GetActiveWindow();

            Res->bMove2 = SetWindowPos(Res->hwnd2, NULL, 260, 0, 0, 0, SWP_NOSIZE);
            PumpMessages();
            Res->hwndActiveAfterMove2 = GetActiveWindow();

            hwndOther = (GetActiveWindow() == Res->hwnd) ? Res->hwnd2 : Res->hwnd;
            Res->bForeground2 = SetForegroundWindow(hwndOther);
            PumpMessages();
            Res->hwndActiveAfterForeground2 = GetActiveWindow();

            DestroyWindow(Res->hwnd2);
        }

        /* Reposition an inactive window of another thread on the same desktop */
        hThread2 = CreateThread(NULL, 0, Thread2Proc, NULL, 0, NULL);
        Res->bThread2Created = (hThread2 != NULL);
        if (hThread2 && WaitPumping(s_hReady2, 30000) && Res->hwnd3 && Res->hwnd4)
        {
            Res->bMove3 = SetWindowPos(Res->hwnd3, NULL, 110, 0, 0, 0, SWP_NOSIZE);
            PumpMessages();
            SetEvent(s_hCheck2);
        }
        else
        {
            SetEvent(s_hCheck2);
        }
        if (hThread2)
        {
            Res->bThread2 = WaitPumping(hThread2, 60000);
            if (Res->bThread2)
                CloseHandle(hThread2);
        }
    }

    if (Res->hwnd)
        DestroyWindow(Res->hwnd);
    PumpMessages();
    return 0;
}

START_TEST(SwitchDesktop)
{
    THREAD_RESULTS *Res = &s_Results;
    WNDCLASSW wc = { 0 };
    HDESK hdeskInput;
    HANDLE hThread = NULL;
    BOOL bOnTestDesktop = FALSE, bReady, bFinished = FALSE;
    HWND hwndMain = NULL, hwndForegroundTaken = NULL, hwndForegroundAfter = NULL;

    hdeskInput = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (!hdeskInput)
    {
        skip("OpenInputDesktop failed (%lu)\n", GetLastError());
        return;
    }

    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = s_szClassName;
    ok(RegisterClassW(&wc) != 0, "RegisterClassW failed (%lu)\n", GetLastError());

    s_hReady = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_hGo = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_hReady2 = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_hCheck2 = CreateEventW(NULL, FALSE, FALSE, NULL);
    s_hdeskTest = CreateDesktopW(s_szDesktopName, NULL, NULL, 0, GENERIC_ALL, NULL);
    ok(s_hReady && s_hGo && s_hReady2 && s_hCheck2, "CreateEventW failed (%lu)\n", GetLastError());
    ok(s_hdeskTest != NULL, "CreateDesktopW failed (%lu)\n", GetLastError());
    if (!s_hReady || !s_hGo || !s_hReady2 || !s_hCheck2 || !s_hdeskTest)
        goto Cleanup;

    bOnTestDesktop = SwitchDesktop(s_hdeskTest);
    ok(bOnTestDesktop, "SwitchDesktop(test) failed (%lu)\n", GetLastError());
    if (!bOnTestDesktop)
        goto Cleanup;

    hThread = CreateThread(NULL, 0, ThreadProc, NULL, 0, NULL);
    ok(hThread != NULL, "CreateThread failed (%lu)\n", GetLastError());
    bReady = hThread && WaitForSingleObject(s_hReady, 30000) == WAIT_OBJECT_0;
    ok(bReady, "The thread did not get ready\n");

    bOnTestDesktop = !SwitchDesktop(hdeskInput);
    ok(!bOnTestDesktop, "SwitchDesktop(input) failed (%lu)\n", GetLastError());
    if (!bReady || bOnTestDesktop)
        goto Cleanup;

    /* A window on the input desktop takes the foreground */
    hwndMain = CreateTestWindow(500);
    ok(hwndMain != NULL, "CreateWindowExW failed (%lu)\n", GetLastError());
    if (!hwndMain)
        goto Cleanup;
    SetForegroundWindow(hwndMain);
    PumpMessages();
    hwndForegroundTaken = GetForegroundWindow();

    SetEvent(s_hGo);
    bFinished = WaitPumping(hThread, 120000);
    ok(bFinished, "The thread did not finish\n");
    if (!bFinished)
        goto Cleanup;
    hwndForegroundAfter = GetForegroundWindow();

    ok(Res->bSetThreadDesktop, "SetThreadDesktop failed\n");
    ok(Res->hwnd != NULL, "CreateWindowExW failed\n");
    ok(Res->bGo, "The thread was not released\n");
    trace("before the switch: foreground %p active %p window %p; main window %p foreground %p\n",
          Res->hwndForegroundBefore, Res->hwndActiveBefore, Res->hwnd, hwndMain, hwndForegroundTaken);
    trace("move %d active %p; top %d active %p; foreground %d active %p\n",
          Res->bMove, Res->hwndActiveAfterMove, Res->bTop, Res->hwndActiveAfterTop,
          Res->bForeground, Res->hwndActiveAfterForeground);
    trace("second window %p active %p; SetActiveWindow active %p; move2 %d active %p; foreground2 %d active %p\n",
          Res->hwnd2, Res->hwndActiveAfterCreate2, Res->hwndActiveAfterSetActive,
          Res->bMove2, Res->hwndActiveAfterMove2, Res->bForeground2, Res->hwndActiveAfterForeground2);

    ok(Res->hwndActiveBefore == Res->hwnd, "Active window before the switch is %p, expected %p\n",
       Res->hwndActiveBefore, Res->hwnd);
    ok(hwndForegroundTaken == hwndMain, "Foreground window is %p, expected %p\n", hwndForegroundTaken, hwndMain);
    ok(Res->bMove, "SetWindowPos failed\n");
    ok(Res->hwndActiveAfterMove == Res->hwnd, "Active window after SetWindowPos is %p, expected %p\n",
       Res->hwndActiveAfterMove, Res->hwnd);
    ok(Res->bTop, "SetWindowPos(HWND_TOP) failed\n");
    ok(Res->hwndActiveAfterTop == Res->hwnd, "Active window after HWND_TOP is %p, expected %p\n",
       Res->hwndActiveAfterTop, Res->hwnd);
    ok(!Res->bForeground, "SetForegroundWindow succeeded for a window on another desktop\n");
    ok(Res->hwndActiveAfterForeground == Res->hwnd, "Active window after SetForegroundWindow is %p, expected %p\n",
       Res->hwndActiveAfterForeground, Res->hwnd);
    ok(Res->hwnd2 != NULL, "Second CreateWindowExW failed\n");
    ok(Res->hwndActiveAfterCreate2 == Res->hwnd2, "Active window after creating it is %p, expected %p\n",
       Res->hwndActiveAfterCreate2, Res->hwnd2);
    ok(Res->hwndActiveAfterSetActive == Res->hwnd, "Active window after SetActiveWindow is %p, expected %p\n",
       Res->hwndActiveAfterSetActive, Res->hwnd);
    ok(Res->bMove2, "SetWindowPos of the second window failed\n");
    ok(Res->hwndActiveAfterMove2 == Res->hwnd2, "Active window after SetWindowPos is %p, expected %p\n",
       Res->hwndActiveAfterMove2, Res->hwnd2);
    ok(!Res->bForeground2, "SetForegroundWindow succeeded for a window on another desktop\n");
    ok(Res->hwndActiveAfterForeground2 == Res->hwndActiveAfterMove2,
       "Active window after SetForegroundWindow is %p, expected %p\n",
       Res->hwndActiveAfterForeground2, Res->hwndActiveAfterMove2);
    ok(Res->bThread2Created, "CreateThread failed for the second thread\n");
    ok(!Res->bThread2Created || Res->bThread2, "The second thread did not finish\n");
    if (Res->bThread2)
    {
        trace("other thread: windows %p %p active %p; move %d active %p\n",
              Res->hwnd3, Res->hwnd4, Res->hwndActive2Before, Res->bMove3, Res->hwndActive2After);
        ok(Res->hwndActive2Before == Res->hwnd4, "Active window of the second thread is %p, expected %p\n",
           Res->hwndActive2Before, Res->hwnd4);
        ok(Res->bMove3, "SetWindowPos of the other thread's window failed\n");
        ok(Res->hwndActive2After == Res->hwnd4,
           "Active window of the second thread after SetWindowPos is %p, expected %p\n",
           Res->hwndActive2After, Res->hwnd4);
    }
    ok(hwndForegroundAfter == hwndMain, "Foreground window after the thread is %p, expected %p\n",
       hwndForegroundAfter, hwndMain);

Cleanup:
    if (bOnTestDesktop)
        ok(SwitchDesktop(hdeskInput), "SwitchDesktop(input) failed in cleanup (%lu)\n", GetLastError());
    if (hThread)
    {
        InterlockedExchange(&s_bAbort, TRUE);
        SetEvent(s_hGo);
        if (!bFinished && !WaitPumping(hThread, 120000))
        {
            /* Leave everything the thread may still use */
            skip("The thread is still running, resources are left in place\n");
            return;
        }
        CloseHandle(hThread);
    }
    if (Res->bThread2Created && !Res->bThread2)
    {
        skip("The second thread is still running, resources are left in place\n");
        return;
    }
    if (hwndMain)
        DestroyWindow(hwndMain);
    if (s_hdeskTest)
        CloseDesktop(s_hdeskTest);
    if (s_hReady)
        CloseHandle(s_hReady);
    if (s_hGo)
        CloseHandle(s_hGo);
    if (s_hReady2)
        CloseHandle(s_hReady2);
    if (s_hCheck2)
        CloseHandle(s_hCheck2);
    CloseDesktop(hdeskInput);
    UnregisterClassW(s_szClassName, GetModuleHandleW(NULL));
}
