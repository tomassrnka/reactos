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
 * Frames are sent through ndisuio too (writes on the binding), which the miniport holds and completes
 * out of order, or only in its halt; and received (reads), with data the miniport transfers at once or later.
 *
 * Optional argument after the test name: a scenario name
 * (serial, abort, abort1, late, deferred, deferred1, sends, receive, sendhalt, remove, readd).
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
    FAKEMP_CONFIG Cfg;  /* As last configured */
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
    Fake->Cfg.QueryMode = QueryMode;
    Fake->Cfg.SetMode = SetMode;
    Fake->Cfg.DelayMs = DelayMs;
    Fake->Cfg.ResetMode = ResetMode;
    Fake->Cfg.AddressingReset = AddressingReset;
    ok(Control(Fake, FAKEMP_IOCTL_SET_CONFIG, &Fake->Cfg, sizeof(Fake->Cfg), NULL, 0),
       "%ls: SET_CONFIG failed: %lu\n", Fake->Name, GetLastError());
}

/* How the miniport handles the test frames; the request configuration stays */
static
void
ConfigureSends(PFAKE Fake, ULONG SendMode, ULONG SendDelayMs)
{
    Fake->Cfg.SendMode = SendMode;
    Fake->Cfg.SendDelayMs = SendDelayMs;
    ok(Control(Fake, FAKEMP_IOCTL_SET_CONFIG, &Fake->Cfg, sizeof(Fake->Cfg), NULL, 0),
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
BOOL
CompleteSends(PFAKE Fake, ULONG Which)
{
    return Control(Fake, FAKEMP_IOCTL_COMPLETE_SEND, &Which, sizeof(Which), NULL, 0);
}

static
void
Unstick(PFAKE Fake)
{
    Control(Fake, FAKEMP_IOCTL_RELEASE_SPIN, NULL, 0, NULL, 0);
    Control(Fake, FAKEMP_IOCTL_FINISH_RESET, NULL, 0, NULL, 0);
    Control(Fake, FAKEMP_IOCTL_COMPLETE_HELD, NULL, 0, NULL, 0);
    CompleteSends(Fake, FAKEMP_COMPLETE_ALL);
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

/* Writes from the protocol side: one thread, Count frames written one after the other, each written once
   its send is done */
typedef struct _SEND
{
    HANDLE Binding;
    HANDLE Thread;
    UCHAR Seed;
    ULONG Count;
    ULONG Sent;             /* Writes that succeeded */
    NTSTATUS Status;        /* Of the last write */
    ULONG_PTR Written;      /* ... and its byte count */
} SEND, *PSEND;

#define FRAME_SIZE  96

static
NTSTATUS
DoWrite(HANDLE Binding, UCHAR Seed, ULONG_PTR *Written)
{
    /* To a unicast address that is not the adapter's (NDIS loops that back), from the adapter's */
    static const UCHAR Destination[6] = { 0x02, 0x00, 0x5E, 0x10, 0x00, 0x01 };
    static const UCHAR Source[6] = { 0x02, 0xFA, 0xCE, 0x4D, 0x50, 0x00 };
    UCHAR Frame[FRAME_SIZE];
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    NTSTATUS Status;
    HANDLE Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ULONG i;

    memcpy(Frame, Destination, 6);
    memcpy(Frame + 6, Source, 6);
    Frame[12] = FAKEMP_TEST_ETHERTYPE >> 8;
    Frame[13] = FAKEMP_TEST_ETHERTYPE & 0xFF;
    for (i = 14; i < FRAME_SIZE; i++)
        Frame[i] = (UCHAR)(Seed + i);

    /* The handle is not synchronous: an offset is required, the driver ignores it */
    IoStatus.Information = 0;
    Offset.QuadPart = 0;
    Status = NtWriteFile(Binding, Event, NULL, NULL, &IoStatus, Frame, sizeof(Frame), &Offset, NULL);
    if (Status == STATUS_PENDING)
    {
        NtWaitForSingleObject(Event, FALSE, NULL);
        Status = IoStatus.Status;
    }
    CloseHandle(Event);
    *Written = NT_SUCCESS(Status) ? IoStatus.Information : 0;
    return Status;
}

static
DWORD
WINAPI
SendThread(PVOID Context)
{
    PSEND Send = Context;
    ULONG i;

    for (i = 0; i < Send->Count; i++)
    {
        Send->Status = DoWrite(Send->Binding, (UCHAR)(Send->Seed + i), &Send->Written);
        if (!NT_SUCCESS(Send->Status))
            break;
        if (Send->Written == FRAME_SIZE)
            Send->Sent++;
    }
    return 0;
}

static
BOOL
StartSend(PFAKE Fake, PSEND Send, UCHAR Seed, ULONG Count)
{
    ZeroMemory(Send, sizeof(*Send));
    Send->Seed = Seed;
    Send->Count = Count;
    Send->Status = STATUS_PENDING;
    if (!Fake->Binding)
    {
        Fake->Binding = OpenBinding(Fake);
        ok(Fake->Binding != INVALID_HANDLE_VALUE, "%ls: no ndisuio binding: %lu\n", Fake->Name, GetLastError());
    }
    Send->Binding = Fake->Binding;
    if (Send->Binding == INVALID_HANDLE_VALUE)
    {
        Fake->Binding = NULL;
        return FALSE;
    }
    Send->Thread = CreateThread(NULL, 0, SendThread, Send, 0, NULL);
    ok(Send->Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    return Send->Thread != NULL;
}

/* As Join, for writes */
static
BOOL
JoinSend(PFAKE Fake, PSEND Send, PCSTR What)
{
    BOOL Ended = TRUE;

    if (!Send->Thread)
        return FALSE;
    if (WaitForSingleObject(Send->Thread, SETTLE_MS) != WAIT_OBJECT_0)
    {
        ok(0, "%ls: %s did not end\n", Fake->Name, What);
        Ended = FALSE;
        Unstick(Fake);
        if (WaitForSingleObject(Send->Thread, SETTLE_MS) != WAIT_OBJECT_0)
        {
            Abandoned = TRUE;
            return FALSE;
        }
    }
    CloseHandle(Send->Thread);
    Send->Thread = NULL;
    return Ended;
}

/* A read on the binding, in a thread of its own (it waits for a frame) */
typedef struct _RECV
{
    HANDLE Binding;
    HANDLE Thread;
    NTSTATUS Status;
    ULONG_PTR Read;
    UCHAR Frame[256];
} RECV, *PRECV;

static
DWORD
WINAPI
RecvThread(PVOID Context)
{
    PRECV Recv = Context;
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    HANDLE Event = CreateEventW(NULL, TRUE, FALSE, NULL);

    IoStatus.Information = 0;
    Offset.QuadPart = 0;
    Recv->Status = NtReadFile(Recv->Binding, Event, NULL, NULL, &IoStatus, Recv->Frame, sizeof(Recv->Frame), &Offset, NULL);
    if (Recv->Status == STATUS_PENDING)
    {
        NtWaitForSingleObject(Event, FALSE, NULL);
        Recv->Status = IoStatus.Status;
    }
    Recv->Read = NT_SUCCESS(Recv->Status) ? IoStatus.Information : 0;
    CloseHandle(Event);
    return 0;
}

static
BOOL
StartRecv(PFAKE Fake, PRECV Recv)
{
    ZeroMemory(Recv, sizeof(*Recv));
    Recv->Status = STATUS_PENDING;
    if (!Fake->Binding)
    {
        Fake->Binding = OpenBinding(Fake);
        ok(Fake->Binding != INVALID_HANDLE_VALUE, "%ls: no ndisuio binding: %lu\n", Fake->Name, GetLastError());
    }
    Recv->Binding = Fake->Binding;
    if (Recv->Binding == INVALID_HANDLE_VALUE)
    {
        Fake->Binding = NULL;
        return FALSE;
    }
    Recv->Thread = CreateThread(NULL, 0, RecvThread, Recv, 0, NULL);
    ok(Recv->Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    return Recv->Thread != NULL;
}

static
VOID
CALLBACK
WakeApc(ULONG_PTR Parameter)
{
}

/* A read waits for a frame until one comes (alertably): one that does not end is woken by an APC */
static
BOOL
JoinRecv(PFAKE Fake, PRECV Recv, PCSTR What)
{
    BOOL Ended = TRUE;

    if (!Recv->Thread)
        return FALSE;
    if (WaitForSingleObject(Recv->Thread, SETTLE_MS) != WAIT_OBJECT_0)
    {
        ok(0, "%ls: %s did not end\n", Fake->Name, What);
        Ended = FALSE;
        Control(Fake, FAKEMP_IOCTL_COMPLETE_TRANSFERS, NULL, 0, NULL, 0);
        QueueUserAPC(WakeApc, Recv->Thread, 0);
        if (WaitForSingleObject(Recv->Thread, SETTLE_MS) != WAIT_OBJECT_0)
        {
            Abandoned = TRUE;
            return FALSE;
        }
    }
    CloseHandle(Recv->Thread);
    Recv->Thread = NULL;
    return Ended;
}

/* The frame the miniport indicated with this seed, header included */
static
BOOL
RecvIsFrame(PRECV Recv, ULONG Seed)
{
    ULONG i;

    if (Recv->Read != FAKEMP_RX_FRAME_SIZE ||
        Recv->Frame[12] != (FAKEMP_TEST_ETHERTYPE >> 8) || Recv->Frame[13] != (FAKEMP_TEST_ETHERTYPE & 0xFF))
        return FALSE;
    for (i = 14; i < FAKEMP_RX_FRAME_SIZE; i++)
    {
        if (Recv->Frame[i] != (UCHAR)(Seed + i))
            return FALSE;
    }
    return TRUE;
}

static
BOOL
IndicateFrame(PFAKE Fake, ULONG Seed, ULONG Lookahead, ULONG TransferMode)
{
    FAKEMP_INDICATE Ind;

    Ind.Seed = Seed;
    Ind.Lookahead = Lookahead;
    Ind.TransferMode = TransferMode;
    return Control(Fake, FAKEMP_IOCTL_INDICATE, &Ind, sizeof(Ind), NULL, 0);
}

static
BOOL
WriteRunning(HANDLE Thread)
{
    return Thread && WaitForSingleObject(Thread, 0) == WAIT_TIMEOUT;
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

#define SEND_THREADS    4
#define SEND_PERTHREAD  16

/* (g) writes pending in the miniport at once: each write ends when its own send completes, not another
   one's (before, a write woke on any completion and freed the frame the miniport still held). Then many
   writes at once, which the miniport completes newest first. */
static
void
TestSends(PFAKE Fake)
{
    SEND First, Second, Many[SEND_THREADS];
    BOOL Started[SEND_THREADS];
    FAKEMP_STATS St;
    ULONG i;

    trace("%ls: sends\n", Fake->Name);

    ConfigureSends(Fake, FAKEMP_SEND_HOLD, 0);
    Control(Fake, FAKEMP_IOCTL_RESET_STATS, NULL, 0, NULL, 0);

    if (!StartSend(Fake, &First, 0x10, 1))
        return;
    ok(WAIT_STAT(Fake, SendsHeld, 1, 5000), "%ls: the first send did not reach the miniport\n", Fake->Name);
    if (StartSend(Fake, &Second, 0x20, 1))
    {
        ok(WAIT_STAT(Fake, SendsHeld, 2, 5000), "%ls: the second send did not reach the miniport\n", Fake->Name);
        Sleep(300);
        ok(WriteRunning(First.Thread), "%ls: the first write ended with its send held (0x%lx)\n", Fake->Name, First.Status);
        ok(WriteRunning(Second.Thread), "%ls: the second write ended with its send held (0x%lx)\n", Fake->Name, Second.Status);

        /* The second send completes: only its write ends */
        ok(CompleteSends(Fake, FAKEMP_COMPLETE_NEWEST), "%ls: no send to complete\n", Fake->Name);
        if (JoinSend(Fake, &Second, "the write whose send completed"))
            ok(Second.Status == STATUS_SUCCESS && Second.Written == FRAME_SIZE,
               "%ls: the second write: 0x%lx, %Iu bytes\n", Fake->Name, Second.Status, Second.Written);
        if (Abandoned)
            return;
        Sleep(300);
        ok(WriteRunning(First.Thread), "%ls: the first write ended with the second send's completion (0x%lx)\n",
           Fake->Name, First.Status);
    }

    ok(CompleteSends(Fake, FAKEMP_COMPLETE_OLDEST), "%ls: the miniport does not hold the first send\n", Fake->Name);
    if (JoinSend(Fake, &First, "the write whose send completed last"))
        ok(First.Status == STATUS_SUCCESS && First.Written == FRAME_SIZE,
           "%ls: the first write: 0x%lx, %Iu bytes\n", Fake->Name, First.Status, First.Written);
    if (Abandoned)
        return;

    St = Stats(Fake);
    ok(St.SendsHeld == 0, "%ls: %lu sends still held\n", Fake->Name, St.SendsHeld);
    ok(St.SendDataBad == 0, "%ls: %lu sends were freed before the miniport completed them\n", Fake->Name, St.SendDataBad);

    /* Many at once, completed out of order */
    ConfigureSends(Fake, FAKEMP_SEND_DELAY, 5);
    Control(Fake, FAKEMP_IOCTL_RESET_STATS, NULL, 0, NULL, 0);
    for (i = 0; i < SEND_THREADS; i++)
        Started[i] = StartSend(Fake, &Many[i], (UCHAR)(0x40 * i), SEND_PERTHREAD);
    for (i = 0; i < SEND_THREADS; i++)
    {
        if (!Started[i])
            continue;
        if (JoinSend(Fake, &Many[i], "a writer"))
            ok(Many[i].Sent == SEND_PERTHREAD && Many[i].Status == STATUS_SUCCESS,
               "%ls: writer %lu: %lu of %u writes, last 0x%lx\n", Fake->Name, i, Many[i].Sent, SEND_PERTHREAD, Many[i].Status);
        if (Abandoned)
            return;
    }
    St = Stats(Fake);
    trace("%ls: %lu sends, %lu completed late, %lu held at once\n", Fake->Name, St.Sends, St.SendsCompleted, St.MaxSendsHeld);
    ok(St.Sends == SEND_THREADS * SEND_PERTHREAD, "%ls: %lu sends reached the miniport\n", Fake->Name, St.Sends);
    ok(St.SendsCompleted == St.Sends, "%ls: %lu of %lu sends completed late\n", Fake->Name, St.SendsCompleted, St.Sends);
    ok(St.SendsHeld == 0, "%ls: %lu sends still held\n", Fake->Name, St.SendsHeld);
    ok(St.SendDataBad == 0, "%ls: %lu sends were freed before the miniport completed them\n", Fake->Name, St.SendDataBad);

    ConfigureSends(Fake, FAKEMP_SEND_IMMEDIATE, 0);
}

/* (i) frames received through ndisuio: all in the lookahead, the rest transferred at once, or transferred
   later (ndisuio does not wait for that in the indication: the frame is read once the transfer completes) */
static
void
TestReceive(PFAKE Fake)
{
    static const struct { ULONG Lookahead; ULONG Mode; PCSTR What; } Cases[] =
    {
        { FAKEMP_RX_FRAME_SIZE, FAKEMP_XFER_SYNC, "a frame all in the lookahead" },
        { 16,                   FAKEMP_XFER_SYNC, "a frame transferred at once" },
        { 16,                   FAKEMP_XFER_HOLD, "a frame transferred later" },
    };
    RECV Recv;
    FAKEMP_STATS St;
    ULONG i, Seed;

    trace("%ls: receive\n", Fake->Name);

    Control(Fake, FAKEMP_IOCTL_RESET_STATS, NULL, 0, NULL, 0);
    for (i = 0; i < ARRAYSIZE(Cases); i++)
    {
        Seed = 0x31 * (i + 1);
        if (!StartRecv(Fake, &Recv))
            return;
        Sleep(100);
        ok(IndicateFrame(Fake, Seed, Cases[i].Lookahead, Cases[i].Mode), "%ls: INDICATE failed: %lu\n", Fake->Name, GetLastError());
        if (Cases[i].Mode == FAKEMP_XFER_HOLD)
        {
            ok(WAIT_STAT(Fake, TransfersHeld, 1, 5000), "%ls: no transfer held\n", Fake->Name);
            Sleep(300);
            ok(WriteRunning(Recv.Thread), "%ls: the read ended before the transfer completed (0x%lx, %Iu bytes)\n",
               Fake->Name, Recv.Status, Recv.Read);
            ok(Control(Fake, FAKEMP_IOCTL_COMPLETE_TRANSFERS, NULL, 0, NULL, 0), "%ls: no transfer to complete\n", Fake->Name);
        }
        if (JoinRecv(Fake, &Recv, Cases[i].What))
            ok(Recv.Status == STATUS_SUCCESS && RecvIsFrame(&Recv, Seed),
               "%ls: %s: 0x%lx, %Iu bytes, not the frame\n", Fake->Name, Cases[i].What, Recv.Status, Recv.Read);
        if (Abandoned)
            return;
    }

    St = Stats(Fake);
    ok(St.Indications == ARRAYSIZE(Cases), "%ls: %lu indications\n", Fake->Name, St.Indications);
    ok(St.Transfers >= 2, "%ls: %lu transfers\n", Fake->Name, St.Transfers);
    ok(St.TransfersHeld == 0, "%ls: %lu transfers still held\n", Fake->Name, St.TransfersHeld);
}

static
BOOL
BringBack(PFAKE Fake);

/* (h) sends the miniport completes only in its halt, when the device is removed: the remove does not wait
   for them forever (NDIS halts the miniport before it unbinds the protocols), each write ends with its own
   send's failure, and the frames are still there when the halt completes them. The device comes back. */
static
void
TestSendHalt(PFAKE Fake)
{
    SEND Sends[2];
    BOOL Started[2];
    FAKEMP_STATS St;
    ULONG i, Halts;
    BOOL Removed;
    DWORD Start;

    trace("%ls: sends completed by the halt\n", Fake->Name);

    Configure(Fake, FAKEMP_REQ_IMMEDIATE, FAKEMP_REQ_IMMEDIATE, 0, FAKEMP_RESET_SYNC, 0);
    ConfigureSends(Fake, FAKEMP_SEND_HOLD, 0);
    Control(Fake, FAKEMP_IOCTL_RESET_STATS, NULL, 0, NULL, 0);
    Halts = Stats(Fake).Halts;

    for (i = 0; i < 2; i++)
    {
        Started[i] = StartSend(Fake, &Sends[i], (UCHAR)(0x80 + 0x20 * i), 1);
        ok(WAIT_STAT(Fake, SendsHeld, i + 1, 5000), "%ls: send %lu did not reach the miniport\n", Fake->Name, i);
    }
    /* A received frame whose transfer the halt fails too: ndisuio drops it, and lets go of the adapter */
    ok(IndicateFrame(Fake, 0x77, 16, FAKEMP_XFER_HOLD), "%ls: INDICATE failed: %lu\n", Fake->Name, GetLastError());
    ok(WAIT_STAT(Fake, TransfersHeld, 1, 5000), "%ls: no transfer held\n", Fake->Name);
    Sleep(300);
    ok(WriteRunning(Sends[0].Thread) && WriteRunning(Sends[1].Thread), "%ls: a write ended with its send held\n", Fake->Name);

    Start = GetTickCount();
    Removed = RemoveDevice(Fake, FALSE);
    trace("%ls: removed in %lu ms\n", Fake->Name, GetTickCount() - Start);
    if (!Removed)
        Unstick(Fake);

    for (i = 0; i < 2; i++)
    {
        if (!Started[i])
            continue;
        if (JoinSend(Fake, &Sends[i], "a write whose send the halt completed"))
            ok(!NT_SUCCESS(Sends[i].Status) && Sends[i].Status != STATUS_PENDING && Sends[i].Written == 0,
               "%ls: write %lu ended with 0x%lx, %Iu bytes\n", Fake->Name, i, Sends[i].Status, Sends[i].Written);
        else if (Abandoned)
            return;
    }

    St = Stats(Fake);
    ok(St.Halts == Halts + 1, "%ls: %lu halts, %lu before\n", Fake->Name, St.Halts, Halts);
    ok(St.SendsCompletedAtHalt == 2, "%ls: the halt completed %lu sends\n", Fake->Name, St.SendsCompletedAtHalt);
    ok(St.SendsHeld == 0, "%ls: %lu sends still held\n", Fake->Name, St.SendsHeld);
    ok(St.SendDataBad == 0, "%ls: %lu sends were freed before the halt completed them\n", Fake->Name, St.SendDataBad);
    ok(St.TransfersCompletedAtHalt == 1, "%ls: the halt completed %lu transfers\n", Fake->Name, St.TransfersCompletedAtHalt);
    ok(St.CallsAfterHalt == 0, "%ls: %lu calls into the halted miniport\n", Fake->Name, St.CallsAfterHalt);

    ConfigureSends(Fake, FAKEMP_SEND_IMMEDIATE, 0);
    DropBinding(Fake);
    if (Removed)
        BringBack(Fake);
}

/* (c) (d) requests pended and queued when the device is removed (ReactOS stops a disabled device the same way):
   the pended request fails, the queued ones fail and never reach the miniport, and nothing calls the halted miniport.
   NDIS unbinds the protocols first: ndisuio lets go of the adapter, and its handle bound to it is dead. */
static
BOOL
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
            return FALSE;
        trace("%ls: %s: 0x%lx\n", Fake->Name, Names[i], Reqs[i].Status);
    }

    St = Stats(Fake);
    ok(St.Halts == Halts + 1, "%ls: %lu halts, %lu before\n", Fake->Name, St.Halts, Halts);
    ok(St.Halted == 1, "%ls: not halted\n", Fake->Name);
    ok(St.CallsAfterHalt == 0, "%ls: %lu calls into the halted miniport\n", Fake->Name, St.CallsAfterHalt);
    ok(St.Requests == 1, "%ls: %lu requests reached the miniport, the queued ones must not\n", Fake->Name, St.Requests);

    if (Removed && Fake->Binding)
    {
        REQ Req;
        HANDLE Binding;

        /* ndisuio was unbound: the handle it had bound to the adapter takes no request (in a thread of its
           own: before, the request went to the freed adapter), and the adapter is not there to bind to */
        if (StartRequest(Fake, &Req, FALSE, 1))
        {
            if (Join(Fake, &Req, "the request on the binding of the removed adapter"))
                ok(!NT_SUCCESS(Req.Status) && Req.Status != STATUS_PENDING,
                   "%ls: a request on the binding of the removed adapter: 0x%lx\n", Fake->Name, Req.Status);
            else if (Abandoned)
                return FALSE;
        }
        DropBinding(Fake);
        Binding = OpenBinding(Fake);
        ok(Binding == INVALID_HANDLE_VALUE, "%ls: ndisuio still binds the removed adapter\n", Fake->Name);
        if (Binding != INVALID_HANDLE_VALUE)
            CloseHandle(Binding);
    }
    return Removed;
}

/* The removed device comes back by a reenumeration of the root, without a reboot: the same registry is still
   there, so a new adapter of the driver starts. Returns whether it came up. */
static
BOOL
BringBack(PFAKE Fake)
{
    DEVINST Root;
    CONFIGRET Cr;
    ULONG Initializes;

    DropBinding(Fake);
    Initializes = Stats(Fake).Initializes;

    Cr = CM_Locate_DevNodeW(&Root, NULL, CM_LOCATE_DEVNODE_NORMAL);
    ok(Cr == CR_SUCCESS, "CM_Locate_DevNodeW(root): 0x%lx\n", Cr);
    if (Cr != CR_SUCCESS)
        return FALSE;
    Cr = CM_Reenumerate_DevNode(Root, CM_REENUMERATE_SYNCHRONOUS);
    ok(Cr == CR_SUCCESS, "CM_Reenumerate_DevNode: 0x%lx\n", Cr);
    if (!WAIT_STAT(Fake, Initializes, Initializes + 1, 30000))
    {
        ok(0, "%ls: the device did not come back\n", Fake->Name);
        return FALSE;
    }
    WaitAdapter(Fake, "the add");
    return TRUE;
}

/* (f) the removed device comes back, without a reboot: a new adapter starts, and NDIS binds the protocols to
   it. They must have let go of the removed adapter (before, ndisuio kept its binding to the freed adapter,
   and the bind walked it). The new adapter works, and goes away the same way. */
static
void
TestReAdd(PFAKE Fake)
{
    FAKEMP_STATS St;

    trace("%ls: add after remove\n", Fake->Name);

    if (!BringBack(Fake))
        return;

    St = Stats(Fake);
    ok(St.Halted == 0, "%ls: halted\n", Fake->Name);
    CheckAlive(Fake, "on the adapter added after a remove");
    DropBinding(Fake);
    if (Abandoned)
        return;

    /* Requests pended and queued on it at its remove end as on the first one */
    TestRemove(Fake);
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

    /* The adapters come and go with the test: both are installed first, then each is removed and
       comes back (readd: an adapter starts after another one was removed) */
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
        if (!Abandoned && Want("sends"))
            TestSends(Fake);
        if (!Abandoned && Want("receive"))
            TestReceive(Fake);
        DropBinding(Fake);
        DropBinding(Fake);
        if (Abandoned)
            break;
    }

    for (i = 0; i < 2 && !Abandoned; i++)
    {
        Fake = &Fakes[i];
        if (!Fake->Installed)
            continue;

        if (Want("sendhalt"))
            TestSendHalt(Fake);
        if (!Abandoned && (Want("remove") || Want("readd")) && TestRemove(Fake) && !Abandoned && Want("readd"))
            TestReAdd(Fake);
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
