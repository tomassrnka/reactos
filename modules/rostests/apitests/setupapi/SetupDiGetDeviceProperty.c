/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for SetupDiGetDevicePropertyW
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected types and errors were measured on Windows Server 2008 R2 (x86 and x64).
 * Windows 10 differs in them, so the test is skipped there.
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#define INITGUID
#include <stdio.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <devpkey.h>

typedef BOOL (WINAPI *FN_SetupDiGetDevicePropertyW)(HDEVINFO, PSP_DEVINFO_DATA, const DEVPROPKEY *, DEVPROPTYPE *,
                                                    PBYTE, DWORD, PDWORD, DWORD);

static FN_SetupDiGetDevicePropertyW pSetupDiGetDevicePropertyW;

typedef CONFIGRET (WINAPI *FN_CM_Get_DevNode_Status)(PULONG, PULONG, DEVINST, ULONG);
typedef CONFIGRET (WINAPI *FN_CM_Get_Parent)(PDEVINST, DEVINST, ULONG);
typedef CONFIGRET (WINAPI *FN_CM_Get_Device_IDW)(DEVINST, PWSTR, ULONG, ULONG);
typedef CONFIGRET (WINAPI *FN_CM_Get_DevNode_Registry_PropertyW)(DEVINST, ULONG, PULONG, PVOID, PULONG, ULONG);

static FN_CM_Get_DevNode_Status pCM_Get_DevNode_Status;
static FN_CM_Get_Parent pCM_Get_Parent;
static FN_CM_Get_Device_IDW pCM_Get_Device_IDW;
static FN_CM_Get_DevNode_Registry_PropertyW pCM_Get_DevNode_Registry_PropertyW;

#define NOT_SET 0xDEADBEEF

/* the configuration flags that decide the installation state */
#define TEST_CONFIGFLAG_REINSTALL       0x00000020
#define TEST_CONFIGFLAG_FAILEDINSTALL   0x00000040
#define TEST_CONFIGFLAG_FINISH_INSTALL  0x00000400

/* The type that each SPDRP_* property has as a device property, indexed by SPDRP_*; 0xFFFF for the unused slots */
static const USHORT ExpectedTypes[] =
{
    DEVPROP_TYPE_STRING, DEVPROP_TYPE_STRING_LIST, DEVPROP_TYPE_STRING_LIST, 0xFFFF, DEVPROP_TYPE_STRING, 0xFFFF, 0xFFFF,
    DEVPROP_TYPE_STRING, DEVPROP_TYPE_GUID, DEVPROP_TYPE_STRING, DEVPROP_TYPE_UINT32, DEVPROP_TYPE_STRING, DEVPROP_TYPE_STRING,
    DEVPROP_TYPE_STRING, DEVPROP_TYPE_STRING, DEVPROP_TYPE_UINT32, DEVPROP_TYPE_UINT32, DEVPROP_TYPE_STRING_LIST,
    DEVPROP_TYPE_STRING_LIST, DEVPROP_TYPE_GUID, DEVPROP_TYPE_UINT32, DEVPROP_TYPE_UINT32, DEVPROP_TYPE_STRING,
    DEVPROP_TYPE_SECURITY_DESCRIPTOR, DEVPROP_TYPE_SECURITY_DESCRIPTOR_STRING, DEVPROP_TYPE_UINT32, DEVPROP_TYPE_BOOLEAN,
    DEVPROP_TYPE_UINT32, DEVPROP_TYPE_UINT32, DEVPROP_TYPE_STRING, DEVPROP_TYPE_BINARY, DEVPROP_TYPE_UINT32,
    DEVPROP_TYPE_UINT32, DEVPROP_TYPE_UINT32, DEVPROP_TYPE_UINT32,
};

/* The properties of the first 35 slots that every Windows version has (SPDRP_MAXIMUM_PROPERTY of the old SDKs) */
#define SPDRP_TESTED 35

static void TestArguments(HDEVINFO DeviceSet, SP_DEVINFO_DATA *Device)
{
    DEVPROPKEY Key = DEVPKEY_Device_DeviceDesc;
    DEVPROPKEY Unknown = { { 0x12345678, 0x1234, 0x1234, { 1, 2, 3, 4, 5, 6, 7, 8 } }, 3 };
    SP_DEVINFO_DATA BadSize = *Device, NoReserved = *Device;
    DEVPROPTYPE Type;
    BYTE Buffer[4096];
    DWORD Required;
    BOOL Ret;

    BadSize.cbSize = 4;
    NoReserved.Reserved = 0;

#define CALL(handle, data, key, type, buf, size, req, flags) \
    do { \
        Type = NOT_SET; Required = NOT_SET; SetLastError(0xDEADBEEF); \
        Ret = pSetupDiGetDevicePropertyW(handle, data, key, type, buf, size, req, flags); \
    } while (0)

#define EXPECT_FAIL(label, error) \
    do { \
        ok(!Ret, "%s: succeeded\n", label); \
        ok(GetLastError() == (error), "%s: error 0x%lx, expected 0x%lx\n", label, GetLastError(), (DWORD)(error)); \
    } while (0)

    CALL(DeviceSet, Device, &Key, &Type, Buffer, sizeof(Buffer), &Required, 0);
    ok(Ret, "The baseline call failed: 0x%lx\n", GetLastError());

    CALL(NULL, Device, &Key, &Type, Buffer, sizeof(Buffer), &Required, 0);
    EXPECT_FAIL("NULL set", ERROR_INVALID_HANDLE);
    CALL(INVALID_HANDLE_VALUE, Device, &Key, &Type, Buffer, sizeof(Buffer), &Required, 0);
    EXPECT_FAIL("invalid set", ERROR_INVALID_HANDLE);
    CALL(NULL, Device, &Key, &Type, Buffer, sizeof(Buffer), &Required, 1);
    EXPECT_FAIL("NULL set, flags", ERROR_INVALID_HANDLE);

    CALL(DeviceSet, NULL, &Key, &Type, Buffer, sizeof(Buffer), &Required, 0);
    EXPECT_FAIL("NULL device", ERROR_INVALID_PARAMETER);
    CALL(DeviceSet, &BadSize, &Key, &Type, Buffer, sizeof(Buffer), &Required, 0);
    EXPECT_FAIL("bad size", ERROR_INVALID_PARAMETER);
    CALL(DeviceSet, &NoReserved, &Key, &Type, Buffer, sizeof(Buffer), &Required, 0);
    EXPECT_FAIL("no reserved", ERROR_INVALID_PARAMETER);
    CALL(DeviceSet, &BadSize, &Key, &Type, Buffer, sizeof(Buffer), &Required, 1);
    EXPECT_FAIL("bad size, flags", ERROR_INVALID_PARAMETER);
    CALL(DeviceSet, NULL, &Key, &Type, Buffer, sizeof(Buffer), &Required, 1);
    EXPECT_FAIL("NULL device, flags", ERROR_INVALID_PARAMETER);

    CALL(DeviceSet, Device, &Key, &Type, Buffer, sizeof(Buffer), &Required, 1);
    EXPECT_FAIL("flags 1", ERROR_INVALID_FLAGS);
    CALL(DeviceSet, Device, &Key, &Type, Buffer, sizeof(Buffer), &Required, 0x80000000);
    EXPECT_FAIL("flags 0x80000000", ERROR_INVALID_FLAGS);
    CALL(DeviceSet, Device, NULL, &Type, Buffer, sizeof(Buffer), &Required, 1);
    EXPECT_FAIL("NULL key, flags", ERROR_INVALID_FLAGS);
    CALL(DeviceSet, Device, &Key, NULL, Buffer, sizeof(Buffer), &Required, 1);
    EXPECT_FAIL("NULL type, flags", ERROR_INVALID_FLAGS);
    CALL(DeviceSet, Device, &Unknown, &Type, Buffer, sizeof(Buffer), &Required, 1);
    EXPECT_FAIL("unknown key, flags", ERROR_INVALID_FLAGS);
    ok(Type == NOT_SET && Required == NOT_SET, "Outputs written: type %lx, size %lu\n", Type, Required);

    CALL(DeviceSet, Device, &Key, NULL, Buffer, sizeof(Buffer), &Required, 0);
    EXPECT_FAIL("NULL type", ERROR_INVALID_USER_BUFFER);
    CALL(DeviceSet, Device, &Key, &Type, NULL, 5, &Required, 0);
    EXPECT_FAIL("NULL buffer with a size", ERROR_INVALID_USER_BUFFER);
    CALL(DeviceSet, Device, NULL, NULL, Buffer, sizeof(Buffer), &Required, 0);
    EXPECT_FAIL("NULL key and type", ERROR_INVALID_USER_BUFFER);
    CALL(DeviceSet, Device, &Unknown, NULL, Buffer, sizeof(Buffer), &Required, 0);
    EXPECT_FAIL("unknown key, NULL type", ERROR_INVALID_USER_BUFFER);

    CALL(DeviceSet, Device, NULL, &Type, Buffer, sizeof(Buffer), &Required, 0);
    EXPECT_FAIL("NULL key", ERROR_INVALID_DATA);

    /* A key nothing knows: not found, the type is "empty" and the size stays */
    CALL(DeviceSet, Device, &Unknown, &Type, Buffer, sizeof(Buffer), &Required, 0);
    EXPECT_FAIL("unknown key", ERROR_NOT_FOUND);
    ok(Type == DEVPROP_TYPE_EMPTY, "Unknown key: type 0x%lx\n", Type);
    ok(Required == NOT_SET, "Unknown key: required size %lu\n", Required);

    /* The device family has no property 0, 1, the unused slots and beyond the last one */
    {
        static const DWORD BadIds[] = { 0, 1, 5, 7, 8, 39, 40, 60, 1000 };
        unsigned i;

        for (i = 0; i < ARRAYSIZE(BadIds); i++)
        {
            Key = DEVPKEY_Device_DeviceDesc;
            Key.pid = BadIds[i];
            CALL(DeviceSet, Device, &Key, &Type, Buffer, sizeof(Buffer), &Required, 0);
            EXPECT_FAIL("bad property id", ERROR_INVALID_REG_PROPERTY);
            ok(Type == DEVPROP_TYPE_EMPTY, "Property id %lu: type 0x%lx\n", BadIds[i], Type);
            ok(Required == NOT_SET, "Property id %lu: required size %lu\n", BadIds[i], Required);
        }
    }

    /* Buffer sizes: too small gives the type and the size, exact and larger work */
    Key = DEVPKEY_Device_DeviceDesc;
    CALL(DeviceSet, Device, &Key, &Type, Buffer, sizeof(Buffer), &Required, 0);
    ok(Ret && Type == DEVPROP_TYPE_STRING, "DeviceDesc: %d, type 0x%lx, error 0x%lx\n", Ret, Type, GetLastError());
    if (Ret)
    {
        DWORD Size = Required;

        ok(Size >= sizeof(WCHAR) && !(Size % sizeof(WCHAR)), "Size %lu\n", Size);
        CALL(DeviceSet, Device, &Key, &Type, NULL, 0, &Required, 0);
        EXPECT_FAIL("size query", ERROR_INSUFFICIENT_BUFFER);
        ok(Type == DEVPROP_TYPE_STRING && Required == Size, "Size query: type 0x%lx, size %lu, expected %lu\n", Type, Required, Size);
        CALL(DeviceSet, Device, &Key, &Type, NULL, 0, NULL, 0);
        EXPECT_FAIL("size query without a size", ERROR_INSUFFICIENT_BUFFER);
        ok(Type == DEVPROP_TYPE_STRING, "Type 0x%lx\n", Type);
        CALL(DeviceSet, Device, &Key, &Type, Buffer, Size - 1, &Required, 0);
        EXPECT_FAIL("one byte short", ERROR_INSUFFICIENT_BUFFER);
        ok(Type == DEVPROP_TYPE_STRING && Required == Size, "One byte short: type 0x%lx, size %lu\n", Type, Required);
        CALL(DeviceSet, Device, &Key, &Type, Buffer, Size, &Required, 0);
        ok(Ret && Required == Size, "Exact size: %d, size %lu, error 0x%lx\n", Ret, Required, GetLastError());
        CALL(DeviceSet, Device, &Key, &Type, Buffer, Size, NULL, 0);
        ok(Ret, "Without a size pointer: error 0x%lx\n", GetLastError());
    }
#undef CALL
#undef EXPECT_FAIL
}

static BOOL ParseGuid(const WCHAR *String, GUID *Guid)
{
    unsigned Part[11];
    int i;

    if (swscanf(String, L"{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &Part[0], &Part[1], &Part[2], &Part[3], &Part[4],
                &Part[5], &Part[6], &Part[7], &Part[8], &Part[9], &Part[10]) != 11)
        return FALSE;
    Guid->Data1 = Part[0];
    Guid->Data2 = (USHORT)Part[1];
    Guid->Data3 = (USHORT)Part[2];
    for (i = 0; i < 8; i++)
        Guid->Data4[i] = (BYTE)Part[3 + i];
    return TRUE;
}

/* A registry property into a heap block; FALSE if the device does not have it */
static BOOL ReadRegistryProperty(HDEVINFO DeviceSet, SP_DEVINFO_DATA *Device, DWORD Property, DWORD *RegType, BYTE **Data, DWORD *Size)
{
    DWORD Capacity = 256;
    int Round;

    for (Round = 0; Round < 8; Round++)
    {
        *Data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Capacity + sizeof(WCHAR));
        *Size = 0;
        if (SetupDiGetDeviceRegistryPropertyW(DeviceSet, Device, Property, RegType, *Data, Capacity, Size))
            return TRUE;
        HeapFree(GetProcessHeap(), 0, *Data);
        *Data = NULL;
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return FALSE;
        Capacity = max(*Size, Capacity * 2);
    }
    return FALSE;
}

/* A device property into a heap block that grows until the value fits */
static BOOL ReadDeviceProperty(HDEVINFO DeviceSet, SP_DEVINFO_DATA *Device, const DEVPROPKEY *Key, DEVPROPTYPE *Type, BYTE **Data, DWORD *Size, DWORD *Error)
{
    DWORD Capacity = 256;
    int Round;

    for (Round = 0; Round < 8; Round++)
    {
        *Data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Capacity + sizeof(WCHAR));
        *Type = NOT_SET;
        *Size = NOT_SET;
        SetLastError(0xDEADBEEF);
        if (pSetupDiGetDevicePropertyW(DeviceSet, Device, Key, Type, *Data, Capacity, Size, 0))
        {
            *Error = ERROR_SUCCESS;
            return TRUE;
        }
        *Error = GetLastError();
        if (*Error != ERROR_INSUFFICIENT_BUFFER)
            break;
        HeapFree(GetProcessHeap(), 0, *Data);
        *Data = NULL;
        Capacity = max(*Size, Capacity * 2);
    }
    return FALSE;
}

/* The size protocol of a property that exists: too small gives the type and the size, the exact size works */
static void CheckBufferProtocol(HDEVINFO DeviceSet, SP_DEVINFO_DATA *Device, const DEVPROPKEY *Key, DEVPROPTYPE ExpectedType, DWORD Size, unsigned Index)
{
    DEVPROPTYPE Type;
    BYTE *Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size + 8);
    DWORD Required;
    BOOL Ret;

    ok(Buffer != NULL, "No memory\n");
    if (!Buffer)
        return;

    /* Windows Server 2008 R2 answers a size query for an empty string with a list type and 4 bytes, and a read into a large buffer with the string and 2 */
    if (Size <= sizeof(WCHAR) && !is_reactos())
    {
        HeapFree(GetProcessHeap(), 0, Buffer);
        return;
    }

    Type = NOT_SET; Required = NOT_SET; SetLastError(0xDEADBEEF);
    Ret = pSetupDiGetDevicePropertyW(DeviceSet, Device, Key, &Type, NULL, 0, &Required, 0);
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER && Type == ExpectedType && Required == Size,
       "Device %u, property %lu, size query: %d, error 0x%lx, type 0x%lx, size %lu, expected %lu\n",
       Index, Key->pid, Ret, GetLastError(), Type, Required, Size);

    if (Size > 1)
    {
        Type = NOT_SET; Required = NOT_SET; SetLastError(0xDEADBEEF);
        Ret = pSetupDiGetDevicePropertyW(DeviceSet, Device, Key, &Type, Buffer, Size - 1, &Required, 0);
        ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER && Type == ExpectedType && Required == Size,
           "Device %u, property %lu, one byte short: %d, error 0x%lx, type 0x%lx, size %lu\n",
           Index, Key->pid, Ret, GetLastError(), Type, Required);
    }

    Type = NOT_SET; Required = NOT_SET; SetLastError(0xDEADBEEF);
    Ret = pSetupDiGetDevicePropertyW(DeviceSet, Device, Key, &Type, Buffer, Size, &Required, 0);
    ok(Ret && Type == ExpectedType && Required == Size, "Device %u, property %lu, exact size: %d, error 0x%lx, type 0x%lx, size %lu\n",
       Index, Key->pid, Ret, GetLastError(), Type, Required);

    HeapFree(GetProcessHeap(), 0, Buffer);
}

static void TestDevice(HDEVINFO DeviceSet, SP_DEVINFO_DATA *Device, unsigned Index, unsigned *Counts)
{
    WCHAR InstanceId[256], Other[256];
    DEVPROPKEY Key;
    DEVPROPTYPE Type;
    BYTE *Value, *Legacy;
    DWORD Size, LegacyType, LegacySize, Property, Error;
    BOOL Ret;
    ULONG Status = 0, Problem = 0, StatusAfter = 0, ProblemAfter = 0;
    BOOL HaveStatus;
    DEVINST Parent;

    ok(SetupDiGetDeviceInstanceIdW(DeviceSet, Device, InstanceId, ARRAYSIZE(InstanceId), NULL), "No instance ID\n");

    /* The instance ID */
    Ret = ReadDeviceProperty(DeviceSet, Device, &DEVPKEY_Device_InstanceId, &Type, &Value, &Size, &Error);
    ok(Ret, "Device %u: instance ID failed: 0x%lx\n", Index, Error);
    if (Ret)
    {
        ok(Type == DEVPROP_TYPE_STRING, "Device %u: instance ID type 0x%lx\n", Index, Type);
        ok(Size == (wcslen(InstanceId) + 1) * sizeof(WCHAR), "Device %u: instance ID size %lu\n", Index, Size);
        ok(!wcscmp((WCHAR *)Value, InstanceId), "Device %u: instance ID '%S', expected '%S'\n", Index, (WCHAR *)Value, InstanceId);
        Counts[0]++;
    }
    if (Value)
        HeapFree(GetProcessHeap(), 0, Value);

    /* Every property that has a registry-property equivalent: same data, the type of the table */
    for (Property = 0; Property < SPDRP_TESTED; Property++)
    {
        /* Asking the root device node for its capabilities (the address, the power data) makes a checked ReactOS kernel assert in the PnP root enumerator */
        if ((Property == SPDRP_ADDRESS || Property == SPDRP_DEVICE_POWER_DATA) && !_wcsnicmp(InstanceId, L"HTREE\\", 6))
        {
            /* The root node has neither: the property is not found, and the capability request is not made */
            if (is_reactos())
            {
                BYTE RootBuffer[128];

                Key = DEVPKEY_Device_DeviceDesc;
                Key.pid = Property + 2;
                Size = 0;
                SetLastError(0xDEADBEEF);
                Ret = pSetupDiGetDevicePropertyW(DeviceSet, Device, &Key, &Type, RootBuffer, sizeof(RootBuffer), &Size, 0);
                ok(!Ret && GetLastError() == ERROR_NOT_FOUND, "Root node, property %lu: %d, error 0x%lx\n", Property, Ret, GetLastError());
            }
            continue;
        }

        Key = DEVPKEY_Device_DeviceDesc;
        Key.pid = Property + 2;
        Ret = ReadDeviceProperty(DeviceSet, Device, &Key, &Type, &Value, &Size, &Error);
        if (Ret && ExpectedTypes[Property] != 0xFFFF)
            CheckBufferProtocol(DeviceSet, Device, &Key, ExpectedTypes[Property], Size, Index);

        if (ExpectedTypes[Property] == 0xFFFF)
        {
            ok(!Ret && Error == ERROR_INVALID_REG_PROPERTY, "Device %u, property %lu: %d, error 0x%lx\n",
               Index, Property, Ret, Error);
            if (Value)
                HeapFree(GetProcessHeap(), 0, Value);
            continue;
        }

        /* The properties that are not read from the same value: each is found exactly when its own source has it */
        if (Property == SPDRP_PHYSICAL_DEVICE_OBJECT_NAME || Property == SPDRP_DEVICE_POWER_DATA || Property == SPDRP_INSTALL_STATE)
        {
            BOOL Expected = FALSE;
            BYTE *Source = NULL;
            DWORD SourceType = 0, SourceSize = 0;
            WCHAR PdoName[260] = L"";

            if (Property == SPDRP_PHYSICAL_DEVICE_OBJECT_NAME)
            {
                ULONG Length = sizeof(PdoName);

                Expected = pCM_Get_DevNode_Registry_PropertyW &&
                           pCM_Get_DevNode_Registry_PropertyW(Device->DevInst, CM_DRP_PHYSICAL_DEVICE_OBJECT_NAME, NULL, PdoName, &Length, 0) == CR_SUCCESS &&
                           PdoName[0] != UNICODE_NULL;
            }
            else
            {
                Expected = ReadRegistryProperty(DeviceSet, Device, Property, &SourceType, &Source, &SourceSize);
                if (Expected && Property == SPDRP_INSTALL_STATE && (SourceType != REG_DWORD || SourceSize != sizeof(DWORD)))
                    Expected = FALSE;
            }

            ok(Ret == Expected, "Device %u, property %lu: the device property %s, its source %s (error 0x%lx)\n",
               Index, Property, Ret ? "exists" : "does not", Expected ? "has it" : "does not", Error);
            if (Ret)
            {
                ok(Type == ExpectedTypes[Property], "Device %u, property %lu: type 0x%lx\n", Index, Property, Type);
                if (Property == SPDRP_PHYSICAL_DEVICE_OBJECT_NAME)
                {
                    ok(!_wcsnicmp((WCHAR *)Value, L"\\Device\\", 8), "Device %u: PDO name '%S'\n", Index, (WCHAR *)Value);
                    ok(!wcscmp((WCHAR *)Value, PdoName), "Device %u: PDO name '%S', expected '%S'\n", Index, (WCHAR *)Value, PdoName);
                }
                else if (Property == SPDRP_DEVICE_POWER_DATA)
                {
                    ok(Size == 56 && *(DWORD *)Value == 56, "Device %u: power data of %lu bytes, PD_Size %lu\n", Index, Size, *(DWORD *)Value);
                    ok(Source && SourceSize >= 56 && Size == 56 && !memcmp(Value, Source, 56), "Device %u: the power data differs\n", Index);
                }
                else
                {
                    DWORD Flags = Source ? *(DWORD *)Source : 0, State = CM_INSTALL_STATE_INSTALLED;

                    /* ReactOS has the configuration flags where Windows has the state */
                    if (is_reactos())
                    {
                        if (Flags & TEST_CONFIGFLAG_FAILEDINSTALL)
                            State = CM_INSTALL_STATE_FAILED_INSTALL;
                        else if (Flags & TEST_CONFIGFLAG_REINSTALL)
                            State = CM_INSTALL_STATE_NEEDS_REINSTALL;
                        else if (Flags & TEST_CONFIGFLAG_FINISH_INSTALL)
                            State = CM_INSTALL_STATE_FINISH_INSTALL;
                    }
                    else
                    {
                        State = Flags;
                    }
                    ok(Size == sizeof(DWORD) && *(DWORD *)Value == State, "Device %u: install state %lu bytes, value %lu, expected %lu\n",
                       Index, Size, *(DWORD *)Value, State);
                }
                Counts[1]++;
            }
            else
            {
                ok(Error == ERROR_NOT_FOUND, "Device %u, property %lu: error 0x%lx\n", Index, Property, Error);
            }
            if (Value)
                HeapFree(GetProcessHeap(), 0, Value);
            if (Source)
                HeapFree(GetProcessHeap(), 0, Source);
            continue;
        }

        if (!ReadRegistryProperty(DeviceSet, Device, Property, &LegacyType, &Legacy, &LegacySize))
        {
            ok(!Ret, "Device %u, property %lu: the registry property fails but the device property succeeds\n", Index, Property);
            ok(Error == ERROR_NOT_FOUND, "Device %u, property %lu: error 0x%lx\n", Index, Property, Error);
            ok(Type == DEVPROP_TYPE_EMPTY, "Device %u, property %lu: type 0x%lx\n", Index, Property, Type);
            if (Value)
                HeapFree(GetProcessHeap(), 0, Value);
            continue;
        }

        ok(Ret, "Device %u, property %lu: failed with 0x%lx\n", Index, Property, Error);
        if (Ret)
        {
            ok(Type == ExpectedTypes[Property], "Device %u, property %lu: type 0x%lx, expected 0x%x\n",
               Index, Property, Type, ExpectedTypes[Property]);
            Counts[1]++;
            if (ExpectedTypes[Property] == DEVPROP_TYPE_GUID && Property == SPDRP_CLASSGUID)
            {
                GUID Guid;

                ok(Size == sizeof(GUID), "Device %u, class GUID size %lu\n", Index, Size);
                ok(ParseGuid((WCHAR *)Legacy, &Guid) && !memcmp(&Guid, Value, sizeof(GUID)), "Device %u: class GUID differs\n", Index);
            }
            else if (ExpectedTypes[Property] == DEVPROP_TYPE_STRING)
            {
                /* A string has one terminator, also when the registry value has more */
                ok(Size == (wcslen((WCHAR *)Value) + 1) * sizeof(WCHAR), "Device %u, property %lu: size %lu, string '%S'\n",
                   Index, Property, Size, (WCHAR *)Value);
                ok(Size <= LegacySize, "Device %u, property %lu: size %lu, the registry value has %lu\n", Index, Property, Size, LegacySize);
                ok(!wcscmp((WCHAR *)Value, (WCHAR *)Legacy), "Device %u, property %lu: '%S', expected '%S'\n",
                   Index, Property, (WCHAR *)Value, (WCHAR *)Legacy);
            }
            else if (ExpectedTypes[Property] == DEVPROP_TYPE_STRING_LIST)
            {
                /* The list ends with one empty string, also when the registry value has more terminators */
                const WCHAR *List = (const WCHAR *)Value;
                DWORD Position = 0;

                while (Position < Size / sizeof(WCHAR) && List[Position])
                    Position += wcslen(&List[Position]) + 1;
                ok(Size == (Position + 1) * sizeof(WCHAR), "Device %u, property %lu: size %lu, strings end at %lu\n", Index, Property, Size, Position);
                ok(Size <= LegacySize, "Device %u, property %lu: size %lu, the registry value has %lu\n", Index, Property, Size, LegacySize);
                ok(Size <= LegacySize && !memcmp(Value, Legacy, Size - sizeof(WCHAR)), "Device %u, property %lu: the data differs\n", Index, Property);
            }
            else if (ExpectedTypes[Property] == DEVPROP_TYPE_BOOLEAN)
            {
                /* The registry value is a DWORD; the property is one byte, 0xFF for any nonzero value */
                ok(LegacyType == REG_DWORD && LegacySize == sizeof(DWORD), "Device %u, property %lu: registry type %lu, size %lu\n",
                   Index, Property, LegacyType, LegacySize);
                ok(Size == 1 && LegacySize == sizeof(DWORD) && Value[0] == (*(DWORD *)Legacy ? 0xFF : 0x00),
                   "Device %u, property %lu: size %lu, value %u\n", Index, Property, Size, Value[0]);
            }
            else if (ExpectedTypes[Property] == DEVPROP_TYPE_UINT32)
            {
                /* A number is a four-byte DWORD; only the default removal policy has fewer bytes in the registry-property call of ReactOS, then the rest is zero */
                if (Property == SPDRP_REMOVAL_POLICY_HW_DEFAULT && is_reactos() && LegacySize == 1)
                {
                    ok(Size == sizeof(DWORD) && *(DWORD *)Value == Legacy[0], "Device %u, property %lu: size %lu, value %lu, the byte is %u\n",
                       Index, Property, Size, *(DWORD *)Value, Legacy[0]);
                }
                else
                {
                    ok(LegacyType == REG_DWORD && LegacySize == sizeof(DWORD), "Device %u, property %lu: the registry value has type %lu, %lu bytes\n",
                       Index, Property, LegacyType, LegacySize);
                    ok(Size == sizeof(DWORD) && LegacySize == sizeof(DWORD) && !memcmp(Value, Legacy, sizeof(DWORD)),
                       "Device %u, property %lu: size %lu, the data differs\n", Index, Property, Size);
                }
            }
            else
            {
                ok(Size == LegacySize, "Device %u, property %lu: size %lu, expected %lu\n", Index, Property, Size, LegacySize);
                ok(Size == LegacySize && !memcmp(Value, Legacy, Size), "Device %u, property %lu: the data differs\n", Index, Property);
            }
        }
        if (Value)
            HeapFree(GetProcessHeap(), 0, Value);
        HeapFree(GetProcessHeap(), 0, Legacy);
    }

    /* The name is the friendly name, else the description */
    Ret = ReadDeviceProperty(DeviceSet, Device, &DEVPKEY_NAME, &Type, &Value, &Size, &Error);
    Legacy = NULL;
    if (!ReadRegistryProperty(DeviceSet, Device, SPDRP_FRIENDLYNAME, &LegacyType, &Legacy, &LegacySize))
        ReadRegistryProperty(DeviceSet, Device, SPDRP_DEVICEDESC, &LegacyType, &Legacy, &LegacySize);
    ok(Ret == (Legacy != NULL), "Device %u: name %d (error 0x%lx), registry %d\n", Index, Ret, Error, Legacy != NULL);
    if (Ret && Legacy)
    {
        ok(Type == DEVPROP_TYPE_STRING, "Device %u: name type 0x%lx\n", Index, Type);
        ok(!wcscmp((WCHAR *)Value, (WCHAR *)Legacy), "Device %u: name '%S', expected '%S'\n", Index, (WCHAR *)Value, (WCHAR *)Legacy);
        Counts[2]++;
    }
    if (Legacy)
        HeapFree(GetProcessHeap(), 0, Legacy);
    if (Value)
        HeapFree(GetProcessHeap(), 0, Value);

    /*
     * The status and the problem code: what the configuration manager says before and after the call. The value is
     * checked when the device did not change in between (it can change or go away at any time); a device that never
     * holds still is not checked.
     */
    {
        static const struct { const DEVPROPKEY *Key; BOOL Problem; PCSTR Name; } Cases[] =
        {
            { &DEVPKEY_Device_DevNodeStatus, FALSE, "status" },
            { &DEVPKEY_Device_ProblemCode, TRUE, "problem code" },
        };
        unsigned c, Attempt;

        for (c = 0; c < ARRAYSIZE(Cases); c++)
        {
            BOOL Checked = FALSE, Present = FALSE;

            for (Attempt = 0; Attempt < 10 && !Checked; Attempt++)
            {
                HaveStatus = pCM_Get_DevNode_Status(&Status, &Problem, Device->DevInst, 0) == CR_SUCCESS;
                Ret = ReadDeviceProperty(DeviceSet, Device, Cases[c].Key, &Type, &Value, &Size, &Error);
                if (Ret)
                {
                    Present = TRUE;
                    ok(Type == DEVPROP_TYPE_UINT32 && Size == sizeof(DWORD), "Device %u: %s type 0x%lx, size %lu\n",
                       Index, Cases[c].Name, Type, Size);
                    if (HaveStatus && Size == sizeof(DWORD) &&
                        pCM_Get_DevNode_Status(&StatusAfter, &ProblemAfter, Device->DevInst, 0) == CR_SUCCESS &&
                        StatusAfter == Status && ProblemAfter == Problem)
                    {
                        DWORD Expected = Cases[c].Problem ? Problem : Status;

                        ok(*(DWORD *)Value == Expected, "Device %u: %s 0x%lx, expected 0x%lx\n", Index, Cases[c].Name, *(DWORD *)Value, Expected);
                        Checked = TRUE;
                    }
                }
                if (Value)
                    HeapFree(GetProcessHeap(), 0, Value);
                Value = NULL;
            }
            /* A device that is gone between the calls has neither; one that stays has both */
            if (!Present && pCM_Get_DevNode_Status(&Status, &Problem, Device->DevInst, 0) == CR_SUCCESS)
                ok(0, "Device %u: %s failed with 0x%lx, but the configuration manager knows the device\n", Index, Cases[c].Name, Error);
            else if (Present)
                Counts[c ? 4 : 3]++;
        }
    }

    /* The parent */
    Ret = ReadDeviceProperty(DeviceSet, Device, &DEVPKEY_Device_Parent, &Type, &Value, &Size, &Error);
    if (pCM_Get_Parent(&Parent, Device->DevInst, 0) == CR_SUCCESS &&
        pCM_Get_Device_IDW(Parent, Other, ARRAYSIZE(Other), 0) == CR_SUCCESS)
    {
        ok(Ret, "Device %u: parent failed: 0x%lx\n", Index, Error);
        if (Ret)
        {
            ok(Type == DEVPROP_TYPE_STRING, "Device %u: parent type 0x%lx\n", Index, Type);
            ok(!_wcsicmp((WCHAR *)Value, Other), "Device %u: parent '%S', expected '%S'\n", Index, (WCHAR *)Value, Other);
            Counts[5]++;
        }
    }
    else
    {
        ok(!Ret, "Device %u: a parent, but the device node has none\n", Index);
    }
    if (Value)
        HeapFree(GetProcessHeap(), 0, Value);
}

static void TestForeignDevice(HDEVINFO DeviceSet, SP_DEVINFO_DATA *Device)
{
    HDEVINFO Other = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    DEVPROPTYPE Type;
    BYTE Buffer[512];
    DWORD Size;
    BOOL Ret;

    if (Other == INVALID_HANDLE_VALUE)
        return;

    /* A device of another set is not a device of this one */
    SetLastError(0xDEADBEEF);
    Ret = pSetupDiGetDevicePropertyW(Other, Device, &DEVPKEY_Device_InstanceId, &Type, Buffer, sizeof(Buffer), &Size, 0);
    ok(!Ret && GetLastError() == ERROR_INVALID_PARAMETER, "Foreign device: %d, error 0x%lx\n", Ret, GetLastError());

    SetupDiDestroyDeviceInfoList(Other);
}

/*
 * ReactOS reads the installation state from the configuration flags of the device: the flags of a device are set to
 * each combination and the property is read (the value that was there is put back). Windows keeps the state apart.
 */
static void TestInstallState(HDEVINFO DeviceSet)
{
    static const struct { DWORD Flags; DWORD State; } Cases[] =
    {
        { 0x00, 0 }, { 0x20, 1 }, { 0x40, 2 }, { 0x400, 3 }, { 0x60, 2 }, { 0x420, 1 }, { 0x440, 2 }, { 0x460, 2 }, { 0x01, 0 }, { 0x04, 0 },
    };
    SP_DEVINFO_DATA Device;
    DWORD Index, Original, Size = sizeof(Original), Type, i;
    WCHAR InstanceId[256];
    BOOL Found = FALSE;

    if (!is_reactos())
        return;

    Device.cbSize = sizeof(Device);
    for (Index = 0; SetupDiEnumDeviceInfo(DeviceSet, Index, &Device); Index++)
    {
        if (!SetupDiGetDeviceInstanceIdW(DeviceSet, &Device, InstanceId, ARRAYSIZE(InstanceId), NULL))
            continue;
        /* a device that nothing depends on while the test runs: one that the root enumerator made */
        if (_wcsnicmp(InstanceId, L"ROOT\\", 5))
            continue;
        Size = sizeof(Original);
        if (SetupDiGetDeviceRegistryPropertyW(DeviceSet, &Device, SPDRP_CONFIGFLAGS, &Type, (BYTE *)&Original, sizeof(Original), &Size) &&
            Type == REG_DWORD && Size == sizeof(Original))
        {
            Found = TRUE;
            break;
        }
    }
    if (!Found)
    {
        skip("No device whose configuration flags can be changed\n");
        return;
    }

    for (i = 0; i < ARRAYSIZE(Cases); i++)
    {
        DWORD Flags = Cases[i].Flags, State = 0xFFFFFFFF, Required = 0;
        DEVPROPTYPE PropertyType = 0;
        BOOL Ret;

        if (!SetupDiSetDeviceRegistryPropertyW(DeviceSet, &Device, SPDRP_CONFIGFLAGS, (BYTE *)&Flags, sizeof(Flags)))
        {
            skip("The configuration flags of %S cannot be set: 0x%lx\n", InstanceId, GetLastError());
            break;
        }
        SetLastError(0xDEADBEEF);
        Ret = pSetupDiGetDevicePropertyW(DeviceSet, &Device, &DEVPKEY_Device_InstallState, &PropertyType, (BYTE *)&State, sizeof(State), &Required, 0);
        ok(Ret, "%S, flags 0x%lx: failed with 0x%lx\n", InstanceId, Flags, GetLastError());
        ok(!Ret || (PropertyType == DEVPROP_TYPE_UINT32 && Required == sizeof(DWORD) && State == Cases[i].State),
           "%S, flags 0x%lx: state %lu (type 0x%lx, %lu bytes), expected %lu\n", InstanceId, Flags, State, PropertyType, Required, Cases[i].State);
    }

    /* A number is a four-byte REG_DWORD: the same bytes under another type, or fewer of them, are not a number */
    {
        static const struct { DWORD RegType; DWORD Size; BOOL Number; } Raw[] =
        {
            { REG_DWORD, 4, TRUE }, { REG_BINARY, 4, FALSE }, { REG_BINARY, 1, FALSE }, { REG_DWORD, 2, FALSE }, { REG_BINARY, 3, FALSE },
        };
        WCHAR KeyName[320];
        HKEY Key;

        wcscpy(KeyName, L"SYSTEM\\CurrentControlSet\\Enum\\");
        wcscat(KeyName, InstanceId);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, KEY_SET_VALUE, &Key))
        {
            skip("The key of %S cannot be opened\n", InstanceId);
        }
        else
        {
            static const BYTE Bytes[4] = { 0x20, 0, 0, 0 };

            for (i = 0; i < ARRAYSIZE(Raw); i++)
            {
                DWORD Value = 0xFFFFFFFF, Required = 0;
                DEVPROPTYPE PropertyType = 0;
                BOOL Ret;

                if (RegSetValueExW(Key, L"ConfigFlags", 0, Raw[i].RegType, Bytes, Raw[i].Size))
                {
                    skip("The configuration flags of %S cannot be written\n", InstanceId);
                    break;
                }
                SetLastError(0xDEADBEEF);
                Ret = pSetupDiGetDevicePropertyW(DeviceSet, &Device, &DEVPKEY_Device_ConfigFlags, &PropertyType, (BYTE *)&Value, sizeof(Value), &Required, 0);
                if (Raw[i].Number)
                {
                    ok(Ret && PropertyType == DEVPROP_TYPE_UINT32 && Value == 0x20, "Registry type %lu, %lu bytes: %d, error 0x%lx, type 0x%lx, value 0x%lx\n",
                       Raw[i].RegType, Raw[i].Size, Ret, GetLastError(), PropertyType, Value);
                }
                else
                {
                    ok(!Ret && GetLastError() == ERROR_NOT_FOUND, "Registry type %lu, %lu bytes: %d, error 0x%lx, expected not found\n",
                       Raw[i].RegType, Raw[i].Size, Ret, GetLastError());
                }
            }
            RegCloseKey(Key);
        }
    }

    SetupDiSetDeviceRegistryPropertyW(DeviceSet, &Device, SPDRP_CONFIGFLAGS, (BYTE *)&Original, sizeof(Original));
}

START_TEST(SetupDiGetDeviceProperty)
{
    HDEVINFO DeviceSet;
    SP_DEVINFO_DATA Device;
    HMODULE hCfgMgr;
    unsigned Index, Counts[6] = { 0 };

    pSetupDiGetDevicePropertyW = (FN_SetupDiGetDevicePropertyW)GetProcAddress(GetModuleHandleW(L"setupapi.dll"),
                                                                              "SetupDiGetDevicePropertyW");
    if (!pSetupDiGetDevicePropertyW)
    {
        skip("SetupDiGetDevicePropertyW is not available\n");
        return;
    }
    if (!is_reactos() && GetNTVersion() > _WIN32_WINNT_WIN7)
    {
        skip("Newer Windows versions differ in the property types, the error codes and the output values on failure\n");
        return;
    }

    hCfgMgr = LoadLibraryW(L"cfgmgr32.dll");
    {
        pCM_Get_DevNode_Status = (FN_CM_Get_DevNode_Status)GetProcAddress(hCfgMgr, "CM_Get_DevNode_Status");
        pCM_Get_Parent = (FN_CM_Get_Parent)GetProcAddress(hCfgMgr, "CM_Get_Parent");
        pCM_Get_Device_IDW = (FN_CM_Get_Device_IDW)GetProcAddress(hCfgMgr, "CM_Get_Device_IDW");
        pCM_Get_DevNode_Registry_PropertyW = (FN_CM_Get_DevNode_Registry_PropertyW)GetProcAddress(hCfgMgr, "CM_Get_DevNode_Registry_PropertyW");
        if (!hCfgMgr || !pCM_Get_DevNode_Status || !pCM_Get_Parent || !pCM_Get_Device_IDW)
        {
            skip("cfgmgr32 is not available\n");
            if (hCfgMgr)
                FreeLibrary(hCfgMgr);
            return;
        }
    }

    DeviceSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    ok(DeviceSet != INVALID_HANDLE_VALUE, "SetupDiGetClassDevsW failed: 0x%lx\n", GetLastError());
    if (DeviceSet == INVALID_HANDLE_VALUE)
    {
        FreeLibrary(hCfgMgr);
        return;
    }

    Device.cbSize = sizeof(Device);
    if (!SetupDiEnumDeviceInfo(DeviceSet, 0, &Device))
    {
        ok(GetLastError() == ERROR_NO_MORE_ITEMS, "The first enumeration failed with 0x%lx\n", GetLastError());
        skip("No devices\n");
        SetupDiDestroyDeviceInfoList(DeviceSet);
        FreeLibrary(hCfgMgr);
        return;
    }

    TestArguments(DeviceSet, &Device);
    TestForeignDevice(DeviceSet, &Device);
    TestInstallState(DeviceSet);

    for (Index = 0; ; Index++)
    {
        Device.cbSize = sizeof(Device);
        if (!SetupDiEnumDeviceInfo(DeviceSet, Index, &Device))
        {
            ok(GetLastError() == ERROR_NO_MORE_ITEMS, "Enumeration stopped at device %u with 0x%lx\n", Index, GetLastError());
            break;
        }
        TestDevice(DeviceSet, &Device, Index, Counts);
    }
    trace("%u devices: %u instance IDs, %u registry-backed properties, %u names, %u statuses, %u problem codes, %u parents\n",
          Index, Counts[0], Counts[1], Counts[2], Counts[3], Counts[4], Counts[5]);
    ok(Counts[0] == Index, "Not every device has an instance ID\n");
    ok(Counts[1] != 0, "No registry-backed property at all\n");

    SetupDiDestroyDeviceInfoList(DeviceSet);
    FreeLibrary(hCfgMgr);
}
