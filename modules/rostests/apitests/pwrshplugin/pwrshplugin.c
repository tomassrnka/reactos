/*
 * PROJECT:     ReactOS apitests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for pwrshplugin.dll (the WSMan plugin placeholder PowerShell 7's MSI looks for)
 * COPYRIGHT:   Copyright 2026 reactos-nitro contributors
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <winver.h>
#include <winerror.h>
#include <stdio.h>

typedef DWORD (WINAPI *PFN_STARTUP)(DWORD, PCWSTR, PCWSTR, PVOID *);
typedef DWORD (WINAPI *PFN_SHUTDOWN)(PVOID, DWORD, DWORD);

static const char *g_exports[] =
{
    "GetCLRVersionForPSVersion",
    "PerformWSManPluginReportCompletion",
    "WSManPluginCommand",
    "WSManPluginConnect",
    "WSManPluginReceive",
    "WSManPluginReleaseCommandContext",
    "WSManPluginReleaseShellContext",
    "WSManPluginSend",
    "WSManPluginShell",
    "WSManPluginShutdown",
    "WSManPluginSignal",
    "WSManPluginStartup",
};

static void test_version(const WCHAR *path)
{
    DWORD handle, size, ms, ls;
    VS_FIXEDFILEINFO *ffi = NULL;
    UINT len = 0;
    void *data;

    size = GetFileVersionInfoSizeW(path, &handle);
    ok(size != 0, "no version resource (error %lu)\n", GetLastError());
    if (!size)
        return;
    data = HeapAlloc(GetProcessHeap(), 0, size);
    ok(GetFileVersionInfoW(path, 0, size, data), "GetFileVersionInfo failed\n");
    ok(VerQueryValueW(data, L"\\", (void **)&ffi, &len) && ffi, "VerQueryValue failed\n");
    if (ffi)
    {
        ms = ffi->dwFileVersionMS;
        ls = ffi->dwFileVersionLS;
        trace("file version %u.%u.%u.%u\n", HIWORD(ms), LOWORD(ms), HIWORD(ls), LOWORD(ls));
        /* The PowerShell 7 MSI requires MinVersion 6.3.9600.16383 */
        ok(ms > MAKELONG(3, 6) || (ms == MAKELONG(3, 6) && ls >= MAKELONG(16383, 9600)),
           "version too low: %lx %lx\n", ms, ls);
    }
    HeapFree(GetProcessHeap(), 0, data);
}

START_TEST(pwrshplugin)
{
    WCHAR path[MAX_PATH];
    HMODULE mod;
    PFN_STARTUP startup;
    PFN_SHUTDOWN shutdown;
    PVOID ctx;
    DWORD ret;
    size_t i;

    GetSystemDirectoryW(path, MAX_PATH);
    lstrcatW(path, L"\\pwrshplugin.dll");

    mod = LoadLibraryW(path);
    ok(mod != NULL, "LoadLibrary(%ls) failed: %lu\n", path, GetLastError());
    if (!mod)
        return;

    test_version(path);

    for (i = 0; i < sizeof(g_exports) / sizeof(g_exports[0]); i++)
        ok(GetProcAddress(mod, g_exports[i]) != NULL, "missing export %s\n", g_exports[i]);

    startup = (PFN_STARTUP)GetProcAddress(mod, "WSManPluginStartup");
    shutdown = (PFN_SHUTDOWN)GetProcAddress(mod, "WSManPluginShutdown");
    if (startup)
    {
        ctx = (PVOID)(ULONG_PTR)0x1234;
        ret = startup(0, L"PowerShell", NULL, &ctx);
        ok(ret == ERROR_NOT_SUPPORTED, "Startup returned %lu\n", ret);
        ok(ctx == NULL, "plugin context handed out: %p\n", ctx);
        ret = startup(0, NULL, NULL, NULL);
        ok(ret == ERROR_NOT_SUPPORTED, "Startup(NULL) returned %lu\n", ret);
    }
    if (shutdown)
    {
        ret = shutdown(NULL, 0, 0);
        ok(ret == ERROR_NOT_SUPPORTED, "Shutdown returned %lu\n", ret);
    }

    FreeLibrary(mod);
}
