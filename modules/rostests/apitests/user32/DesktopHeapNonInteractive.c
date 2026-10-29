/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     The heap of a desktop of a non-interactive window station holds 900 windows
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The desktop of a service (Service-0x0-3e7$\Default) is on a non-interactive window station and
 * its heap is shared by every process that runs there: 768 KB on Windows x64, 512 KB on x86.
 * With 128 KB the processes of the Chrome updater exhausted it and the next process failed to
 * initialize (STATUS_DLL_INIT_FAILED, 0xC0000142).
 */

#include "precomp.h"

#define WINDOW_COUNT 900

static const WCHAR s_szClassName[] = L"DesktopHeapNonInteractiveTest";

typedef struct _HEAP_THREAD
{
    HDESK hdesk;
    BOOL bSetThreadDesktop;
    UINT cCreated;
    DWORD dwError;
} HEAP_THREAD;

static DWORD WINAPI HeapThreadProc(LPVOID pParam)
{
    HEAP_THREAD *pThread = pParam;
    HWND ahwnd[WINDOW_COUNT];
    UINT i;

    pThread->bSetThreadDesktop = SetThreadDesktop(pThread->hdesk);
    if (!pThread->bSetThreadDesktop)
    {
        pThread->dwError = GetLastError();
        return 0;
    }

    for (i = 0; i < WINDOW_COUNT; i++)
    {
        SetLastError(0xDEADBEEF);
        ahwnd[i] = CreateWindowExW(0, s_szClassName, NULL, WS_POPUP, 0, 0, 10, 10, HWND_MESSAGE,
                                   NULL, GetModuleHandleW(NULL), NULL);
        if (!ahwnd[i])
        {
            pThread->dwError = GetLastError();
            break;
        }
    }
    pThread->cCreated = i;

    while (i-- > 0)
        DestroyWindow(ahwnd[i]);
    return 0;
}

START_TEST(DesktopHeapNonInteractive)
{
    WNDCLASSW wc = { 0 };
    HWINSTA hwinstaOld, hwinsta;
    HDESK hdesk;
    HANDLE hThread;
    HEAP_THREAD thread = { 0 };
    WCHAR szWinsta[64];

    hwinstaOld = GetProcessWindowStation();
    wsprintfW(szWinsta, L"DesktopHeapNoIo%lu", GetCurrentProcessId());
    hwinsta = CreateWindowStationW(szWinsta, 0, WINSTA_ALL_ACCESS, NULL);
    if (!hwinsta)
    {
        skip("CreateWindowStationW failed with %lu\n", GetLastError());
        return;
    }

    if (!SetProcessWindowStation(hwinsta))
    {
        skip("SetProcessWindowStation failed with %lu\n", GetLastError());
        CloseWindowStation(hwinsta);
        return;
    }

    hdesk = CreateDesktopW(L"Default", NULL, NULL, 0, GENERIC_ALL, NULL);
    ok(hdesk != NULL, "CreateDesktopW failed with %lu\n", GetLastError());

    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = s_szClassName;
    ok(RegisterClassW(&wc) != 0, "RegisterClassW failed with %lu\n", GetLastError());

    if (hdesk)
    {
        thread.hdesk = hdesk;
        hThread = CreateThread(NULL, 0, HeapThreadProc, &thread, 0, NULL);
        ok(hThread != NULL, "CreateThread failed with %lu\n", GetLastError());
        if (hThread)
        {
            WaitForSingleObject(hThread, INFINITE);
            CloseHandle(hThread);
        }
        ok(thread.bSetThreadDesktop, "SetThreadDesktop failed with %lu\n", thread.dwError);
        if (thread.bSetThreadDesktop)
            ok(thread.cCreated == WINDOW_COUNT, "Created %u of %u windows, error %lu\n",
               thread.cCreated, WINDOW_COUNT, thread.dwError);
    }

    UnregisterClassW(s_szClassName, GetModuleHandleW(NULL));
    SetProcessWindowStation(hwinstaOld);
    if (hdesk)
        CloseDesktop(hdesk);
    CloseWindowStation(hwinsta);
}
