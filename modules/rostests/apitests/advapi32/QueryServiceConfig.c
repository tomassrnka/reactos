/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for QueryServiceConfigA/W (layout of the returned structure)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

static BOOL InBuffer(const void *p, const void *Buffer, DWORD Size)
{
    return (const BYTE *)p >= (const BYTE *)Buffer && (const BYTE *)p < (const BYTE *)Buffer + Size;
}

static void TestW(SC_HANDLE Service)
{
    LPQUERY_SERVICE_CONFIGW Config;
    DWORD Needed = 0, Size;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = QueryServiceConfigW(Service, NULL, 0, &Needed);
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "Ret %d error %lu\n", Ret, GetLastError());
    ok(Needed > sizeof(*Config), "Needed %lu\n", Needed);
    Size = Needed;
    Config = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (!Config)
    {
        skip("No memory\n");
        return;
    }
    Ret = QueryServiceConfigW(Service, Config, Size, &Needed);
    ok(Ret, "QueryServiceConfigW failed %lu\n", GetLastError());
    if (Ret)
    {
        ok(Config->dwServiceType & SERVICE_WIN32, "dwServiceType 0x%lx\n", Config->dwServiceType);
        ok(Config->dwStartType <= SERVICE_DISABLED, "dwStartType %lu\n", Config->dwStartType);
        ok(Config->dwErrorControl <= SERVICE_ERROR_CRITICAL, "dwErrorControl %lu\n", Config->dwErrorControl);
        ok(Config->lpBinaryPathName && InBuffer(Config->lpBinaryPathName, Config, Size),
           "lpBinaryPathName %p (buffer %p+%lu)\n", Config->lpBinaryPathName, Config, Size);
        ok(Config->lpDisplayName && InBuffer(Config->lpDisplayName, Config, Size),
           "lpDisplayName %p (buffer %p+%lu)\n", Config->lpDisplayName, Config, Size);
        ok(!Config->lpLoadOrderGroup || InBuffer(Config->lpLoadOrderGroup, Config, Size),
           "lpLoadOrderGroup %p\n", Config->lpLoadOrderGroup);
        ok(!Config->lpDependencies || InBuffer(Config->lpDependencies, Config, Size),
           "lpDependencies %p\n", Config->lpDependencies);
        ok(!Config->lpServiceStartName || InBuffer(Config->lpServiceStartName, Config, Size),
           "lpServiceStartName %p\n", Config->lpServiceStartName);
        if (Config->lpBinaryPathName && InBuffer(Config->lpBinaryPathName, Config, Size))
        {
            WCHAR Path[MAX_PATH];
            lstrcpynW(Path, Config->lpBinaryPathName, ARRAYSIZE(Path));
            _wcslwr(Path);
            ok(wcsstr(Path, L".exe") != NULL, "lpBinaryPathName %s\n", wine_dbgstr_w(Config->lpBinaryPathName));
        }
        if (Config->lpDisplayName && InBuffer(Config->lpDisplayName, Config, Size))
            ok(Config->lpDisplayName[0] != 0, "empty lpDisplayName\n");
    }
    HeapFree(GetProcessHeap(), 0, Config);
}

static void TestA(SC_HANDLE Service)
{
    LPQUERY_SERVICE_CONFIGA Config;
    DWORD Needed = 0, Size;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = QueryServiceConfigA(Service, NULL, 0, &Needed);
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "Ret %d error %lu\n", Ret, GetLastError());
    Size = Needed;
    Config = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (!Config)
    {
        skip("No memory\n");
        return;
    }
    Ret = QueryServiceConfigA(Service, Config, Size, &Needed);
    ok(Ret, "QueryServiceConfigA failed %lu\n", GetLastError());
    if (Ret)
    {
        ok(Config->dwServiceType & SERVICE_WIN32, "dwServiceType 0x%lx\n", Config->dwServiceType);
        ok(Config->lpBinaryPathName && InBuffer(Config->lpBinaryPathName, Config, Size),
           "lpBinaryPathName %p (buffer %p+%lu)\n", Config->lpBinaryPathName, Config, Size);
        ok(Config->lpDisplayName && InBuffer(Config->lpDisplayName, Config, Size),
           "lpDisplayName %p (buffer %p+%lu)\n", Config->lpDisplayName, Config, Size);
        if (Config->lpBinaryPathName && InBuffer(Config->lpBinaryPathName, Config, Size))
        {
            CHAR Path[MAX_PATH];
            lstrcpynA(Path, Config->lpBinaryPathName, ARRAYSIZE(Path));
            _strlwr(Path);
            ok(strstr(Path, ".exe") != NULL, "lpBinaryPathName %s\n", Config->lpBinaryPathName);
        }
    }
    HeapFree(GetProcessHeap(), 0, Config);
}

START_TEST(QueryServiceConfig)
{
    SC_HANDLE Manager, Service;

    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    ok(Manager != NULL, "OpenSCManagerW failed %lu\n", GetLastError());
    if (!Manager)
        return;
    /* A service present on every installation */
    Service = OpenServiceW(Manager, L"EventLog", SERVICE_QUERY_CONFIG);
    ok(Service != NULL, "OpenServiceW failed %lu\n", GetLastError());
    if (Service)
    {
        TestW(Service);
        TestA(Service);
        CloseServiceHandle(Service);
    }
    CloseServiceHandle(Manager);
}
