/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     timeGetTime while other threads call timeBeginPeriod and timeEndPeriod
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <mmsystem.h>
#include <pseh/pseh2.h>

/* timeGetTime reads the frequency it divides by, and timeEndPeriod clears it. If the two accesses are
   separate, a thread that checked the frequency divides by zero when another one ends the period in
   between. The readers start before the first timeBeginPeriod, so the first use is raced too. The window
   is a few instructions wide: the test is statistical, and on one processor it rarely hits it. */

#define STATUS_DIVIDE_BY_ZERO_CODE 0xC0000094UL
#define HAMMER_MS       3000
#define MAX_READERS     16

static volatile LONG g_Stop;
static volatile LONG g_Started;
static volatile LONG g_DivZero, g_OtherFaults, g_Calls;

static DWORD WINAPI ReaderThread(LPVOID Parameter)
{
    LONG Calls = 0;

    InterlockedIncrement(&g_Started);
    while (!g_Stop)
    {
        _SEH2_TRY
        {
            timeGetTime();
            Calls++;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            if (_SEH2_GetExceptionCode() == STATUS_DIVIDE_BY_ZERO_CODE)
                InterlockedIncrement(&g_DivZero);
            else
                InterlockedIncrement(&g_OtherFaults);
        }
        _SEH2_END;
    }
    InterlockedExchangeAdd(&g_Calls, Calls);
    return 0;
}

static VOID TestIncreases(VOID)
{
    DWORD Before, After;

    Before = timeGetTime();
    Sleep(200);
    After = timeGetTime();
    ok(After - Before >= 150, "timeGetTime advanced by %lu ms during a 200 ms sleep\n", After - Before);
}

START_TEST(timeGetTime)
{
    HANDLE hThreads[MAX_READERS];
    SYSTEM_INFO si;
    DWORD Count, Created = 0, i, Start;
    ULONG Rounds = 0;
    MMRESULT mr;

    GetSystemInfo(&si);
    Count = min(max(si.dwNumberOfProcessors * 2, 4), MAX_READERS);

    /* Plain tick mode, before anybody asked for a period */
    TestIncreases();

    for (i = 0; i < Count; i++)
    {
        hThreads[i] = CreateThread(NULL, 0, ReaderThread, NULL, 0, NULL);
        if (!hThreads[i])
        {
            ok(0, "CreateThread failed with %lu\n", GetLastError());
            break;
        }
        Created++;
    }

    if (Created != 0)
    {
        while (g_Started != (LONG)Created)
            Sleep(1);

        Start = GetTickCount();
        while (GetTickCount() - Start < HAMMER_MS)
        {
            mr = timeBeginPeriod(1);
            if (mr != TIMERR_NOERROR)
            {
                ok(0, "timeBeginPeriod(1) returned %u\n", mr);
                break;
            }
            mr = timeEndPeriod(1);
            if (mr != TIMERR_NOERROR)
            {
                ok(0, "timeEndPeriod(1) returned %u\n", mr);
                break;
            }
            Rounds++;
        }
    }

    g_Stop = 1;
    if (Created != 0)
    {
        ok(WaitForMultipleObjects(Created, hThreads, TRUE, 30000) == WAIT_OBJECT_0, "Readers did not stop\n");
        for (i = 0; i < Created; i++)
            CloseHandle(hThreads[i]);
    }

    trace("%lu begin/end rounds, %ld timeGetTime calls from %lu threads\n", Rounds, g_Calls, Created);
    ok(Rounds >= 1000, "Only %lu begin/end rounds ran\n", Rounds);
    ok(g_Calls >= 1000, "The readers made only %ld calls\n", g_Calls);
    ok(g_DivZero == 0, "timeGetTime divided by zero %ld times\n", g_DivZero);
    ok(g_OtherFaults == 0, "timeGetTime raised %ld other exceptions\n", g_OtherFaults);

    /* High resolution mode, as timeBeginPeriod(1) selects */
    ok(timeBeginPeriod(1) == TIMERR_NOERROR, "timeBeginPeriod(1) failed\n");
    TestIncreases();
    ok(timeEndPeriod(1) == TIMERR_NOERROR, "timeEndPeriod(1) failed\n");
    TestIncreases();
}
