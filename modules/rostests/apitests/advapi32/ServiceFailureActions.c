/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Test for the FailureActions registry value of a service
 */

#include "precomp.h"

#define TEST_SERVICE L"advapi32_apitest_FailureActions"
#define TEST_KEY     L"System\\CurrentControlSet\\Services\\" TEST_SERVICE

/* Five DWORDs, then the actions; the same on every architecture */
#define HEADER_SIZE 20

static BOOL
ReadValue(HKEY hKey, PCWSTR name, PBYTE buffer, DWORD *size, DWORD *type)
{
    LONG ret = RegQueryValueExW(hKey, name, NULL, type, buffer, size);
    return ret == ERROR_SUCCESS;
}

static void
CheckRegistry(HKEY hKey, DWORD resetPeriod, DWORD cActions, const SC_ACTION *actions)
{
    BYTE buffer[256];
    DWORD size = sizeof(buffer), type = 0, i;
    DWORD *header = (DWORD *)buffer;

    ok(ReadValue(hKey, L"FailureActions", buffer, &size, &type), "FailureActions is missing\n");
    ok(type == REG_BINARY, "type = %lu\n", type);
    ok(size == HEADER_SIZE + cActions * sizeof(SC_ACTION), "size = %lu, expected %lu\n",
       size, (DWORD)(HEADER_SIZE + cActions * sizeof(SC_ACTION)));
    if (size < HEADER_SIZE)
        return;

    ok(header[0] == resetPeriod, "reset period = %lu, expected %lu\n", header[0], resetPeriod);
    ok(header[3] == cActions, "action count = %lu, expected %lu\n", header[3], cActions);
    ok(header[4] == HEADER_SIZE, "action offset = %lu, expected %u\n", header[4], HEADER_SIZE);

    for (i = 0; i < cActions && size >= HEADER_SIZE + (i + 1) * sizeof(SC_ACTION); i++)
    {
        SC_ACTION *a = (SC_ACTION *)(buffer + HEADER_SIZE) + i;
        ok(a->Type == actions[i].Type, "action %lu type = %d, expected %d\n", i, a->Type, actions[i].Type);
        ok(a->Delay == actions[i].Delay, "action %lu delay = %lu, expected %lu\n", i, a->Delay, actions[i].Delay);
    }
}

static void
CheckQuery(SC_HANDLE hService, DWORD resetPeriod, DWORD cActions, const SC_ACTION *actions,
           PCWSTR command, PCWSTR rebootMsg)
{
    BYTE buffer[1024];
    DWORD needed = 0, i;
    SERVICE_FAILURE_ACTIONSW *fa = (SERVICE_FAILURE_ACTIONSW *)buffer;
    SERVICE_FAILURE_ACTIONSA *faA = (SERVICE_FAILURE_ACTIONSA *)buffer;
    BOOL ret;

    ret = QueryServiceConfig2W(hService, SERVICE_CONFIG_FAILURE_ACTIONS, buffer, sizeof(buffer), &needed);
    ok(ret, "QueryServiceConfig2W failed, error %lu\n", GetLastError());
    if (!ret)
        return;

    ok(fa->dwResetPeriod == resetPeriod, "W reset period = %lu\n", fa->dwResetPeriod);
    ok(fa->cActions == cActions, "W action count = %lu, expected %lu\n", fa->cActions, cActions);
    if (fa->cActions == cActions && cActions > 0)
    {
        ok(fa->lpsaActions != NULL, "W actions are NULL\n");
        for (i = 0; fa->lpsaActions && i < cActions; i++)
        {
            ok(fa->lpsaActions[i].Type == actions[i].Type, "W action %lu type = %d\n", i, fa->lpsaActions[i].Type);
            ok(fa->lpsaActions[i].Delay == actions[i].Delay, "W action %lu delay = %lu\n", i, fa->lpsaActions[i].Delay);
        }
    }
    if (command)
        ok(fa->lpCommand && !wcscmp(fa->lpCommand, command), "W command = %ls\n", fa->lpCommand ? fa->lpCommand : L"(null)");
    if (rebootMsg)
        ok(fa->lpRebootMsg && !wcscmp(fa->lpRebootMsg, rebootMsg), "W reboot message = %ls\n", fa->lpRebootMsg ? fa->lpRebootMsg : L"(null)");

    memset(buffer, 0xcc, sizeof(buffer));
    ret = QueryServiceConfig2A(hService, SERVICE_CONFIG_FAILURE_ACTIONS, buffer, sizeof(buffer), &needed);
    ok(ret, "QueryServiceConfig2A failed, error %lu\n", GetLastError());
    if (!ret)
        return;

    ok(faA->dwResetPeriod == resetPeriod, "A reset period = %lu\n", faA->dwResetPeriod);
    ok(faA->cActions == cActions, "A action count = %lu, expected %lu\n", faA->cActions, cActions);
    if (faA->cActions == cActions && cActions > 0)
    {
        ok(faA->lpsaActions != NULL, "A actions are NULL\n");
        for (i = 0; faA->lpsaActions && i < cActions; i++)
        {
            ok(faA->lpsaActions[i].Type == actions[i].Type, "A action %lu type = %d\n", i, faA->lpsaActions[i].Type);
            ok(faA->lpsaActions[i].Delay == actions[i].Delay, "A action %lu delay = %lu\n", i, faA->lpsaActions[i].Delay);
        }
    }

    if (command)
    {
        CHAR expected[64];

        WideCharToMultiByte(CP_ACP, 0, command, -1, expected, sizeof(expected), NULL, NULL);
        ok(faA->lpCommand && !strcmp(faA->lpCommand, expected), "A command = %s\n", faA->lpCommand ? faA->lpCommand : "(null)");
    }
    if (rebootMsg)
    {
        CHAR expected[64];

        WideCharToMultiByte(CP_ACP, 0, rebootMsg, -1, expected, sizeof(expected), NULL, NULL);
        ok(faA->lpRebootMsg && !strcmp(faA->lpRebootMsg, expected), "A reboot message = %s\n", faA->lpRebootMsg ? faA->lpRebootMsg : "(null)");
    }
}

static void
ChangeFailureActions(SC_HANDLE hService, SERVICE_FAILURE_ACTIONSW *sfa)
{
    BOOL ret;
    DWORD error;

    SetLastError(0xdeadbeef);
    ret = ChangeServiceConfig2W(hService, SERVICE_CONFIG_FAILURE_ACTIONS, sfa);
    error = GetLastError();
    ok(ret, "ChangeServiceConfig2W failed, error %lu\n", error);
}

START_TEST(ServiceFailureActions)
{
    SC_HANDLE hScm, hService;
    SERVICE_FAILURE_ACTIONSW sfa;
    SC_ACTION actions[3] = { { SC_ACTION_RESTART, 1000 }, { SC_ACTION_RUN_COMMAND, 2000 }, { SC_ACTION_NONE, 0 } };
    SC_ACTION other[2] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RUN_COMMAND, 7000 } };
    WCHAR command[] = L"cmd.exe /c rem";
    WCHAR rebootMsg[] = L"rebooting";
    BYTE blob[HEADER_SIZE + 2 * sizeof(SC_ACTION)];
    HKEY hKey;
    LONG lRet;

    hScm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hScm)
    {
        skip("OpenSCManager failed, error %lu\n", GetLastError());
        return;
    }

    hService = CreateServiceW(hScm, TEST_SERVICE, NULL, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                              SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE,
                              L"%systemroot%\\system32\\cmd.exe", NULL, NULL, NULL, NULL, NULL);
    if (!hService && GetLastError() == ERROR_SERVICE_EXISTS)
    {
        hService = OpenServiceW(hScm, TEST_SERVICE, SERVICE_ALL_ACCESS);
        if (hService)
            DeleteService(hService);
        CloseServiceHandle(hService);
        hService = CreateServiceW(hScm, TEST_SERVICE, NULL, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                  SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE,
                                  L"%systemroot%\\system32\\cmd.exe", NULL, NULL, NULL, NULL, NULL);
    }
    ok(hService != NULL, "CreateServiceW failed, error %lu\n", GetLastError());
    if (!hService)
    {
        CloseServiceHandle(hScm);
        return;
    }

    lRet = RegOpenKeyExW(HKEY_LOCAL_MACHINE, TEST_KEY, 0, KEY_READ | KEY_WRITE, &hKey);
    ok(lRet == ERROR_SUCCESS, "RegOpenKeyExW failed with %ld\n", lRet);
    if (lRet != ERROR_SUCCESS)
        goto cleanup;

    /* Three actions, a command and a reboot message */
    sfa.dwResetPeriod = 86400;
    sfa.lpRebootMsg = rebootMsg;
    sfa.lpCommand = command;
    sfa.cActions = 3;
    sfa.lpsaActions = actions;
    ChangeFailureActions(hService, &sfa);
    CheckRegistry(hKey, 86400, 3, actions);
    CheckQuery(hService, 86400, 3, actions, command, rebootMsg);

    /* No action array: the stored reset period and actions are kept */
    sfa.dwResetPeriod = 1;
    sfa.lpRebootMsg = NULL;
    sfa.lpCommand = NULL;
    sfa.cActions = 0;
    sfa.lpsaActions = NULL;
    ChangeFailureActions(hService, &sfa);
    CheckRegistry(hKey, 86400, 3, actions);

    /* Two other actions replace the three */
    sfa.dwResetPeriod = 120;
    sfa.cActions = 2;
    sfa.lpsaActions = other;
    ChangeFailureActions(hService, &sfa);
    CheckRegistry(hKey, 120, 2, other);
    CheckQuery(hService, 120, 2, other, NULL, NULL);

    /* A value in the layout the registry holds on every architecture */
    memset(blob, 0, sizeof(blob));
    ((DWORD *)blob)[0] = 3600;
    ((DWORD *)blob)[3] = 2;
    ((DWORD *)blob)[4] = HEADER_SIZE;
    memcpy(blob + HEADER_SIZE, actions, 2 * sizeof(SC_ACTION));
    lRet = RegSetValueExW(hKey, L"FailureActions", 0, REG_BINARY, blob, sizeof(blob));
    ok(lRet == ERROR_SUCCESS, "RegSetValueExW failed with %ld\n", lRet);
    CheckQuery(hService, 3600, 2, actions, NULL, NULL);

#ifdef _M_AMD64
    if (is_reactos())
    {
        /* The 40-byte in-memory layout that earlier ReactOS amd64 builds stored still reads */
        BYTE legacy[40 + 3 * sizeof(SC_ACTION)];

        memset(legacy, 0, sizeof(legacy));
        ((DWORD *)legacy)[0] = 3600;
        ((DWORD *)legacy)[6] = 3;
        memcpy(legacy + 40, actions, 3 * sizeof(SC_ACTION));
        lRet = RegSetValueExW(hKey, L"FailureActions", 0, REG_BINARY, legacy, sizeof(legacy));
        ok(lRet == ERROR_SUCCESS, "RegSetValueExW failed with %ld\n", lRet);
        CheckQuery(hService, 3600, 3, actions, NULL, NULL);
    }
#endif

    /* An empty action array removes the actions */
    sfa.dwResetPeriod = 5;
    sfa.cActions = 0;
    sfa.lpsaActions = other;
    ChangeFailureActions(hService, &sfa);
    CheckQuery(hService, 0, 0, NULL, NULL, NULL);

    RegCloseKey(hKey);

cleanup:
    DeleteService(hService);
    CloseServiceHandle(hService);
    CloseServiceHandle(hScm);
}
