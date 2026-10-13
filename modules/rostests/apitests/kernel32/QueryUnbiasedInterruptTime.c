/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for QueryUnbiasedInterruptTime
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef BOOL (WINAPI *FN_QueryUnbiasedInterruptTime)(PULONGLONG);
typedef ULONGLONG (WINAPI *FN_GetTickCount64)(VOID);

START_TEST(QueryUnbiasedInterruptTime)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    FN_QueryUnbiasedInterruptTime pQueryUnbiasedInterruptTime;
    FN_GetTickCount64 pGetTickCount64;
    ULONGLONG Time1, Time2, Ticks;
    BOOL Ret;
    int i;

    pQueryUnbiasedInterruptTime = (FN_QueryUnbiasedInterruptTime)GetProcAddress(hKernel32, "QueryUnbiasedInterruptTime");
    ok(pQueryUnbiasedInterruptTime != NULL, "QueryUnbiasedInterruptTime is not exported\n");
    if (!pQueryUnbiasedInterruptTime)
        return;
    pGetTickCount64 = (FN_GetTickCount64)GetProcAddress(hKernel32, "GetTickCount64");

    SetLastError(0xdeadbeef);
    Ret = pQueryUnbiasedInterruptTime(NULL);
    ok(Ret == FALSE, "Ret = %d\n", Ret);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "GetLastError() = %lu\n", GetLastError());

    Time1 = 0;
    SetLastError(0xdeadbeef);
    Ret = pQueryUnbiasedInterruptTime(&Time1);
    ok(Ret == TRUE, "Ret = %d\n", Ret);
    ok(GetLastError() == 0xdeadbeef, "GetLastError() = %lu\n", GetLastError());
    ok(Time1 != 0, "Time1 is 0\n");

    /* The unbiased time leaves out sleep, so it can never run ahead of the tick count (100 ns units vs ms) */
    if (pGetTickCount64)
    {
        Ticks = pGetTickCount64();
        ok(Time1 <= (Ticks + 1000) * 10000, "Time1 %I64u is ahead of the tick count %I64u ms\n", Time1, Ticks);
    }

    Sleep(100);
    Ret = pQueryUnbiasedInterruptTime(&Time2);
    ok(Ret == TRUE, "Ret = %d\n", Ret);
    ok(Time2 >= Time1 + 80 * 10000, "Time advanced by only %I64u\n", Time2 - Time1);
    ok(Time2 < Time1 + 10000ULL * 10000, "Time advanced by %I64u\n", Time2 - Time1);

    /* Never goes backwards */
    for (i = 0; i < 100000; i++)
    {
        Time1 = Time2;
        pQueryUnbiasedInterruptTime(&Time2);
        if (Time2 < Time1)
            break;
    }
    ok(i == 100000, "Time went backwards at %d: %I64u -> %I64u\n", i, Time1, Time2);
}
