/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for RtlSetTimeZoneInformation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <winbase.h>
#include <winreg.h>

#define TZ_KEY L"SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation"
#define TEST_KEY_NAME L"Test Standard Time"

/* Vista+; not in the NDK headers */
#ifndef SE_TIME_ZONE_PRIVILEGE
#define SE_TIME_ZONE_PRIVILEGE 34L
#endif

static
BOOL
SetKeyName(
    _In_ PCWSTR KeyName)
{
    HKEY hKey;
    LONG lError;

    lError = RegOpenKeyExW(HKEY_LOCAL_MACHINE, TZ_KEY, 0, KEY_SET_VALUE, &hKey);
    if (lError != ERROR_SUCCESS)
        return FALSE;

    lError = RegSetValueExW(hKey,
                            L"TimeZoneKeyName",
                            0,
                            REG_SZ,
                            (const BYTE *)KeyName,
                            (DWORD)(wcslen(KeyName) + 1) * sizeof(WCHAR));
    RegCloseKey(hKey);
    return (lError == ERROR_SUCCESS);
}

static
LONG
QueryKeyName(
    _Out_ PDWORD Type,
    _Out_writes_bytes_(*Size) PWCHAR Data,
    _Inout_ PDWORD Size)
{
    HKEY hKey;
    LONG lError;

    lError = RegOpenKeyExW(HKEY_LOCAL_MACHINE, TZ_KEY, 0, KEY_QUERY_VALUE, &hKey);
    if (lError != ERROR_SUCCESS)
        return lError;

    lError = RegQueryValueExW(hKey, L"TimeZoneKeyName", NULL, Type, (LPBYTE)Data, Size);
    RegCloseKey(hKey);
    return lError;
}

static
VOID
CheckKeyNameCleared(
    _In_ PCSTR After)
{
    WCHAR Data[128];
    DWORD Type = REG_NONE;
    DWORD Size = sizeof(Data);
    LONG lError;

    lError = QueryKeyName(&Type, Data, &Size);
    ok(lError == ERROR_SUCCESS, "%s: TimeZoneKeyName query error %ld\n", After, lError);
    if (lError != ERROR_SUCCESS)
        return;

    ok(Type == REG_SZ, "%s: TimeZoneKeyName type %lu\n", After, Type);
    ok(Size >= sizeof(WCHAR) && Data[0] == UNICODE_NULL,
       "%s: TimeZoneKeyName is '%.*S' (%lu bytes), expected empty\n",
       After, (int)(Size / sizeof(WCHAR)), Data, Size);
}

START_TEST(RtlSetTimeZoneInformation)
{
    RTL_TIME_ZONE_INFORMATION RtlInfo;
    TIME_ZONE_INFORMATION Info;
    WCHAR OldKeyName[128];
    DWORD OldType;
    DWORD OldSize = sizeof(OldKeyName);
    BOOLEAN bOldPrivilege;
    BOOL bHadKeyName;
    NTSTATUS Status;
    HKEY hKey;

    bHadKeyName = (QueryKeyName(&OldType, OldKeyName, &OldSize) == ERROR_SUCCESS);

    if (!SetKeyName(TEST_KEY_NAME))
    {
        skip("No write access to the TimeZoneInformation key\n");
        return;
    }

    /* Setting the zone through the native API empties the key name */
    Status = RtlQueryTimeZoneInformation(&RtlInfo);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = RtlSetTimeZoneInformation(&RtlInfo);
        ok_ntstatus(Status, STATUS_SUCCESS);
        CheckKeyNameCleared("RtlSetTimeZoneInformation");
    }

    /* And so does SetTimeZoneInformation, which may need the time zone privilege */
    ok(SetKeyName(TEST_KEY_NAME), "SetKeyName failed\n");
    RtlAdjustPrivilege(SE_TIME_ZONE_PRIVILEGE, TRUE, FALSE, &bOldPrivilege);
    ok(GetTimeZoneInformation(&Info) != TIME_ZONE_ID_INVALID,
       "GetTimeZoneInformation failed with %lu\n", GetLastError());
    if (SetTimeZoneInformation(&Info))
        CheckKeyNameCleared("SetTimeZoneInformation");
    else
        skip("SetTimeZoneInformation failed with %lu\n", GetLastError());

    if (bHadKeyName)
    {
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, TZ_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
        {
            RegSetValueExW(hKey, L"TimeZoneKeyName", 0, OldType, (const BYTE *)OldKeyName, OldSize);
            RegCloseKey(hKey);
        }
    }
    else if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, TZ_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
    {
        RegDeleteValueW(hKey, L"TimeZoneKeyName");
        RegCloseKey(hKey);
    }
}
