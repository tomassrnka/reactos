/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the version resource of ucrtbase and the Visual C++ runtime registry keys
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winver.h>

static void test_version_resource(void)
{
    WCHAR path[MAX_PATH];
    DWORD handle, size;
    VS_FIXEDFILEINFO *info;
    BYTE *data;
    UINT infolen;
    BOOL ret;

    GetSystemDirectoryW(path, ARRAYSIZE(path));
    lstrcatW(path, L"\\ucrtbase.dll");

    SetLastError(0xdeadbeef);
    size = GetFileVersionInfoSizeW(path, &handle);
    ok(size != 0, "GetFileVersionInfoSizeW failed, error %lu\n", GetLastError());
    if (!size)
        return;

    data = HeapAlloc(GetProcessHeap(), 0, size);
    ret = GetFileVersionInfoW(path, 0, size, data);
    ok(ret, "GetFileVersionInfoW failed, error %lu\n", GetLastError());
    if (ret)
    {
        info = NULL;
        ret = VerQueryValueW(data, L"\\", (void **)&info, &infolen);
        ok(ret && info && infolen >= sizeof(*info), "VerQueryValueW root failed\n");
        if (ret && info)
        {
            ok(info->dwSignature == 0xfeef04bd, "signature %08lx\n", info->dwSignature);
            ok(HIWORD(info->dwFileVersionMS) >= 10, "file version %lu.%lu\n",
               HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS));
            ok(info->dwFileType == VFT_DLL, "file type %lu\n", info->dwFileType);
        }
    }
    HeapFree(GetProcessHeap(), 0, data);
}

static void check_runtime_key(const WCHAR *prefix, const WCHAR *arch)
{
    WCHAR path[128];
    HKEY key;
    DWORD type, value, size;
    LONG ret;

    lstrcpyW(path, prefix);
    lstrcatW(path, L"Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\");
    lstrcatW(path, arch);
    ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key);
    ok(ret == ERROR_SUCCESS, "%s: RegOpenKeyExW returned %ld\n", wine_dbgstr_w(path), ret);
    if (ret != ERROR_SUCCESS)
        return;

    value = 0;
    size = sizeof(value);
    ret = RegQueryValueExW(key, L"Installed", NULL, &type, (BYTE *)&value, &size);
    ok(ret == ERROR_SUCCESS && type == REG_DWORD && value == 1, "%s: Installed %ld type %lu value %lu\n",
       wine_dbgstr_w(arch), ret, type, value);

    value = 0;
    size = sizeof(value);
    ret = RegQueryValueExW(key, L"Major", NULL, &type, (BYTE *)&value, &size);
    ok(ret == ERROR_SUCCESS && type == REG_DWORD && value == 14, "%s: Major %ld type %lu value %lu\n",
       wine_dbgstr_w(arch), ret, type, value);

    size = 0;
    ret = RegQueryValueExW(key, L"Version", NULL, &type, NULL, &size);
    ok(ret == ERROR_SUCCESS && type == REG_SZ, "%s: Version %ld type %lu\n", wine_dbgstr_w(arch), ret, type);
    RegCloseKey(key);
}

static void test_runtime_keys(void)
{
#ifdef _WIN64
    /* The 64-bit view has the 64-bit runtime; the 32-bit one is in the Wow6432Node subtree */
    check_runtime_key(L"SOFTWARE\\", L"x64");
    check_runtime_key(L"SOFTWARE\\Wow6432Node\\", L"x86");
#else
    /* On a 64-bit system a 32-bit process sees the Wow6432Node copy through the default view */
    check_runtime_key(L"SOFTWARE\\", L"x86");
#endif
}

START_TEST(version)
{
    test_version_resource();
    test_runtime_keys();
}
