/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     NDIS request serialization and abort on reset, halt and remove, with a fake miniport
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The fake miniport (modules/rostests/drivers/fakemp, in a serialized and a deserialized build) is
 * a root-enumerated device the test creates itself. NDIS requests come from the protocol side
 * through ndisuio (set and query OID on a binding), one thread per request, and the miniport is
 * told what to do with them (pend, never complete, spin in the handler, reset) over its control device.
 *
 * Run it once per boot: a removed root device stays a leaked device node until then.
 *
 * Optional argument after the test name: a scenario name
 * (serial, abort, abort1, late, deferred, deferred1, halt, remove).
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <ndk/iofuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>
#include <winioctl.h>
#include <winreg.h>
#include <cfgmgr32.h>
#include <shlwapi.h>

#include "fakemp.h"

#define FormatW(Buffer, ...) _snwprintf(Buffer, ARRAYSIZE(Buffer) - 1, __VA_ARGS__)

/* NDIS statuses, as ndisuio hands them back as the IOCTL's NTSTATUS */
#define NDIS_STATUS_REQUEST_ABORTED_    ((NTSTATUS)0xC001000C)

#define IOCTL_NDISUIO_OPEN_DEVICE_      CTL_CODE(FILE_DEVICE_NETWORK, 0x200, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NDISUIO_QUERY_OID_        CTL_CODE(FILE_DEVICE_NETWORK, 0x201, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_NDISUIO_SET_OID_          CTL_CODE(FILE_DEVICE_NETWORK, 0x205, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define NET_CLASS_GUID      L"{4D36E972-E325-11CE-BFC1-08002BE10318}"
#define CLASS_KEY           L"SYSTEM\\CurrentControlSet\\Control\\Class\\" NET_CLASS_GUID
#define ENUM_ROOT_KEY       L"SYSTEM\\CurrentControlSet\\Enum\\Root"
#define SERVICES_KEY        L"SYSTEM\\CurrentControlSet\\Services"

#define SETTLE_MS           10000   /* a request that should end ends within this */

typedef struct _FAKE
{
    BOOL Deserialized;
    PCWSTR Name;        /* device and control device name */
    PCWSTR Service;
    PCWSTR HardwareId;  /* ROOT\FAKEMPS */
    PCWSTR Instance;    /* ROOT\FAKEMPS\0000 */
    PCWSTR EnumName;    /* FAKEMPS */
    PCWSTR Guid;
    PCWSTR ClassIndex;
    WCHAR Adapter[64];  /* \Device\{guid}, the name ndisuio binds */
    HANDLE Control;
    HANDLE Binding;     /* The one ndisuio binding: a read-write open is exclusive, so the requests share it */
    BOOL Installed;
} FAKE, *PFAKE;

static FAKE Fakes[2] =
{
    { FALSE, L"FakeMpS", L"fakemps", L"ROOT\\FAKEMPS", L"ROOT\\FAKEMPS\\0000", L"FAKEMPS",
      L"{7A1D0F53-6D50-4C2A-9E70-46414B454D53}", L"0950" },
    { TRUE,  L"FakeMpD", L"fakempd", L"ROOT\\FAKEMPD", L"ROOT\\FAKEMPD\\0000", L"FAKEMPD",
      L"{7A1D0F53-6D50-4C2A-9E70-46414B454D44}", L"0951" },
};

static PCSTR ScenarioFilter;
static BOOL Abandoned;

static
BOOL
Want(PCSTR Name)
{
    return !ScenarioFilter || !strcmp(Name, ScenarioFilter);
}

static
LONG
SetString(HKEY Key, PCWSTR Name, DWORD Type, PCWSTR Value)
{
    return RegSetValueExW(Key, Name, 0, Type, (const BYTE *)Value, (lstrlenW(Value) + 1) * sizeof(WCHAR));
}

static
LONG
SetDword(HKEY Key, PCWSTR Name, DWORD Value)
{
    return RegSetValueExW(Key, Name, 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
}

static
void
DriverPath(PFAKE Fake, PWSTR Path, ULONG Length, BOOL Destination)
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
    lstrcatW(Path, Fake->Service);
    lstrcatW(Path, L".sys");
}

/* The ndisuio binding for the fake adapter, or INVALID_HANDLE_VALUE */
static
HANDLE
OpenBinding(PFAKE Fake)
{
    HANDLE Handle;
    DWORD Returned;

    Handle = CreateFileW(L"\\\\.\\Ndisuio", GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;

    if (!DeviceIoControl(Handle, IOCTL_NDISUIO_OPEN_DEVICE_, Fake->Adapter,
                         lstrlenW(Fake->Adapter) * sizeof(WCHAR), NULL, 0, &Returned, NULL))
    {
        CloseHandle(Handle);
        return INVALID_HANDLE_VALUE;
    }
    return Handle;
}

static
HANDLE
OpenControl(PFAKE Fake)
{
    WCHAR Path[64];

    FormatW(Path, L"\\\\.\\%s", Fake->Name);
    return CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

static
BOOL
Control(PFAKE Fake, ULONG Code, PVOID In, ULONG InLength, PVOID Out, ULONG OutLength)
{
    DWORD Returned;

    return DeviceIoControl(Fake->Control, Code, In, InLength, Out, OutLength, &Returned, NULL);
}

static
FAKEMP_STATS
Stats(PFAKE Fake)
{
    FAKEMP_STATS St;

    ZeroMemory(&St, sizeof(St));
    ok(Control(Fake, FAKEMP_IOCTL_GET_STATS, NULL, 0, &St, sizeof(St)),
       "%ls: GET_STATS failed: %lu\n", Fake->Name, GetLastError());
    return St;
}

static
void
Configure(PFAKE Fake, ULONG QueryMode, ULONG SetMode, ULONG DelayMs, ULONG ResetMode, ULONG AddressingReset)
{
    FAKEMP_CONFIG Cfg;

    Cfg.QueryMode = QueryMode;
    Cfg.SetMode = SetMode;
    Cfg.DelayMs = DelayMs;
    Cfg.ResetMode = ResetMode;
    Cfg.AddressingReset = AddressingReset;
    ok(Control(Fake, FAKEMP_IOCTL_SET_CONFIG, &Cfg, sizeof(Cfg), NULL, 0),
       "%ls: SET_CONFIG failed: %lu\n", Fake->Name, GetLastError());
}

static
BOOL
WaitStat(PFAKE Fake, SIZE_T Offset, ULONG Value, ULONG Milliseconds)
{
    DWORD Start = GetTickCount();
    FAKEMP_STATS St;

    do
    {
        ZeroMemory(&St, sizeof(St));
        Control(Fake, FAKEMP_IOCTL_GET_STATS, NULL, 0, &St, sizeof(St));
        if (*(PULONG)((PUCHAR)&St + Offset) >= Value)
            return TRUE;
        Sleep(20);
    } while (GetTickCount() - Start < Milliseconds);
    return FALSE;
}

#define WAIT_STAT(Fake, Field, Value, Ms) \
    WaitStat(Fake, FIELD_OFFSET(FAKEMP_STATS, Field), Value, Ms)

/* A request from the protocol side: one thread, one binding of its own */
typedef struct _REQ
{
    HANDLE Binding;
    HANDLE Thread;
    BOOL Set;
    NTSTATUS Status;
    ULONG Out;
    ULONG Count;
    DWORD_PTR Affinity;     /* 0: any */
    volatile LONG Done;
} REQ, *PREQ;

static
NTSTATUS
DoRequest(HANDLE Binding, BOOL Set, ULONG *Value)
{
    struct { ULONG Oid; ULONG Data; } Buffer;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;
    HANDLE Event = CreateEventW(NULL, TRUE, FALSE, NULL);

    Buffer.Oid = Set ? FAKEMP_OID_TEST_SET : FAKEMP_OID_TEST_QUERY;
    Buffer.Data = 0x12345678;
    Status = NtDeviceIoControlFile(Binding, Event, NULL, NULL, &IoStatus,
                                   Set ? IOCTL_NDISUIO_SET_OID_ : IOCTL_NDISUIO_QUERY_OID_,
                                   &Buffer, sizeof(Buffer), &Buffer, sizeof(Buffer));
    if (Status == STATUS_PENDING)
    {
        NtWaitForSingleObject(Event, FALSE, NULL);
        Status = IoStatus.Status;
    }
    CloseHandle(Event);
    if (Value)
        *Value = Buffer.Data;
    return Status;
}

static
DWORD
WINAPI
RequestThread(PVOID Context)
{
    PREQ Req = Context;
    ULONG i;

    if (Req->Affinity)
        SetThreadAffinityMask(GetCurrentThread(), Req->Affinity);
    for (i = 0; i < Req->Count; i++)
    {
        Req->Status = DoRequest(Req->Binding, Req->Set, &Req->Out);
        if (!NT_SUCCESS(Req->Status))
            break;
    }
    InterlockedExchange(&Req->Done, 1);
    return 0;
}

static
void
DropBinding(PFAKE Fake)
{
    if (Fake->Binding && Fake->Binding != INVALID_HANDLE_VALUE)
        CloseHandle(Fake->Binding);
    Fake->Binding = NULL;
}

static
BOOL
StartRequestOn(PFAKE Fake, PREQ Req, BOOL Set, ULONG Count, DWORD_PTR Affinity)
{
    ZeroMemory(Req, sizeof(*Req));
    Req->Set = Set;
    Req->Count = Count;
    Req->Affinity = Affinity;
    if (!Fake->Binding)
    {
        Fake->Binding = OpenBinding(Fake);
        ok(Fake->Binding != INVALID_HANDLE_VALUE, "%ls: no ndisuio binding: %lu\n", Fake->Name, GetLastError());
    }
    Req->Binding = Fake->Binding;
    if (Req->Binding == INVALID_HANDLE_VALUE)
    {
        Fake->Binding = NULL;
        return FALSE;
    }
    Req->Thread = CreateThread(NULL, 0, RequestThread, Req, 0, NULL);
    ok(Req->Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    return Req->Thread != NULL;
}

static
BOOL
StartRequest(PFAKE Fake, PREQ Req, BOOL Set, ULONG Count)
{
    return StartRequestOn(Fake, Req, Set, Count, 0);
}

static
void
Unstick(PFAKE Fake)
{
    Control(Fake, FAKEMP_IOCTL_RELEASE_SPIN, NULL, 0, NULL, 0);
    Control(Fake, FAKEMP_IOCTL_FINISH_RESET, NULL, 0, NULL, 0);
    Control(Fake, FAKEMP_IOCTL_COMPLETE_HELD, NULL, 0, NULL, 0);
}

/* Waits for the request to end. A hung one is a failure; it is pushed to the end, and if that fails
   the scenarios stop (a hung thread cannot be cleaned up after) */
static
BOOL
Join(PFAKE Fake, PREQ Req, PCSTR What)
{
    BOOL Ended = TRUE;

    if (!Req->Thread)
        return FALSE;
    if (WaitForSingleObject(Req->Thread, SETTLE_MS) != WAIT_OBJECT_0)
    {
        ok(0, "%ls: %s did not end\n", Fake->Name, What);
        Ended = FALSE;
        Unstick(Fake);
        if (WaitForSingleObject(Req->Thread, SETTLE_MS) != WAIT_OBJECT_0)
        {
            Abandoned = TRUE;
            return FALSE;
        }
    }
    CloseHandle(Req->Thread);
    Req->Thread = NULL;
    return Ended;
}

static
BOOL
DevNodeOf(PFAKE Fake, DEVINST *DevInst)
{
    return CM_Locate_DevNodeW(DevInst, (DEVINSTID_W)Fake->Instance, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
}

static
void
WaitAdapter(PFAKE Fake, PCSTR What)
{
    DWORD Start = GetTickCount();
    HANDLE Binding;

    do
    {
        Binding = OpenBinding(Fake);
        if (Binding != INVALID_HANDLE_VALUE)
        {
            CloseHandle(Binding);
            return;
        }
        Sleep(200);
    } while (GetTickCount() - Start < 30000);
    ok(0, "%ls: no ndisuio binding after %s\n", Fake->Name, What);
}

static
BOOL
InstallFake(PFAKE Fake)
{
    WCHAR Source[MAX_PATH], Destination[MAX_PATH], Path[MAX_PATH];
    HKEY Key, SubKey;
    DEVINST Root, DevInst;
    LONG Error;
    DWORD Start;
    WCHAR HardwareId[64];
    CONFIGRET Cr;

    FormatW(Fake->Adapter, L"\\Device\\%s", Fake->Guid);

    DriverPath(Fake, Source, MAX_PATH, FALSE);
    DriverPath(Fake, Destination, MAX_PATH, TRUE);
    if (!CopyFileW(Source, Destination, FALSE))
    {
        skip("Cannot copy %ls: %lu\n", Source, GetLastError());
        return FALSE;
    }

    /* The service */
    FormatW(Path, SERVICES_KEY L"\\%s", Fake->Service);
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL);
    if (Error != ERROR_SUCCESS)
    {
        skip("Cannot create the service key: %ld\n", Error);
        return FALSE;
    }
    FormatW(Path, L"system32\\drivers\\%s.sys", Fake->Service);
    SetDword(Key, L"Type", 1);
    SetDword(Key, L"Start", 3);
    SetDword(Key, L"ErrorControl", 0);
    SetString(Key, L"ImagePath", REG_EXPAND_SZ, Path);
    SetString(Key, L"Group", REG_SZ, L"NDIS");
    RegCloseKey(Key);

    /* The class key: the driver key of the device, with the linkage NDIS wants */
    FormatW(Path, CLASS_KEY L"\\%s", Fake->ClassIndex);
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL);
    ok(Error == ERROR_SUCCESS, "Class key: %ld\n", Error);
    if (Error != ERROR_SUCCESS)
        return FALSE;
    SetString(Key, L"DriverDesc", REG_SZ, Fake->Name);
    SetString(Key, L"ProviderName", REG_SZ, L"ReactOS Project");
    Error = RegCreateKeyExW(Key, L"Linkage", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &SubKey, NULL);
    if (Error == ERROR_SUCCESS)
    {
        SetString(SubKey, L"Export", REG_SZ, Fake->Adapter);
        RegCloseKey(SubKey);
    }
    RegCloseKey(Key);

    /* The device */
    FormatW(Path, ENUM_ROOT_KEY L"\\%s\\0000", Fake->EnumName);
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL);
    ok(Error == ERROR_SUCCESS, "Enum key: %ld\n", Error);
    if (Error != ERROR_SUCCESS)
        return FALSE;
    ZeroMemory(HardwareId, sizeof(HardwareId));
    lstrcpyW(HardwareId, Fake->HardwareId);
    RegSetValueExW(Key, L"HardwareID", 0, REG_MULTI_SZ, (const BYTE *)HardwareId,
                   (lstrlenW(HardwareId) + 2) * sizeof(WCHAR));
    SetString(Key, L"Service", REG_SZ, Fake->Service);
    SetString(Key, L"Class", REG_SZ, L"Net");
    SetString(Key, L"ClassGUID", REG_SZ, NET_CLASS_GUID);
    FormatW(Path, NET_CLASS_GUID L"\\%s", Fake->ClassIndex);
    SetString(Key, L"Driver", REG_SZ, Path);
    SetString(Key, L"DeviceDesc", REG_SZ, Fake->Name);
    SetDword(Key, L"ConfigFlags", 0);
    RegCloseKey(Key);

    Cr = CM_Locate_DevNodeW(&Root, NULL, CM_LOCATE_DEVNODE_NORMAL);
    ok(Cr == CR_SUCCESS, "CM_Locate_DevNodeW(root): 0x%lx\n", Cr);
    if (Cr != CR_SUCCESS)
        return FALSE;
    Cr = CM_Reenumerate_DevNode(Root, CM_REENUMERATE_SYNCHRONOUS);
    ok(Cr == CR_SUCCESS, "CM_Reenumerate_DevNode: 0x%lx\n", Cr);

    Start = GetTickCount();
    while (!DevNodeOf(Fake, &DevInst) && GetTickCount() - Start < 15000)
        Sleep(200);
    ok(DevNodeOf(Fake, &DevInst), "%ls: no device node\n", Fake->Name);

    /* The driver loads for the device, and so does the control device */
    Start = GetTickCount();
    do
    {
        Fake->Control = OpenControl(Fake);
        if (Fake->Control != INVALID_HANDLE_VALUE)
            break;
        Sleep(200);
    } while (GetTickCount() - Start < 30000);
    ok(Fake->Control != INVALID_HANDLE_VALUE, "%ls: no control device: %lu\n", Fake->Name, GetLastError());
    if (Fake->Control == INVALID_HANDLE_VALUE)
    {
        Fake->Control = NULL;
        return FALSE;
    }
    Fake->Installed = TRUE;

    WaitAdapter(Fake, "install");
    return TRUE;
}

/* Removes the device node, as PnP does for the "remove" in the device manager. The registry stays: a
   reenumeration brings the device back */
static
BOOL
RemoveDevice(PFAKE Fake, BOOL Quiet)
{
    DEVINST DevInst;
    PNP_VETO_TYPE Veto = PNP_VetoTypeUnknown;
    WCHAR VetoName[MAX_PATH] = L"";
    CONFIGRET Cr;

    if (!DevNodeOf(Fake, &DevInst))
        return FALSE;
    Cr = CM_Query_And_Remove_SubTreeW(DevInst, &Veto, VetoName, ARRAYSIZE(VetoName), CM_REMOVE_NO_RESTART);
    ok(Quiet || Cr == CR_SUCCESS, "%ls: CM_Query_And_Remove_SubTreeW: 0x%lx (veto %d %ls)\n", Fake->Name, Cr, Veto, VetoName);
    return Cr == CR_SUCCESS;
}

static
void
RemoveFake(PFAKE Fake)
{
    WCHAR Path[MAX_PATH];
    HKEY Root;

    DropBinding(Fake);
    if (Fake->Control && Fake->Control != INVALID_HANDLE_VALUE)
        CloseHandle(Fake->Control);
    Fake->Control = NULL;

    /* A device that was removed by a test is gone (or a phantom): nothing to report */
    RemoveDevice(Fake, TRUE);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ENUM_ROOT_KEY, 0, DELETE | KEY_ENUMERATE_SUB_KEYS, &Root) == ERROR_SUCCESS)
    {
        SHDeleteKeyW(Root, Fake->EnumName);
        RegCloseKey(Root);
    }
    FormatW(Path, CLASS_KEY L"\\%s", Fake->ClassIndex);
    SHDeleteKeyW(HKEY_LOCAL_MACHINE, Path);
    FormatW(Path, SERVICES_KEY L"\\%s", Fake->Service);
    SHDeleteKeyW(HKEY_LOCAL_MACHINE, Path);
    DriverPath(Fake, Path, MAX_PATH, TRUE);
    DeleteFileW(Path); /* fails while the driver is loaded */
    Fake->Installed = FALSE;
}

/* The miniport answers again, as configured */
static
void
CheckAlive(PFAKE Fake, PCSTR When)
{
    REQ Req;

    Configure(Fake, FAKEMP_REQ_IMMEDIATE, FAKEMP_REQ_IMMEDIATE, 0, FAKEMP_RESET_SYNC, 0);
    if (!StartRequest(Fake, &Req, FALSE, 1))
        return;
    if (Join(Fake, &Req, "the request"))
    {
        ok(Req.Status == STATUS_SUCCESS, "%ls: query %s: 0x%lx\n", Fake->Name, When, Req.Status);
        ok(Req.Out == 0xFA4E0001, "%ls: query %s returned 0x%lx\n", Fake->Name, When, Req.Out);
    }
}

#define NTHREADS    6
#define PERTHREAD   8

/* (a) one request at a time */
static
void
TestSerialized(PFAKE Fake, ULONG QueryMode, ULONG DelayMs)
{
    REQ Reqs[NTHREADS];
    FAKEMP_STATS St;
    ULONG i;
    BOOL Started[NTHREADS];

    trace("%ls: serialized mode %lu delay %lu\n", Fake->Name, QueryMode, DelayMs);

    Configure(Fake, QueryMode, QueryMode, DelayMs, FAKEMP_RESET_SYNC, 0);
    Control(Fake, FAKEMP_IOCTL_RESET_STATS, NULL, 0, NULL, 0);

    for (i = 0; i < NTHREADS; i++)
        Started[i] = StartRequest(Fake, &Reqs[i], i & 1, PERTHREAD);
    for (i = 0; i < NTHREADS; i++)
    {
        if (Started[i] && Join(Fake, &Reqs[i], "a request") && !Abandoned)
            ok(Reqs[i].Status == STATUS_SUCCESS, "%ls: request %lu: 0x%lx\n", Fake->Name, i, Reqs[i].Status);
        if (Abandoned)
            return;
    }

    St = Stats(Fake);
    trace("%ls mode %lu: %lu requests, %lu at once in the handler, %lu outstanding, %lu completions\n",
          Fake->Name, QueryMode, St.Requests, St.MaxInHandler, St.MaxOutstanding, St.Completions);
    ok(St.Requests >= NTHREADS * PERTHREAD, "%ls: %lu requests reached the miniport\n", Fake->Name, St.Requests);
    ok(St.MaxInHandler == 1, "%ls: %lu requests in the handler at once\n", Fake->Name, St.MaxInHandler);
    ok(St.MaxOutstanding == 1, "%ls: %lu requests outstanding at once\n", Fake->Name, St.MaxOutstanding);
    ok(St.Outstanding == 0, "%ls: %lu requests still outstanding\n", Fake->Name, St.Outstanding);
}

/* A reset the test asks for, and waits to start */
static
BOOL
CauseReset(PFAKE Fake, ULONG Count)
{
    Control(Fake, FAKEMP_IOCTL_TRIGGER_HANG, NULL, 0, NULL, 0);
    return WAIT_STAT(Fake, Resets, Count, 8000);
}

/* (b) a request pending at reset is aborted, a late completion is ignored, and the requests that
   waited behind it run after the reset */
static
void
TestResetAbort(PFAKE Fake, ULONG Addressing)
{
    REQ Pended, Queued;
    FAKEMP_STATS St;

    trace("%ls: reset abort, addressing %lu\n", Fake->Name, Addressing);

    Configure(Fake, FAKEMP_REQ_PEND_NEVER, FAKEMP_REQ_PEND_NEVER, 0, FAKEMP_RESET_SYNC, Addressing);
    Control(Fake, FAKEMP_IOCTL_RESET_STATS, NULL, 0, NULL, 0);

    if (!StartRequest(Fake, &Pended, FALSE, 1))
        return;
    ok(WAIT_STAT(Fake, Held, 1, 5000), "%ls: the request did not reach the miniport\n", Fake->Name);

    /* A second request waits behind the pended one; it is not to be pended again */
    ok(StartRequest(Fake, &Queued, TRUE, 1), "no second request\n");
    Sleep(300);
    St = Stats(Fake);
    ok(St.Requests == 1, "%ls: %lu requests in the miniport with one pended\n", Fake->Name, St.Requests);
    Configure(Fake, FAKEMP_REQ_IMMEDIATE, FAKEMP_REQ_IMMEDIATE, 0, FAKEMP_RESET_SYNC, Addressing);

    ok(CauseReset(Fake, 1), "%ls: no reset\n", Fake->Name);
    if (Join(Fake, &Pended, "the pended request"))
        ok(Pended.Status == NDIS_STATUS_REQUEST_ABORTED_,
           "%ls (addressing %lu): the pended request ended with 0x%lx, not aborted\n", Fake->Name, Addressing, Pended.Status);
    if (Abandoned)
        return;
    if (Join(Fake, &Queued, "the queued request"))
        ok(Queued.Status == STATUS_SUCCESS,
           "%ls (addressing %lu): the queued request ended with 0x%lx\n", Fake->Name, Addressing, Queued.Status);
    if (Abandoned)
        return;

    St = Stats(Fake);
    ok(St.Resets == 1, "%ls: %lu resets\n", Fake->Name, St.Resets);

    /* The miniport completes the request, late: nothing is left for NDIS to complete */
    ok(Control(Fake, FAKEMP_IOCTL_COMPLETE_HELD, NULL, 0, NULL, 0),
       "%ls: the miniport had forgotten the pended request\n", Fake->Name);
    Sleep(200);
    CheckAlive(Fake, "after a late completion");

    St = Stats(Fake);
    ok(St.CallsAfterHalt == 0, "%ls: %lu calls after halt\n", Fake->Name, St.CallsAfterHalt);
}

/* (b) continued: the reset itself pends. A completion during the reset is the miniport's own, the
   abort comes only when the reset ends */
static
void
TestLateCompletion(PFAKE Fake)
{
    REQ Req;
    FAKEMP_STATS St;

    trace("%ls: late completion\n", Fake->Name);

    /* Completes while the reset is going on */
    Configure(Fake, FAKEMP_REQ_PEND_NEVER, FAKEMP_REQ_PEND_NEVER, 0, FAKEMP_RESET_PEND, 0);
    Control(Fake, FAKEMP_IOCTL_RESET_STATS, NULL, 0, NULL, 0);
    if (!StartRequest(Fake, &Req, FALSE, 1))
        return;
    ok(WAIT_STAT(Fake, Held, 1, 5000), "%ls: the request did not reach the miniport\n", Fake->Name);
    ok(CauseReset(Fake, 1), "%ls: no reset\n", Fake->Name);
    ok(WAIT_STAT(Fake, ResetPending, 1, 2000), "%ls: the reset is not pending\n", Fake->Name);
    Sleep(300);
    ok(WaitForSingleObject(Req.Thread, 0) == WAIT_TIMEOUT, "%ls: the request ended before the reset did\n", Fake->Name);
    ok(Control(Fake, FAKEMP_IOCTL_COMPLETE_HELD, NULL, 0, NULL, 0), "%ls: nothing to complete\n", Fake->Name);
    if (Join(Fake, &Req, "the request completed during the reset"))
        ok(Req.Status == STATUS_SUCCESS, "%ls: completed during the reset: 0x%lx\n", Fake->Name, Req.Status);
    if (Abandoned)
        return;
    ok(Control(Fake, FAKEMP_IOCTL_FINISH_RESET, NULL, 0, NULL, 0), "%ls: no reset to finish\n", Fake->Name);
    Sleep(300);
    CheckAlive(Fake, "after a reset that pended");

    /* Aborted when the reset ends; the miniport's completion after that is ignored */
    Configure(Fake, FAKEMP_REQ_PEND_NEVER, FAKEMP_REQ_PEND_NEVER, 0, FAKEMP_RESET_PEND, 0);
    Control(Fake, FAKEMP_IOCTL_RESET_STATS, NULL, 0, NULL, 0);
    if (!StartRequest(Fake, &Req, TRUE, 1))
        return;
    ok(WAIT_STAT(Fake, Held, 1, 5000), "%ls: the request did not reach the miniport\n", Fake->Name);
    ok(CauseReset(Fake, 1), "%ls: no reset\n", Fake->Name);
    ok(WAIT_STAT(Fake, ResetPending, 1, 2000), "%ls: the reset is not pending\n", Fake->Name);
    ok(Control(Fake, FAKEMP_IOCTL_FINISH_RESET, NULL, 0, NULL, 0), "%ls: no reset to finish\n", Fake->Name);
    if (Join(Fake, &Req, "the request pended across the reset"))
        ok(Req.Status == NDIS_STATUS_REQUEST_ABORTED_, "%ls: aborted at reset end: 0x%lx\n", Fake->Name, Req.Status);
    if (Abandoned)
        return;
    ok(Control(Fake, FAKEMP_IOCTL_COMPLETE_HELD, NULL, 0, NULL, 0), "%ls: the miniport forgot the request\n", Fake->Name);
    Sleep(200);
    CheckAlive(Fake, "after a completion that came late");
    St = Stats(Fake);
    ok(St.CallsAfterHalt == 0, "%ls: %lu calls after halt\n", Fake->Name, St.CallsAfterHalt);
}

/* (e) a reset while the request is still in the miniport's handler: the abort waits for the handler to return */
static
void
TestDeferredAbort(PFAKE Fake, ULONG Addressing)
{
    DWORD_PTR ProcessMask, SystemMask, HangMask, SpinMask, TestMask;
    REQ Req;
    FAKEMP_STATS St;

    trace("%ls: deferred abort, addressing %lu\n", Fake->Name, Addressing);

    GetProcessAffinityMask(GetCurrentProcess(), &ProcessMask, &SystemMask);
    if ((ProcessMask & (ProcessMask - 1)) == 0)
    {
        skip("The handler and the reset need two processors\n");
        return;
    }

    /* A processor that runs the handler at DISPATCH_LEVEL runs nothing else meanwhile, and the reset is a
       DPC of NDIS's hang timer, on a processor of its own: the handler goes on another one, and the test
       stays on the DPC's (on the handler's, it would wait for the handler to end) */
    ok(WAIT_STAT(Fake, HangChecks, 1, 5000), "%ls: no hang check yet\n", Fake->Name);
    St = Stats(Fake);
    HangMask = (DWORD_PTR)1 << St.HangCpu;
    SpinMask = ProcessMask & ~HangMask;
    if (!(ProcessMask & HangMask) || !SpinMask)
    {
        skip("The hang check runs on processor %lu, outside the test's\n", St.HangCpu);
        return;
    }
    SpinMask &= ~SpinMask + 1; /* the lowest one */
    TestMask = SetThreadAffinityMask(GetCurrentThread(), HangMask);

    /* The handler stays half a second after the reset has started: the abort finds it inside */
    Configure(Fake, FAKEMP_REQ_SPIN, FAKEMP_REQ_SPIN, 500, FAKEMP_RESET_SYNC, Addressing);
    Control(Fake, FAKEMP_IOCTL_RESET_STATS, NULL, 0, NULL, 0);
    if (!StartRequestOn(Fake, &Req, FALSE, 1, SpinMask))
    {
        SetThreadAffinityMask(GetCurrentThread(), TestMask);
        return;
    }
    if (!WAIT_STAT(Fake, Spinning, 1, 5000))
    {
        St = Stats(Fake);
        ok(0, "%ls: the request is not in the handler (requests %lu, outstanding %lu, held %lu, resets %lu)\n",
           Fake->Name, St.Requests, St.Outstanding, St.Held, St.Resets);
    }
    ok(CauseReset(Fake, 1), "%ls: no reset\n", Fake->Name);
    St = Stats(Fake);
    ok(St.Spinning == 1 && WaitForSingleObject(Req.Thread, 0) == WAIT_TIMEOUT,
       "%ls: the reset did not find the request in the handler\n", Fake->Name);
    if (Join(Fake, &Req, "the request that was in the handler"))
        ok(Req.Status == NDIS_STATUS_REQUEST_ABORTED_,
           "%ls (addressing %lu): the request in the handler at reset ended with 0x%lx\n", Fake->Name, Addressing, Req.Status);
    SetThreadAffinityMask(GetCurrentThread(), TestMask);
    if (Abandoned)
        return;
    St = Stats(Fake);
    ok(St.Resets == 1, "%ls: %lu resets\n", Fake->Name, St.Resets);
    ok(St.Held == 1, "%ls: the miniport does not hold the request\n", Fake->Name);
    ok(Control(Fake, FAKEMP_IOCTL_COMPLETE_HELD, NULL, 0, NULL, 0), "%ls: the miniport forgot the request\n", Fake->Name);
    Sleep(200);
    CheckAlive(Fake, "after the deferred abort");
}

/* (c) (d) requests pended and queued when the device is removed (ReactOS stops a disabled device the same way):
   the pended request fails, the queued ones fail and never reach the miniport, and nothing calls the halted miniport.
   NDIS does not unbind the protocols from a removed adapter (ndisuio keeps its binding to the freed adapter), so
   this goes last, and nothing may use the adapter's binding afterwards. */
static
void
TestRemove(PFAKE Fake)
{
    REQ Reqs[3];
    BOOL Started[3];
    FAKEMP_STATS St;
    ULONG i, Halts;
    BOOL Removed;
    const char *Names[3] = { "the pended request", "the first queued request", "the second queued request" };

    trace("%ls: remove\n", Fake->Name);

    Configure(Fake, FAKEMP_REQ_PEND_NEVER, FAKEMP_REQ_PEND_NEVER, 0, FAKEMP_RESET_SYNC, 0);
    Control(Fake, FAKEMP_IOCTL_RESET_STATS, NULL, 0, NULL, 0);
    Halts = Stats(Fake).Halts;

    Started[0] = StartRequest(Fake, &Reqs[0], FALSE, 1);
    ok(WAIT_STAT(Fake, Held, 1, 5000), "%ls: the request did not reach the miniport\n", Fake->Name);
    Started[1] = StartRequest(Fake, &Reqs[1], TRUE, 1);
    Started[2] = StartRequest(Fake, &Reqs[2], FALSE, 1);
    Sleep(300);
    St = Stats(Fake);
    ok(St.Requests == 1, "%ls: %lu requests in the miniport with one pended\n", Fake->Name, St.Requests);

    Removed = RemoveDevice(Fake, FALSE);
    if (!Removed)
        Unstick(Fake);

    for (i = 0; i < 3; i++)
    {
        if (!Started[i])
            continue;
        if (Join(Fake, &Reqs[i], Names[i]))
            ok(!NT_SUCCESS(Reqs[i].Status) && Reqs[i].Status != STATUS_PENDING,
               "%ls: %s ended with 0x%lx\n", Fake->Name, Names[i], Reqs[i].Status);
        else if (Abandoned)
            return;
        trace("%ls: %s: 0x%lx\n", Fake->Name, Names[i], Reqs[i].Status);
    }

    St = Stats(Fake);
    ok(St.Halts == Halts + 1, "%ls: %lu halts, %lu before\n", Fake->Name, St.Halts, Halts);
    ok(St.Halted == 1, "%ls: not halted\n", Fake->Name);
    ok(St.CallsAfterHalt == 0, "%ls: %lu calls into the halted miniport\n", Fake->Name, St.CallsAfterHalt);
    ok(St.Requests == 1, "%ls: %lu requests reached the miniport, the queued ones must not\n", Fake->Name, St.Requests);
}

START_TEST(FakeMiniport)
{
    char **Argv;
    int Argc;
    ULONG i;
    PFAKE Fake;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc > 2)
        ScenarioFilter = Argv[2];

    if (!is_reactos())
    {
        skip("The fake miniport is a ReactOS driver\n");
        return;
    }

    /* The adapters come and go with the test: both are installed first, so that nothing starts
       an adapter after another one was removed */
    for (i = 0; i < 2; i++)
    {
        Fake = &Fakes[i];
        if (!InstallFake(Fake))
        {
            RemoveFake(Fake);
            continue;
        }

        if (Want("serial"))
        {
            TestSerialized(Fake, FAKEMP_REQ_IMMEDIATE, 0);
            if (!Abandoned)
                TestSerialized(Fake, FAKEMP_REQ_PEND_DELAY, 1);
            if (!Abandoned)
                TestSerialized(Fake, FAKEMP_REQ_PEND_DELAY, 15);
            DropBinding(Fake);
        }
        if (!Abandoned && Want("abort"))
            TestResetAbort(Fake, 0);
        if (!Abandoned && Want("late"))
            TestLateCompletion(Fake);
        if (!Abandoned && Want("deferred"))
            TestDeferredAbort(Fake, 0);
        if (!Abandoned && Want("abort1"))
            TestResetAbort(Fake, 1);
        if (!Abandoned && Want("deferred1"))
            TestDeferredAbort(Fake, 1);
        DropBinding(Fake);
        if (Abandoned)
            break;
    }

    for (i = 0; i < 2 && !Abandoned; i++)
    {
        Fake = &Fakes[i];
        if (!Fake->Installed)
            continue;
        if (Want("remove"))
            TestRemove(Fake);
        DropBinding(Fake);
    }

    if (Abandoned)
    {
        skip("A request hung; the fake miniports stay installed\n");
        return;
    }
    for (i = 0; i < 2; i++)
    {
        if (Fakes[i].Installed)
            RemoveFake(&Fakes[i]);
    }
}
