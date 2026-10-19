/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that flushed values of a large key survive a hive reload
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define TEST_VALUE_COUNT 1500
#define TEST_FLUSH_EVERY 25

static const WCHAR MountName[] = L"ReactOS_RegFlushKey";

static BOOL
EnablePrivilege(LPCWSTR Name)
{
    HANDLE Token;
    TOKEN_PRIVILEGES Privileges;
    BOOL Success;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &Token))
        return FALSE;

    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    Success = LookupPrivilegeValueW(NULL, Name, &Privileges.Privileges[0].Luid) &&
              AdjustTokenPrivileges(Token, FALSE, &Privileges, 0, NULL, NULL) &&
              GetLastError() == ERROR_SUCCESS;
    CloseHandle(Token);
    return Success;
}

static void
DeleteHiveFiles(LPCWSTR HivePath)
{
    static const LPCWSTR LogSuffixes[] = { L".LOG", L".LOG1", L".LOG2" };
    WCHAR LogPath[MAX_PATH];
    ULONG i;

    DeleteFileW(HivePath);
    for (i = 0; i < _countof(LogSuffixes); i++)
    {
        if (SUCCEEDED(StringCchPrintfW(LogPath, _countof(LogPath), L"%s%s",
                                       HivePath, LogSuffixes[i])))
        {
            DeleteFileW(LogPath);
        }
    }
}

static BOOL
CreateEmptyHive(LPCWSTR HivePath)
{
    static const WCHAR KeyName[] = L"Software\\ReactOS_apitest_RegFlushKey";
    HKEY Key;
    LONG Error;

    Error = RegCreateKeyExW(HKEY_CURRENT_USER, KeyName, 0, NULL, 0,
                            KEY_ALL_ACCESS, NULL, &Key, NULL);
    ok_long(Error, ERROR_SUCCESS);
    if (Error != ERROR_SUCCESS)
        return FALSE;

    Error = RegSaveKeyW(Key, HivePath, NULL);
    ok_long(Error, ERROR_SUCCESS);
    RegCloseKey(Key);
    RegDeleteKeyW(HKEY_CURRENT_USER, KeyName);
    return Error == ERROR_SUCCESS;
}

START_TEST(RegFlushKey)
{
    WCHAR TempDir[MAX_PATH], HivePath[MAX_PATH], Name[16];
    HKEY Key;
    LONG Error;
    DWORD i, Data, Type, Size, ValueCount, Missing, Wrong;

    if (!EnablePrivilege(L"SeBackupPrivilege") || !EnablePrivilege(L"SeRestorePrivilege"))
    {
        skip("Cannot enable the backup and restore privileges\n");
        return;
    }

    i = GetTempPathW(_countof(TempDir), TempDir);
    if (i == 0 || i >= _countof(TempDir) ||
        FAILED(StringCchPrintfW(HivePath, _countof(HivePath), L"%sRegFlushKey.hiv", TempDir)))
    {
        skip("No usable temporary directory\n");
        return;
    }
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, MountName);
    DeleteHiveFiles(HivePath);

    if (!CreateEmptyHive(HivePath))
    {
        DeleteHiveFiles(HivePath);
        return;
    }

    /*
     * Above about 1000 values the value list is larger than one hive block
     * (4 KB), so values added after a flush land in its second block.
     */
    Error = RegLoadKeyW(HKEY_LOCAL_MACHINE, MountName, HivePath);
    ok_long(Error, ERROR_SUCCESS);
    if (Error != ERROR_SUCCESS)
    {
        DeleteHiveFiles(HivePath);
        return;
    }

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, MountName, 0, KEY_ALL_ACCESS, &Key);
    ok_long(Error, ERROR_SUCCESS);
    if (Error == ERROR_SUCCESS)
    {
        for (i = 0; i < TEST_VALUE_COUNT; i++)
        {
            StringCchPrintfW(Name, _countof(Name), L"v%05lu", i);
            Error = RegSetValueExW(Key, Name, 0, REG_DWORD, (PBYTE)&i, sizeof(i));
            if (Error != ERROR_SUCCESS)
            {
                ok(0, "RegSetValueExW(%ls) returned %ld\n", Name, Error);
                break;
            }
            if ((i + 1) % TEST_FLUSH_EVERY == 0)
            {
                Error = RegFlushKey(Key);
                if (Error != ERROR_SUCCESS)
                {
                    ok(0, "RegFlushKey after %lu values returned %ld\n", i + 1, Error);
                    break;
                }
            }
        }
        ok_long(RegFlushKey(Key), ERROR_SUCCESS);
        RegCloseKey(Key);
    }

    /* Unload and reload the hive so that the values are read from the file */
    Error = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, MountName);
    ok_long(Error, ERROR_SUCCESS);
    Error = RegLoadKeyW(HKEY_LOCAL_MACHINE, MountName, HivePath);
    ok_long(Error, ERROR_SUCCESS);
    if (Error != ERROR_SUCCESS)
    {
        DeleteHiveFiles(HivePath);
        return;
    }

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, MountName, 0, KEY_READ, &Key);
    ok_long(Error, ERROR_SUCCESS);
    if (Error == ERROR_SUCCESS)
    {
        ValueCount = 0;
        Error = RegQueryInfoKeyW(Key, NULL, NULL, NULL, NULL, NULL, NULL,
                                 &ValueCount, NULL, NULL, NULL, NULL);
        ok_long(Error, ERROR_SUCCESS);
        ok_long(ValueCount, TEST_VALUE_COUNT);

        Missing = Wrong = 0;
        for (i = 0; i < TEST_VALUE_COUNT; i++)
        {
            StringCchPrintfW(Name, _countof(Name), L"v%05lu", i);
            Size = sizeof(Data);
            Error = RegQueryValueExW(Key, Name, NULL, &Type, (PBYTE)&Data, &Size);
            if (Error != ERROR_SUCCESS)
            {
                if (Missing++ == 0)
                    trace("First missing value: %ls (%ld)\n", Name, Error);
            }
            else if (Type != REG_DWORD || Size != sizeof(Data) || Data != i)
            {
                Wrong++;
            }
        }
        ok(Missing == 0, "%lu of %u values are missing after the reload\n",
           Missing, TEST_VALUE_COUNT);
        ok(Wrong == 0, "%lu of %u values have wrong data after the reload\n",
           Wrong, TEST_VALUE_COUNT);
        RegCloseKey(Key);
    }

    Error = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, MountName);
    ok_long(Error, ERROR_SUCCESS);
    DeleteHiveFiles(HivePath);
}
