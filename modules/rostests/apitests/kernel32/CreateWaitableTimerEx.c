/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for CreateWaitableTimerExA/W
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef CREATE_WAITABLE_TIMER_MANUAL_RESET
#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#endif

typedef HANDLE WINAPI FN_CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
typedef HANDLE WINAPI FN_CreateWaitableTimerExA(LPSECURITY_ATTRIBUTES, LPCSTR, DWORD, DWORD);

static FN_CreateWaitableTimerExW *pCreateWaitableTimerExW;
static FN_CreateWaitableTimerExA *pCreateWaitableTimerExA;

static
VOID
TestReset(_In_ HANDLE Timer, _In_ BOOL ManualReset)
{
    LARGE_INTEGER Due;

    Due.QuadPart = -1;
    ok(SetWaitableTimer(Timer, &Due, 0, NULL, NULL, FALSE), "SetWaitableTimer failed: %lu\n", GetLastError());
    ok(WaitForSingleObject(Timer, 5000) == WAIT_OBJECT_0, "Timer did not fire\n");

    /* A satisfied wait resets a synchronization timer only */
    ok(WaitForSingleObject(Timer, 0) == (ManualReset ? WAIT_OBJECT_0 : WAIT_TIMEOUT),
       "Unexpected state, manual reset %d\n", ManualReset);
}

START_TEST(CreateWaitableTimerEx)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    LARGE_INTEGER Due;
    HANDLE Timer, Timer2;

    pCreateWaitableTimerExW = (FN_CreateWaitableTimerExW *)GetProcAddress(Kernel32, "CreateWaitableTimerExW");
    pCreateWaitableTimerExA = (FN_CreateWaitableTimerExA *)GetProcAddress(Kernel32, "CreateWaitableTimerExA");
    if (!pCreateWaitableTimerExW || !pCreateWaitableTimerExA)
    {
        skip("CreateWaitableTimerExA/W are not available\n");
        return;
    }

    Timer = pCreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    ok(Timer != NULL, "CreateWaitableTimerExW failed: %lu\n", GetLastError());
    if (Timer)
    {
        TestReset(Timer, FALSE);
        CloseHandle(Timer);
    }

    Timer = pCreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_MANUAL_RESET, TIMER_ALL_ACCESS);
    ok(Timer != NULL, "CreateWaitableTimerExW failed: %lu\n", GetLastError());
    if (Timer)
    {
        TestReset(Timer, TRUE);
        CloseHandle(Timer);
    }

    SetLastError(0xdeadbeef);
    Timer = pCreateWaitableTimerExW(NULL, NULL, 0x80000000, TIMER_ALL_ACCESS);
    ok(Timer == NULL && GetLastError() == ERROR_INVALID_PARAMETER,
       "Timer %p, error %lu\n", Timer, GetLastError());
    if (Timer) CloseHandle(Timer);

    /* The requested access is the access of the handle */
    Timer = pCreateWaitableTimerExW(NULL, NULL, 0, SYNCHRONIZE);
    ok(Timer != NULL, "CreateWaitableTimerExW failed: %lu\n", GetLastError());
    if (Timer)
    {
        Due.QuadPart = -1;
        SetLastError(0xdeadbeef);
        ok(!SetWaitableTimer(Timer, &Due, 0, NULL, NULL, FALSE) && GetLastError() == ERROR_ACCESS_DENIED,
           "SetWaitableTimer error %lu\n", GetLastError());
        CloseHandle(Timer);
    }

    /* The ANSI version creates the named object */
    Timer = pCreateWaitableTimerExA(NULL, "rostest_timer_ex", 0, TIMER_ALL_ACCESS);
    ok(Timer != NULL, "CreateWaitableTimerExA failed: %lu\n", GetLastError());
    Timer2 = OpenWaitableTimerW(TIMER_ALL_ACCESS, FALSE, L"rostest_timer_ex");
    ok(Timer2 != NULL, "OpenWaitableTimerW failed: %lu\n", GetLastError());
    if (Timer2) CloseHandle(Timer2);
    if (Timer) CloseHandle(Timer);
}
