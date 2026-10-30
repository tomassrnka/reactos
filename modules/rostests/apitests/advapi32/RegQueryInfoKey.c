/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         LGPLv2.1+ - See COPYING.Lib in the top level directory
 * PURPOSE:         Test for RegQueryInfoKey
 * PROGRAMMER:      Thomas Faber <thomas.faber@reactos.org>
 */

#include "precomp.h"

#define TestKeyAccess(da, er, es) TestKeyAccess_(__FILE__, __LINE__, da, er, es)
static
VOID
TestKeyAccess_(
    _In_ PCSTR File,
    _In_ INT Line,
    _In_ REGSAM DesiredAccess,
    _In_ LONG ExpectedReturn,
    _In_ BOOLEAN ExpectSd)
{
    DWORD cbSd;
    HKEY hKey;
    LONG ret;

    ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software", 0, DesiredAccess, &hKey);
    ok_(File, Line)(ret == NO_ERROR, "RegOpenKeyEx returned %ld\n", ret);
    if (ret == NO_ERROR)
    {
        cbSd = 0x55555555;
        ret = RegQueryInfoKeyW(hKey, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &cbSd, NULL);
        ok_(File, Line)(ret == ExpectedReturn, "RegQueryInfoKeyW returned %ld\n", ret);
        if (ExpectSd)
            ok_(File, Line)(cbSd != 0 && cbSd != 0x55555555, "RegQueryInfoKeyW - cbSd = %lu\n", cbSd);
        else
            ok_(File, Line)(cbSd == 0, "RegQueryInfoKeyW - cbSd = %lu\n", cbSd);

        cbSd = 0x55555555;
        ret = RegQueryInfoKeyA(hKey, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &cbSd, NULL);
        ok_(File, Line)(ret == ExpectedReturn, "RegQueryInfoKeyA returned %ld\n", ret);
        if (ExpectSd)
            ok_(File, Line)(cbSd != 0 && cbSd != 0x55555555, "RegQueryInfoKeyA - cbSd = %lu\n", cbSd);
        else
            ok_(File, Line)(cbSd == 0, "RegQueryInfoKeyA - cbSd = %lu\n", cbSd);
        ret = RegCloseKey(hKey);
        ok_(File, Line)(ret == NO_ERROR, "RegCloseKey returned %ld\n", ret);
    }
    else
    {
        skip_(File, Line)("No key handle\n");
    }
}

static
VOID
TestPerformanceDataKey(VOID)
{
    WCHAR Name[64];
    WCHAR Class[16] = L"x";
    DWORD cbName;
    DWORD cbClass;
    DWORD SubKeys, MaxSubKeyLen, MaxClassLen;
    FILETIME LastWrite;
    LONG ret;

    /* The performance data key is not a real registry key, but it can be queried.
     * The number and size of its values differ between Windows versions: do not check them. */
    SubKeys = MaxSubKeyLen = MaxClassLen = 0x55555555;
    ret = RegQueryInfoKeyW(HKEY_PERFORMANCE_DATA, NULL, NULL, NULL, &SubKeys, &MaxSubKeyLen, &MaxClassLen,
                           NULL, NULL, NULL, NULL, &LastWrite);
    ok(ret == NO_ERROR, "RegQueryInfoKeyW returned %ld\n", ret);
    ok(SubKeys == 0, "SubKeys = %lu\n", SubKeys);
    ok(MaxSubKeyLen == 0, "MaxSubKeyLen = %lu\n", MaxSubKeyLen);
    ok(MaxClassLen == 0, "MaxClassLen = %lu\n", MaxClassLen);

    SubKeys = 0x55555555;
    ret = RegQueryInfoKeyA(HKEY_PERFORMANCE_DATA, NULL, NULL, NULL, &SubKeys, NULL, NULL,
                           NULL, NULL, NULL, NULL, NULL);
    ok(ret == NO_ERROR, "RegQueryInfoKeyA returned %ld\n", ret);
    ok(SubKeys == 0, "SubKeys = %lu\n", SubKeys);

    ret = RegQueryInfoKeyW(HKEY_PERFORMANCE_DATA, NULL, NULL, NULL, NULL, NULL, NULL,
                           NULL, NULL, NULL, NULL, NULL);
    ok(ret == NO_ERROR, "RegQueryInfoKeyW returned %ld\n", ret);

    cbClass = ARRAYSIZE(Class);
    ret = RegQueryInfoKeyW(HKEY_PERFORMANCE_DATA, Class, &cbClass, NULL, NULL, NULL, NULL,
                           NULL, NULL, NULL, NULL, NULL);
    ok(ret == NO_ERROR, "RegQueryInfoKeyW returned %ld\n", ret);
    ok(cbClass == 0, "cbClass = %lu\n", cbClass);

    cbName = ARRAYSIZE(Name);
    ret = RegEnumKeyExW(HKEY_PERFORMANCE_DATA, 0, Name, &cbName, NULL, NULL, NULL, NULL);
    ok(ret == ERROR_NO_MORE_ITEMS, "RegEnumKeyExW returned %ld\n", ret);

    cbName = ARRAYSIZE(Name);
    ret = RegEnumValueW(HKEY_PERFORMANCE_DATA, 100, Name, &cbName, NULL, NULL, NULL, NULL);
    ok(ret == ERROR_NO_MORE_ITEMS, "RegEnumValueW returned %ld\n", ret);
}

START_TEST(RegQueryInfoKey)
{
    /* 0 access just fails the open */
    if (0)
    TestKeyAccess(0,                                ERROR_ACCESS_DENIED, FALSE);
    /* Without KEY_QUERY_VALUE we can't query anything */
    TestKeyAccess(READ_CONTROL,                     ERROR_ACCESS_DENIED, FALSE);
    /* Without READ_CONTROL we'll get success but SD size will yield 0 */
    TestKeyAccess(KEY_QUERY_VALUE,                  NO_ERROR, FALSE);
    /* With the two combined we get everything */
    TestKeyAccess(KEY_QUERY_VALUE | READ_CONTROL,   NO_ERROR, TRUE);
    /* Write rights return nothing on 2003 (but succeed and return SD size on Win7) */
    TestKeyAccess(KEY_SET_VALUE,                    ERROR_ACCESS_DENIED, FALSE);
    TestKeyAccess(KEY_CREATE_SUB_KEY,               ERROR_ACCESS_DENIED, FALSE);
    TestKeyAccess(KEY_CREATE_LINK,                  ERROR_ACCESS_DENIED, FALSE);
    TestKeyAccess(DELETE,                           ERROR_ACCESS_DENIED, FALSE);
    TestKeyAccess(WRITE_DAC,                        ERROR_ACCESS_DENIED, FALSE);
    TestKeyAccess(WRITE_OWNER,                      ERROR_ACCESS_DENIED, FALSE);
    /* But these return nothing */
    TestKeyAccess(KEY_ENUMERATE_SUB_KEYS,           ERROR_ACCESS_DENIED, FALSE);
    TestKeyAccess(KEY_NOTIFY,                       ERROR_ACCESS_DENIED, FALSE);
    TestPerformanceDataKey();
}
