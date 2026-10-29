/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test RegDeleteTree/RegDeleteKeyEx on open handles, as .NET uses them
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ndk/cmfuncs.h>
#include <ndk/obfuncs.h>

/*
 * .NET RegistryKey.DeleteSubKeyTree(name) opens the key with KEY_READ |
 * KEY_WRITE, calls RegDeleteTree(key, "") to delete its subkeys and values,
 * then RegDeleteKeyEx(key, "", view, 0) to delete the key itself, and closes
 * the handle. RegDeleteTree with an empty subkey name must therefore leave
 * the key in place. The other cases pin down what Windows 10 returns for a
 * key deleted while handles to it are open: the old handles see
 * ERROR_KEY_DELETED, new opens see ERROR_FILE_NOT_FOUND, and a re-create of
 * the name makes a new, independent key.
 */

#define TEST_ROOT L"Software\\ReactOS RegDeleteTreeOpenHandles"

static LSTATUS (WINAPI *pRegDeleteTreeW)(HKEY, LPCWSTR);
#define RegDeleteTreeW pRegDeleteTreeW

static
VOID
MakeKey(
    _In_ PCWSTR Path,
    _In_ INT Variant)
{
    HKEY Key, Sub1, Sub2;
    DWORD Data = 7;
    LONG Error;

    Error = RegCreateKeyExW(HKEY_CURRENT_USER, Path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL);
    ok_long(Error, ERROR_SUCCESS);
    if (Error)
        return;
    if (Variant >= 1)
        ok_long(RegSetValueExW(Key, L"v", 0, REG_DWORD, (PBYTE)&Data, sizeof(Data)), ERROR_SUCCESS);
    if (Variant >= 2)
    {
        ok_long(RegCreateKeyExW(Key, L"sub1", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Sub1, NULL), ERROR_SUCCESS);
        ok_long(RegSetValueExW(Sub1, L"sv", 0, REG_DWORD, (PBYTE)&Data, sizeof(Data)), ERROR_SUCCESS);
        ok_long(RegCreateKeyExW(Sub1, L"sub2", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Sub2, NULL), ERROR_SUCCESS);
        RegCloseKey(Sub2);
        RegCloseKey(Sub1);
    }
    RegCloseKey(Key);
}

static
LONG
OpenByName(
    _In_ PCWSTR Path)
{
    HKEY Key;
    LONG Error;

    Error = RegOpenKeyExW(HKEY_CURRENT_USER, Path, 0, KEY_READ, &Key);
    if (Error == ERROR_SUCCESS)
        RegCloseKey(Key);
    return Error;
}

static
NTSTATUS
NtOpenRelative(
    _In_ HANDLE Root,
    _In_ ACCESS_MASK Access)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING Empty = RTL_CONSTANT_STRING(L"");
    HANDLE Key = NULL;
    NTSTATUS Status;

    InitializeObjectAttributes(&ObjectAttributes, &Empty, OBJ_CASE_INSENSITIVE, Root, NULL);
    Status = NtOpenKey(&Key, Access, &ObjectAttributes);
    if (NT_SUCCESS(Status))
        NtClose(Key);
    return Status;
}

/* RegistryKey.DeleteSubKeyTree, call for call */
static
VOID
TestDotNetDeleteSubKeyTree(
    _In_ INT Variant)
{
    WCHAR Path[MAX_PATH];
    HKEY Key;
    DWORD SubKeys = 0xdead, Values = 0xdead;
    LONG Error;

    StringCchPrintfW(Path, _countof(Path), TEST_ROOT L"\\DotNet%d", Variant);
    trace("DeleteSubKeyTree variant %d\n", Variant);
    MakeKey(Path, Variant);

    Error = RegOpenKeyExW(HKEY_CURRENT_USER, Path, 0, KEY_READ | KEY_WRITE, &Key);
    ok_long(Error, ERROR_SUCCESS);
    if (Error)
        return;

    /* Deletes subkeys and values, not the key */
    ok_long(RegDeleteTreeW(Key, L""), ERROR_SUCCESS);
    ok_long(OpenByName(Path), ERROR_SUCCESS);
    ok_long(RegQueryInfoKeyW(Key, NULL, NULL, NULL, &SubKeys, NULL, NULL, &Values, NULL, NULL, NULL, NULL), ERROR_SUCCESS);
    ok_long(SubKeys, 0);
    ok_long(Values, 0);

    /* Now the key itself, through the same handle */
    ok_long(RegDeleteKeyExW(Key, L"", 0, 0), ERROR_SUCCESS);
    ok_long(OpenByName(Path), ERROR_FILE_NOT_FOUND);
    ok_long(RegQueryInfoKeyW(Key, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL), ERROR_KEY_DELETED);

    ok_long(RegCloseKey(Key), ERROR_SUCCESS);
    ok_long(OpenByName(Path), ERROR_FILE_NOT_FOUND);
    RegDeleteTreeW(HKEY_CURRENT_USER, Path);
}

/* NULL subkey name: same as "" */
static
VOID
TestDeleteTreeNull(VOID)
{
    PCWSTR Path = TEST_ROOT L"\\TreeNull";
    HKEY Key;
    DWORD SubKeys = 0xdead, Values = 0xdead;

    MakeKey(Path, 2);
    ok_long(RegOpenKeyExW(HKEY_CURRENT_USER, Path, 0, KEY_ALL_ACCESS, &Key), ERROR_SUCCESS);
    ok_long(RegDeleteTreeW(Key, NULL), ERROR_SUCCESS);
    ok_long(OpenByName(Path), ERROR_SUCCESS);
    ok_long(RegQueryInfoKeyW(Key, NULL, NULL, NULL, &SubKeys, NULL, NULL, &Values, NULL, NULL, NULL, NULL), ERROR_SUCCESS);
    ok_long(SubKeys, 0);
    ok_long(Values, 0);
    RegCloseKey(Key);

    /* A real subkey name deletes that key */
    MakeKey(Path, 2);
    ok_long(RegDeleteTreeW(HKEY_CURRENT_USER, Path), ERROR_SUCCESS);
    ok_long(OpenByName(Path), ERROR_FILE_NOT_FOUND);
}

/* Delete by name with two handles open, operations on the old handles, re-create */
static
VOID
TestOpenHandles(VOID)
{
    PCWSTR Path = TEST_ROOT L"\\OpenHandles";
    HKEY Key1, Key2, NewKey, Temp;
    DWORD Data = 5, Size, Type, Disposition = 0, SubKeys = 0xdead, Values = 0xdead;
    WCHAR Name[64];

    MakeKey(Path, 1);
    ok_long(RegOpenKeyExW(HKEY_CURRENT_USER, Path, 0, KEY_ALL_ACCESS, &Key1), ERROR_SUCCESS);
    ok_long(RegOpenKeyExW(HKEY_CURRENT_USER, Path, 0, KEY_READ, &Key2), ERROR_SUCCESS);

    ok_long(RegDeleteKeyW(HKEY_CURRENT_USER, Path), ERROR_SUCCESS);

    /* The old handles refer to a deleted key */
    Size = sizeof(Data);
    ok_long(RegQueryValueExW(Key1, L"v", NULL, &Type, (PBYTE)&Data, &Size), ERROR_KEY_DELETED);
    ok_long(RegSetValueExW(Key1, L"w", 0, REG_DWORD, (PBYTE)&Data, sizeof(Data)), ERROR_KEY_DELETED);
    ok_long(RegDeleteValueW(Key1, L"v"), ERROR_KEY_DELETED);
    Size = _countof(Name);
    ok_long(RegEnumKeyExW(Key1, 0, Name, &Size, NULL, NULL, NULL, NULL), ERROR_KEY_DELETED);
    Size = _countof(Name);
    ok_long(RegEnumValueW(Key2, 0, Name, &Size, NULL, NULL, NULL, NULL), ERROR_KEY_DELETED);
    ok_long(RegQueryInfoKeyW(Key2, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL), ERROR_KEY_DELETED);
    ok_long(RegCreateKeyExW(Key1, L"child", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Temp, NULL), ERROR_KEY_DELETED);
    ok_long(RegOpenKeyExW(Key1, L"", 0, KEY_READ, &Temp), ERROR_KEY_DELETED);
    ok_hex(NtOpenRelative(Key1, KEY_READ), STATUS_KEY_DELETED);
    ok_hex(NtOpenRelative(Key1, DELETE), STATUS_KEY_DELETED);
    ok_hex(NtDeleteKey(Key1), STATUS_SUCCESS);
    ok_hex(NtFlushKey(Key1), STATUS_KEY_DELETED);
    ok_long(RegDeleteKeyExW(Key1, L"", 0, 0), ERROR_KEY_DELETED);
    ok_long(RegDeleteTreeW(Key1, L""), ERROR_KEY_DELETED);
    ok_long(RegDeleteTreeW(Key1, NULL), ERROR_KEY_DELETED);

    /* New opens do not find it */
    ok_long(OpenByName(Path), ERROR_FILE_NOT_FOUND);

    /* Re-create the name while the old handles are open: a new, empty key */
    ok_long(RegCreateKeyExW(HKEY_CURRENT_USER, Path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &NewKey, &Disposition), ERROR_SUCCESS);
    ok_long(Disposition, REG_CREATED_NEW_KEY);
    Data = 9;
    ok_long(RegSetValueExW(NewKey, L"n", 0, REG_DWORD, (PBYTE)&Data, sizeof(Data)), ERROR_SUCCESS);
    Size = sizeof(Data);
    ok_long(RegQueryValueExW(NewKey, L"v", NULL, &Type, (PBYTE)&Data, &Size), ERROR_FILE_NOT_FOUND);
    Size = sizeof(Data);
    ok_long(RegQueryValueExW(Key1, L"n", NULL, &Type, (PBYTE)&Data, &Size), ERROR_KEY_DELETED);
    ok_long(RegSetValueExW(Key1, L"x", 0, REG_DWORD, (PBYTE)&Data, sizeof(Data)), ERROR_KEY_DELETED);
    ok_long(RegQueryInfoKeyW(NewKey, NULL, NULL, NULL, &SubKeys, NULL, NULL, &Values, NULL, NULL, NULL, NULL), ERROR_SUCCESS);
    ok_long(SubKeys, 0);
    ok_long(Values, 1);

    ok_long(RegCloseKey(Key1), ERROR_SUCCESS);
    ok_long(RegCloseKey(Key2), ERROR_SUCCESS);

    /* Closing the old handles leaves the new key alone */
    ok_long(RegOpenKeyExW(HKEY_CURRENT_USER, Path, 0, KEY_READ, &Temp), ERROR_SUCCESS);
    Data = 0;
    Size = sizeof(Data);
    ok_long(RegQueryValueExW(Temp, L"n", NULL, &Type, (PBYTE)&Data, &Size), ERROR_SUCCESS);
    ok_long(Data, 9);
    RegCloseKey(Temp);

    /* Delete through the handle itself; a second delete sees the deleted key */
    ok_long(RegDeleteKeyExW(NewKey, L"", 0, 0), ERROR_SUCCESS);
    ok_long(RegDeleteKeyExW(NewKey, L"", 0, 0), ERROR_KEY_DELETED);
    RegCloseKey(NewKey);
    ok_long(OpenByName(Path), ERROR_FILE_NOT_FOUND);
    RegDeleteTreeW(HKEY_CURRENT_USER, Path);
}

/* A handle to a grandchild is open while the whole tree is deleted */
static
VOID
TestChildHandleOpen(VOID)
{
    PCWSTR Path = TEST_ROOT L"\\Parent";
    PCWSTR Deep = TEST_ROOT L"\\Parent\\sub1\\sub2";
    HKEY Child, Temp;

    MakeKey(Path, 2);
    ok_long(RegOpenKeyExW(HKEY_CURRENT_USER, Deep, 0, KEY_ALL_ACCESS, &Child), ERROR_SUCCESS);
    ok_long(RegDeleteTreeW(HKEY_CURRENT_USER, Path), ERROR_SUCCESS);
    ok_long(RegQueryInfoKeyW(Child, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL), ERROR_KEY_DELETED);
    ok_long(OpenByName(Path), ERROR_FILE_NOT_FOUND);
    ok_long(RegCreateKeyExW(HKEY_CURRENT_USER, Deep, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Temp, NULL), ERROR_SUCCESS);
    RegCloseKey(Temp);
    RegCloseKey(Child);
    ok_long(RegDeleteTreeW(HKEY_CURRENT_USER, Path), ERROR_SUCCESS);
}

START_TEST(RegDeleteTreeOpenHandles)
{
    HKEY Root;

    pRegDeleteTreeW = (PVOID)GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "RegDeleteTreeW");
    if (!pRegDeleteTreeW)
    {
        skip("RegDeleteTreeW is not available\n");
        return;
    }

    RegDeleteTreeW(HKEY_CURRENT_USER, TEST_ROOT);
    ok_long(RegCreateKeyExW(HKEY_CURRENT_USER, TEST_ROOT, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Root, NULL), ERROR_SUCCESS);
    RegCloseKey(Root);

    TestDotNetDeleteSubKeyTree(0);
    TestDotNetDeleteSubKeyTree(1);
    TestDotNetDeleteSubKeyTree(2);
    TestDeleteTreeNull();
    TestOpenHandles();
    TestChildHandleOpen();

    ok_long(RegDeleteTreeW(HKEY_CURRENT_USER, TEST_ROOT), ERROR_SUCCESS);
    ok_long(OpenByName(TEST_ROOT), ERROR_FILE_NOT_FOUND);
}
