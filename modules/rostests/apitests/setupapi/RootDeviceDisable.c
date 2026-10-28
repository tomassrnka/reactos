/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Disable and re-enable a started root-enumerated device
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The device is the fake NDIS miniport (modules/rostests/drivers/fakemp) as a root device the
 * test creates itself. Its halt and initialize counters tell whether the driver stack was
 * really torn down and built again. Run it once per boot, and not together with ndis_apitest:
 * both install the same driver.
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <winioctl.h>
#include <winreg.h>
#include <cfgmgr32.h>
#include <shlwapi.h>

#include "fakemp.h"

#define FormatW(Buffer, ...) _snwprintf(Buffer, ARRAYSIZE(Buffer) - 1, __VA_ARGS__)

#define NET_CLASS_GUID  L"{4D36E972-E325-11CE-BFC1-08002BE10318}"
#define CLASS_KEY       L"SYSTEM\\CurrentControlSet\\Control\\Class\\" NET_CLASS_GUID L"\\0952"
#define ENUM_ROOT_KEY   L"SYSTEM\\CurrentControlSet\\Enum\\Root"
#define ENUM_NAME       L"FAKEMPS"
#define SERVICE_NAME    L"fakemps"
#define SERVICE_KEY     L"SYSTEM\\CurrentControlSet\\Services\\" SERVICE_NAME
#define DEVICE_INSTANCE L"ROOT\\" ENUM_NAME L"\\0000"

static HANDLE Control = INVALID_HANDLE_VALUE;

static
void
DriverPath(PWSTR Path, ULONG Length, BOOL Destination)
{
    PWSTR Slash;

    if (Destination)
    {
        GetSystemDirectoryW(Path, Length);
        lstrcatW(Path, L"\\drivers\\");
    }
    else
    {
        GetModuleFileNameW(NULL, Path, Length);
        Slash = wcsrchr(Path, L'\\');
        Slash[1] = UNICODE_NULL;
    }
    lstrcatW(Path, SERVICE_NAME L".sys");
}

static
void
SetString(HKEY Key, PCWSTR Name, DWORD Type, PCWSTR Value)
{
    RegSetValueExW(Key, Name, 0, Type, (const BYTE *)Value, (lstrlenW(Value) + 1) * sizeof(WCHAR));
}

static
void
SetDword(HKEY Key, PCWSTR Name, DWORD Value)
{
    RegSetValueExW(Key, Name, 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
}

static
BOOL
Locate(DEVINST *DevInst)
{
    return CM_Locate_DevNodeW(DevInst, (DEVINSTID_W)DEVICE_INSTANCE, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
}

static
BOOL
Install(void)
{
    WCHAR Source[MAX_PATH], Destination[MAX_PATH];
    WCHAR HardwareId[64];
    HKEY Key, SubKey;
    DEVINST Root, DevInst;
    CONFIGRET Cr;
    LONG Error;
    DWORD Start;

    DriverPath(Source, MAX_PATH, FALSE);
    DriverPath(Destination, MAX_PATH, TRUE);
    if (!CopyFileW(Source, Destination, FALSE))
    {
        skip("Cannot copy %ls: %lu\n", Source, GetLastError());
        return FALSE;
    }

    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, SERVICE_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL);
    if (Error != ERROR_SUCCESS)
    {
        skip("Cannot create the service key: %ld\n", Error);
        return FALSE;
    }
    SetDword(Key, L"Type", 1);
    SetDword(Key, L"Start", 3);
    SetDword(Key, L"ErrorControl", 0);
    SetString(Key, L"ImagePath", REG_EXPAND_SZ, L"system32\\drivers\\" SERVICE_NAME L".sys");
    SetString(Key, L"Group", REG_SZ, L"NDIS");
    RegCloseKey(Key);

    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, CLASS_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL);
    ok(Error == ERROR_SUCCESS, "Class key: %ld\n", Error);
    if (Error != ERROR_SUCCESS)
        return FALSE;
    SetString(Key, L"DriverDesc", REG_SZ, L"FakeMpS");
    SetString(Key, L"ProviderName", REG_SZ, L"ReactOS Project");
    if (RegCreateKeyExW(Key, L"Linkage", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &SubKey, NULL) == ERROR_SUCCESS)
    {
        SetString(SubKey, L"Export", REG_SZ, L"\\Device\\{7A1D0F53-6D50-4C2A-9E70-46414B454D53}");
        RegCloseKey(SubKey);
    }
    RegCloseKey(Key);

    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, ENUM_ROOT_KEY L"\\" ENUM_NAME L"\\0000", 0, NULL, 0,
                            KEY_ALL_ACCESS, NULL, &Key, NULL);
    ok(Error == ERROR_SUCCESS, "Enum key: %ld\n", Error);
    if (Error != ERROR_SUCCESS)
        return FALSE;
    ZeroMemory(HardwareId, sizeof(HardwareId));
    lstrcpyW(HardwareId, L"ROOT\\" ENUM_NAME);
    RegSetValueExW(Key, L"HardwareID", 0, REG_MULTI_SZ, (const BYTE *)HardwareId,
                   (lstrlenW(HardwareId) + 2) * sizeof(WCHAR));
    SetString(Key, L"Service", REG_SZ, SERVICE_NAME);
    SetString(Key, L"Class", REG_SZ, L"Net");
    SetString(Key, L"ClassGUID", REG_SZ, NET_CLASS_GUID);
    SetString(Key, L"Driver", REG_SZ, NET_CLASS_GUID L"\\0952");
    SetString(Key, L"DeviceDesc", REG_SZ, L"FakeMpS");
    SetDword(Key, L"ConfigFlags", 0);
    RegCloseKey(Key);

    Cr = CM_Locate_DevNodeW(&Root, NULL, CM_LOCATE_DEVNODE_NORMAL);
    ok(Cr == CR_SUCCESS, "CM_Locate_DevNodeW(root): 0x%lx\n", Cr);
    if (Cr != CR_SUCCESS)
        return FALSE;
    Cr = CM_Reenumerate_DevNode(Root, CM_REENUMERATE_SYNCHRONOUS);
    ok(Cr == CR_SUCCESS, "CM_Reenumerate_DevNode: 0x%lx\n", Cr);

    Start = GetTickCount();
    while (!Locate(&DevInst) && GetTickCount() - Start < 15000)
        Sleep(200);
    ok(Locate(&DevInst), "No device node\n");

    /* The driver loads for the device, and so does its control device */
    Start = GetTickCount();
    do
    {
        Control = CreateFileW(L"\\\\.\\FakeMpS", GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (Control != INVALID_HANDLE_VALUE)
            break;
        Sleep(200);
    } while (GetTickCount() - Start < 30000);
    ok(Control != INVALID_HANDLE_VALUE, "No control device: %lu\n", GetLastError());
    return Control != INVALID_HANDLE_VALUE;
}

static
FAKEMP_STATS
Stats(void)
{
    FAKEMP_STATS St;
    DWORD Returned;

    ZeroMemory(&St, sizeof(St));
    ok(DeviceIoControl(Control, FAKEMP_IOCTL_GET_STATS, NULL, 0, &St, sizeof(St), &Returned, NULL),
       "GET_STATS failed: %lu\n", GetLastError());
    return St;
}

/* Waits for the device to be started without a problem */
static
BOOL
WaitRunning(DEVINST DevInst, PCSTR When)
{
    ULONG Status = 0, Problem = 0;
    CONFIGRET Cr = CR_FAILURE;
    DWORD Start = GetTickCount();

    do
    {
        Cr = CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0);
        if (Cr == CR_SUCCESS && (Status & DN_STARTED) && !(Status & DN_HAS_PROBLEM))
            break;
        Sleep(100);
    } while (GetTickCount() - Start < 15000);

    ok(Cr == CR_SUCCESS, "%s: CM_Get_DevNode_Status failed: 0x%lx\n", When, Cr);
    ok((Status & DN_STARTED) && !(Status & DN_HAS_PROBLEM) && Problem == 0,
       "%s: status 0x%lx, problem %lu: not running\n", When, Status, Problem);
    return Cr == CR_SUCCESS && (Status & DN_STARTED);
}

static
void
CheckDisabled(DEVINST DevInst, ULONG Halts, ULONG Initializes, PCSTR When)
{
    ULONG Status = 0, Problem = 0;
    FAKEMP_STATS St;
    DEVINST Again;
    CONFIGRET Cr;

    /* The device node stays in the tree: it is what the device is enabled through */
    Cr = CM_Locate_DevNodeW(&Again, (DEVINSTID_W)DEVICE_INSTANCE, CM_LOCATE_DEVNODE_NORMAL);
    ok(Cr == CR_SUCCESS, "%s: CM_Locate_DevNodeW failed: 0x%lx\n", When, Cr);

    Cr = CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0);
    ok(Cr == CR_SUCCESS, "%s: CM_Get_DevNode_Status failed: 0x%lx\n", When, Cr);
    if (Cr == CR_SUCCESS)
    {
        ok(!(Status & DN_STARTED), "%s: status 0x%lx: still started\n", When, Status);
        ok(Status & DN_HAS_PROBLEM, "%s: status 0x%lx: no problem\n", When, Status);
        ok(Problem == CM_PROB_DISABLED, "%s: problem %lu, expected %u\n", When, Problem, CM_PROB_DISABLED);
    }

    /* The stack is gone: the miniport was halted, and is not initialized again */
    St = Stats();
    ok(St.Halts == Halts, "%s: %lu halts, expected %lu\n", When, St.Halts, Halts);
    ok(St.Initializes == Initializes, "%s: %lu initializes, expected %lu\n", When, St.Initializes, Initializes);
    ok(St.Halted == 1, "%s: the miniport is not halted\n", When);
}

static
void
Cleanup(void)
{
    WCHAR Path[MAX_PATH];
    DEVINST DevInst;
    PNP_VETO_TYPE Veto;
    WCHAR VetoName[MAX_PATH];
    HKEY Root;

    if (Control != INVALID_HANDLE_VALUE)
        CloseHandle(Control);
    Control = INVALID_HANDLE_VALUE;

    if (Locate(&DevInst))
        CM_Query_And_Remove_SubTreeW(DevInst, &Veto, VetoName, ARRAYSIZE(VetoName), CM_REMOVE_NO_RESTART);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ENUM_ROOT_KEY, 0, DELETE | KEY_ENUMERATE_SUB_KEYS, &Root) == ERROR_SUCCESS)
    {
        SHDeleteKeyW(Root, ENUM_NAME);
        RegCloseKey(Root);
    }
    SHDeleteKeyW(HKEY_LOCAL_MACHINE, CLASS_KEY);
    SHDeleteKeyW(HKEY_LOCAL_MACHINE, SERVICE_KEY);
    DriverPath(Path, MAX_PATH, TRUE);
    DeleteFileW(Path); /* fails while the driver is loaded */
}

#define BAD_ENUM_NAME   L"ROSTEST_BADSVC"
#define BAD_INSTANCE    L"ROOT\\" BAD_ENUM_NAME L"\\0000"

/* A device whose service does not exist never starts. It can still be disabled, and enabling
 * it again reports that it does not start */
static
void
TestFailedDevice(void)
{
    WCHAR HardwareId[64];
    DEVINST Root, DevInst;
    ULONG Status = 0, Problem = 0, i;
    CONFIGRET Cr;
    HKEY Key, EnumKey;
    DWORD Start;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ENUM_ROOT_KEY, 0, DELETE | KEY_CREATE_SUB_KEY, &EnumKey) != ERROR_SUCCESS)
    {
        skip("No write access to the Root enumerator key\n");
        return;
    }
    if (RegCreateKeyExW(EnumKey, BAD_ENUM_NAME L"\\0000", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL) != ERROR_SUCCESS)
    {
        skip("Cannot create a root device key\n");
        RegCloseKey(EnumKey);
        return;
    }
    ZeroMemory(HardwareId, sizeof(HardwareId));
    lstrcpyW(HardwareId, L"ROOT\\" BAD_ENUM_NAME);
    RegSetValueExW(Key, L"HardwareID", 0, REG_MULTI_SZ, (const BYTE *)HardwareId,
                   (lstrlenW(HardwareId) + 2) * sizeof(WCHAR));
    SetString(Key, L"Service", REG_SZ, L"rostestnosuchservice");
    SetDword(Key, L"ConfigFlags", 0);
    RegCloseKey(Key);

    Cr = CM_Locate_DevNodeW(&Root, NULL, CM_LOCATE_DEVNODE_NORMAL);
    ok(Cr == CR_SUCCESS, "CM_Locate_DevNodeW(root): 0x%lx\n", Cr);
    if (Cr == CR_SUCCESS)
        CM_Reenumerate_DevNode(Root, CM_REENUMERATE_SYNCHRONOUS);

    Start = GetTickCount();
    while (CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)BAD_INSTANCE, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS &&
           GetTickCount() - Start < 10000)
        Sleep(200);
    Cr = CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)BAD_INSTANCE, CM_LOCATE_DEVNODE_NORMAL);
    ok(Cr == CR_SUCCESS, "No device node for the device without a service: 0x%lx\n", Cr);
    if (Cr == CR_SUCCESS)
    {
        Cr = CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0);
        ok(Cr == CR_SUCCESS && !(Status & DN_STARTED), "The device without a service is started: 0x%lx\n", Status);

        if (is_reactos())
        {
            Cr = CM_Disable_DevNode(DevInst, 0);
            ok(Cr == CR_SUCCESS, "CM_Disable_DevNode of a device that does not start failed: 0x%lx\n", Cr);
            Status = Problem = 0;
            Cr = CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0);
            ok(Cr == CR_SUCCESS && Problem == CM_PROB_DISABLED, "status 0x%lx, problem %lu: not disabled\n", Status, Problem);

            for (i = 1; i <= 2; i++)
            {
                Cr = CM_Enable_DevNode(DevInst, 0);
                ok(Cr != CR_SUCCESS, "Attempt %lu: CM_Enable_DevNode of a device that cannot start succeeded\n", i);

                /* The device stays, with a problem, and can be tried again */
                Cr = CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)BAD_INSTANCE, CM_LOCATE_DEVNODE_NORMAL);
                ok(Cr == CR_SUCCESS, "Attempt %lu: the device node is gone after the failed enable: 0x%lx\n", i, Cr);
                Status = Problem = 0;
                Cr = CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0);
                ok(Cr == CR_SUCCESS && !(Status & DN_STARTED) && (Status & DN_HAS_PROBLEM) && Problem != 0,
                   "Attempt %lu: status 0x%lx, problem %lu, cr 0x%lx\n", i, Status, Problem, Cr);
            }
        }
    }

    SHDeleteKeyW(EnumKey, BAD_ENUM_NAME);
    RegCloseKey(EnumKey);
}

START_TEST(RootDeviceDisable)
{
    DEVINST DevInst, RootDevInst;
    CONFIGRET Cr;
    FAKEMP_STATS St;
    PNP_VETO_TYPE Veto = PNP_VetoTypeUnknown;
    WCHAR VetoName[MAX_PATH] = L"";
    ULONG i;

    if (!Install())
    {
        Cleanup();
        return;
    }
    if (!Locate(&DevInst) || !WaitRunning(DevInst, "after install"))
    {
        Cleanup();
        return;
    }

    St = Stats();
    ok(St.Initializes == 1, "%lu initializes after install\n", St.Initializes);
    ok(St.Halted == 0, "Halted after install\n");

    /* Enabling a device that runs is not an error */
    Cr = CM_Enable_DevNode(DevInst, 0);
    ok(Cr == CR_SUCCESS, "CM_Enable_DevNode of a running device failed: 0x%lx\n", Cr);

    for (i = 1; i <= 2; i++)
    {
        Cr = CM_Disable_DevNode(DevInst, 0);
        ok(Cr == CR_SUCCESS, "Cycle %lu: CM_Disable_DevNode failed: 0x%lx\n", i, Cr);
        CheckDisabled(DevInst, i, i, "after disable");

        /* Disabling again changes nothing (as measured on ReactOS) */
        if (is_reactos())
        {
            Cr = CM_Disable_DevNode(DevInst, 0);
            ok(Cr == CR_SUCCESS, "Cycle %lu: second CM_Disable_DevNode failed: 0x%lx\n", i, Cr);
            CheckDisabled(DevInst, i, i, "after the second disable");
        }

        /* A reenumeration does not bring a disabled device back */
        Cr = CM_Locate_DevNodeW(&RootDevInst, NULL, CM_LOCATE_DEVNODE_NORMAL);
        ok(Cr == CR_SUCCESS, "CM_Locate_DevNodeW(root): 0x%lx\n", Cr);
        if (Cr == CR_SUCCESS)
            CM_Reenumerate_DevNode(RootDevInst, CM_REENUMERATE_SYNCHRONOUS);
        ok(Locate(&DevInst), "Cycle %lu: the disabled device node is gone\n", i);
        CheckDisabled(DevInst, i, i, "after a reenumeration");

        Cr = CM_Enable_DevNode(DevInst, 0);
        ok(Cr == CR_SUCCESS, "Cycle %lu: CM_Enable_DevNode failed: 0x%lx\n", i, Cr);
        if (WaitRunning(DevInst, "after enable"))
        {
            St = Stats();
            ok(St.Initializes == i + 1, "Cycle %lu: %lu initializes, expected %lu\n", i, St.Initializes, i + 1);
            ok(St.Halted == 0, "Cycle %lu: the miniport is halted after the enable\n", i);
        }
    }

    /* Removing a disabled device removes its node */
    Cr = CM_Disable_DevNode(DevInst, 0);
    ok(Cr == CR_SUCCESS, "CM_Disable_DevNode failed: 0x%lx\n", Cr);
    Cr = CM_Query_And_Remove_SubTreeW(DevInst, &Veto, VetoName, ARRAYSIZE(VetoName), CM_REMOVE_NO_RESTART);
    ok(Cr == CR_SUCCESS, "CM_Query_And_Remove_SubTreeW of a disabled device failed: 0x%lx (veto %d %ls)\n",
       Cr, Veto, VetoName);
    /* The removed root device stays a leaked device node until the next boot (a known
     * limitation, see ndis_apitest): all that is asked is that it does not run again */
    Cr = CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)DEVICE_INSTANCE, CM_LOCATE_DEVNODE_NORMAL);
    if (Cr == CR_SUCCESS)
    {
        ULONG Status = 0, Problem = 0;

        Cr = CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0);
        ok(Cr != CR_SUCCESS || !(Status & DN_STARTED), "The removed device is started: 0x%lx\n", Status);
    }
    St = Stats();
    ok(St.Halted == 1 && St.Initializes == 3, "The removed device was initialized again: %lu, halted %lu\n",
       St.Initializes, St.Halted);

    TestFailedDevice();

    Cleanup();
}
