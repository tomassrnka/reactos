/* ReactOS registry view tests. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "precomp.h"

static VOID CheckValue(HKEY Key, DWORD Expected)
{
    DWORD Value = 0, Size = sizeof(Value), Type = 0;
    LONG Error = RegQueryValueExW(Key, L"Value", NULL, &Type, (BYTE *)&Value, &Size);
    ok(Error == ERROR_SUCCESS, "query: %ld\n", Error);
    ok(Type == REG_DWORD && Size == sizeof(Value) && Value == Expected,
       "value %lu, type %lu, size %lu (expected %lu)\n", Value, Type, Size, Expected);
}

static VOID CheckShared(HKEY Root, PCWSTR Prefix)
{
    WCHAR Path[160];
    HKEY Key32, Key64;
    LONG Error;
    DWORD Value = 0x5a;
    _snwprintf(Path, _countof(Path), L"%ls\\Wow64Shared_%lu", Prefix, GetCurrentProcessId());
    Error = RegCreateKeyExW(Root, Path, 0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_32KEY, NULL, &Key32, NULL);
    ok(Error == ERROR_SUCCESS, "shared create %S: %ld\n", Path, Error);
    if (Error) return;
    ok(RegSetValueExW(Key32, L"Value", 0, REG_DWORD, (BYTE *)&Value, sizeof(Value)) == ERROR_SUCCESS, "shared set\n");
    Error = RegOpenKeyExW(Root, Path, 0, KEY_READ | KEY_WOW64_64KEY, &Key64);
    ok(Error == ERROR_SUCCESS, "shared open64 %S: %ld\n", Path, Error);
    if (!Error) { CheckValue(Key64, Value); RegCloseKey(Key64); }
    RegCloseKey(Key32);
    ok(RegDeleteKeyExW(Root, Path, KEY_WOW64_64KEY, 0) == ERROR_SUCCESS, "shared delete\n");
}

START_TEST(RegWow64)
{
    SYSTEM_INFO Native;
    HKEY Key32 = NULL, Key64 = NULL, Default = NULL, Physical = NULL, Child = NULL;
    WCHAR Path[128], PhysicalPath[160];
    LONG Error;
    DWORD Value;
    BOOL Wow64 = FALSE;
    GetNativeSystemInfo(&Native);
    IsWow64Process(GetCurrentProcess(), &Wow64);
    CheckShared(HKEY_CURRENT_USER, L"Software");
    CheckShared(HKEY_LOCAL_MACHINE, L"SYSTEM");
    CheckShared(HKEY_LOCAL_MACHINE, L"Software\\Classes");
    _snwprintf(Path, _countof(Path), L"Software\\Wow64ApiTest_%lu", GetCurrentProcessId());
    _snwprintf(PhysicalPath, _countof(PhysicalPath), L"Software\\Wow6432Node\\Wow64ApiTest_%lu", GetCurrentProcessId());
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_32KEY, NULL, &Key32, NULL);
    ok(Error == ERROR_SUCCESS, "create32: %ld\n", Error);
    if (Error) return;
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, NULL, &Key64, NULL);
    ok(Error == ERROR_SUCCESS, "create64: %ld\n", Error);
    if (Error) goto Cleanup;
    Value = 32;
    ok(RegSetValueExW(Key32, L"Value", 0, REG_DWORD, (BYTE *)&Value, sizeof(Value)) == ERROR_SUCCESS, "set32\n");
    Value = 64;
    ok(RegSetValueExW(Key64, L"Value", 0, REG_DWORD, (BYTE *)&Value, sizeof(Value)) == ERROR_SUCCESS, "set64\n");
    CheckValue(Key64, 64);
    CheckValue(Key32, Native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? 32 : 64);
    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_READ, &Default);
    ok(Error == ERROR_SUCCESS, "default open: %ld\n", Error);
    if (!Error) CheckValue(Default, Wow64 ? 32 : 64);
    if (Native.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) goto Cleanup;

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, PhysicalPath, 0, KEY_READ | KEY_WOW64_64KEY, &Physical);
    ok(Error == ERROR_SUCCESS, "physical Wow6432Node: %ld\n", Error);
    if (!Error) CheckValue(Physical, 32);
    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_READ | KEY_WOW64_32KEY | KEY_WOW64_64KEY, &Child);
    ok(Error == ERROR_INVALID_PARAMETER, "conflicting view flags: %ld\n", Error);
    if (!Error) { RegCloseKey(Child); Child = NULL; }
    Error = RegCreateKeyExW(Key32, L"Child", 0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_32KEY, NULL, &Child, NULL);
    ok(Error == ERROR_SUCCESS, "relative create32: %ld\n", Error);
    if (!Error)
    {
        Value = 32;
        RegSetValueExW(Child, L"Value", 0, REG_DWORD, (BYTE *)&Value, sizeof(Value));
        RegCloseKey(Child);
        Child = NULL;
        Error = RegOpenKeyExW(Key64, L"Child", 0, KEY_READ | KEY_WOW64_32KEY, &Child);
        ok(Error == ERROR_SUCCESS, "relative view switch: %ld\n", Error);
        if (!Error) { CheckValue(Child, 32); RegCloseKey(Child); Child = NULL; }
        ok(RegDeleteKeyExW(Key32, L"Child", KEY_WOW64_32KEY, 0) == ERROR_SUCCESS, "delete child\n");
    }
Cleanup:
    if (Physical) RegCloseKey(Physical);
    if (Default) RegCloseKey(Default);
    if (Key64) RegCloseKey(Key64);
    if (Key32) RegCloseKey(Key32);
    Error = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, Path, KEY_WOW64_64KEY, 0);
    ok(Error == ERROR_SUCCESS, "delete64: %ld\n", Error);
    if (Native.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
    {
        Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_READ | KEY_WOW64_32KEY, &Key32);
        ok(Error == ERROR_SUCCESS, "delete64 affected 32-bit view: %ld\n", Error);
        if (!Error) { CheckValue(Key32, 32); RegCloseKey(Key32); }
        Error = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, Path, KEY_WOW64_32KEY, 0);
        ok(Error == ERROR_SUCCESS, "delete32: %ld\n", Error);
    }
}
