/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     timeBeginPeriod, timeEndPeriod and timeGetDevCaps against the kernel timer resolution
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <mmsystem.h>
#include <stdarg.h>

/* Periods are requests: the system timer resolution follows the finest period that any caller holds, a period is
   released by the same number of timeEndPeriod calls, and Sleep(1) and timeGetTime follow the resolution. The
   resolution is read back with NtQueryTimerResolution, in units of 100 ns. */

#define RES_DEFAULT_MIN     100000UL    /* a default resolution is 10 ms or coarser */
#define RES_HELD_MAX        20000UL     /* a 1 ms request gives 2 ms or finer on any clock */

typedef LONG (NTAPI *PNtQueryTimerResolution)(PULONG, PULONG, PULONG);

static PNtQueryTimerResolution pNtQueryTimerResolution;
static LARGE_INTEGER g_Freq;

static ULONG GetResolution(VOID)
{
    ULONG Min = 0, Max = 0, Actual = 0;

    pNtQueryTimerResolution(&Min, &Max, &Actual);
    return Actual;
}

static double NowUs(VOID)
{
    LARGE_INTEGER Count;

    QueryPerformanceCounter(&Count);
    return (double)Count.QuadPart * 1000000.0 / (double)g_Freq.QuadPart;
}

static int CompareDoubles(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;

    return x < y ? -1 : x > y;
}

/* Median duration of Sleep(1) in microseconds */
static double MedianSleepUs(UINT Samples)
{
    double Durations[101], Start;
    UINT i;

    if (Samples > 101)
        Samples = 101;
    for (i = 0; i < Samples; i++)
    {
        Start = NowUs();
        Sleep(1);
        Durations[i] = NowUs() - Start;
    }
    qsort(Durations, Samples, sizeof(Durations[0]), CompareDoubles);
    return Durations[Samples / 2];
}

/* Number of times timeGetTime changed value in 300 ms */
static ULONG CountClockChanges(VOID)
{
    DWORD Last = timeGetTime(), Now;
    ULONG Changes = 0;
    double Start = NowUs();

    while (NowUs() - Start < 300000.0)
    {
        Now = timeGetTime();
        if (Now != Last)
        {
            Changes++;
            Last = Now;
        }
    }
    return Changes;
}

/* Resolution and timing checks of a scenario record their first failure; a scenario that fails runs again, since
   another process may hold a period or the machine may be busy, and only the last failure is reported */
static char g_Failure[256];

static VOID Fail(const char *Format, ...)
{
    va_list Args;

    if (g_Failure[0])
        return;
    va_start(Args, Format);
    _vsnprintf(g_Failure, sizeof(g_Failure) - 1, Format, Args);
    va_end(Args);
}

#define CHECK(Condition, ...) do { if (!(Condition)) Fail(__VA_ARGS__); } while (0)

static VOID RunScenario(const char *Name, VOID (*Scenario)(VOID))
{
    int Attempt;
    BOOL Ran = FALSE;

    for (Attempt = 0; Attempt < 3; Attempt++)
    {
        if (Attempt != 0)
            Sleep(500);
        if (GetResolution() < RES_DEFAULT_MIN)
            continue;
        Ran = TRUE;
        g_Failure[0] = 0;
        Scenario();
        if (!g_Failure[0])
            break;
    }

    if (!Ran)
        skip("%s: the timer resolution is raised by another process\n", Name);
    else
        ok(!g_Failure[0], "%s: %s\n", Name, g_Failure);
}

static VOID CALLBACK EventProc(UINT Id, UINT Msg, DWORD_PTR User, DWORD_PTR Param1, DWORD_PTR Param2)
{
}

/* Static, so that a callback that is still running when the test ends cannot touch a dead stack */
static volatile LONG g_Fired[2];

static VOID CALLBACK CountProc(UINT Id, UINT Msg, DWORD_PTR User, DWORD_PTR Param1, DWORD_PTR Param2)
{
    InterlockedIncrement(&g_Fired[User]);
}

/* Events still fire on time: one shot after 50 ms, and a periodic one every 10 ms or the resolution */
static VOID TestEventFires(VOID)
{
    UINT IdOne, IdPeriodic;
    LONG OneShot, Periodic;

    g_Fired[0] = g_Fired[1] = 0;
    IdOne = timeSetEvent(50, 0, CountProc, 0, TIME_ONESHOT | TIME_KILL_SYNCHRONOUS);
    IdPeriodic = timeSetEvent(10, 0, CountProc, 1, TIME_PERIODIC | TIME_KILL_SYNCHRONOUS);
    ok(IdOne != 0 && IdPeriodic != 0, "timeSetEvent returned %u and %u\n", IdOne, IdPeriodic);
    Sleep(500);
    if (IdPeriodic != 0)
        timeKillEvent(IdPeriodic);
    if (IdOne != 0)
        timeKillEvent(IdOne);
    OneShot = g_Fired[0];
    Periodic = g_Fired[1];
    ok(OneShot == 1, "The one shot event fired %ld times\n", OneShot);
    ok(Periodic >= 10 && Periodic <= 60, "The periodic event fired %ld times in 500 ms\n", Periodic);
}

static VOID TestEventDelay(UINT Delay, BOOL Accepted)
{
    UINT Id = timeSetEvent(Delay, 0, EventProc, 0, TIME_ONESHOT);

    ok((Id != 0) == Accepted, "timeSetEvent with a delay of %u returned %u\n", Delay, Id);
    if (Id != 0)
        timeKillEvent(Id);
}

static VOID TestCaps(VOID)
{
    TIMECAPS Caps;
    MMRESULT Result;

    Result = timeGetDevCaps(NULL, sizeof(Caps));
    ok(Result == TIMERR_NOCANDO, "timeGetDevCaps(NULL) returned %u\n", Result);
    Result = timeGetDevCaps(&Caps, sizeof(Caps) - 1);
    ok(Result == TIMERR_NOCANDO, "timeGetDevCaps with a short size returned %u\n", Result);

    memset(&Caps, 0xAA, sizeof(Caps));
    Result = timeGetDevCaps(&Caps, sizeof(Caps));
    ok(Result == TIMERR_NOERROR, "timeGetDevCaps returned %u\n", Result);
    ok(Caps.wPeriodMin == 1, "wPeriodMin is %u\n", Caps.wPeriodMin);
    ok(Caps.wPeriodMax == 1000000, "wPeriodMax is %u\n", Caps.wPeriodMax);

    /* The same range holds for the delay of an event */
    TestEventDelay(0, FALSE);
    TestEventDelay(Caps.wPeriodMin, TRUE);
    TestEventDelay(65536, TRUE);
    TestEventDelay(Caps.wPeriodMax, TRUE);
    TestEventDelay(Caps.wPeriodMax + 1, FALSE);
}

static VOID TestReturnCodes(VOID)
{
    static const UINT Coarse[] = {16, 100, 1000, 65535, 65536, 1000000};
    MMRESULT Result;
    UINT Period, i;

    /* A period of zero is never valid */
    Result = timeBeginPeriod(0);
    ok(Result == TIMERR_NOCANDO, "timeBeginPeriod(0) returned %u\n", Result);
    Result = timeEndPeriod(0);
    ok(Result == TIMERR_NOCANDO, "timeEndPeriod(0) returned %u\n", Result);

    /* A period that was not begun cannot be ended */
    for (Period = 1; Period <= 15; Period++)
    {
        Result = timeEndPeriod(Period);
        ok(Result == TIMERR_NOCANDO, "timeEndPeriod(%u) without a begin returned %u\n", Period, Result);
    }

    /* Every begin is answered by exactly one end, and the end after that fails */
    for (Period = 1; Period <= 15; Period += 14)
    {
        Result = timeBeginPeriod(Period);
        ok(Result == TIMERR_NOERROR, "timeBeginPeriod(%u) returned %u\n", Period, Result);
        Result = timeBeginPeriod(Period);
        ok(Result == TIMERR_NOERROR, "second timeBeginPeriod(%u) returned %u\n", Period, Result);
        Result = timeEndPeriod(Period);
        ok(Result == TIMERR_NOERROR, "timeEndPeriod(%u) returned %u\n", Period, Result);
        Result = timeEndPeriod(Period);
        ok(Result == TIMERR_NOERROR, "second timeEndPeriod(%u) returned %u\n", Period, Result);
        Result = timeEndPeriod(Period);
        ok(Result == TIMERR_NOCANDO, "third timeEndPeriod(%u) returned %u\n", Period, Result);
    }

    /* A period that ends is not the period that began */
    Result = timeBeginPeriod(1);
    ok(Result == TIMERR_NOERROR, "timeBeginPeriod(1) returned %u\n", Result);
    Result = timeEndPeriod(2);
    ok(Result == TIMERR_NOCANDO, "timeEndPeriod(2) after timeBeginPeriod(1) returned %u\n", Result);
    Result = timeEndPeriod(1);
    ok(Result == TIMERR_NOERROR, "timeEndPeriod(1) returned %u\n", Result);

    /* Periods that are no finer than the default resolution are accepted and held by nobody */
    for (i = 0; i < sizeof(Coarse) / sizeof(Coarse[0]); i++)
    {
        Period = Coarse[i];
        Result = timeEndPeriod(Period);
        ok(Result == TIMERR_NOERROR, "timeEndPeriod(%u) without a begin returned %u\n", Period, Result);
        Result = timeBeginPeriod(Period);
        ok(Result == TIMERR_NOERROR, "timeBeginPeriod(%u) returned %u\n", Period, Result);
        Result = timeEndPeriod(Period);
        ok(Result == TIMERR_NOERROR, "timeEndPeriod(%u) returned %u\n", Period, Result);
    }
}

static BOOL Held(ULONG Resolution)
{
    return Resolution <= RES_HELD_MAX;
}

static VOID NestingScenario(VOID)
{
    ULONG Res1, Res5, Res;
    MMRESULT Result;

    /* The same period twice: two ends are needed */
    Result = timeBeginPeriod(1);
    CHECK(Result == TIMERR_NOERROR, "timeBeginPeriod(1) returned %u", Result);
    Res1 = GetResolution();
    CHECK(Held(Res1), "The resolution is %lu after timeBeginPeriod(1)", Res1);
    timeBeginPeriod(1);
    timeEndPeriod(1);
    Res = GetResolution();
    CHECK(Held(Res), "The resolution is %lu after one of two timeEndPeriod(1)", Res);
    timeEndPeriod(1);
    Res = GetResolution();
    CHECK(Res >= RES_DEFAULT_MIN, "The resolution is %lu after both timeEndPeriod(1)", Res);

    /* A finer and a coarser period: the finer one decides, and ending it hands the resolution to the coarser one */
    timeBeginPeriod(1);
    timeBeginPeriod(5);
    Res = GetResolution();
    CHECK(Res <= Res1, "The resolution is %lu with periods 1 and 5, %lu with 1", Res, Res1);
    timeEndPeriod(1);
    Res5 = GetResolution();
    CHECK(Res5 > Res1 && Res5 <= 50000, "The resolution is %lu with period 5 held, %lu with period 1", Res5, Res1);
    timeEndPeriod(5);
    Res = GetResolution();
    CHECK(Res >= RES_DEFAULT_MIN, "The resolution is %lu after both periods ended", Res);

    /* The order of begin does not matter */
    timeBeginPeriod(5);
    Res = GetResolution();
    CHECK(Res > Res1 && Res <= 50000, "The resolution is %lu with period 5 held", Res);
    timeBeginPeriod(1);
    Res = GetResolution();
    CHECK(Held(Res), "The resolution is %lu with periods 5 and 1 held", Res);
    timeEndPeriod(1);
    Res = GetResolution();
    CHECK(Res > Res1 && Res <= 50000, "The resolution is %lu with period 5 held again", Res);
    timeEndPeriod(5);

    /* A wrong end changes nothing */
    timeBeginPeriod(1);
    Result = timeEndPeriod(2);
    CHECK(Result == TIMERR_NOCANDO, "timeEndPeriod(2) returned %u", Result);
    Res = GetResolution();
    CHECK(Held(Res), "The resolution is %lu after a failed timeEndPeriod", Res);
    timeEndPeriod(1);
    Res = GetResolution();
    CHECK(Res >= RES_DEFAULT_MIN, "The resolution is %lu after timeEndPeriod(1)", Res);

    /* Periods that are no finer than the default hold nothing */
    timeBeginPeriod(20);
    Res = GetResolution();
    CHECK(Res >= RES_DEFAULT_MIN, "The resolution is %lu with period 20 held", Res);
    timeEndPeriod(20);
}

static VOID SleepAndClockScenario(VOID)
{
    double Coarse, Fine;
    ULONG CoarseChanges, FineChanges;

    Coarse = MedianSleepUs(60);
    CoarseChanges = CountClockChanges();

    CHECK(timeBeginPeriod(1) == TIMERR_NOERROR, "timeBeginPeriod(1) failed");
    Sleep(50);
    Fine = MedianSleepUs(100);
    FineChanges = CountClockChanges();
    CHECK(timeEndPeriod(1) == TIMERR_NOERROR, "timeEndPeriod(1) failed");

    trace("Sleep(1) median %.0f us default, %.0f us with period 1; timeGetTime changed %lu and %lu times in 300 ms\n",
          Coarse, Fine, CoarseChanges, FineChanges);

    CHECK(Coarse >= 8000.0, "Sleep(1) took %.0f us at the default resolution", Coarse);
    CHECK(Fine < 4000.0, "Sleep(1) took %.0f us with timeBeginPeriod(1)", Fine);
    CHECK(CoarseChanges <= 60, "timeGetTime changed %lu times in 300 ms at the default resolution", CoarseChanges);
    CHECK(FineChanges >= 100, "timeGetTime changed only %lu times in 300 ms with timeBeginPeriod(1)", FineChanges);

    Sleep(50);
    Coarse = MedianSleepUs(60);
    CHECK(Coarse >= 8000.0, "Sleep(1) took %.0f us after timeEndPeriod(1)", Coarse);
}

START_TEST(timeBeginPeriod)
{
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    pNtQueryTimerResolution = (PNtQueryTimerResolution)GetProcAddress(hNtdll, "NtQueryTimerResolution");
    if (!pNtQueryTimerResolution || !QueryPerformanceFrequency(&g_Freq))
    {
        skip("NtQueryTimerResolution or the performance counter is not available\n");
        return;
    }

    trace("The timer resolution at start is %lu\n", GetResolution());

    TestCaps();
    TestEventFires();
    TestReturnCodes();
    RunScenario("Nesting", NestingScenario);
    RunScenario("Sleep and timeGetTime", SleepAndClockScenario);
}
