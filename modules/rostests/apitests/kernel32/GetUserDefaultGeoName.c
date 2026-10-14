/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for GetUserDefaultGeoName (Windows 10)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows 10 22H2 (x64 and WOW64), where the region is the United States.
 */

#include "precomp.h"

typedef int (WINAPI *PFN_GetUserDefaultGeoName)(LPWSTR, int);

#define PENDING_ERROR 0xdeadbeef

START_TEST(GetUserDefaultGeoName)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    PFN_GetUserDefaultGeoName pGetUserDefaultGeoName;
    WCHAR Expected[16], Buffer[16];
    int Length, Result, i;

    pGetUserDefaultGeoName = (PFN_GetUserDefaultGeoName)GetProcAddress(Kernel32, "GetUserDefaultGeoName");
    if (!pGetUserDefaultGeoName)
    {
        pGetUserDefaultGeoName = (PFN_GetUserDefaultGeoName)GetProcAddress(LoadLibraryW(L"kernelbase.dll"), "GetUserDefaultGeoName");
        /* ReactOS has it from export level 0x601, which a kernel32 with the Windows 7 functions shows */
        ok(pGetUserDefaultGeoName || !(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")), "GetUserDefaultGeoName is not exported\n");
        if (!pGetUserDefaultGeoName)
        {
            skip("GetUserDefaultGeoName is not exported\n");
            return;
        }
    }

    /* the name is the one the user set; without it the code of the nation, or "001" for the world */
    memset(Expected, 0, sizeof(Expected));
    {
        HKEY Key;
        DWORD Size = sizeof(Expected), Type;

        Length = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\International\\Geo", 0, KEY_QUERY_VALUE, &Key) == ERROR_SUCCESS)
        {
            if (RegQueryValueExW(Key, L"Name", NULL, &Type, (PBYTE)Expected, &Size) == ERROR_SUCCESS && Type == REG_SZ &&
                Expected[0])
            {
                Length = lstrlenW(Expected) + 1;
            }
            RegCloseKey(Key);
        }
        if (Length == 0)
        {
            GEOID Nation = GetUserGeoID(GEOCLASS_NATION);

            if (Nation != 39070)
                Length = GetGeoInfoW(Nation, GEO_ISO2, Expected, RTL_NUMBER_OF(Expected), 0);
            if (Length <= 0)
            {
                lstrcpyW(Expected, L"001");
                Length = 4;
            }
        }
    }

    /* the size is asked with a count of zero, and a buffer is not needed */
    SetLastError(PENDING_ERROR);
    Result = pGetUserDefaultGeoName(NULL, 0);
    ok_dec(Result, Length);
    ok_hex(GetLastError(), PENDING_ERROR);
    memset(Buffer, 0x55, sizeof(Buffer));
    Result = pGetUserDefaultGeoName(Buffer, 0);
    ok_dec(Result, Length);
    ok_hex(Buffer[0], 0x5555);

    /* the terminator is counted */
    for (i = 0; i < Length; i++)
    {
        memset(Buffer, 0x55, sizeof(Buffer));
        SetLastError(PENDING_ERROR);
        Result = pGetUserDefaultGeoName(Buffer, i);
        if (i == 0)
        {
            ok_dec(Result, Length);
            continue;
        }
        ok_dec(Result, 0);
        ok_dec(GetLastError(), ERROR_INSUFFICIENT_BUFFER);
        ok_hex(Buffer[0], 0x5555);
    }
    for (i = Length; i <= 16; i++)
    {
        memset(Buffer, 0x55, sizeof(Buffer));
        Result = pGetUserDefaultGeoName(Buffer, i);
        ok_dec(Result, Length);
        ok(!wcscmp(Buffer, Expected), "got %ls, expected %ls\n", Buffer, Expected);
        ok_hex(Buffer[Length], 0x5555);
    }

    /* bad arguments */
    SetLastError(PENDING_ERROR);
    Result = pGetUserDefaultGeoName(NULL, 5);
    ok_dec(Result, 0);
    ok_dec(GetLastError(), ERROR_INVALID_PARAMETER);
    SetLastError(PENDING_ERROR);
    Result = pGetUserDefaultGeoName(Buffer, -1);
    ok_dec(Result, 0);
    ok_dec(GetLastError(), ERROR_INVALID_PARAMETER);
}
