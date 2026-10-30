/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         The calls .NET ServiceController and PowerShell (Get-Service, Set-Service) make:
 *                  the access masks of OpenService, QueryServiceConfig and QueryServiceConfig2 at levels
 *                  1 (description), 3 (delayed automatic start), 5 (SID type), 7 (preshutdown),
 *                  and ChangeServiceConfig2 at the same levels.
 *
 * The first part only reads the Spooler service and runs on Windows, with the results of Windows 10 22H2.
 * The second part creates a service and runs on ReactOS only.
 */

#include "precomp.h"

#define TEST_SERVICE  L"advapi32_apitest_ServiceControllerQuery"
#define TEST_KEY      L"System\\CurrentControlSet\\Services\\" TEST_SERVICE
#define SPOOLER       L"Spooler"

/* The structures of these levels are not in the headers of every compiler */
typedef struct { BOOL fDelayedAutostart; } TEST_DELAYED_AUTO_START_INFO;
typedef struct { DWORD dwServiceSidType; } TEST_SID_INFO;
typedef struct { DWORD dwPreshutdownTimeout; } TEST_PRESHUTDOWN_INFO;

#define LEVEL_DELAYED   3
#define LEVEL_SID       5
#define LEVEL_PRESHUT   7

/* Query a DWORD level the way ServiceController does: first the size, then the data */
static BOOL
QueryDwordLevel(SC_HANDLE hService, DWORD level, DWORD *value, DWORD *error)
{
    DWORD needed = 0xdeadbeef;
    BOOL ret;

    SetLastError(0xdeadbeef);
    ret = QueryServiceConfig2W(hService, level, NULL, 0, &needed);
    *error = GetLastError();
    if (ret || *error != ERROR_INSUFFICIENT_BUFFER)
        return FALSE;
    ok(needed == sizeof(DWORD), "level %lu: needed = %lu\n", level, needed);

    *value = 0xdeadbeef;
    SetLastError(0xdeadbeef);
    ret = QueryServiceConfig2W(hService, level, (LPBYTE)value, sizeof(*value), &needed);
    *error = GetLastError();
    return ret;
}

static void
TestSpooler(void)
{
    SC_HANDLE hScm, hService;
    SERVICE_STATUS_PROCESS ssp;
    BYTE buffer[1024];
    DWORD needed, error, value, level, i;
    BOOL ret;
    const DWORD dwordLevels[] = { LEVEL_DELAYED, LEVEL_SID, LEVEL_PRESHUT };

    hScm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hScm)
    {
        skip("OpenSCManager failed, error %lu\n", GetLastError());
        return;
    }

    /* ServiceController.Status */
    hService = OpenServiceW(hScm, SPOOLER, SERVICE_QUERY_STATUS);
    if (!hService)
    {
        skip("The Spooler service is not available, error %lu\n", GetLastError());
        CloseServiceHandle(hScm);
        return;
    }
    ret = QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed);
    ok(ret, "QueryServiceStatusEx failed, error %lu\n", GetLastError());

    /* SERVICE_QUERY_STATUS does not allow the dependents to be listed */
    {
        DWORD count;

        SetLastError(0xdeadbeef);
        ret = EnumDependentServicesW(hService, SERVICE_STATE_ALL, NULL, 0, &needed, &count);
        ok(!ret && GetLastError() == ERROR_ACCESS_DENIED, "EnumDependentServicesW with QUERY_STATUS: ret %d, error %lu\n", ret, GetLastError());
    }

    /* Without SERVICE_QUERY_CONFIG every query of the configuration is refused */
    SetLastError(0xdeadbeef);
    ret = QueryServiceConfigW(hService, NULL, 0, &needed);
    ok(!ret && GetLastError() == ERROR_ACCESS_DENIED, "QueryServiceConfigW: ret %d, error %lu\n", ret, GetLastError());
    ok(QueryDwordLevel(hService, SERVICE_CONFIG_DESCRIPTION, &value, &error) == FALSE && error == ERROR_ACCESS_DENIED,
       "level 1 without QUERY_CONFIG: error %lu\n", error);
    for (i = 0; i < ARRAYSIZE(dwordLevels); i++)
    {
        ret = QueryDwordLevel(hService, dwordLevels[i], &value, &error);
        ok(!ret && error == ERROR_ACCESS_DENIED, "level %lu without QUERY_CONFIG: ret %d, error %lu\n",
           dwordLevels[i], ret, error);
    }

    /* A level that does not exist is refused before the access is checked */
    for (level = 0; level <= 10; level += 10)
    {
        SetLastError(0xdeadbeef);
        ret = QueryServiceConfig2W(hService, level, buffer, sizeof(buffer), &needed);
        ok(!ret && GetLastError() == ERROR_INVALID_LEVEL, "level %lu: ret %d, error %lu\n", level, ret, GetLastError());
    }
    CloseServiceHandle(hService);

    /* ServiceController.DependentServices */
    hService = OpenServiceW(hScm, SPOOLER, SERVICE_ENUMERATE_DEPENDENTS);
    ok(hService != NULL, "OpenService(ENUMERATE_DEPENDENTS) failed, error %lu\n", GetLastError());
    if (hService)
    {
        DWORD count;

        SetLastError(0xdeadbeef);
        ret = EnumDependentServicesW(hService, SERVICE_STATE_ALL, NULL, 0, &needed, &count);
        error = GetLastError();
        ok(ret || error == ERROR_MORE_DATA, "EnumDependentServicesW: error %lu\n", error);
        SetLastError(0xdeadbeef);
        ret = EnumDependentServicesW(hService, SERVICE_STATE_ALL, (LPENUM_SERVICE_STATUSW)buffer, sizeof(buffer), &needed, &count);
        error = GetLastError();
        ok(ret || error == ERROR_MORE_DATA, "EnumDependentServicesW with a buffer: error %lu\n", error);
        CloseServiceHandle(hService);
    }

    /* ServiceController.StartType, and what PowerShell reads for UserName, BinaryPathName and Description */
    hService = OpenServiceW(hScm, SPOOLER, SERVICE_QUERY_CONFIG);
    ok(hService != NULL, "OpenService(QUERY_CONFIG) failed, error %lu\n", GetLastError());
    if (hService)
    {
        LPQUERY_SERVICE_CONFIGW config = (LPQUERY_SERVICE_CONFIGW)buffer;
        SERVICE_DESCRIPTIONW *desc = (SERVICE_DESCRIPTIONW *)buffer;
        TEST_DELAYED_AUTO_START_INFO delayed;
        BYTE small[2];

        SetLastError(0xdeadbeef);
        ret = QueryServiceConfigW(hService, NULL, 0, &needed);
        ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "QueryServiceConfigW: ret %d, error %lu\n", ret, GetLastError());
        ok(needed > sizeof(QUERY_SERVICE_CONFIGW), "needed = %lu\n", needed);

        ret = QueryServiceConfigW(hService, config, sizeof(buffer), &needed);
        ok(ret, "QueryServiceConfigW failed, error %lu\n", GetLastError());
        if (ret)
        {
            ok(config->lpBinaryPathName && *config->lpBinaryPathName, "no binary path\n");
            ok(config->lpServiceStartName && *config->lpServiceStartName, "no account\n");
            ok(config->dwStartType <= SERVICE_DISABLED, "start type %lu\n", config->dwStartType);
        }

        /* level 1: size, then data; the description may be NULL */
        SetLastError(0xdeadbeef);
        ret = QueryServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, NULL, 0, &needed);
        ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "level 1: ret %d, error %lu\n", ret, GetLastError());
        ok(needed >= sizeof(SERVICE_DESCRIPTIONW), "level 1: needed = %lu\n", needed);
        ret = QueryServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, buffer, sizeof(buffer), &needed);
        ok(ret, "level 1 failed, error %lu\n", GetLastError());
        if (ret && desc->lpDescription)
            ok((BYTE *)desc->lpDescription >= buffer && (BYTE *)desc->lpDescription < buffer + sizeof(buffer),
               "description points outside the buffer\n");

        /* levels 3, 5 and 7: a DWORD each */
        ret = QueryDwordLevel(hService, LEVEL_DELAYED, &value, &error);
        ok(ret, "level 3 (delayed automatic start) failed, error %lu\n", error);
        if (ret)
            ok(value <= 1, "delayed automatic start = %lu\n", value);

        ret = QueryDwordLevel(hService, LEVEL_SID, &value, &error);
        ok(ret, "level 5 (SID type) failed, error %lu\n", error);
        if (ret)
            ok(value == SERVICE_SID_TYPE_NONE || value == SERVICE_SID_TYPE_UNRESTRICTED || value == SERVICE_SID_TYPE_RESTRICTED,
               "SID type = %lu\n", value);

        ret = QueryDwordLevel(hService, LEVEL_PRESHUT, &value, &error);
        ok(ret, "level 7 (preshutdown) failed, error %lu\n", error);
        if (ret)
            ok(value != 0, "preshutdown timeout = %lu\n", value);

        /* a buffer smaller than the structure */
        SetLastError(0xdeadbeef);
        needed = 0;
        ret = QueryServiceConfig2W(hService, LEVEL_DELAYED, small, sizeof(small), &needed);
        ok(!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "level 3 small buffer: ret %d, error %lu\n", ret, GetLastError());
        ok(needed == sizeof(delayed), "level 3 small buffer: needed = %lu\n", needed);

        /* the ANSI function: the same levels */
        for (i = 0; i < ARRAYSIZE(dwordLevels); i++)
        {
            value = 0xdeadbeef;
            SetLastError(0xdeadbeef);
            ret = QueryServiceConfig2A(hService, dwordLevels[i], (LPBYTE)&value, sizeof(value), &needed);
            ok(ret, "QueryServiceConfig2A level %lu failed, error %lu\n", dwordLevels[i], GetLastError());
            ok(!ret || value != 0xdeadbeef, "QueryServiceConfig2A level %lu left the buffer alone\n", dwordLevels[i]);
        }

        /* nothing can be changed with a handle that may only query: the call is refused */
        for (i = 0; i < ARRAYSIZE(dwordLevels); i++)
        {
            DWORD data = 0;

            SetLastError(0xdeadbeef);
            ret = ChangeServiceConfig2W(hService, dwordLevels[i], &data);
            ok(!ret && GetLastError() == ERROR_ACCESS_DENIED, "ChangeServiceConfig2W level %lu: ret %d, error %lu\n",
               dwordLevels[i], ret, GetLastError());
        }
        for (level = 0; level <= 10; level += 10)
        {
            DWORD data = 0;

            SetLastError(0xdeadbeef);
            ret = ChangeServiceConfig2W(hService, level, &data);
            ok(!ret && GetLastError() == ERROR_INVALID_LEVEL, "ChangeServiceConfig2W level %lu: ret %d, error %lu\n",
               level, ret, GetLastError());
        }
        CloseServiceHandle(hService);
    }

    CloseServiceHandle(hScm);
}

static void
CheckLevel(SC_HANDLE hService, DWORD level, DWORD expected, PCSTR what)
{
    DWORD value = 0, error = 0;
    BOOL ret = QueryDwordLevel(hService, level, &value, &error);

    ok(ret, "%s: query failed, error %lu\n", what, error);
    if (ret)
        ok(value == expected, "%s = %lu, expected %lu\n", what, value, expected);
}

static void
CheckRegistryDword(PCWSTR name, BOOL expectPresent, DWORD expected)
{
    HKEY hKey;
    DWORD value = 0, size = sizeof(value), type = 0;
    LONG lRet = RegOpenKeyExW(HKEY_LOCAL_MACHINE, TEST_KEY, 0, KEY_QUERY_VALUE, &hKey);

    ok(lRet == ERROR_SUCCESS, "RegOpenKeyEx failed, error %ld\n", lRet);
    if (lRet != ERROR_SUCCESS)
        return;

    lRet = RegQueryValueExW(hKey, name, NULL, &type, (LPBYTE)&value, &size);
    if (expectPresent)
    {
        ok(lRet == ERROR_SUCCESS, "%ls is missing (%ld)\n", name, lRet);
        ok(type == REG_DWORD, "%ls: type %lu\n", name, type);
        ok(value == expected, "%ls = %lu, expected %lu\n", name, value, expected);
    }
    else
    {
        ok(lRet == ERROR_FILE_NOT_FOUND, "%ls: %ld\n", name, lRet);
    }
    RegCloseKey(hKey);
}

/* The changes Set-Service and New-Service make; the service is ours */
static void
TestOwnService(void)
{
    SC_HANDLE hScm, hService, hQuery;
    TEST_DELAYED_AUTO_START_INFO delayed;
    TEST_SID_INFO sid;
    TEST_PRESHUTDOWN_INFO preshutdown;
    SERVICE_DESCRIPTIONW desc;
    WCHAR text[] = L"Set by the test";
    BYTE buffer[512];
    DWORD needed, error, value;
    BOOL ret;

    hScm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hScm)
    {
        skip("OpenSCManager failed, error %lu\n", GetLastError());
        return;
    }

    hService = OpenServiceW(hScm, TEST_SERVICE, DELETE);
    if (hService)
    {
        DeleteService(hService);
        CloseServiceHandle(hService);
    }

    hService = OpenServiceW(hScm, TEST_SERVICE L"_Dependent", DELETE);
    if (hService)
    {
        DeleteService(hService);
        CloseServiceHandle(hService);
    }

    hService = CreateServiceW(hScm, TEST_SERVICE, L"ServiceControllerQuery test", SERVICE_ALL_ACCESS,
                              SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                              L"%SystemRoot%\\system32\\cmd.exe", NULL, NULL, NULL, NULL, NULL);
    ok(hService != NULL, "CreateService failed, error %lu\n", GetLastError());
    if (!hService)
    {
        CloseServiceHandle(hScm);
        return;
    }

    /* defaults of a new service */
    CheckLevel(hService, LEVEL_DELAYED, 0, "default delayed automatic start");
    CheckLevel(hService, LEVEL_SID, SERVICE_SID_TYPE_NONE, "default SID type");
    CheckLevel(hService, LEVEL_PRESHUT, 10000, "default preshutdown timeout");

    /* Set-Service -Description */
    desc.lpDescription = text;
    SetLastError(0xdeadbeef);
    ret = ChangeServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, &desc);
    ok(ret, "ChangeServiceConfig2W(description) failed, error %lu\n", GetLastError());
    ret = QueryServiceConfig2W(hService, SERVICE_CONFIG_DESCRIPTION, buffer, sizeof(buffer), &needed);
    ok(ret, "QueryServiceConfig2W(description) failed, error %lu\n", GetLastError());
    if (ret)
    {
        SERVICE_DESCRIPTIONW *got = (SERVICE_DESCRIPTIONW *)buffer;
        ok(got->lpDescription && !wcscmp(got->lpDescription, text), "description = %ls\n",
           got->lpDescription ? got->lpDescription : L"(null)");
    }

    /* Set-Service -StartupType Manual: the delayed start flag is written with FALSE, then TRUE for AutomaticDelayedStart */
    delayed.fDelayedAutostart = FALSE;
    SetLastError(0xdeadbeef);
    ret = ChangeServiceConfig2W(hService, LEVEL_DELAYED, &delayed);
    ok(ret, "ChangeServiceConfig2W(delayed = FALSE) failed, error %lu\n", GetLastError());
    CheckLevel(hService, LEVEL_DELAYED, 0, "delayed automatic start");

    delayed.fDelayedAutostart = TRUE;
    SetLastError(0xdeadbeef);
    ret = ChangeServiceConfig2W(hService, LEVEL_DELAYED, &delayed);
    ok(ret, "ChangeServiceConfig2W(delayed = TRUE) failed, error %lu\n", GetLastError());
    CheckLevel(hService, LEVEL_DELAYED, 1, "delayed automatic start");
    CheckRegistryDword(L"DelayedAutostart", TRUE, 1);

    /* Windows refuses anything but FALSE and TRUE */
    delayed.fDelayedAutostart = 3;
    SetLastError(0xdeadbeef);
    ret = ChangeServiceConfig2W(hService, LEVEL_DELAYED, &delayed);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "delayed = 3: ret %d, error %lu\n", ret, GetLastError());
    CheckLevel(hService, LEVEL_DELAYED, 1, "delayed automatic start after a refused change");

    delayed.fDelayedAutostart = FALSE;
    ret = ChangeServiceConfigW(hService, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
                               NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    ok(ret, "ChangeServiceConfigW failed, error %lu\n", GetLastError());
    ret = ChangeServiceConfig2A(hService, LEVEL_DELAYED, &delayed);
    ok(ret, "ChangeServiceConfig2A(delayed) failed, error %lu\n", GetLastError());
    CheckLevel(hService, LEVEL_DELAYED, 0, "delayed automatic start after the ANSI call");

    /* SID type */
    sid.dwServiceSidType = SERVICE_SID_TYPE_UNRESTRICTED;
    SetLastError(0xdeadbeef);
    ret = ChangeServiceConfig2W(hService, LEVEL_SID, &sid);
    ok(ret, "ChangeServiceConfig2W(SID type) failed, error %lu\n", GetLastError());
    CheckLevel(hService, LEVEL_SID, SERVICE_SID_TYPE_UNRESTRICTED, "SID type");
    sid.dwServiceSidType = 2;
    SetLastError(0xdeadbeef);
    ret = ChangeServiceConfig2W(hService, LEVEL_SID, &sid);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "SID type 2: ret %d, error %lu\n", ret, GetLastError());

    /* preshutdown timeout */
    preshutdown.dwPreshutdownTimeout = 30000;
    SetLastError(0xdeadbeef);
    ret = ChangeServiceConfig2W(hService, LEVEL_PRESHUT, &preshutdown);
    ok(ret, "ChangeServiceConfig2W(preshutdown) failed, error %lu\n", GetLastError());
    CheckLevel(hService, LEVEL_PRESHUT, 30000, "preshutdown timeout");
    CheckRegistryDword(L"PreshutdownTimeout", TRUE, 30000);

    /* a handle that may only query refuses the changes, and keeps reading */
    hQuery = OpenServiceW(hScm, TEST_SERVICE, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
    ok(hQuery != NULL, "OpenService failed, error %lu\n", GetLastError());
    if (hQuery)
    {
        delayed.fDelayedAutostart = TRUE;
        SetLastError(0xdeadbeef);
        ret = ChangeServiceConfig2W(hQuery, LEVEL_DELAYED, &delayed);
        ok(!ret && GetLastError() == ERROR_ACCESS_DENIED, "change with a query handle: ret %d, error %lu\n", ret, GetLastError());
        CheckLevel(hQuery, LEVEL_DELAYED, 0, "delayed automatic start through a query handle");
        CloseServiceHandle(hQuery);
    }

    /* a handle that may change the configuration but not read it */
    hQuery = OpenServiceW(hScm, TEST_SERVICE, SERVICE_CHANGE_CONFIG);
    ok(hQuery != NULL, "OpenService failed, error %lu\n", GetLastError());
    if (hQuery)
    {
        SetLastError(0xdeadbeef);
        ret = QueryDwordLevel(hQuery, LEVEL_DELAYED, &value, &error);
        ok(!ret && error == ERROR_ACCESS_DENIED, "query with a change handle: ret %d, error %lu\n", ret, error);
        delayed.fDelayedAutostart = TRUE;
        SetLastError(0xdeadbeef);
        ret = ChangeServiceConfig2W(hQuery, LEVEL_DELAYED, &delayed);
        ok(ret, "change with a change handle failed, error %lu\n", GetLastError());
        CloseServiceHandle(hQuery);
    }
    CheckLevel(hService, LEVEL_DELAYED, 1, "delayed automatic start after the change handle");

    /* ServiceController.DependentServices: one service depends on ours */
    {
        SC_HANDLE hDependent, hEnum;
        DWORD count = 0;

        hDependent = CreateServiceW(hScm, TEST_SERVICE L"_Dependent", NULL, SERVICE_ALL_ACCESS,
                                    SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                                    L"%SystemRoot%\\system32\\cmd.exe", NULL, NULL, TEST_SERVICE L"\0", NULL, NULL);
        ok(hDependent != NULL, "CreateService(dependent) failed, error %lu\n", GetLastError());

        hEnum = OpenServiceW(hScm, TEST_SERVICE, SERVICE_ENUMERATE_DEPENDENTS);
        ok(hEnum != NULL, "OpenService failed, error %lu\n", GetLastError());
        if (hEnum && hDependent)
        {
            LPENUM_SERVICE_STATUSW list = (LPENUM_SERVICE_STATUSW)buffer;

            SetLastError(0xdeadbeef);
            ret = EnumDependentServicesW(hEnum, SERVICE_STATE_ALL, list, sizeof(buffer), &needed, &count);
            ok(ret, "EnumDependentServicesW failed, error %lu\n", GetLastError());
            ok(count == 1, "%lu dependent services\n", count);
            if (ret && count == 1)
                ok(!_wcsicmp(list[0].lpServiceName, TEST_SERVICE L"_Dependent"), "dependent = %ls\n", list[0].lpServiceName);

            /* it is stopped */
            SetLastError(0xdeadbeef);
            ret = EnumDependentServicesW(hEnum, SERVICE_ACTIVE, list, sizeof(buffer), &needed, &count);
            ok(ret, "EnumDependentServicesW(SERVICE_ACTIVE) failed, error %lu\n", GetLastError());
            ok(count == 0, "%lu active dependent services\n", count);
        }
        if (hEnum)
            CloseServiceHandle(hEnum);
        if (hDependent)
        {
            DeleteService(hDependent);
            CloseServiceHandle(hDependent);
        }
    }

    ret = DeleteService(hService);
    ok(ret, "DeleteService failed, error %lu\n", GetLastError());
    CloseServiceHandle(hService);
    CloseServiceHandle(hScm);
}

START_TEST(ServiceControllerQuery)
{
    TestSpooler();

    if (!is_reactos())
    {
        skip("Creating a service is only done on ReactOS\n");
        return;
    }

    TestOwnService();
}
