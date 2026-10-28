/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Disable and re-enable a root-enumerated bus device that has children
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The bus is a fake bus driver (modules/rostests/drivers/fakebus) as a root device the test
 * creates itself. It reports two raw child devices, and keeps their PDOs when it is removed.
 * Disabling the bus must delete the child device nodes, and enabling it must bring the bus
 * and both children back, started, every time.
 */

#include <apitest.h>

#include <winreg.h>
#include <cfgmgr32.h>
#include <shlwapi.h>

#define SYSTEM_CLASS_GUID   L"{4D36E97D-E325-11CE-BFC1-08002BE10318}"
#define ENUM_ROOT_KEY       L"SYSTEM\\CurrentControlSet\\Enum\\Root"
#define ENUM_NAME           L"FAKEBUS"
#define SERVICE_NAME        L"fakebus"
#define SERVICE_KEY         L"SYSTEM\\CurrentControlSet\\Services\\" SERVICE_NAME
#define BUS_INSTANCE        L"ROOT\\" ENUM_NAME L"\\0000"
#define CHILD_COUNT         2

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
LocateBus(DEVINST *DevInst)
{
    return CM_Locate_DevNodeW(DevInst, (DEVINSTID_W)BUS_INSTANCE, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
}

static
BOOL
Install(void)
{
    WCHAR Source[MAX_PATH], Destination[MAX_PATH];
    WCHAR HardwareId[64];
    HKEY Key;
    LONG Error;

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
    SetString(Key, L"Class", REG_SZ, L"System");
    SetString(Key, L"ClassGUID", REG_SZ, SYSTEM_CLASS_GUID);
    SetString(Key, L"DeviceDesc", REG_SZ, L"FakeBus");
    SetDword(Key, L"ConfigFlags", 0);
    RegCloseKey(Key);
    return TRUE;
}

static
void
ReenumerateRoot(void)
{
    DEVINST Root;
    CONFIGRET Cr = CM_Locate_DevNodeW(&Root, NULL, CM_LOCATE_DEVNODE_NORMAL);

    ok(Cr == CR_SUCCESS, "CM_Locate_DevNodeW(root): 0x%lx\n", Cr);
    if (Cr == CR_SUCCESS)
    {
        Cr = CM_Reenumerate_DevNode(Root, CM_REENUMERATE_SYNCHRONOUS);
        ok(Cr == CR_SUCCESS, "CM_Reenumerate_DevNode: 0x%lx\n", Cr);
    }
}

static
BOOL
IsRunning(DEVINST DevInst)
{
    ULONG Status = 0, Problem = 0;

    return CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0) == CR_SUCCESS &&
           (Status & DN_STARTED) && !(Status & DN_HAS_PROBLEM);
}

/* Number of the bus' children, and how many of them are running */
static
ULONG
Children(DEVINST Bus, ULONG *Running)
{
    DEVINST Child;
    ULONG Count = 0;
    CONFIGRET Cr;

    *Running = 0;
    for (Cr = CM_Get_Child(&Child, Bus, 0); Cr == CR_SUCCESS; Cr = CM_Get_Sibling(&Child, Child, 0))
    {
        Count++;
        if (IsRunning(Child))
            (*Running)++;
    }
    return Count;
}

/* Waits for the bus and its children to be running */
static
void
WaitRunning(DEVINST Bus, PCSTR When)
{
    ULONG Count = 0, Running = 0;
    DWORD Start = GetTickCount();

    do
    {
        Count = Children(Bus, &Running);
        if (Count == CHILD_COUNT && Running == CHILD_COUNT && IsRunning(Bus))
            break;
        Sleep(100);
    } while (GetTickCount() - Start < 20000);

    ok(IsRunning(Bus), "%s: the bus is not running\n", When);
    ok(Count == CHILD_COUNT, "%s: %lu children, expected %u\n", When, Count, CHILD_COUNT);
    ok(Running == CHILD_COUNT, "%s: %lu children running, expected %u\n", When, Running, CHILD_COUNT);
}

static
void
CheckDisabled(DEVINST Bus, PCSTR When)
{
    ULONG Status = 0, Problem = 0, Running;
    CONFIGRET Cr;
    DEVINST Again;

    Cr = CM_Locate_DevNodeW(&Again, (DEVINSTID_W)BUS_INSTANCE, CM_LOCATE_DEVNODE_NORMAL);
    ok(Cr == CR_SUCCESS, "%s: the disabled bus node is gone: 0x%lx\n", When, Cr);

    Cr = CM_Get_DevNode_Status(&Status, &Problem, Bus, 0);
    ok(Cr == CR_SUCCESS, "%s: CM_Get_DevNode_Status failed: 0x%lx\n", When, Cr);
    ok(!(Status & DN_STARTED) && (Status & DN_HAS_PROBLEM) && Problem == CM_PROB_DISABLED,
       "%s: status 0x%lx, problem %lu\n", When, Status, Problem);

    /* The children went with the stack: their device nodes are deleted */
    ok(Children(Bus, &Running) == 0, "%s: the disabled bus still has child device nodes\n", When);
}

/* The bus driver fails IRP_MN_START_DEVICE while this value is set */
static
void
SetFailStart(DWORD Value)
{
    HKEY Key;

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, SERVICE_KEY L"\\Parameters", 0, NULL, 0, KEY_ALL_ACCESS,
                        NULL, &Key, NULL) == ERROR_SUCCESS)
    {
        SetDword(Key, L"FailStart", Value);
        RegCloseKey(Key);
    }
}

static
void
Cleanup(void)
{
    WCHAR Path[MAX_PATH], VetoName[MAX_PATH] = L"";
    PNP_VETO_TYPE Veto;
    DEVINST DevInst;
    HKEY Root;

    if (LocateBus(&DevInst))
        CM_Query_And_Remove_SubTreeW(DevInst, &Veto, VetoName, ARRAYSIZE(VetoName), CM_REMOVE_NO_RESTART);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ENUM_ROOT_KEY, 0, DELETE | KEY_ENUMERATE_SUB_KEYS, &Root) == ERROR_SUCCESS)
    {
        SHDeleteKeyW(Root, ENUM_NAME);
        RegCloseKey(Root);
    }
    SHDeleteKeyW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\FAKEBUS");
    SHDeleteKeyW(HKEY_LOCAL_MACHINE, SERVICE_KEY);
    DriverPath(Path, MAX_PATH, TRUE);
    DeleteFileW(Path); /* fails while the driver is loaded */
}

START_TEST(RootBusDisable)
{
    DEVINST Bus;
    CONFIGRET Cr;
    DWORD Start;
    ULONG i;

    if (!Install())
    {
        Cleanup();
        return;
    }

    ReenumerateRoot();
    Start = GetTickCount();
    while (!LocateBus(&Bus) && GetTickCount() - Start < 15000)
        Sleep(200);
    if (!LocateBus(&Bus))
    {
        ok(0, "No device node for the bus\n");
        Cleanup();
        return;
    }
    WaitRunning(Bus, "after install");

    for (i = 1; i <= 2; i++)
    {
        Cr = CM_Disable_DevNode(Bus, 0);
        ok(Cr == CR_SUCCESS, "Cycle %lu: CM_Disable_DevNode failed: 0x%lx\n", i, Cr);
        CheckDisabled(Bus, "after disable");

        /* A reenumeration brings neither the bus nor its children back */
        ReenumerateRoot();
        ok(LocateBus(&Bus), "Cycle %lu: the disabled bus node is gone\n", i);
        CheckDisabled(Bus, "after a reenumeration");

        Cr = CM_Enable_DevNode(Bus, 0);
        ok(Cr == CR_SUCCESS, "Cycle %lu: CM_Enable_DevNode failed: 0x%lx\n", i, Cr);
        WaitRunning(Bus, "after enable");
    }

    /* An enable that fails leaves the bus in the tree, disabled, to be enabled again: its PDO is
     * not destroyed by the removal that the failed start causes */
    if (is_reactos())
    {
        Cr = CM_Disable_DevNode(Bus, 0);
        ok(Cr == CR_SUCCESS, "CM_Disable_DevNode failed: 0x%lx\n", Cr);
        SetFailStart(1);
        for (i = 1; i <= 2; i++)
        {
            ULONG Status = 0, Problem = 0;

            Cr = CM_Enable_DevNode(Bus, 0);
            ok(Cr != CR_SUCCESS, "Attempt %lu: CM_Enable_DevNode of a bus that fails to start succeeded\n", i);
            ok(LocateBus(&Bus), "Attempt %lu: the bus node is gone after the failed enable\n", i);
            Cr = CM_Get_DevNode_Status(&Status, &Problem, Bus, 0);
            ok(Cr == CR_SUCCESS && !(Status & DN_STARTED) && (Status & DN_HAS_PROBLEM),
               "Attempt %lu: status 0x%lx, problem %lu, cr 0x%lx\n", i, Status, Problem, Cr);
        }
        SetFailStart(0);
        Cr = CM_Enable_DevNode(Bus, 0);
        ok(Cr == CR_SUCCESS, "CM_Enable_DevNode failed once the bus can start: 0x%lx\n", Cr);
        WaitRunning(Bus, "after the failed enables");
    }

    Cleanup();
}
