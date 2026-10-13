/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GetDynamicTimeZoneInformation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef DWORD (WINAPI *FN_GetDynamicTimeZoneInformation)(PDYNAMIC_TIME_ZONE_INFORMATION);

typedef BOOL (WINAPI *FN_SetDynamicTimeZoneInformation)(const DYNAMIC_TIME_ZONE_INFORMATION *);

#define TZ_KEY L"SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation"

static BOOL EnableTimeZonePrivilege(void)
{
    TOKEN_PRIVILEGES Privileges;
    HANDLE hToken;
    BOOL Ret;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken))
        return FALSE;
    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    Ret = LookupPrivilegeValueW(NULL, L"SeTimeZonePrivilege", &Privileges.Privileges[0].Luid) &&
          AdjustTokenPrivileges(hToken, FALSE, &Privileges, 0, NULL, NULL) &&
          GetLastError() == ERROR_SUCCESS;
    CloseHandle(hToken);
    return Ret;
}

static BOOL ReadZone(PCWSTR KeyName, TIME_ZONE_INFORMATION *Info)
{
    struct { LONG Bias, StandardBias, DaylightBias; SYSTEMTIME StandardDate, DaylightDate; } Tzi;
    WCHAR Path[256];
    DWORD Size;
    HKEY hKey;
    BOOL Ret;

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\%s", KeyName);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_QUERY_VALUE, &hKey))
        return FALSE;
    ZeroMemory(Info, sizeof(*Info));
    Size = sizeof(Tzi);
    Ret = !RegQueryValueExW(hKey, L"TZI", NULL, NULL, (PBYTE)&Tzi, &Size) && Size == sizeof(Tzi);
    Size = sizeof(Info->StandardName) - sizeof(WCHAR);
    Ret = Ret && !RegQueryValueExW(hKey, L"Std", NULL, NULL, (PBYTE)Info->StandardName, &Size);
    Size = sizeof(Info->DaylightName) - sizeof(WCHAR);
    Ret = Ret && !RegQueryValueExW(hKey, L"Dlt", NULL, NULL, (PBYTE)Info->DaylightName, &Size);
    RegCloseKey(hKey);
    Info->Bias = Tzi.Bias;
    Info->StandardBias = Tzi.StandardBias;
    Info->DaylightBias = Tzi.DaylightBias;
    Info->StandardDate = Tzi.StandardDate;
    Info->DaylightDate = Tzi.DaylightDate;
    return Ret;
}

static BOOL SetStoredValue(PCWSTR Name, DWORD Type, const void *Data, DWORD Size)
{
    HKEY hKey;
    LONG Error;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, TZ_KEY, 0, KEY_SET_VALUE, &hKey))
        return FALSE;
    if (Data)
        Error = RegSetValueExW(hKey, Name, 0, Type, (const BYTE *)Data, Size);
    else
        Error = RegDeleteValueW(hKey, Name);
    RegCloseKey(hKey);
    return Error == ERROR_SUCCESS || (!Data && Error == ERROR_FILE_NOT_FOUND);
}

/* A stored value as it was: present or not, and if present its type and bytes */
typedef struct _SAVED_VALUE
{
    BOOL Present;
    DWORD Type;
    DWORD Size;
    BYTE Data[1024];
} SAVED_VALUE;

/* FALSE when the value cannot be saved completely (then nothing must be changed) */
static BOOL SaveValue(PCWSTR Name, SAVED_VALUE *Saved)
{
    HKEY hKey;
    LONG Error;

    ZeroMemory(Saved, sizeof(*Saved));
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, TZ_KEY, 0, KEY_QUERY_VALUE, &hKey))
        return FALSE;
    Saved->Size = sizeof(Saved->Data);
    Error = RegQueryValueExW(hKey, Name, NULL, &Saved->Type, Saved->Data, &Saved->Size);
    RegCloseKey(hKey);
    if (Error == ERROR_FILE_NOT_FOUND)
        return TRUE;
    Saved->Present = (Error == ERROR_SUCCESS);
    return Saved->Present;
}

static BOOL RestoreValue(PCWSTR Name, const SAVED_VALUE *Saved)
{
    return SetStoredValue(Name, Saved->Type, Saved->Present ? Saved->Data : NULL, Saved->Size);
}

/*
 * What GetDynamicTimeZoneInformation makes of the stored values. On Windows
 * SetTimeZoneInformation stores DynamicDaylightTimeDisabled = 1 and an empty
 * key name; the transitions still apply, so the result is the one of
 * GetTimeZoneInformation. A zone of each hemisphere: one of them observes
 * daylight saving time on any date.
 */
static void TestStoredValues(FN_GetDynamicTimeZoneInformation pGet)
{
    static const PCWSTR Zones[] = { L"W. Europe Standard Time", L"AUS Eastern Standard Time" };
    FN_SetDynamicTimeZoneInformation pSetDynamic;
    DYNAMIC_TIME_ZONE_INFORMATION Original, Dynamic;
    TIME_ZONE_INFORMATION Target, Static;
    SAVED_VALUE SavedKeyName, SavedDisabled;
    DWORD Disabled = 1, Result, StaticResult, i;
    BOOL Ret;

    if (!EnableTimeZonePrivilege())
    {
        skip("No SeTimeZonePrivilege\n");
        return;
    }
    if (!SaveValue(L"TimeZoneKeyName", &SavedKeyName) || !SaveValue(L"DynamicDaylightTimeDisabled", &SavedDisabled))
    {
        skip("Cannot save the stored values\n");
        return;
    }
    ZeroMemory(&Original, sizeof(Original));
    if (pGet(&Original) == TIME_ZONE_ID_INVALID)
        return;
    Original.TimeZoneKeyName[ARRAYSIZE(Original.TimeZoneKeyName) - 1] = UNICODE_NULL;

    for (i = 0; i < ARRAYSIZE(Zones); i++)
    {
        if (!ReadZone(Zones[i], &Target))
        {
            skip("No zone %S\n", Zones[i]);
            continue;
        }
        Ret = SetTimeZoneInformation(&Target);
        ok(Ret, "%S: SetTimeZoneInformation failed: %lu\n", Zones[i], GetLastError());
        ok(SetStoredValue(L"DynamicDaylightTimeDisabled", REG_DWORD, &Disabled, sizeof(Disabled)),
           "Cannot store DynamicDaylightTimeDisabled\n");

        RtlFillMemory(&Dynamic, sizeof(Dynamic), 0xCC);
        Result = pGet(&Dynamic);
        StaticResult = GetTimeZoneInformation(&Static);
        trace("%S: result %lu, GetTimeZoneInformation %lu, dynamic daylight time disabled %u\n",
              Zones[i], Result, StaticResult, Dynamic.DynamicDaylightTimeDisabled);
        ok(Result == StaticResult, "%S: result %lu, GetTimeZoneInformation gave %lu\n", Zones[i], Result, StaticResult);
        ok(Dynamic.Bias == Static.Bias, "%S: bias %ld, expected %ld\n", Zones[i], Dynamic.Bias, Static.Bias);
        /* ReactOS reads the registry; Windows may keep the flag elsewhere */
        if (is_reactos())
            ok(Dynamic.DynamicDaylightTimeDisabled == TRUE, "%S: flag %u\n", Zones[i], Dynamic.DynamicDaylightTimeDisabled);
    }

    /* An empty stored key name is a zone without one, a missing one a registry from before it existed */
    ok(SetStoredValue(L"TimeZoneKeyName", REG_SZ, L"", sizeof(WCHAR)), "Cannot store the key name\n");
    RtlFillMemory(&Dynamic, sizeof(Dynamic), 0xCC);
    ok(pGet(&Dynamic) != TIME_ZONE_ID_INVALID, "Failed: %lu\n", GetLastError());
    Dynamic.TimeZoneKeyName[ARRAYSIZE(Dynamic.TimeZoneKeyName) - 1] = UNICODE_NULL;
    trace("Empty stored key name: key '%S'\n", Dynamic.TimeZoneKeyName);
    if (is_reactos())
        ok(!Dynamic.TimeZoneKeyName[0], "Empty stored key name: key '%S'\n", Dynamic.TimeZoneKeyName);
    ok(SetStoredValue(L"TimeZoneKeyName", REG_SZ, NULL, 0), "Cannot remove the key name\n");
    RtlFillMemory(&Dynamic, sizeof(Dynamic), 0xCC);
    ok(pGet(&Dynamic) != TIME_ZONE_ID_INVALID, "Failed: %lu\n", GetLastError());
    Dynamic.TimeZoneKeyName[ARRAYSIZE(Dynamic.TimeZoneKeyName) - 1] = UNICODE_NULL;
    trace("No stored key name: key '%S'\n", Dynamic.TimeZoneKeyName);

    /* Put everything back */
    pSetDynamic = (FN_SetDynamicTimeZoneInformation)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetDynamicTimeZoneInformation");
    if (pSetDynamic && !is_reactos())
    {
        Ret = pSetDynamic(&Original);
        ok(Ret, "SetDynamicTimeZoneInformation failed: %lu\n", GetLastError());
    }
    else
    {
        Ret = SetTimeZoneInformation((TIME_ZONE_INFORMATION *)&Original);
        ok(Ret, "SetTimeZoneInformation failed: %lu\n", GetLastError());
    }
    ok(RestoreValue(L"TimeZoneKeyName", &SavedKeyName), "Cannot restore the stored key name\n");
    ok(RestoreValue(L"DynamicDaylightTimeDisabled", &SavedDisabled), "Cannot restore DynamicDaylightTimeDisabled\n");
    RtlFillMemory(&Dynamic, sizeof(Dynamic), 0xCC);
    ok(pGet(&Dynamic) != TIME_ZONE_ID_INVALID, "Failed: %lu\n", GetLastError());
    Dynamic.TimeZoneKeyName[ARRAYSIZE(Dynamic.TimeZoneKeyName) - 1] = UNICODE_NULL;
    ok(!wcscmp(Dynamic.TimeZoneKeyName, Original.TimeZoneKeyName), "Key name '%S' after restore, expected '%S'\n",
       Dynamic.TimeZoneKeyName, Original.TimeZoneKeyName);
    ok(Dynamic.Bias == Original.Bias, "Bias %ld after restore, expected %ld\n", Dynamic.Bias, Original.Bias);
}

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
    if (Result == TIME_ZONE_ID_INVALID)
        return;

    /* The part GetTimeZoneInformation returns is the same */
    ok(Dynamic.Bias == Static.Bias, "Bias %ld, expected %ld\n", Dynamic.Bias, Static.Bias);
    ok(!wcscmp(Dynamic.StandardName, Static.StandardName), "StandardName '%S', expected '%S'\n", Dynamic.StandardName, Static.StandardName);
    ok(!wcscmp(Dynamic.DaylightName, Static.DaylightName), "DaylightName '%S', expected '%S'\n", Dynamic.DaylightName, Static.DaylightName);
    ok(!memcmp(&Dynamic.StandardDate, &Static.StandardDate, sizeof(SYSTEMTIME)), "StandardDate differs\n");
    ok(!memcmp(&Dynamic.DaylightDate, &Static.DaylightDate, sizeof(SYSTEMTIME)), "DaylightDate differs\n");
    ok(Dynamic.StandardBias == Static.StandardBias, "StandardBias %ld, expected %ld\n", Dynamic.StandardBias, Static.StandardBias);
    ok(Dynamic.DaylightBias == Static.DaylightBias, "DaylightBias %ld, expected %ld\n", Dynamic.DaylightBias, Static.DaylightBias);

    /* The flag means fixed transition dates; the zone in effect is the one GetTimeZoneInformation reports */
    ok(Dynamic.DynamicDaylightTimeDisabled == FALSE || Dynamic.DynamicDaylightTimeDisabled == TRUE,
       "DynamicDaylightTimeDisabled %u\n", Dynamic.DynamicDaylightTimeDisabled);
    ok(Result == StaticResult, "Result %lu, GetTimeZoneInformation gave %lu\n", Result, StaticResult);

    /* The key name is terminated and, if there is one, names a Time Zones entry (its TZI is the zone's default, not this year's rule) */
    Size = (DWORD)wcsnlen(Dynamic.TimeZoneKeyName, ARRAYSIZE(Dynamic.TimeZoneKeyName));
    ok(Size < ARRAYSIZE(Dynamic.TimeZoneKeyName), "TimeZoneKeyName is not terminated\n");
    if (Size >= ARRAYSIZE(Dynamic.TimeZoneKeyName))
        return;
    /* Empty when the zone was set with SetTimeZoneInformation */
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

    TestStoredValues(pGetDynamicTimeZoneInformation);
}
