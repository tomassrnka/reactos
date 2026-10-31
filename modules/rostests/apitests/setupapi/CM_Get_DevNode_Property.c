/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for CM_Get_DevNode_PropertyW (cfgmgr32.dll)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The types, the return codes and the outputs on failure were measured on Windows 10 22H2. The devnodes differ from
 * system to system, so the values are compared with what the older calls of cfgmgr32 return for the same devnode.
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#define INITGUID
#include <stdio.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <devpkey.h>

typedef CONFIGRET (WINAPI *FN_CM_Get_DevNode_PropertyW)(DEVINST, const DEVPROPKEY *, DEVPROPTYPE *, PBYTE, PULONG, ULONG);
typedef CONFIGRET (WINAPI *FN_CM_Get_DevNode_Property_ExW)(DEVINST, const DEVPROPKEY *, DEVPROPTYPE *, PBYTE, PULONG, ULONG, HMACHINE);

static FN_CM_Get_DevNode_PropertyW pGetProperty;

#define NOT_SET 0xDEADBEEF

static const struct
{
    const DEVPROPKEY *Key;
    PCSTR Name;
    ULONG RegProperty;          /* the CM_DRP_* property with the same value, or 0 */
    DEVPROPTYPE Type;
} Keys[] =
{
    { &DEVPKEY_Device_DeviceDesc, "DeviceDesc", CM_DRP_DEVICEDESC, DEVPROP_TYPE_STRING },
    { &DEVPKEY_Device_HardwareIds, "HardwareIds", CM_DRP_HARDWAREID, DEVPROP_TYPE_STRING_LIST },
    { &DEVPKEY_Device_CompatibleIds, "CompatibleIds", CM_DRP_COMPATIBLEIDS, DEVPROP_TYPE_STRING_LIST },
    { &DEVPKEY_Device_Service, "Service", CM_DRP_SERVICE, DEVPROP_TYPE_STRING },
    { &DEVPKEY_Device_Class, "Class", CM_DRP_CLASS, DEVPROP_TYPE_STRING },
    { &DEVPKEY_Device_ClassGuid, "ClassGuid", CM_DRP_CLASSGUID, DEVPROP_TYPE_GUID },  /* a string in the registry property */
    { &DEVPKEY_Device_Driver, "Driver", CM_DRP_DRIVER, DEVPROP_TYPE_STRING },
    { &DEVPKEY_Device_ConfigFlags, "ConfigFlags", CM_DRP_CONFIGFLAGS, DEVPROP_TYPE_UINT32 },
    { &DEVPKEY_Device_Manufacturer, "Manufacturer", CM_DRP_MFG, DEVPROP_TYPE_STRING },
    { &DEVPKEY_Device_FriendlyName, "FriendlyName", CM_DRP_FRIENDLYNAME, DEVPROP_TYPE_STRING },
    { &DEVPKEY_Device_LocationInfo, "LocationInfo", CM_DRP_LOCATION_INFORMATION, DEVPROP_TYPE_STRING },
    { &DEVPKEY_Device_PDOName, "PDOName", CM_DRP_PHYSICAL_DEVICE_OBJECT_NAME, DEVPROP_TYPE_STRING },
    { &DEVPKEY_Device_Capabilities, "Capabilities", CM_DRP_CAPABILITIES, DEVPROP_TYPE_UINT32 },
    { &DEVPKEY_Device_UINumber, "UINumber", CM_DRP_UI_NUMBER, DEVPROP_TYPE_UINT32 },
    { &DEVPKEY_Device_UpperFilters, "UpperFilters", CM_DRP_UPPERFILTERS, DEVPROP_TYPE_STRING_LIST },
    { &DEVPKEY_Device_LowerFilters, "LowerFilters", CM_DRP_LOWERFILTERS, DEVPROP_TYPE_STRING_LIST },
    { &DEVPKEY_Device_EnumeratorName, "EnumeratorName", CM_DRP_ENUMERATOR_NAME, DEVPROP_TYPE_STRING },
    { &DEVPKEY_Device_BusNumber, "BusNumber", CM_DRP_BUSNUMBER, DEVPROP_TYPE_UINT32 },
    { &DEVPKEY_Device_Address, "Address", CM_DRP_ADDRESS, DEVPROP_TYPE_UINT32 },
    { &DEVPKEY_Device_RemovalPolicy, "RemovalPolicy", CM_DRP_REMOVAL_POLICY, DEVPROP_TYPE_UINT32 },
};

static ULONG TypeSize(DEVPROPTYPE Type)
{
    return Type == DEVPROP_TYPE_UINT32 ? sizeof(ULONG) : 0;
}

/* The devnodes the checks run on: the children of the root, and their children */
static void CheckDevNode(HMODULE hCfgMgr, DEVINST Dev, ULONG *Counts)
{
    WCHAR Id[MAX_DEVICE_ID_LEN], Parent[MAX_DEVICE_ID_LEN];
    BYTE Buffer[4096], Legacy[4096];
    DEVPROPTYPE Type;
    ULONG Size, Need, LegacySize, Status, Problem, StatusExpected, ProblemExpected, i;
    CONFIGRET cr, crLegacy;
    DEVINST ParentDev;

    if (CM_Get_Device_IDW(Dev, Id, ARRAYSIZE(Id), 0) != CR_SUCCESS)
        return;

    /* the instance ID, the enumerator, the parent, the status */
    Type = NOT_SET;
    Size = sizeof(Buffer);
    cr = pGetProperty(Dev, &DEVPKEY_Device_InstanceId, &Type, Buffer, &Size, 0);
    ok(cr == CR_SUCCESS, "%ls: InstanceId: %lx\n", Id, cr);
    if (cr == CR_SUCCESS)
    {
        ok(Type == DEVPROP_TYPE_STRING, "%ls: InstanceId type %lx\n", Id, Type);
        ok(!wcscmp((PWSTR)Buffer, Id), "%ls: InstanceId is %ls\n", Id, (PWSTR)Buffer);
        ok(Size == (wcslen(Id) + 1) * sizeof(WCHAR), "%ls: InstanceId size %lu\n", Id, Size);
    }

    Type = NOT_SET;
    Size = sizeof(Buffer);
    cr = pGetProperty(Dev, &DEVPKEY_Device_Parent, &Type, Buffer, &Size, 0);
    if (CM_Get_Parent(&ParentDev, Dev, 0) == CR_SUCCESS && CM_Get_Device_IDW(ParentDev, Parent, ARRAYSIZE(Parent), 0) == CR_SUCCESS)
    {
        ok(cr == CR_SUCCESS, "%ls: Parent: %lx\n", Id, cr);
        if (cr == CR_SUCCESS)
        {
            ok(Type == DEVPROP_TYPE_STRING, "%ls: Parent type %lx\n", Id, Type);
            ok(!_wcsicmp((PWSTR)Buffer, Parent), "%ls: Parent is %ls, expected %ls\n", Id, (PWSTR)Buffer, Parent);
        }
    }

    if (CM_Get_DevNode_Status(&StatusExpected, &ProblemExpected, Dev, 0) == CR_SUCCESS)
    {
        Type = NOT_SET;
        Size = sizeof(Buffer);
        cr = pGetProperty(Dev, &DEVPKEY_Device_DevNodeStatus, &Type, Buffer, &Size, 0);
        Status = *(PULONG)Buffer;
        ok(cr == CR_SUCCESS && Type == DEVPROP_TYPE_UINT32 && Size == sizeof(ULONG), "%ls: DevNodeStatus: %lx type %lx size %lu\n", Id, cr, Type, Size);
        ok(Status == StatusExpected, "%ls: DevNodeStatus %lx, expected %lx\n", Id, Status, StatusExpected);
        Size = sizeof(Buffer);
        cr = pGetProperty(Dev, &DEVPKEY_Device_ProblemCode, &Type, Buffer, &Size, 0);
        Problem = *(PULONG)Buffer;
        ok(cr == CR_SUCCESS && Type == DEVPROP_TYPE_UINT32, "%ls: ProblemCode: %lx type %lx\n", Id, cr, Type);
        ok(Problem == ProblemExpected, "%ls: ProblemCode %lx, expected %lx\n", Id, Problem, ProblemExpected);
    }

    /* every key with a registry-property twin: the same value, and the type of Windows 10 */
    for (i = 0; i < ARRAYSIZE(Keys); i++)
    {
        Type = NOT_SET;
        Size = sizeof(Buffer);
        memset(Buffer, 0xcc, 16);
        cr = pGetProperty(Dev, Keys[i].Key, &Type, Buffer, &Size, 0);

        LegacySize = sizeof(Legacy);
        crLegacy = CM_Get_DevNode_Registry_PropertyW(Dev, Keys[i].RegProperty, NULL, Legacy, &LegacySize, 0);

        if (cr == CR_SUCCESS)
        {
            Counts[0]++;
            ok(Type == Keys[i].Type, "%ls: %s: type %lx, expected %lx\n", Id, Keys[i].Name, Type, Keys[i].Type);
            if (TypeSize(Type))
                ok(Size == TypeSize(Type), "%ls: %s: size %lu\n", Id, Keys[i].Name, Size);
            if (Keys[i].Type == DEVPROP_TYPE_GUID)
                ok(Size == sizeof(GUID), "%ls: %s: size %lu\n", Id, Keys[i].Name, Size);
            if (crLegacy == CR_SUCCESS && Keys[i].Key != &DEVPKEY_Device_ClassGuid)
            {
                /* a multi-sz list ends with the terminator of the list in both */
                ok(Size == LegacySize && !memcmp(Buffer, Legacy, Size),
                   "%ls: %s: %lu bytes differ from the %lu of the registry property\n", Id, Keys[i].Name, Size, LegacySize);
            }
            else if (Keys[i].Key == &DEVPKEY_Device_ClassGuid)
            {
                WCHAR GuidText[64];
                GUID *Guid = (GUID *)Buffer;

                wsprintfW(GuidText, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", Guid->Data1, Guid->Data2, Guid->Data3,
                         Guid->Data4[0], Guid->Data4[1], Guid->Data4[2], Guid->Data4[3], Guid->Data4[4], Guid->Data4[5],
                         Guid->Data4[6], Guid->Data4[7]);
                ok(crLegacy != CR_SUCCESS || !_wcsicmp(GuidText, (PWSTR)Legacy), "%ls: ClassGuid %ls, registry %ls\n", Id, GuidText, (PWSTR)Legacy);
            }

            /* a buffer one byte short, and none at all, report the size */
            if (Size > 1)
            {
                ULONG Short = Size - 1;

                Type = NOT_SET;
                Need = Short;
                cr = pGetProperty(Dev, Keys[i].Key, &Type, Buffer, &Need, 0);
                ok(cr == CR_BUFFER_SMALL, "%ls: %s: short buffer: %lx\n", Id, Keys[i].Name, cr);
                ok(Need == Size, "%ls: %s: short buffer: size %lu, expected %lu\n", Id, Keys[i].Name, Need, Size);
                ok(Type == Keys[i].Type, "%ls: %s: short buffer: type %lx\n", Id, Keys[i].Name, Type);
            }
            Type = NOT_SET;
            Need = 0;
            cr = pGetProperty(Dev, Keys[i].Key, &Type, NULL, &Need, 0);
            ok(cr == CR_BUFFER_SMALL && Need == Size && Type == Keys[i].Type, "%ls: %s: size query: %lx, size %lu, type %lx\n",
               Id, Keys[i].Name, cr, Need, Type);
        }
        else
        {
            Counts[1]++;
            ok(cr == CR_NO_SUCH_VALUE, "%ls: %s: %lx\n", Id, Keys[i].Name, cr);
            ok(Size == 0, "%ls: %s: size %lu after CR_NO_SUCH_VALUE\n", Id, Keys[i].Name, Size);
            ok(Type == NOT_SET, "%ls: %s: type %lx after CR_NO_SUCH_VALUE\n", Id, Keys[i].Name, Type);
        }
    }

    UNREFERENCED_PARAMETER(hCfgMgr);
}

START_TEST(CM_Get_DevNode_Property)
{
    HMODULE hCfgMgr = LoadLibraryW(L"cfgmgr32.dll");
    FN_CM_Get_DevNode_Property_ExW pGetPropertyEx;
    DEVPROPKEY Unknown = {{ 0x12345678, 0x1234, 0x1234, { 1, 2, 3, 4, 5, 6, 7, 8 }}, 7 };
    DEVINST Root, Child, Grandchild;
    DEVPROPTYPE Type;
    BYTE Buffer[64];
    ULONG Size, Counts[2] = { 0 };
    CONFIGRET cr;
    unsigned Seen = 0;

    pGetProperty = hCfgMgr ? (FN_CM_Get_DevNode_PropertyW)GetProcAddress(hCfgMgr, "CM_Get_DevNode_PropertyW") : NULL;
    pGetPropertyEx = hCfgMgr ? (FN_CM_Get_DevNode_Property_ExW)GetProcAddress(hCfgMgr, "CM_Get_DevNode_Property_ExW") : NULL;
    if (!pGetProperty)
    {
        ok(!is_reactos(), "CM_Get_DevNode_PropertyW is not exported by cfgmgr32.dll\n");
        skip("CM_Get_DevNode_PropertyW is not available\n");
        return;
    }
    ok(pGetPropertyEx != NULL, "CM_Get_DevNode_Property_ExW is not exported by cfgmgr32.dll\n");

    cr = CM_Locate_DevNodeW(&Root, NULL, 0);
    ok(cr == CR_SUCCESS, "CM_Locate_DevNode failed with %lx\n", cr);
    if (cr != CR_SUCCESS)
        return;

    /* parameters */
    Type = NOT_SET;
    Size = sizeof(Buffer);
    cr = pGetProperty(0, &DEVPKEY_NAME, &Type, Buffer, &Size, 0);
    ok(cr == CR_INVALID_DEVNODE, "devnode 0: %lx\n", cr);
    Size = sizeof(Buffer);
    cr = pGetProperty(0x12345, &DEVPKEY_NAME, &Type, Buffer, &Size, 0);
    ok(cr == CR_INVALID_DEVNODE, "bogus devnode: %lx\n", cr);
    Size = sizeof(Buffer);
    cr = pGetProperty(Root, NULL, &Type, Buffer, &Size, 0);
    ok(cr == CR_FAILURE, "no key: %lx\n", cr);
    Size = sizeof(Buffer);
    cr = pGetProperty(Root, &DEVPKEY_Device_InstanceId, NULL, Buffer, &Size, 0);
    ok(cr == CR_INVALID_POINTER, "no type: %lx\n", cr);
    cr = pGetProperty(Root, &DEVPKEY_Device_InstanceId, &Type, Buffer, NULL, 0);
    ok(cr == CR_INVALID_POINTER, "no size: %lx\n", cr);
    Size = sizeof(Buffer);
    cr = pGetProperty(Root, &DEVPKEY_Device_InstanceId, &Type, NULL, &Size, 0);
    ok(cr == CR_INVALID_POINTER, "no buffer with a size: %lx\n", cr);
    Size = sizeof(Buffer);
    cr = pGetProperty(Root, &DEVPKEY_Device_InstanceId, &Type, Buffer, &Size, 1);
    ok(cr == CR_INVALID_FLAG, "flags 1: %lx\n", cr);

    /* a key nothing has */
    Type = NOT_SET;
    Size = sizeof(Buffer);
    cr = pGetProperty(Root, &Unknown, &Type, Buffer, &Size, 0);
    ok(cr == CR_NO_SUCH_VALUE && Size == 0 && Type == NOT_SET, "unknown key: %lx, size %lu, type %lx\n", cr, Size, Type);
    Size = 0;
    cr = pGetProperty(Root, &Unknown, &Type, NULL, &Size, 0);
    ok(cr == CR_NO_SUCH_VALUE && Size == 0 && Type == NOT_SET, "unknown key, no buffer: %lx, size %lu, type %lx\n", cr, Size, Type);

    /* the extended call with the local machine */
    if (pGetPropertyEx)
    {
        Type = NOT_SET;
        Size = sizeof(Buffer);
        cr = pGetPropertyEx(Root, &DEVPKEY_Device_InstanceId, &Type, Buffer, &Size, 0, NULL);
        ok(cr == CR_SUCCESS && Type == DEVPROP_TYPE_STRING, "extended call: %lx, type %lx\n", cr, Type);
    }

    /* the devnodes below the root (not the root: its capabilities are not queried on ReactOS, which asserts there) */
    for (cr = CM_Get_Child(&Child, Root, 0); cr == CR_SUCCESS && Seen < 6; cr = CM_Get_Sibling(&Child, Child, 0), Seen++)
    {
        CheckDevNode(hCfgMgr, Child, Counts);
        if (CM_Get_Child(&Grandchild, Child, 0) == CR_SUCCESS)
            CheckDevNode(hCfgMgr, Grandchild, Counts);
    }
    ok(Seen > 0, "the root has no children\n");
    ok(Counts[0] > 0, "no property was found on any devnode\n");
}
