/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for local time across time zone changes and daylight saving transitions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define TICKS_PER_SECOND 10000000LL
#define TICKS_PER_MINUTE (60 * TICKS_PER_SECOND)

/* Allowed difference between the UTC and the tick count elapsed times */
#define MAX_UTC_DRIFT (2 * TICKS_PER_SECOND)

/* The transitions are set this far ahead of the clock */
#define LEAD_SECONDS 12

static
LONGLONG
GetUtcNow(VOID)
{
    FILETIME FileTime;
    ULARGE_INTEGER Value;

    GetSystemTimeAsFileTime(&FileTime);
    Value.LowPart = FileTime.dwLowDateTime;
    Value.HighPart = FileTime.dwHighDateTime;
    return (LONGLONG)Value.QuadPart;
}

/* The bias in minutes that GetLocalTime applies now (UTC - local) */
static
LONG
GetLocalBias(VOID)
{
    SYSTEMTIME LocalTime;
    FILETIME FileTime;
    ULARGE_INTEGER Local;
    LONGLONG Utc, Diff;

    Utc = GetUtcNow();
    GetLocalTime(&LocalTime);
    if (!SystemTimeToFileTime(&LocalTime, &FileTime))
        return MAXLONG;
    Local.LowPart = FileTime.dwLowDateTime;
    Local.HighPart = FileTime.dwHighDateTime;

    Diff = Utc - (LONGLONG)Local.QuadPart;
    if (Diff >= 0)
        return (LONG)((Diff + TICKS_PER_MINUTE / 2) / TICKS_PER_MINUTE);
    return -(LONG)((-Diff + TICKS_PER_MINUTE / 2) / TICKS_PER_MINUTE);
}

static
VOID
EnablePrivilege(PCWSTR Name)
{
    HANDLE Token;
    TOKEN_PRIVILEGES Privileges;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token))
        return;
    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(NULL, Name, &Privileges.Privileges[0].Luid))
        AdjustTokenPrivileges(Token, FALSE, &Privileges, 0, NULL, NULL);
    CloseHandle(Token);
}

static
VOID
TimeToSystemTime(LONGLONG Time, PSYSTEMTIME SystemTime)
{
    FILETIME FileTime;

    FileTime.dwLowDateTime = (DWORD)Time;
    FileTime.dwHighDateTime = (DWORD)(Time >> 32);
    FileTimeToSystemTime(&FileTime, SystemTime);
}

/* Fill a recurring cutover date that matches the given local time */
static
VOID
MakeCutover(LONGLONG LocalTime, PSYSTEMTIME Cutover)
{
    SYSTEMTIME Fields;

    TimeToSystemTime(LocalTime, &Fields);
    ZeroMemory(Cutover, sizeof(*Cutover));
    Cutover->wMonth = Fields.wMonth;
    Cutover->wDayOfWeek = Fields.wDayOfWeek;
    /* Week of the month; 5 means the last one, which it then is */
    Cutover->wDay = (Fields.wDay - 1) / 7 + 1;
    Cutover->wHour = Fields.wHour;
    Cutover->wMinute = Fields.wMinute;
    Cutover->wSecond = Fields.wSecond;
}

/* A cutover about half a year away from the given local time */
static
VOID
MakeFarCutover(LONGLONG LocalTime, PSYSTEMTIME Cutover)
{
    SYSTEMTIME Fields;

    TimeToSystemTime(LocalTime, &Fields);
    ZeroMemory(Cutover, sizeof(*Cutover));
    Cutover->wMonth = (Fields.wMonth + 5) % 12 + 1;
    Cutover->wDayOfWeek = 0;
    Cutover->wDay = 1;
    Cutover->wHour = 2;
}

static
BOOL
SetZone(PTIME_ZONE_INFORMATION Tzi, PLONGLONG UtcJump)
{
    LONGLONG Utc0, Utc1;
    DWORD Tick0, Tick1;
    BOOL Ret;

    Tick0 = GetTickCount();
    Utc0 = GetUtcNow();
    Ret = SetTimeZoneInformation(Tzi);
    Utc1 = GetUtcNow();
    Tick1 = GetTickCount();

    *UtcJump = (Utc1 - Utc0) - (LONGLONG)(Tick1 - Tick0) * 10000;
    return Ret;
}

static
VOID
TestZoneChange(LONG Bias)
{
    TIME_ZONE_INFORMATION Tzi, Current;
    LONGLONG UtcJump;
    DWORD Id;

    ZeroMemory(&Tzi, sizeof(Tzi));
    Tzi.Bias = Bias;
    StringCchCopyW(Tzi.StandardName, _countof(Tzi.StandardName), L"Test Standard Time");
    StringCchCopyW(Tzi.DaylightName, _countof(Tzi.DaylightName), L"Test Standard Time");

    ok(SetZone(&Tzi, &UtcJump), "SetTimeZoneInformation failed, error %lu\n", GetLastError());
    ok(UtcJump > -MAX_UTC_DRIFT && UtcJump < MAX_UTC_DRIFT,
       "Bias %ld: UTC jumped by %I64d ms when the zone changed\n", Bias, UtcJump / 10000);

    Id = GetTimeZoneInformation(&Current);
    ok(Id == TIME_ZONE_ID_UNKNOWN, "Bias %ld: id %lu\n", Bias, Id);
    ok(Current.Bias == Bias, "Bias %ld: GetTimeZoneInformation bias %ld\n", Bias, Current.Bias);
    ok(GetLocalBias() == Bias, "Bias %ld: local time bias %ld\n", Bias, GetLocalBias());
}

/*
 * Set a zone whose next transition is 12 to 42 seconds ahead, then check that
 * local time switches at the transition while UTC keeps running.
 */
static
VOID
TestTransition(BOOL ToDaylight)
{
    TIME_ZONE_INFORMATION Tzi, Current;
    LONGLONG Now, Cutover, UtcJump, Utc0, Utc1;
    DWORD Tick0, Tick1;
    LONG BiasBefore, BiasAfter, Bias;
    DWORD IdBefore, IdAfter, Id;
    PCSTR Name = ToDaylight ? "to daylight" : "to standard";

    ZeroMemory(&Tzi, sizeof(Tzi));
    Tzi.Bias = 300;
    Tzi.StandardBias = 0;
    Tzi.DaylightBias = -60;
    StringCchCopyW(Tzi.StandardName, _countof(Tzi.StandardName), L"Test Standard Time");
    StringCchCopyW(Tzi.DaylightName, _countof(Tzi.DaylightName), L"Test Daylight Time");

    /*
     * Whole seconds, so that the cutover fields hold the instant exactly, and
     * at least half a minute into the minute, so that a check that ignores
     * the seconds of the cutover sees the transition too early.
     */
    Now = GetUtcNow();
    Cutover = (Now / TICKS_PER_SECOND + LEAD_SECONDS) * TICKS_PER_SECOND;
    if ((Cutover / TICKS_PER_SECOND) % 60 < 30)
        Cutover += (30 - (Cutover / TICKS_PER_SECOND) % 60) * TICKS_PER_SECOND;

    if (ToDaylight)
    {
        /* The daylight date is given in standard local time */
        MakeCutover(Cutover - (Tzi.Bias + Tzi.StandardBias) * TICKS_PER_MINUTE, &Tzi.DaylightDate);
        MakeFarCutover(Cutover - Tzi.Bias * TICKS_PER_MINUTE, &Tzi.StandardDate);
        IdBefore = TIME_ZONE_ID_STANDARD;
        IdAfter = TIME_ZONE_ID_DAYLIGHT;
        BiasBefore = Tzi.Bias + Tzi.StandardBias;
        BiasAfter = Tzi.Bias + Tzi.DaylightBias;
    }
    else
    {
        /* The standard date is given in daylight local time */
        MakeCutover(Cutover - (Tzi.Bias + Tzi.DaylightBias) * TICKS_PER_MINUTE, &Tzi.StandardDate);
        MakeFarCutover(Cutover - Tzi.Bias * TICKS_PER_MINUTE, &Tzi.DaylightDate);
        IdBefore = TIME_ZONE_ID_DAYLIGHT;
        IdAfter = TIME_ZONE_ID_STANDARD;
        BiasBefore = Tzi.Bias + Tzi.DaylightBias;
        BiasAfter = Tzi.Bias + Tzi.StandardBias;
    }

    ok(SetZone(&Tzi, &UtcJump), "%s: SetTimeZoneInformation failed, error %lu\n", Name, GetLastError());
    ok(UtcJump > -MAX_UTC_DRIFT && UtcJump < MAX_UTC_DRIFT,
       "%s: UTC jumped by %I64d ms when the zone changed\n", Name, UtcJump / 10000);

    Tick0 = GetTickCount();
    Utc0 = GetUtcNow();
    if (Utc0 >= Cutover - 2 * TICKS_PER_SECOND)
    {
        skip("%s: the transition is too close (%I64d ms)\n", Name, (Cutover - Utc0) / 10000);
        return;
    }

    Id = GetTimeZoneInformation(&Current);
    Bias = GetLocalBias();
    ok(Id == IdBefore, "%s: id %lu after setting the zone\n", Name, Id);
    ok(Bias == BiasBefore, "%s: local time bias %ld after setting the zone, expected %ld\n", Name, Bias, BiasBefore);

    /* Shortly before the transition, in the same minute */
    while (GetUtcNow() < Cutover - 2 * TICKS_PER_SECOND &&
           GetTickCount() - Tick0 < (LEAD_SECONDS + 60) * 1000)
    {
        Sleep(100);
    }

    if (GetUtcNow() < Cutover - TICKS_PER_SECOND)
    {
        Id = GetTimeZoneInformation(&Current);
        Bias = GetLocalBias();
        ok(Id == IdBefore, "%s: id %lu just before the transition\n", Name, Id);
        ok(Bias == BiasBefore, "%s: local time bias %ld just before the transition, expected %ld\n", Name, Bias, BiasBefore);
    }
    else
    {
        skip("%s: woke up too late for the check just before the transition\n", Name);
    }

    /* Wait until shortly after the transition */
    while (GetUtcNow() < Cutover + 1500 * 10000 &&
           GetTickCount() - Tick0 < (LEAD_SECONDS + 60) * 1000)
    {
        Sleep(100);
    }

    Utc1 = GetUtcNow();
    Tick1 = GetTickCount();
    UtcJump = (Utc1 - Utc0) - (LONGLONG)(Tick1 - Tick0) * 10000;
    ok(UtcJump > -MAX_UTC_DRIFT && UtcJump < MAX_UTC_DRIFT,
       "%s: UTC moved by %I64d ms more than the tick count across the transition\n", Name, UtcJump / 10000);
    ok(Utc1 >= Cutover, "%s: the clock did not reach the transition (%I64d ms short)\n", Name, (Cutover - Utc1) / 10000);

    Id = GetTimeZoneInformation(&Current);
    Bias = GetLocalBias();
    ok(Id == IdAfter, "%s: id %lu after the transition\n", Name, Id);
    ok(Bias == BiasAfter, "%s: local time bias %ld after the transition, expected %ld\n", Name, Bias, BiasAfter);
}

static
VOID
CheckLocal(PTIME_ZONE_INFORMATION Tzi, WORD Month, WORD Day, WORD Hour, WORD Minute, WORD Second, WORD Milliseconds,
           WORD LocalHour, WORD LocalMinute, WORD LocalSecond, WORD LocalMilliseconds)
{
    SYSTEMTIME Utc = { 2026, Month, 0, Day, Hour, Minute, Second, Milliseconds };
    SYSTEMTIME Local;
    BOOL Ret;

    ZeroMemory(&Local, sizeof(Local));
    Ret = SystemTimeToTzSpecificLocalTime(Tzi, &Utc, &Local);
    ok(Ret, "SystemTimeToTzSpecificLocalTime failed, error %lu\n", GetLastError());
    ok(Local.wHour == LocalHour && Local.wMinute == LocalMinute &&
       Local.wSecond == LocalSecond && Local.wMilliseconds == LocalMilliseconds,
       "UTC %02u-%02u %02u:%02u:%02u.%03u: local %02u:%02u:%02u.%03u, expected %02u:%02u:%02u.%03u\n",
       Month, Day, Hour, Minute, Second, Milliseconds,
       Local.wHour, Local.wMinute, Local.wSecond, Local.wMilliseconds,
       LocalHour, LocalMinute, LocalSecond, LocalMilliseconds);
}

/* Transitions take effect at their second and millisecond */
static
VOID
TestCutoverPrecision(VOID)
{
    TIME_ZONE_INFORMATION Tzi;

    ZeroMemory(&Tzi, sizeof(Tzi));
    Tzi.Bias = 300;
    Tzi.DaylightBias = -60;
    /* Second Sunday of March (2026-03-08) 02:00:30.500 standard time = 07:00:30.500 UTC */
    Tzi.DaylightDate.wMonth = 3;
    Tzi.DaylightDate.wDay = 2;
    Tzi.DaylightDate.wHour = 2;
    Tzi.DaylightDate.wSecond = 30;
    Tzi.DaylightDate.wMilliseconds = 500;
    /* First Sunday of November (2026-11-01) 01:59:59.999 daylight time = 05:59:59.999 UTC */
    Tzi.StandardDate.wMonth = 11;
    Tzi.StandardDate.wDay = 1;
    Tzi.StandardDate.wHour = 1;
    Tzi.StandardDate.wMinute = 59;
    Tzi.StandardDate.wSecond = 59;
    Tzi.StandardDate.wMilliseconds = 999;

    CheckLocal(&Tzi, 3, 8, 7, 0, 0, 0, 2, 0, 0, 0);
    CheckLocal(&Tzi, 3, 8, 7, 0, 30, 499, 2, 0, 30, 499);
    CheckLocal(&Tzi, 3, 8, 7, 0, 30, 500, 3, 0, 30, 500);
    CheckLocal(&Tzi, 11, 1, 5, 59, 0, 0, 1, 59, 0, 0);
    CheckLocal(&Tzi, 11, 1, 5, 59, 59, 998, 1, 59, 59, 998);
    CheckLocal(&Tzi, 11, 1, 5, 59, 59, 999, 0, 59, 59, 999);
}

START_TEST(LocalTimeZone)
{
    TIME_ZONE_INFORMATION Saved, Probe;
    LONGLONG UtcJump;

    TestCutoverPrecision();

    EnablePrivilege(L"SeTimeZonePrivilege");
    EnablePrivilege(L"SeSystemtimePrivilege");

    if (GetTimeZoneInformation(&Saved) == TIME_ZONE_ID_INVALID)
    {
        skip("GetTimeZoneInformation failed, error %lu\n", GetLastError());
        return;
    }

    /* Setting the current zone again needs the same rights as any change */
    Probe = Saved;
    if (!SetZone(&Probe, &UtcJump))
    {
        skip("Cannot set the time zone, error %lu\n", GetLastError());
        return;
    }

    TestZoneChange(-330);
    TestZoneChange(600);
    TestTransition(TRUE);
    TestTransition(FALSE);

    ok(SetZone(&Saved, &UtcJump), "Restoring the time zone failed, error %lu\n", GetLastError());
    ok(UtcJump > -MAX_UTC_DRIFT && UtcJump < MAX_UTC_DRIFT,
       "UTC jumped by %I64d ms when the zone was restored\n", UtcJump / 10000);
}
