/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for BeginPanningFeedback, UpdatePanningFeedback and EndPanningFeedback
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <windows.h>

typedef BOOL (WINAPI *FN_BeginPanningFeedback)(HWND);
typedef BOOL (WINAPI *FN_UpdatePanningFeedback)(HWND, LONG, LONG, BOOL);
typedef BOOL (WINAPI *FN_EndPanningFeedback)(HWND, BOOL);

START_TEST(PanningFeedback)
{
    HMODULE hUxtheme = LoadLibraryW(L"uxtheme.dll");
    FN_BeginPanningFeedback pBegin;
    FN_UpdatePanningFeedback pUpdate;
    FN_EndPanningFeedback pEnd;
    HWND hwnd;
    RECT Before, Rect;
    BOOL Ret;

    pBegin = (FN_BeginPanningFeedback)GetProcAddress(hUxtheme, "BeginPanningFeedback");
    pUpdate = (FN_UpdatePanningFeedback)GetProcAddress(hUxtheme, "UpdatePanningFeedback");
    pEnd = (FN_EndPanningFeedback)GetProcAddress(hUxtheme, "EndPanningFeedback");
    if (!pBegin && is_reactos() && !GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcesses"))
    {
        skip("No NT 6.1 exports\n");
        return;
    }
    ok(pBegin && pUpdate && pEnd, "Panning feedback functions are not exported\n");
    if (!pBegin || !pUpdate || !pEnd)
        return;

    hwnd = CreateWindowExW(0, L"STATIC", L"PanningFeedback", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 100, 300, 200, NULL, NULL, NULL, NULL);
    ok(hwnd != NULL, "CreateWindowExW failed: %lu\n", GetLastError());
    if (!hwnd)
        return;
    GetWindowRect(hwnd, &Before);

    Ret = pBegin(hwnd);
    if (is_reactos())
    {
        /* No visual feedback: an honest failure, the window stays where it is */
        ok(Ret == FALSE && GetLastError() == ERROR_NOT_SUPPORTED, "BeginPanningFeedback: %d, %lu\n", Ret, GetLastError());
        Ret = pUpdate(hwnd, 10, 20, FALSE);
        ok(Ret == FALSE && GetLastError() == ERROR_NOT_SUPPORTED, "UpdatePanningFeedback: %d, %lu\n", Ret, GetLastError());
        Ret = pEnd(hwnd, TRUE);
        ok(Ret == FALSE && GetLastError() == ERROR_NOT_SUPPORTED, "EndPanningFeedback: %d, %lu\n", Ret, GetLastError());
    }
    else
    {
        ok(Ret == TRUE, "BeginPanningFeedback failed: %lu\n", GetLastError());
        Ret = pUpdate(hwnd, 10, 20, FALSE);
        ok(Ret == TRUE, "UpdatePanningFeedback failed: %lu\n", GetLastError());
        Ret = pEnd(hwnd, FALSE);
        ok(Ret == TRUE, "EndPanningFeedback failed: %lu\n", GetLastError());
    }
    GetWindowRect(hwnd, &Rect);
    ok(EqualRect(&Rect, &Before), "After the feedback the window is at (%ld,%ld), started at (%ld,%ld)\n",
       Rect.left, Rect.top, Before.left, Before.top);

    DestroyWindow(hwnd);

    /* A window that does not exist (Windows 7 does not check it) */
    SetLastError(0xdeadbeef);
    Ret = pBegin(hwnd);
    if (is_reactos() || LOBYTE(LOWORD(GetVersion())) > 6 || HIBYTE(LOWORD(GetVersion())) > 1)
        ok(Ret == FALSE, "BeginPanningFeedback on a destroyed window succeeded\n");
    if (is_reactos())
        ok(GetLastError() == ERROR_INVALID_WINDOW_HANDLE, "GetLastError() = %lu\n", GetLastError());

    FreeLibrary(hUxtheme);
}
