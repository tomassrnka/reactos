/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test repeated removal of a root-enumerated device
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <winreg.h>
#include <cfgmgr32.h>
#include <shlwapi.h>

#define ENUM_ROOT_KEY L"SYSTEM\\CurrentControlSet\\Enum\\Root"
#define DEVICE_NAME L"ROSTEST_RMTWICE"
#define DEVICE_INSTANCE L"Root\\" DEVICE_NAME L"\\0000"

START_TEST(RootDeviceRemove)
{
    static const WCHAR Service[] = L"Null";
    HKEY RootKey, InstanceKey;
    DEVINST RootDevInst, DevInst;
    ULONG Status, Problem;
    CONFIGRET Cr;
    LONG Error;
    ULONG i;

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ENUM_ROOT_KEY, 0,
                          KEY_CREATE_SUB_KEY | DELETE, &RootKey);
    if (Error == ERROR_ACCESS_DENIED)
    {
        skip("No write access to the Root enumerator key\n");
        return;
    }
    ok(Error == ERROR_SUCCESS, "RegOpenKeyExW failed: %ld\n", Error);
    if (Error != ERROR_SUCCESS)
        return;

    /* A root device whose service has no AddDevice routine fails to add,
     * and the PnP manager tries to remove it on each later enumeration.
     * Without a function driver nothing succeeds the query-remove, so the
     * removal is vetoed and the device stays in the device tree until the
     * next boot. */
    Error = RegCreateKeyExW(RootKey, DEVICE_NAME L"\\0000", 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                            &InstanceKey, NULL);
    if (Error == ERROR_ACCESS_DENIED)
    {
        skip("Cannot create a root device key\n");
        RegCloseKey(RootKey);
        return;
    }
    ok(Error == ERROR_SUCCESS, "RegCreateKeyExW failed: %ld\n", Error);
    if (Error != ERROR_SUCCESS)
    {
        RegCloseKey(RootKey);
        return;
    }
    Error = RegSetValueExW(InstanceKey, L"Service", 0, REG_SZ,
                           (const BYTE *)Service, sizeof(Service));
    ok(Error == ERROR_SUCCESS, "RegSetValueExW failed: %ld\n", Error);
    RegCloseKey(InstanceKey);

    Cr = CM_Locate_DevNodeW(&RootDevInst, NULL, CM_LOCATE_DEVNODE_NORMAL);
    ok(Cr == CR_SUCCESS, "CM_Locate_DevNodeW(root) failed: 0x%lx\n", Cr);
    if (Cr == CR_SUCCESS)
    {
        for (i = 0; i < 4; i++)
        {
            Cr = CM_Reenumerate_DevNode(RootDevInst, CM_REENUMERATE_SYNCHRONOUS);
            ok(Cr == CR_SUCCESS, "CM_Reenumerate_DevNode #%lu failed: 0x%lx\n", i, Cr);

            Cr = CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)DEVICE_INSTANCE,
                                    CM_LOCATE_DEVNODE_PHANTOM);
            ok(Cr == CR_SUCCESS, "CM_Locate_DevNodeW #%lu failed: 0x%lx\n", i, Cr);
            if (Cr != CR_SUCCESS)
                continue;
            Cr = CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0);
            ok(Cr == CR_SUCCESS, "CM_Get_DevNode_Status #%lu failed: 0x%lx\n", i, Cr);
            if (Cr == CR_SUCCESS)
            {
                ok(Problem == CM_PROB_FAILED_ADD, "Pass %lu: problem %lu\n", i, Problem);
                trace("Pass %lu: status 0x%lx, problem %lu\n", i, Status, Problem);
            }
        }
    }

    Error = SHDeleteKeyW(RootKey, DEVICE_NAME);
    ok(Error == ERROR_SUCCESS, "SHDeleteKeyW failed: %ld\n", Error);
    RegCloseKey(RootKey);
}
