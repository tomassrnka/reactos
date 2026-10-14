/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for SetCoalescableTimer (Windows 7 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows 10 22H2 (x64 and WOW64): the timer is the one of SetTimer; the tolerance has
 * to be no coalescing (0xFFFFFFFF) or at most 0x7FFFFFF5.
 */

#include "precomp.h"

typedef UINT_PTR (WINAPI *PFN_SetCoalescableTimer)(HWND, UINT_PTR, UINT, TIMERPROC, ULONG);

#define PENDING_ERROR 0xdeadbeef

static LONG g_Ticks;

static VOID CALLBACK TimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time)
{
    InterlockedIncrement(&g_Ticks);
}

/* One message at a time until the timer of the window has gone off Need times, or three seconds have passed */
static LONG Pump(HWND Window, UINT_PTR Id, LONG Need)
{
    DWORD End = GetTickCount() + 3000;
    LONG Count = 0;
    MSG Msg;

    while (Count < Need && (LONG)(End - GetTickCount()) > 0)
    {
        if (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
        {
            if (Msg.message == WM_TIMER && Msg.hwnd == Window && Msg.wParam == Id)
                Count++;
            DispatchMessageW(&Msg);
        }
        else
        {
            Sleep(5);
        }
    }
    return Count;
}

START_TEST(SetCoalescableTimer)
{
    HMODULE User32 = GetModuleHandleW(L"user32.dll");
    PFN_SetCoalescableTimer pSetCoalescableTimer;
    static const ULONG Good[] = { 0, 1, 0x100, 0x7ffffff5, 0xffffffff };
    static const ULONG Bad[] = { 0x7ffffff6, 0x80000000, 0xfffffffe };
    UINT_PTR Id;
    DWORD Error;
    HWND Window, BadWindow;
    ULONG i;

    pSetCoalescableTimer = (PFN_SetCoalescableTimer)GetProcAddress(User32, "SetCoalescableTimer");
    /* Windows Vista and older have none; ReactOS has it from export level 0x601, which a kernel32 with the Windows 7 functions shows */
    if (!pSetCoalescableTimer)
    {
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
           "SetCoalescableTimer is not exported\n");
        skip("SetCoalescableTimer is not exported\n");
        return;
    }

    Window = CreateWindowExW(0, L"STATIC", L"timer", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    ok(Window != NULL, "CreateWindowEx failed\n");
    if (!Window)
        return;

    /* the tolerance that is allowed gives the timer of SetTimer: the identifier that was asked for */
    for (i = 0; i < RTL_NUMBER_OF(Good); i++)
    {
        SetLastError(PENDING_ERROR);
        Id = pSetCoalescableTimer(Window, 5, 100, NULL, Good[i]);
        Error = GetLastError();
        ok(Id == 5, "tolerance %#lx: got %Iu\n", Good[i], Id);
        ok(Error == PENDING_ERROR, "tolerance %#lx: the last error became %lu\n", Good[i], Error);
        ok(KillTimer(Window, 5), "tolerance %#lx: the timer was not there\n", Good[i]);
    }
    for (i = 0; i < RTL_NUMBER_OF(Bad); i++)
    {
        SetLastError(PENDING_ERROR);
        Id = pSetCoalescableTimer(Window, 5, 100, NULL, Bad[i]);
        Error = GetLastError();
        ok(Id == 0, "tolerance %#lx: got %Iu\n", Bad[i], Id);
        ok(Error == ERROR_INVALID_PARAMETER, "tolerance %#lx: last error %lu\n", Bad[i], Error);
        ok(!KillTimer(Window, 5), "tolerance %#lx: a timer was made\n", Bad[i]);
    }

    /* the interval is not refused whatever it is */
    for (i = 0; i < 4; i++)
    {
        static const UINT Elapse[] = { 0, 9, 0x7fffffff, 0xffffffff };

        Id = pSetCoalescableTimer(Window, 6, Elapse[i], NULL, 0);
        ok(Id == 6, "elapse %#x: got %Iu\n", Elapse[i], Id);
        KillTimer(Window, 6);
    }

    /* a window that is not one */
    BadWindow = (HWND)CreateMenu();
    ok(BadWindow != NULL, "CreateMenu failed\n");
    SetLastError(PENDING_ERROR);
    Id = pSetCoalescableTimer(BadWindow, 7, 100, NULL, 0);
    Error = GetLastError();
    ok(Id == 0, "bad window: got %Iu\n", Id);
    ok(Error == ERROR_INVALID_WINDOW_HANDLE, "bad window: last error %lu\n", Error);

    /* a timer without a window gets an identifier of its own, and the same identifier replaces the timer */
    Id = pSetCoalescableTimer(NULL, 0, 100, NULL, 0);
    ok(Id != 0, "no window: no timer\n");
    ok(KillTimer(NULL, Id), "no window: the timer was not there\n");
    ok(pSetCoalescableTimer(Window, 10, 100, NULL, 0) == 10, "first timer\n");
    ok(pSetCoalescableTimer(Window, 10, 200, NULL, 0) == 10, "replacing timer\n");
    ok(KillTimer(Window, 10), "the timer was not there\n");
    SetLastError(PENDING_ERROR);
    ok(!KillTimer(Window, 10), "the timer is still there\n");

    /* the timers go off, with a tolerance too */
    ok(pSetCoalescableTimer(Window, 8, 50, NULL, 0) == 8, "window timer\n");
    ok(Pump(Window, 8, 2) >= 2, "the window timer did not go off\n");
    KillTimer(Window, 8);
    ok(pSetCoalescableTimer(Window, 9, 50, NULL, 0x100) == 9, "window timer with tolerance\n");
    ok(Pump(Window, 9, 2) >= 2, "the window timer with a tolerance did not go off\n");
    KillTimer(Window, 9);
    g_Ticks = 0;
    Id = pSetCoalescableTimer(NULL, 0, 50, TimerProc, 0);
    ok(Id != 0, "callback timer\n");
    {
        DWORD End = GetTickCount() + 3000;
        MSG Msg;

        while (g_Ticks < 2 && (LONG)(End - GetTickCount()) > 0)
        {
            if (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
                DispatchMessageW(&Msg);
            else
                Sleep(5);
        }
    }
    ok(g_Ticks >= 2, "the callback timer went off %ld times\n", g_Ticks);
    KillTimer(NULL, Id);

    DestroyMenu((HMENU)BadWindow);
    DestroyWindow(Window);
}
