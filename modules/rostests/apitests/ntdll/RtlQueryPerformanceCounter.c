/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for RtlQueryPerformanceCounter, RtlQueryPerformanceFrequency and RtlQueryUnbiasedInterruptTime (Windows 8+)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef BOOL (NTAPI *FN_RtlQueryPerformance)(PLARGE_INTEGER);
typedef BOOL (NTAPI *FN_RtlQueryUnbiasedInterruptTime)(PULONGLONG);

START_TEST(RtlQueryPerformanceCounter)
{
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    FN_RtlQueryPerformance pRtlQueryPerformanceCounter, pRtlQueryPerformanceFrequency;
    FN_RtlQueryUnbiasedInterruptTime pRtlQueryUnbiasedInterruptTime;
    LARGE_INTEGER Before, Counter, After, Frequency, Expected;
    ULONGLONG Time;
    BOOL Ret;

    pRtlQueryPerformanceCounter = (FN_RtlQueryPerformance)GetProcAddress(hNtdll, "RtlQueryPerformanceCounter");
    pRtlQueryPerformanceFrequency = (FN_RtlQueryPerformance)GetProcAddress(hNtdll, "RtlQueryPerformanceFrequency");
    pRtlQueryUnbiasedInterruptTime = (FN_RtlQueryUnbiasedInterruptTime)GetProcAddress(hNtdll, "RtlQueryUnbiasedInterruptTime");
    if (!pRtlQueryPerformanceCounter && is_reactos() && !GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcesses"))
    {
        skip("No NT 6.1 exports\n");
        return;
    }
    ok(pRtlQueryPerformanceCounter != NULL, "RtlQueryPerformanceCounter is not exported\n");
    ok(pRtlQueryPerformanceFrequency != NULL, "RtlQueryPerformanceFrequency is not exported\n");

    if (pRtlQueryPerformanceCounter)
    {
        ok(QueryPerformanceCounter(&Before), "QueryPerformanceCounter failed\n");
        Counter.QuadPart = 0;
        Ret = pRtlQueryPerformanceCounter(&Counter);
        ok(QueryPerformanceCounter(&After), "QueryPerformanceCounter failed\n");
        ok(Ret != FALSE, "Ret = %d\n", Ret);
        ok(Counter.QuadPart >= Before.QuadPart && Counter.QuadPart <= After.QuadPart,
           "Counter %I64d is not between %I64d and %I64d\n", Counter.QuadPart, Before.QuadPart, After.QuadPart);
    }

    if (pRtlQueryPerformanceFrequency)
    {
        ok(QueryPerformanceFrequency(&Expected), "QueryPerformanceFrequency failed\n");
        Frequency.QuadPart = 0;
        Ret = pRtlQueryPerformanceFrequency(&Frequency);
        ok(Ret != FALSE, "Ret = %d\n", Ret);
        ok(Frequency.QuadPart == Expected.QuadPart, "Frequency %I64d, expected %I64d\n", Frequency.QuadPart, Expected.QuadPart);
    }

    /* ntdll exports it from Windows 8 on (Windows 7 implements QueryUnbiasedInterruptTime in kernel32) */
    if (!pRtlQueryUnbiasedInterruptTime)
    {
        skip("RtlQueryUnbiasedInterruptTime is not exported\n");
    }
    else
    {
        Time = 0;
        Ret = pRtlQueryUnbiasedInterruptTime(&Time);
        ok(Ret != FALSE, "Ret = %d\n", Ret);
        ok(Time != 0, "Time is 0\n");

        SetLastError(0xdeadbeef);
        Ret = pRtlQueryUnbiasedInterruptTime(NULL);
        ok(Ret == FALSE, "Ret = %d\n", Ret);
        ok(GetLastError() == ERROR_INVALID_PARAMETER, "GetLastError() = %lu\n", GetLastError());
    }
}
