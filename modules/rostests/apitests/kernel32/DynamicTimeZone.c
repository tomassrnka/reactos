/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GetDynamicTimeZoneInformation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef DWORD (WINAPI *FN_GetDynamicTimeZoneInformation)(PDYNAMIC_TIME_ZONE_INFORMATION);

START_TEST(DynamicTimeZone)
{
    FN_GetDynamicTimeZoneInformation pGetDynamicTimeZoneInformation;
    DYNAMIC_TIME_ZONE_INFORMATION Dynamic;
    TIME_ZONE_INFORMATION Static;
    WCHAR KeyPath[256];
    DWORD Result, StaticResult, Tzi[11], Size;
    HKEY hKey;
    LONG Error;

    pGetDynamicTimeZoneInformation = (FN_GetDynamicTimeZoneInformation)
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetDynamicTimeZoneInformation");
    if (!pGetDynamicTimeZoneInformation && is_reactos() && !GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64"))
    {
        skip("No NT 6 exports\n");
        return;
    }
    ok(pGetDynamicTimeZoneInformation != NULL, "GetDynamicTimeZoneInformation is not exported\n");
    if (!pGetDynamicTimeZoneInformation)
        return;

    StaticResult = GetTimeZoneInformation(&Static);
    RtlFillMemory(&Dynamic, sizeof(Dynamic), 0xCC);
    Result = pGetDynamicTimeZoneInformation(&Dynamic);
    ok(Result != TIME_ZONE_ID_INVALID, "Failed: %lu\n", GetLastError());

    /* The part GetTimeZoneInformation returns is the same */
    ok(Dynamic.Bias == Static.Bias, "Bias %ld, expected %ld\n", Dynamic.Bias, Static.Bias);
    ok(!wcscmp(Dynamic.StandardName, Static.StandardName), "StandardName '%S', expected '%S'\n", Dynamic.StandardName, Static.StandardName);
    ok(!wcscmp(Dynamic.DaylightName, Static.DaylightName), "DaylightName '%S', expected '%S'\n", Dynamic.DaylightName, Static.DaylightName);
    ok(!memcmp(&Dynamic.StandardDate, &Static.StandardDate, sizeof(SYSTEMTIME)), "StandardDate differs\n");
    ok(!memcmp(&Dynamic.DaylightDate, &Static.DaylightDate, sizeof(SYSTEMTIME)), "DaylightDate differs\n");
    ok(Dynamic.StandardBias == Static.StandardBias, "StandardBias %ld, expected %ld\n", Dynamic.StandardBias, Static.StandardBias);
    ok(Dynamic.DaylightBias == Static.DaylightBias, "DaylightBias %ld, expected %ld\n", Dynamic.DaylightBias, Static.DaylightBias);

    /* With automatic daylight saving adjustment disabled no daylight saving time is in use */
    ok(Dynamic.DynamicDaylightTimeDisabled == FALSE || Dynamic.DynamicDaylightTimeDisabled == TRUE,
       "DynamicDaylightTimeDisabled %u\n", Dynamic.DynamicDaylightTimeDisabled);
    if (Dynamic.DynamicDaylightTimeDisabled)
        ok(Result == TIME_ZONE_ID_UNKNOWN, "Result %lu with daylight saving disabled\n", Result);
    else
        ok(Result == StaticResult, "Result %lu, GetTimeZoneInformation gave %lu\n", Result, StaticResult);

    /* The key name is terminated and names a Time Zones entry (its TZI is the zone's default, not this year's rule) */
    ok(wcsnlen(Dynamic.TimeZoneKeyName, ARRAYSIZE(Dynamic.TimeZoneKeyName)) < ARRAYSIZE(Dynamic.TimeZoneKeyName),
       "TimeZoneKeyName is not terminated\n");
    ok(Dynamic.TimeZoneKeyName[0] != UNICODE_NULL, "No TimeZoneKeyName\n");
    if (Dynamic.TimeZoneKeyName[0])
    {
        StringCchPrintfW(KeyPath, ARRAYSIZE(KeyPath), L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\%s",
                         Dynamic.TimeZoneKeyName);
        Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyPath, 0, KEY_QUERY_VALUE, &hKey);
        ok(Error == ERROR_SUCCESS, "Time zone key '%S' does not open: %ld\n", Dynamic.TimeZoneKeyName, Error);
        if (Error == ERROR_SUCCESS)
        {
            Size = sizeof(Tzi);
            Error = RegQueryValueExW(hKey, L"TZI", NULL, NULL, (PBYTE)Tzi, &Size);
            ok(Error == ERROR_SUCCESS, "No TZI: %ld\n", Error);
            RegCloseKey(hKey);
        }
    }
}
