/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the class returned by RegEnumKeyExW and RegEnumKeyExA
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

#define BASE_KEY "Software\\ReactOS_apitest_RegEnumKeyClass"

/* The subkeys enumerate as AKey (class "TheClass") at index 0 and BKey (no class) at index 1 */

static
LONG
EnumW(HKEY hKey, DWORD Index, DWORD ClassSize, WCHAR *Name, DWORD *NameLen, WCHAR *Class, DWORD *ClassLen)
{
    FillMemory(Name, 32 * sizeof(WCHAR), 0x55);
    FillMemory(Class, 32 * sizeof(WCHAR), 0x55);
    *NameLen = 32;
    *ClassLen = ClassSize;
    return RegEnumKeyExW(hKey, Index, Name, NameLen, NULL, Class, ClassLen, NULL);
}

static
LONG
EnumA(HKEY hKey, DWORD Index, DWORD ClassSize, CHAR *Name, DWORD *NameLen, CHAR *Class, DWORD *ClassLen)
{
    FillMemory(Name, 32, 0x55);
    FillMemory(Class, 32, 0x55);
    *NameLen = 32;
    *ClassLen = ClassSize;
    return RegEnumKeyExA(hKey, Index, Name, NameLen, NULL, Class, ClassLen, NULL);
}

static
void
TestW(HKEY hKey)
{
    LONG Error;
    WCHAR Name[32], Class[32];
    DWORD NameLen, ClassLen;

    /* The class comes back with its length, the name stays the name */
    Error = EnumW(hKey, 0, 32, Name, &NameLen, Class, &ClassLen);
    ok_dec(Error, ERROR_SUCCESS);
    if (Error == ERROR_SUCCESS)
    {
        ok(!wcscmp(Name, L"AKey"), "Wrong name\n");
        ok_dec(NameLen, 4);
        ok(!wcsncmp(Class, L"TheClass", 9), "Wrong class\n");
        ok_dec(ClassLen, 8);
        ok_hex(Class[9], 0x5555);
    }

    /* A key without a class gives an empty class */
    Error = EnumW(hKey, 1, 32, Name, &NameLen, Class, &ClassLen);
    ok_dec(Error, ERROR_SUCCESS);
    if (Error == ERROR_SUCCESS)
    {
        ok(!wcscmp(Name, L"BKey"), "Wrong name\n");
        ok_hex(Class[0], 0);
        ok_dec(ClassLen, 0);
    }

    /* The class needs room for its terminator */
    Error = EnumW(hKey, 0, 8, Name, &NameLen, Class, &ClassLen);
    ok_dec(Error, ERROR_MORE_DATA);
    Error = EnumW(hKey, 0, 9, Name, &NameLen, Class, &ClassLen);
    ok_dec(Error, ERROR_SUCCESS);
    if (Error == ERROR_SUCCESS)
    {
        ok(!wcsncmp(Class, L"TheClass", 9), "Wrong class\n");
        ok_dec(ClassLen, 8);
    }
}

static
void
TestA(HKEY hKey)
{
    LONG Error;
    CHAR Name[32], Class[32];
    DWORD NameLen, ClassLen;

    Error = EnumA(hKey, 0, 32, Name, &NameLen, Class, &ClassLen);
    ok_dec(Error, ERROR_SUCCESS);
    if (Error == ERROR_SUCCESS)
    {
        ok(!strcmp(Name, "AKey"), "Wrong name\n");
        ok_dec(NameLen, 4);
        ok(!strncmp(Class, "TheClass", 9), "Wrong class '%.8s'\n", Class);
        ok_dec(ClassLen, 8);
        ok_hex((UCHAR)Class[9], 0x55);
    }

    Error = EnumA(hKey, 1, 32, Name, &NameLen, Class, &ClassLen);
    ok_dec(Error, ERROR_SUCCESS);
    if (Error == ERROR_SUCCESS)
    {
        ok(!strcmp(Name, "BKey"), "Wrong name\n");
        ok_hex(Class[0], 0);
        ok_dec(ClassLen, 0);
    }

    Error = EnumA(hKey, 0, 8, Name, &NameLen, Class, &ClassLen);
    ok_dec(Error, ERROR_MORE_DATA);
    Error = EnumA(hKey, 0, 9, Name, &NameLen, Class, &ClassLen);
    ok_dec(Error, ERROR_SUCCESS);
    if (Error == ERROR_SUCCESS)
    {
        ok(!strncmp(Class, "TheClass", 9), "Wrong class '%.8s'\n", Class);
        ok_dec(ClassLen, 8);
    }

    /* A class buffer of size 0 is never written to (the result differs:
     * Windows returns ERROR_MORE_DATA, ReactOS success) */
    EnumA(hKey, 1, 0, Name, &NameLen, Class, &ClassLen);
    ok_hex((UCHAR)Class[0], 0x55);
}

START_TEST(RegEnumKeyClass)
{
    LONG Error;
    HKEY hKey, hSubKey;

    RegDeleteKeyA(HKEY_CURRENT_USER, BASE_KEY "\\AKey");
    RegDeleteKeyA(HKEY_CURRENT_USER, BASE_KEY "\\BKey");
    RegDeleteKeyA(HKEY_CURRENT_USER, BASE_KEY);

    Error = RegCreateKeyExA(HKEY_CURRENT_USER, BASE_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey, NULL);
    ok_dec(Error, ERROR_SUCCESS);
    if (Error != ERROR_SUCCESS)
    {
        skip("Cannot create the test key\n");
        return;
    }

    Error = RegCreateKeyExA(hKey, "AKey", 0, "TheClass", 0, KEY_READ, NULL, &hSubKey, NULL);
    ok_dec(Error, ERROR_SUCCESS);
    if (Error == ERROR_SUCCESS)
    {
        RegCloseKey(hSubKey);
        Error = RegCreateKeyExA(hKey, "BKey", 0, NULL, 0, KEY_READ, NULL, &hSubKey, NULL);
        ok_dec(Error, ERROR_SUCCESS);
        if (Error == ERROR_SUCCESS)
            RegCloseKey(hSubKey);
    }

    if (Error == ERROR_SUCCESS)
    {
        TestW(hKey);
        TestA(hKey);
    }
    else
    {
        skip("Cannot create the subkeys\n");
    }

    ok_dec(RegDeleteKeyA(hKey, "AKey"), ERROR_SUCCESS);
    ok_dec(RegDeleteKeyA(hKey, "BKey"), ERROR_SUCCESS);
    RegCloseKey(hKey);
    ok_dec(RegDeleteKeyA(HKEY_CURRENT_USER, BASE_KEY), ERROR_SUCCESS);
}
