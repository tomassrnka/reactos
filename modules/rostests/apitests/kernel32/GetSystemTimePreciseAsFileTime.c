/*
 * PROJECT:     ReactOS kernel32.dll API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for GetSystemTimePreciseAsFileTime
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

typedef VOID (WINAPI *PGetSystemTimePreciseAsFileTime)(LPFILETIME);

static ULONGLONG ToUll(const FILETIME *ft)
{
    return ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

/* One reading of the precise time between two readings of the system time */
static void Sample(PGetSystemTimePreciseAsFileTime pPrecise, ULONGLONG *pSysBefore, ULONGLONG *pNow, ULONGLONG *pSysAfter)
{
    FILETIME before, ft, after;

    GetSystemTimeAsFileTime(&before);
    pPrecise(&ft);
    GetSystemTimeAsFileTime(&after);
    *pSysBefore = ToUll(&before);
    *pNow = ToUll(&ft);
    *pSysAfter = ToUll(&after);
}

START_TEST(GetSystemTimePreciseAsFileTime)
{
    PGetSystemTimePreciseAsFileTime pPrecise;
    FILETIME ft;
    ULONGLONG last, now, sysBefore, sysAfter, prevSysBefore, prevSysAfter;
    LONGLONG diff;
    ULONG attempt, calls = 0, subTick = 0, backwards = 0, outside = 0;
    BOOL good;
    DWORD t0;

    pPrecise = (PGetSystemTimePreciseAsFileTime)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                               "GetSystemTimePreciseAsFileTime");
    if (!pPrecise)
    {
        if (is_reactos())
            ok(0, "GetSystemTimePreciseAsFileTime is not exported\n");
        else
            skip("GetSystemTimePreciseAsFileTime is not available\n");
        return;
    }

    /* It writes the time, not before the system time read before the call and at most 100 ms after the one read
       after it (the check of Wine's kernel32 time test is 100 ms from the system time); a few tries, as a thread can
       lose the processor between the readings */
    for (attempt = 0, good = FALSE; attempt < 5 && !good; attempt++)
    {
        Sample(pPrecise, &sysBefore, &now, &sysAfter);
        good = now != 0 && now >= sysBefore && now <= sysAfter + 1000000;
    }
    ok(good, "Precise time %I64u, system time before the call %I64u and after it %I64u\n", now, sysBefore, sysAfter);

    /* The time advances in steps below 1 ms (Wine's second check), seen at least once in a few tries */
    for (attempt = 0, good = FALSE; attempt < 20 && !good; attempt++)
    {
        pPrecise(&ft);
        last = ToUll(&ft);
        t0 = GetTickCount();
        do
        {
            pPrecise(&ft);
            now = ToUll(&ft);
        } while (now == last && GetTickCount() - t0 < 50);
        diff = (LONGLONG)(now - last);
        good = diff > 0 && diff < 10000;
    }
    ok(good, "Precise time advanced by %I64d (100 ns units) at the last try, expected less than 1 ms\n", diff);

    /* Windows of 100 ms: never backwards, never outside the system time readings around the call, and the time
       changes between two calls whose four system time readings are all the same: only the performance counter can
       have done that. A window in which the thread lost the processor shows too few changes, so up to
       twenty windows are taken; a failure of the first two checks counts at once */
    for (attempt = 0; attempt < 20 && subTick < 10; attempt++)
    {
        Sample(pPrecise, &prevSysBefore, &last, &prevSysAfter);
        t0 = GetTickCount();
        do
        {
            Sample(pPrecise, &sysBefore, &now, &sysAfter);
            calls++;
            if (now < last)
                backwards++;
            else if (now > last && prevSysBefore == prevSysAfter && prevSysAfter == sysBefore && sysBefore == sysAfter)
                subTick++;
            if (now < sysBefore || now > sysAfter + 1000000)
                outside++;
            last = now;
            prevSysBefore = sysBefore;
            prevSysAfter = sysAfter;
        } while (GetTickCount() - t0 < 100);
    }

    ok(backwards == 0, "Precise time went backwards %lu times in %lu calls\n", backwards, calls);
    ok(outside == 0, "Precise time was outside the window of the system time %lu times in %lu calls\n", outside, calls);
    ok(subTick >= 10, "Precise time changed only %lu times between two clock ticks in %lu calls\n", subTick, calls);
}
