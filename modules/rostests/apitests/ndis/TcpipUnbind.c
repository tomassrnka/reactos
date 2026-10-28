/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     tcpip lets go of an adapter that NDIS unbinds while traffic uses it
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * A fake miniport (fakempt, fakemp with its link up, as a root device: ROOT\FAKEMPT) is bound to tcpip with a
 * static address, and is removed and brought back (a reenumeration of the root) again and again
 * while threads use the interface:
 *  - UDP senders to neighbors that are not resolved (the senders wait on their neighbor entries),
 *    to the subnet broadcast, and through a gateway route;
 *  - frames looped back to tcpip through ndisuio (NDIS loops a frame to the adapter's own address
 *    back to the bindings): ARP replies that resolve the neighbors (queued packets then go out),
 *    ARP requests for our address (tcpip replies) and ICMP echo requests (tcpip routes a reply);
 *  - information requests to tcpip: entity list, interface, ARP and route tables of every interface;
 *    static ARP entries and routes added on the interface and removed (TDI requests straight to
 *    \Device\Tcp: iphlpapi's table copies overrun their buffers when the tables change meanwhile);
 *    and SendARP, which keeps the interface while it waits.
 * The neighbors, routes, in-flight sends and requests that use the interface when NDIS unbinds it
 * must not reach the freed interface or adapter; with special pool for the tcpip tags (LAN IPIF NCE FIB)
 * any use after free faults. Each removal must take the address away, and each return bring it back.
 *
 * Optional argument after the test name: the number of remove/return cycles (default 20).
 * Run it once per boot: a removed root device stays a leaked device node until then.
 */

#include <apitest.h>
#include <winsock2.h>
#include <iphlpapi.h>

#define WIN32_NO_STATUS
#include <ndk/iofuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>
#include <winioctl.h>
#include <tdi.h>
#include <tdiinfo.h>
#include <tcpioctl.h>
#include <winreg.h>
#include <cfgmgr32.h>
#include <shlwapi.h>

#define FormatW(Buffer, ...) _snwprintf(Buffer, ARRAYSIZE(Buffer) - 1, __VA_ARGS__)

#define IOCTL_NDISUIO_OPEN_DEVICE_  CTL_CODE(FILE_DEVICE_NETWORK, 0x200, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define NET_CLASS_GUID      L"{4D36E972-E325-11CE-BFC1-08002BE10318}"
#define CLASS_KEY           L"SYSTEM\\CurrentControlSet\\Control\\Class\\" NET_CLASS_GUID
#define ENUM_ROOT_KEY       L"SYSTEM\\CurrentControlSet\\Enum\\Root"
#define SERVICES_KEY        L"SYSTEM\\CurrentControlSet\\Services"
#define TCPIP_LINKAGE_KEY   SERVICES_KEY L"\\Tcpip\\Linkage"
#define TCPIP_IF_KEY        SERVICES_KEY L"\\Tcpip\\Parameters\\Interfaces"

#define FAKE_SERVICE        L"fakempt"
#define FAKE_ENUM_NAME      L"FAKEMPT"
#define FAKE_INSTANCE       L"ROOT\\FAKEMPT\\0000"
#define FAKE_GUID           L"{7A1D0F53-6D50-4C2A-9E70-46414B454D54}"
#define FAKE_ADAPTER        L"\\Device\\" FAKE_GUID
#define FAKE_CLASS_INDEX    L"0952"

/* The fake miniport's address (fakempt) */
static const UCHAR OurMac[6] = { 0x02, 0xFA, 0xCE, 0x4D, 0x50, 0x54 };

/* 10.250.0.0/24: we are .2, the gateway is .1, neighbors .3 - .40 */
#define NET(n)              (0x0000FA0AUL | ((ULONG)(n) << 24))  /* 10.250.0.n, network order */
#define OUR_ADDRESS         NET(2)
#define GATEWAY             NET(1)
#define FIRST_NEIGHBOR      3
#define NEIGHBORS           38
#define ROUTED_NET          0x006433C6UL    /* 198.51.100.0 (TEST-NET-2): through the default gateway */
#define STATIC_ROUTED_NET   0x000200C0UL    /* 192.0.2.0 (TEST-NET-1), added and removed by the helper thread */

#define UDP_SENDERS         3
#define DEFAULT_CYCLES      20

/* tcpip's route entry for IP_MIB_ARPTABLE_ENTRY_ID on a network layer entity (IPROUTE_ENTRY) */
typedef struct _ROUTE_ENTRY
{
    ULONG Dest, Index, Metric1, Metric2, Metric3, Metric4, Gw, Type, Proto, Age, Mask, Metric5, Info;
} ROUTE_ENTRY;
#define ROUTE_ADD           3
#define ROUTE_DELETE        2

typedef struct _COUNTS
{
    volatile LONG Ok;
    volatile LONG Failed;
} COUNTS;

static volatile LONG Stop;
static COUNTS UdpCounts, InjectCounts, HelperCounts, ArpCounts;
static BOOL BindValueChanged;
static PWSTR SavedBind;     /* The Tcpip Linkage\Bind value before the test, NULL if there was none */
static DWORD SavedBindSize;

static
ULONG
Random(ULONG Range)
{
    static volatile LONG Seed = 0x2545F491;
    LONG Value = InterlockedExchangeAdd(&Seed, 0x9E3779B9);

    Value ^= Value >> 13;
    Value *= 0x5BD1E995;
    Value ^= Value >> 15;
    return (ULONG)Value % Range;
}

static
LONG
SetString(HKEY Key, PCWSTR Name, DWORD Type, PCWSTR Value)
{
    return RegSetValueExW(Key, Name, 0, Type, (const BYTE *)Value, (lstrlenW(Value) + 1) * sizeof(WCHAR));
}

/* A REG_MULTI_SZ of one string */
static
LONG
SetMulti(HKEY Key, PCWSTR Name, PCWSTR Value)
{
    WCHAR Buffer[64];

    ZeroMemory(Buffer, sizeof(Buffer));
    lstrcpynW(Buffer, Value, ARRAYSIZE(Buffer) - 1);
    return RegSetValueExW(Key, Name, 0, REG_MULTI_SZ, (const BYTE *)Buffer, (lstrlenW(Buffer) + 2) * sizeof(WCHAR));
}

static
LONG
SetDword(HKEY Key, PCWSTR Name, DWORD Value)
{
    return RegSetValueExW(Key, Name, 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
}

/* tcpip binds only the adapters of its Linkage\Bind value when there is one (a NIC was installed):
   add ours, and put the value back at the end. Without the value NDIS binds it to every adapter. */
static
BOOL
AddToTcpipBind(void)
{
    HKEY Key;
    DWORD Type, Size = 0;
    PWSTR Value, p;
    LONG Error;
    SIZE_T Add = (lstrlenW(FAKE_ADAPTER) + 1) * sizeof(WCHAR);

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, TCPIP_LINKAGE_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &Key);
    if (Error != ERROR_SUCCESS)
        return TRUE;

    Error = RegQueryValueExW(Key, L"Bind", NULL, &Type, NULL, &Size);
    if (Error != ERROR_SUCCESS || Type != REG_MULTI_SZ)
    {
        RegCloseKey(Key);
        return TRUE;
    }

    Value = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size + Add + 2 * sizeof(WCHAR));
    SavedBind = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size + sizeof(WCHAR));
    if (!Value || !SavedBind)
    {
        RegCloseKey(Key);
        return FALSE;
    }
    RegQueryValueExW(Key, L"Bind", NULL, &Type, (PBYTE)SavedBind, &Size);
    SavedBindSize = Size;
    memcpy(Value, SavedBind, Size);

    /* The end of the list */
    for (p = Value; *p; p += lstrlenW(p) + 1)
    {
        if (!lstrcmpiW(p, FAKE_ADAPTER))
        {
            HeapFree(GetProcessHeap(), 0, Value);
            RegCloseKey(Key);
            return TRUE;
        }
    }
    lstrcpyW(p, FAKE_ADAPTER);
    p += lstrlenW(p) + 1;
    *p++ = UNICODE_NULL;

    Error = RegSetValueExW(Key, L"Bind", 0, REG_MULTI_SZ, (PBYTE)Value, (DWORD)((PBYTE)p - (PBYTE)Value));
    ok(Error == ERROR_SUCCESS, "Setting the Tcpip Bind value: %ld\n", Error);
    BindValueChanged = (Error == ERROR_SUCCESS);
    HeapFree(GetProcessHeap(), 0, Value);
    RegCloseKey(Key);
    return BindValueChanged;
}

static
void
RestoreTcpipBind(void)
{
    HKEY Key;

    if (!BindValueChanged)
        return;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, TCPIP_LINKAGE_KEY, 0, KEY_SET_VALUE, &Key) == ERROR_SUCCESS)
    {
        RegSetValueExW(Key, L"Bind", 0, REG_MULTI_SZ, (PBYTE)SavedBind, SavedBindSize);
        RegCloseKey(Key);
    }
    BindValueChanged = FALSE;
}

static
BOOL
DevNode(DEVINST *DevInst)
{
    return CM_Locate_DevNodeW(DevInst, (DEVINSTID_W)FAKE_INSTANCE, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
}

static
BOOL
Reenumerate(void)
{
    DEVINST Root;
    CONFIGRET Cr;

    Cr = CM_Locate_DevNodeW(&Root, NULL, CM_LOCATE_DEVNODE_NORMAL);
    if (Cr == CR_SUCCESS)
        Cr = CM_Reenumerate_DevNode(Root, CM_REENUMERATE_SYNCHRONOUS);
    ok(Cr == CR_SUCCESS, "Reenumerating the root: 0x%lx\n", Cr);
    return Cr == CR_SUCCESS;
}

static
BOOL
RemoveDevice(BOOL Quiet)
{
    DEVINST DevInst;
    PNP_VETO_TYPE Veto = PNP_VetoTypeUnknown;
    WCHAR VetoName[MAX_PATH] = L"";
    CONFIGRET Cr;

    if (!DevNode(&DevInst))
        return FALSE;
    Cr = CM_Query_And_Remove_SubTreeW(DevInst, &Veto, VetoName, ARRAYSIZE(VetoName), CM_REMOVE_NO_RESTART);
    ok(Quiet || Cr == CR_SUCCESS, "CM_Query_And_Remove_SubTreeW: 0x%lx (veto %d %ls)\n", Cr, Veto, VetoName);
    return Cr == CR_SUCCESS;
}

static
void
Count(COUNTS *Counts, BOOL Ok)
{
    InterlockedIncrement(Ok ? &Counts->Ok : &Counts->Failed);
}

static
void
Count_(COUNTS *Counts, NTSTATUS Status)
{
    Count(Counts, NT_SUCCESS(Status));
}

/* --- TDI requests to tcpip --- */

static
HANDLE
OpenTcp(void)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(L"\\Device\\Tcp");
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Io;
    HANDLE Handle;
    NTSTATUS Status;

    InitializeObjectAttributes(&Attributes, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Handle, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &Attributes, &Io,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
    return NT_SUCCESS(Status) ? Handle : NULL;
}

/* Bounded: tcpip copies nothing when the buffer is too small, and reports the size */
static
NTSTATUS
TdiQuery(HANDLE Tcp, ULONG Entity, ULONG Instance, ULONG Class, ULONG Type, ULONG Id,
         PVOID Out, ULONG OutSize, PULONG Returned)
{
    TCP_REQUEST_QUERY_INFORMATION_EX Query;
    IO_STATUS_BLOCK Io;
    NTSTATUS Status;

    ZeroMemory(&Query, sizeof(Query));
    Query.ID.toi_entity.tei_entity = Entity;
    Query.ID.toi_entity.tei_instance = Instance;
    Query.ID.toi_class = Class;
    Query.ID.toi_type = Type;
    Query.ID.toi_id = Id;
    Io.Information = 0;
    Status = NtDeviceIoControlFile(Tcp, NULL, NULL, NULL, &Io, IOCTL_TCP_QUERY_INFORMATION_EX,
                                   &Query, sizeof(Query), Out, OutSize);
    if (Returned)
        *Returned = (ULONG)Io.Information;
    return Status;
}

static
NTSTATUS
TdiSet(HANDLE Tcp, ULONG Entity, ULONG Instance, ULONG Id, PVOID In, ULONG InSize)
{
    BYTE Buffer[sizeof(TCP_REQUEST_SET_INFORMATION_EX) + 128];
    PTCP_REQUEST_SET_INFORMATION_EX Set = (PTCP_REQUEST_SET_INFORMATION_EX)Buffer;
    IO_STATUS_BLOCK Io;

    ZeroMemory(Buffer, sizeof(Buffer));
    Set->ID.toi_entity.tei_entity = Entity;
    Set->ID.toi_entity.tei_instance = Instance;
    Set->ID.toi_class = INFO_CLASS_PROTOCOL;
    Set->ID.toi_type = INFO_TYPE_PROVIDER;
    Set->ID.toi_id = Id;
    Set->BufferSize = InSize;
    memcpy(Set->Buffer, In, InSize);
    return NtDeviceIoControlFile(Tcp, NULL, NULL, NULL, &Io, IOCTL_TCP_SET_INFORMATION_EX,
                                 Set, FIELD_OFFSET(TCP_REQUEST_SET_INFORMATION_EX, Buffer) + InSize, NULL, 0);
}

static
ULONG
Entities(HANDLE Tcp, TDIEntityID *List, ULONG Capacity)
{
    ULONG Returned;

    if (!NT_SUCCESS(TdiQuery(Tcp, GENERIC_ENTITY, 0, INFO_CLASS_GENERIC, INFO_TYPE_PROVIDER, ENTITY_LIST_ID,
                             List, Capacity * sizeof(TDIEntityID), &Returned)))
        return 0;
    return min(Returned / sizeof(TDIEntityID), Capacity);
}

typedef struct _OURS
{
    ULONG IfIndex;      /* tcpip's interface index */
    ULONG NlInstance;   /* Its network layer (CL_NL) entity */
    ULONG AtInstance;   /* Its address translation (AT) entity, -1 if not found */
} OURS;

/* Whether tcpip has an interface with our address, and its entities */
static
BOOL
FindOurs(HANDLE Tcp, OURS *Ours)
{
    TDIEntityID List[128];
    MIB_IPADDRROW Addr[4];
    ULONG Count, i, Returned, AtInfo[2];
    BOOL Found = FALSE;

    Count = Entities(Tcp, List, ARRAYSIZE(List));
    for (i = 0; i < Count && !Found; i++)
    {
        if (List[i].tei_entity != CL_NL_ENTITY)
            continue;
        if (NT_SUCCESS(TdiQuery(Tcp, CL_NL_ENTITY, List[i].tei_instance, INFO_CLASS_PROTOCOL, INFO_TYPE_PROVIDER,
                                IP_MIB_ADDRTABLE_ENTRY_ID, Addr, sizeof(Addr), &Returned)) &&
            Returned >= sizeof(Addr[0]) && Addr[0].dwAddr == OUR_ADDRESS)
        {
            Ours->IfIndex = Addr[0].dwIndex;
            Ours->NlInstance = List[i].tei_instance;
            Found = TRUE;
        }
    }
    if (!Found)
        return FALSE;

    Ours->AtInstance = (ULONG)-1;
    for (i = 0; i < Count; i++)
    {
        if (List[i].tei_entity != AT_ENTITY)
            continue;
        if (NT_SUCCESS(TdiQuery(Tcp, AT_ENTITY, List[i].tei_instance, INFO_CLASS_PROTOCOL, INFO_TYPE_PROVIDER,
                                IF_MIB_STATS_ID, AtInfo, sizeof(AtInfo), &Returned)) &&
            Returned == sizeof(AtInfo) && AtInfo[1] == Ours->IfIndex)
        {
            Ours->AtInstance = List[i].tei_instance;
            break;
        }
    }
    return TRUE;
}

/* Every table of every interface (counted in HelperCounts) */
static
void
QueryAll(HANDLE Tcp, PBYTE Buffer, ULONG Size)
{
    TDIEntityID List[128];
    ULONG Count, i, Returned;

    Count = Entities(Tcp, List, ARRAYSIZE(List));
    for (i = 0; i < Count; i++)
    {
        switch (List[i].tei_entity)
        {
            case IF_ENTITY:
                Count_(&HelperCounts, TdiQuery(Tcp, IF_ENTITY, List[i].tei_instance, INFO_CLASS_PROTOCOL,
                                               INFO_TYPE_PROVIDER, IF_MIB_STATS_ID, Buffer, Size, &Returned));
                break;
            case AT_ENTITY:
                Count_(&HelperCounts, TdiQuery(Tcp, AT_ENTITY, List[i].tei_instance, INFO_CLASS_PROTOCOL,
                                               INFO_TYPE_PROVIDER, IP_MIB_ARPTABLE_ENTRY_ID, Buffer, Size, &Returned));
                break;
            case CL_NL_ENTITY:
                /* The routes of the interface, and its statistics */
                Count_(&HelperCounts, TdiQuery(Tcp, CL_NL_ENTITY, List[i].tei_instance, INFO_CLASS_PROTOCOL,
                                               INFO_TYPE_PROVIDER, IP_MIB_ARPTABLE_ENTRY_ID, Buffer, Size, &Returned));
                Count_(&HelperCounts, TdiQuery(Tcp, CL_NL_ENTITY, List[i].tei_instance, INFO_CLASS_PROTOCOL,
                                               INFO_TYPE_PROVIDER, IF_MIB_STATS_ID, Buffer, Size, &Returned));
                break;
        }
    }
}

/* The interface index of our address, 0 if tcpip does not have it (loopback has 0) */
static
DWORD
OurIfIndex(void)
{
    static HANDLE Tcp;
    OURS Ours;

    if (!Tcp)
        Tcp = OpenTcp();
    if (!Tcp || !FindOurs(Tcp, &Ours))
        return 0;
    return Ours.IfIndex;
}

static
DWORD
WaitAddress(BOOL Present, DWORD TimeoutMs)
{
    DWORD Start = GetTickCount(), Index;

    for (;;)
    {
        Index = OurIfIndex();
        if ((Index != 0) == Present || GetTickCount() - Start >= TimeoutMs)
            return Index;
        Sleep(100);
    }
}

static
BOOL
Install(void)
{
    WCHAR Source[MAX_PATH], Destination[MAX_PATH], Path[MAX_PATH], HardwareId[64];
    PWSTR Slash;
    HKEY Key, SubKey;
    LONG Error;

    GetModuleFileNameW(NULL, Source, MAX_PATH);
    Slash = wcsrchr(Source, L'\\');
    lstrcpyW(Slash + 1, FAKE_SERVICE L".sys");
    GetSystemDirectoryW(Destination, MAX_PATH);
    lstrcatW(Destination, L"\\drivers\\" FAKE_SERVICE L".sys");
    if (!CopyFileW(Source, Destination, FALSE) && GetFileAttributesW(Destination) == INVALID_FILE_ATTRIBUTES)
    {
        skip("Cannot copy %ls: %lu\n", Source, GetLastError());
        return FALSE;
    }

    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, SERVICES_KEY L"\\" FAKE_SERVICE, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL);
    if (Error != ERROR_SUCCESS)
    {
        skip("Cannot create the service key: %ld\n", Error);
        return FALSE;
    }
    SetDword(Key, L"Type", 1);
    SetDword(Key, L"Start", 3);
    SetDword(Key, L"ErrorControl", 0);
    SetString(Key, L"ImagePath", REG_EXPAND_SZ, L"system32\\drivers\\" FAKE_SERVICE L".sys");
    SetString(Key, L"Group", REG_SZ, L"NDIS");
    RegCloseKey(Key);

    /* The driver key with the linkage that the network class installer writes: tcpip finds the
       adapter's description through RootDevice */
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, CLASS_KEY L"\\" FAKE_CLASS_INDEX, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL);
    ok(Error == ERROR_SUCCESS, "Class key: %ld\n", Error);
    if (Error != ERROR_SUCCESS)
        return FALSE;
    SetString(Key, L"DriverDesc", REG_SZ, L"FakeMpT");
    SetString(Key, L"ProviderName", REG_SZ, L"ReactOS Project");
    if (RegCreateKeyExW(Key, L"Linkage", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &SubKey, NULL) == ERROR_SUCCESS)
    {
        SetString(SubKey, L"Export", REG_SZ, FAKE_ADAPTER);
        SetString(SubKey, L"RootDevice", REG_SZ, FAKE_GUID);
        SetString(SubKey, L"UpperBind", REG_SZ, L"Tcpip");
        RegCloseKey(SubKey);
    }
    RegCloseKey(Key);

    /* A static address */
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, TCPIP_IF_KEY L"\\" FAKE_GUID, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL);
    ok(Error == ERROR_SUCCESS, "Tcpip interface key: %ld\n", Error);
    if (Error != ERROR_SUCCESS)
        return FALSE;
    SetDword(Key, L"EnableDHCP", 0);
    SetMulti(Key, L"IPAddress", L"10.250.0.2");
    SetMulti(Key, L"SubnetMask", L"255.255.255.0");
    /* A default route through a neighbor on the interface (a FIB entry with its NCE) */
    SetMulti(Key, L"DefaultGateway", L"10.250.0.1");
    RegCloseKey(Key);

    if (!AddToTcpipBind())
        return FALSE;

    FormatW(Path, ENUM_ROOT_KEY L"\\%s\\0000", FAKE_ENUM_NAME);
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &Key, NULL);
    ok(Error == ERROR_SUCCESS, "Enum key: %ld\n", Error);
    if (Error != ERROR_SUCCESS)
        return FALSE;
    ZeroMemory(HardwareId, sizeof(HardwareId));
    lstrcpyW(HardwareId, L"ROOT\\FAKEMPT");
    RegSetValueExW(Key, L"HardwareID", 0, REG_MULTI_SZ, (const BYTE *)HardwareId, (lstrlenW(HardwareId) + 2) * sizeof(WCHAR));
    SetString(Key, L"Service", REG_SZ, FAKE_SERVICE);
    SetString(Key, L"Class", REG_SZ, L"Net");
    SetString(Key, L"ClassGUID", REG_SZ, NET_CLASS_GUID);
    SetString(Key, L"Driver", REG_SZ, NET_CLASS_GUID L"\\" FAKE_CLASS_INDEX);
    SetString(Key, L"DeviceDesc", REG_SZ, L"FakeMpT");
    SetDword(Key, L"ConfigFlags", 0);
    RegCloseKey(Key);
    return TRUE;
}

static
void
Uninstall(void)
{
    HKEY Root;
    WCHAR Path[MAX_PATH];

    RemoveDevice(TRUE);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ENUM_ROOT_KEY, 0, DELETE | KEY_ENUMERATE_SUB_KEYS, &Root) == ERROR_SUCCESS)
    {
        SHDeleteKeyW(Root, FAKE_ENUM_NAME);
        RegCloseKey(Root);
    }
    SHDeleteKeyW(HKEY_LOCAL_MACHINE, CLASS_KEY L"\\" FAKE_CLASS_INDEX);
    SHDeleteKeyW(HKEY_LOCAL_MACHINE, TCPIP_IF_KEY L"\\" FAKE_GUID);
    RestoreTcpipBind();
    /* As FakeMiniport removes its own */
    SHDeleteKeyW(HKEY_LOCAL_MACHINE, SERVICES_KEY L"\\" FAKE_SERVICE);
    GetSystemDirectoryW(Path, MAX_PATH);
    lstrcatW(Path, L"\\drivers\\" FAKE_SERVICE L".sys");
    DeleteFileW(Path); /* fails while the driver is loaded */
}

/* --- Traffic --- */

static
DWORD
WINAPI
UdpSender(PVOID Context)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKADDR_IN To;
    char Data[64];
    BOOL On = TRUE;
    ULONG Kind;

    UNREFERENCED_PARAMETER(Context);
    if (s == INVALID_SOCKET)
        return 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&On, sizeof(On));
    memset(Data, 0x5A, sizeof(Data));
    ZeroMemory(&To, sizeof(To));
    To.sin_family = AF_INET;
    To.sin_port = htons(9);

    while (!Stop)
    {
        Kind = Random(8);
        if (Kind < 5)
            To.sin_addr.s_addr = NET(FIRST_NEIGHBOR + Random(NEIGHBORS));    /* on-link neighbor */
        else if (Kind < 6)
            To.sin_addr.s_addr = NET(255);                                  /* subnet broadcast (also looped locally) */
        else
            To.sin_addr.s_addr = ROUTED_NET | ((1 + Random(200)) << 24);    /* through a gateway */

        if (sendto(s, Data, sizeof(Data), 0, (SOCKADDR *)&To, sizeof(To)) == sizeof(Data))
            InterlockedIncrement(&UdpCounts.Ok);
        else
        {
            InterlockedIncrement(&UdpCounts.Failed);
            Sleep(5);
        }
    }
    closesocket(s);
    return 0;
}

static
HANDLE
OpenBinding(void)
{
    HANDLE Handle;
    DWORD Returned;

    Handle = CreateFileW(L"\\\\.\\Ndisuio", GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;
    if (!DeviceIoControl(Handle, IOCTL_NDISUIO_OPEN_DEVICE_, (PVOID)FAKE_ADAPTER,
                         lstrlenW(FAKE_ADAPTER) * sizeof(WCHAR), NULL, 0, &Returned, NULL))
    {
        CloseHandle(Handle);
        return INVALID_HANDLE_VALUE;
    }
    return Handle;
}

static
NTSTATUS
WriteFrame(HANDLE Binding, HANDLE Event, PUCHAR Frame, ULONG Length)
{
    IO_STATUS_BLOCK IoStatus;
    LARGE_INTEGER Offset;
    NTSTATUS Status;

    Offset.QuadPart = 0;
    ResetEvent(Event);
    Status = NtWriteFile(Binding, Event, NULL, NULL, &IoStatus, Frame, Length, &Offset, NULL);
    if (Status == STATUS_PENDING)
    {
        NtWaitForSingleObject(Event, FALSE, NULL);
        Status = IoStatus.Status;
    }
    return Status;
}

static
void
EthHeader(PUCHAR Frame, const UCHAR *Destination, const UCHAR *Source, USHORT EtherType)
{
    memcpy(Frame, Destination, 6);
    memcpy(Frame + 6, Source, 6);
    Frame[12] = (UCHAR)(EtherType >> 8);
    Frame[13] = (UCHAR)EtherType;
}

/* Frame length 42 */
static
void
ArpFrame(PUCHAR Frame, USHORT Opcode, const UCHAR *SenderMac, ULONG SenderIp, const UCHAR *TargetMac, ULONG TargetIp)
{
    EthHeader(Frame, OurMac, SenderMac, 0x0806);
    Frame[14] = 0; Frame[15] = 1;           /* Ethernet */
    Frame[16] = 0x08; Frame[17] = 0x00;     /* IPv4 */
    Frame[18] = 6; Frame[19] = 4;
    Frame[20] = 0; Frame[21] = (UCHAR)Opcode;
    memcpy(Frame + 22, SenderMac, 6);
    memcpy(Frame + 28, &SenderIp, 4);
    memcpy(Frame + 32, TargetMac, 6);
    memcpy(Frame + 38, &TargetIp, 4);
}

static
USHORT
Checksum(const UCHAR *Data, ULONG Length)
{
    ULONG Sum = 0, i;

    for (i = 0; i + 1 < Length; i += 2)
        Sum += (Data[i] << 8) | Data[i + 1];
    if (Length & 1)
        Sum += Data[Length - 1] << 8;
    while (Sum >> 16)
        Sum = (Sum & 0xFFFF) + (Sum >> 16);
    return (USHORT)~Sum;
}

/* An ICMP echo request from a neighbor to us: frame length 14 + 20 + 8 + 32 */
static
ULONG
EchoFrame(PUCHAR Frame, const UCHAR *SenderMac, ULONG SenderIp, USHORT Sequence)
{
    PUCHAR Ip = Frame + 14, Icmp = Ip + 20;
    USHORT Sum;
    ULONG i;

    EthHeader(Frame, OurMac, SenderMac, 0x0800);
    ZeroMemory(Ip, 20 + 8 + 32);
    Ip[0] = 0x45;
    Ip[3] = 20 + 8 + 32;
    Ip[8] = 64;
    Ip[9] = 1;
    memcpy(Ip + 12, &SenderIp, 4);
    *(ULONG UNALIGNED *)(Ip + 16) = OUR_ADDRESS;
    Sum = Checksum(Ip, 20);
    Ip[10] = (UCHAR)(Sum >> 8); Ip[11] = (UCHAR)Sum;
    Icmp[0] = 8;
    Icmp[6] = (UCHAR)(Sequence >> 8); Icmp[7] = (UCHAR)Sequence;
    for (i = 0; i < 32; i++)
        Icmp[8 + i] = (UCHAR)i;
    Sum = Checksum(Icmp, 8 + 32);
    Icmp[2] = (UCHAR)(Sum >> 8); Icmp[3] = (UCHAR)Sum;
    return 14 + 20 + 8 + 32;
}

static
DWORD
WINAPI
Injector(PVOID Context)
{
    HANDLE Binding = INVALID_HANDLE_VALUE;
    HANDLE Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    UCHAR Frame[128], Mac[6] = { 0x02, 0x00, 0x5E, 0x20, 0x00, 0x00 };
    UCHAR Zero[6] = { 0 };
    ULONG Neighbor, Length, Kind;
    USHORT Sequence = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Context);

    while (!Stop)
    {
        if (Binding == INVALID_HANDLE_VALUE)
        {
            Binding = OpenBinding();
            if (Binding == INVALID_HANDLE_VALUE)
            {
                Sleep(20);
                continue;
            }
        }

        Kind = Random(10);
        Neighbor = (Kind == 0) ? 1 : FIRST_NEIGHBOR + Random(NEIGHBORS);
        Mac[5] = (UCHAR)Neighbor;
        /* A new link address now and then: the neighbor entry is updated */
        Mac[4] = (UCHAR)Random(3);
        ZeroMemory(Frame, sizeof(Frame));

        if (Kind < 6)
        {
            /* A reply to our solicitation (or to none): resolves the neighbor */
            ArpFrame(Frame, 2, Mac, NET(Neighbor), OurMac, OUR_ADDRESS);
            Length = 60;
        }
        else if (Kind < 8)
        {
            /* Who has our address: tcpip answers */
            ArpFrame(Frame, 1, Mac, NET(Neighbor), Zero, OUR_ADDRESS);
            Length = 60;
        }
        else
        {
            Length = EchoFrame(Frame, Mac, NET(Neighbor), Sequence++);
        }

        Status = WriteFrame(Binding, Event, Frame, Length);
        if (NT_SUCCESS(Status))
        {
            InterlockedIncrement(&InjectCounts.Ok);
        }
        else
        {
            /* The adapter was removed: ndisuio let go of it, open it again when it is back */
            InterlockedIncrement(&InjectCounts.Failed);
            CloseHandle(Binding);
            Binding = INVALID_HANDLE_VALUE;
        }
    }

    if (Binding != INVALID_HANDLE_VALUE)
        CloseHandle(Binding);
    CloseHandle(Event);
    return 0;
}

static
DWORD
WINAPI
Helper(PVOID Context)
{
    static BYTE Buffer[16384];
    HANDLE Tcp = OpenTcp();
    OURS Ours;
    MIB_IPNETROW Arp;
    ROUTE_ENTRY Route;
    UCHAR Mac[6] = { 0x02, 0x00, 0x5E, 0x30, 0x00, 0x00 };

    UNREFERENCED_PARAMETER(Context);
    if (!Tcp)
        return 1;

    while (!Stop)
    {
        QueryAll(Tcp, Buffer, sizeof(Buffer));

        if (!FindOurs(Tcp, &Ours))
        {
            Sleep(10);
            continue;
        }

        /* A static neighbor and a route through the gateway on our interface; both go again.
           The interface may be gone meanwhile: the requests then fail, they must not reach it */
        if (Ours.AtInstance != (ULONG)-1)
        {
            ZeroMemory(&Arp, sizeof(Arp));
            Arp.dwIndex = Ours.IfIndex;
            Arp.dwPhysAddrLen = 6;
            Mac[5] = (UCHAR)(200 + Random(20));
            memcpy(Arp.bPhysAddr, Mac, 6);
            Arp.dwAddr = NET(200 + Random(20));
            Arp.dwType = MIB_IPNET_TYPE_STATIC;
            Count_(&HelperCounts, TdiSet(Tcp, AT_ENTITY, Ours.AtInstance, IP_MIB_ARPTABLE_ENTRY_ID, &Arp, sizeof(Arp)));
        }

        ZeroMemory(&Route, sizeof(Route));
        Route.Dest = STATIC_ROUTED_NET | ((Random(4)) << 16);
        Route.Mask = 0x00FFFFFF;
        Route.Gw = GATEWAY;
        Route.Index = Ours.IfIndex;
        Route.Metric1 = 1;
        Route.Type = ROUTE_ADD;
        Count_(&HelperCounts, TdiSet(Tcp, CL_NL_ENTITY, Ours.NlInstance, IP_MIB_ARPTABLE_ENTRY_ID, &Route, sizeof(Route)));
        Sleep(Random(20));
        Route.Type = ROUTE_DELETE;
        Count_(&HelperCounts, TdiSet(Tcp, CL_NL_ENTITY, Ours.NlInstance, IP_MIB_ARPTABLE_ENTRY_ID, &Route, sizeof(Route)));
    }

    CloseHandle(Tcp);
    return 0;
}

/* SendARP waits up to 10 seconds for an answer that does not come, and keeps the interface meanwhile */
static
DWORD
WINAPI
ArpWaiter(PVOID Context)
{
    ULONG Mac[2], Length;

    UNREFERENCED_PARAMETER(Context);

    while (!Stop)
    {
        Length = sizeof(Mac);
        Count(&ArpCounts, SendARP(NET(100 + Random(50)), OUR_ADDRESS, Mac, &Length) == NO_ERROR);
        if (!OurIfIndex())
            Sleep(50);
    }
    return 0;
}

START_TEST(TcpipUnbind)
{
    char **Argv;
    int Argc;
    ULONG Cycles = DEFAULT_CYCLES, Cycle, Removed = 0, Returned = 0, i;
    HANDLE Threads[UDP_SENDERS + 3];
    ULONG ThreadCount = 0;
    DWORD Index, Wait;
    WSADATA WsaData;
    DEVINST DevInst;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc > 2 && atoi(Argv[2]) > 0)
        Cycles = atoi(Argv[2]);

    if (!is_reactos())
    {
        skip("The fake miniport is a ReactOS driver\n");
        return;
    }

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    {
        skip("WSAStartup failed\n");
        return;
    }

    if (DevNode(&DevInst))
    {
        skip("%ls is there already (run once per boot)\n", FAKE_INSTANCE);
        WSACleanup();
        return;
    }

    if (!Install())
    {
        Uninstall();
        WSACleanup();
        return;
    }

    Reenumerate();
    Index = WaitAddress(TRUE, 30000);
    ok(Index != 0, "tcpip has no interface with 10.250.0.2 after the install\n");
    if (!Index)
    {
        Uninstall();
        WSACleanup();
        return;
    }

    for (i = 0; i < UDP_SENDERS; i++)
        Threads[ThreadCount++] = CreateThread(NULL, 0, UdpSender, NULL, 0, NULL);
    Threads[ThreadCount++] = CreateThread(NULL, 0, Injector, NULL, 0, NULL);
    Threads[ThreadCount++] = CreateThread(NULL, 0, Helper, NULL, 0, NULL);
    Threads[ThreadCount++] = CreateThread(NULL, 0, ArpWaiter, NULL, 0, NULL);
    for (i = 0; i < ThreadCount; i++)
        ok(Threads[i] != NULL, "CreateThread %lu failed: %lu\n", i, GetLastError());

    for (Cycle = 0; Cycle < Cycles; Cycle++)
    {
        /* Traffic on the interface for a while */
        Sleep(300 + Random(1200));

        if (!RemoveDevice(FALSE))
            break;
        Removed++;

        /* NDIS unbound tcpip in the removal: the address went with the interface */
        Index = OurIfIndex();
        ok(Index == 0, "Cycle %lu: 10.250.0.2 is still there after the removal (interface %lu)\n", Cycle, Index);

        /* The senders and helpers run on without the interface for a bit */
        Sleep(100 + Random(500));

        if (!Reenumerate())
            break;
        Index = WaitAddress(TRUE, 30000);
        ok(Index != 0, "Cycle %lu: no 10.250.0.2 after the device came back\n", Cycle);
        if (!Index)
            break;
        Returned++;
    }

    trace("%lu cycles: %lu removals, %lu returns\n", Cycles, Removed, Returned);
    ok(Removed == Cycles && Returned == Cycles, "%lu of %lu removals, %lu returns\n", Removed, Cycles, Returned);

    /* The last removal with the traffic still running */
    Sleep(500);
    if (Returned == Removed)
    {
        ok(RemoveDevice(FALSE), "The final removal failed\n");
        ok(WaitAddress(FALSE, 1000) == 0, "10.250.0.2 is still there after the final removal\n");
    }

    InterlockedExchange(&Stop, TRUE);
    Wait = WaitForMultipleObjects(ThreadCount, Threads, TRUE, 60000);
    ok(Wait != WAIT_TIMEOUT, "The traffic threads did not stop\n");
    for (i = 0; i < ThreadCount; i++)
    {
        if (Threads[i])
            CloseHandle(Threads[i]);
    }

    trace("UDP sends: %ld ok, %ld failed\n", UdpCounts.Ok, UdpCounts.Failed);
    trace("Frames looped to tcpip: %ld ok, %ld failed\n", InjectCounts.Ok, InjectCounts.Failed);
    trace("IP helper calls: %ld ok, %ld failed\n", HelperCounts.Ok, HelperCounts.Failed);
    trace("SendARP: %ld ok, %ld failed\n", ArpCounts.Ok, ArpCounts.Failed);
    ok(UdpCounts.Ok > 0, "No UDP datagram was sent\n");
    ok(InjectCounts.Ok > 0, "No frame reached tcpip\n");

    Uninstall();
    WSACleanup();
}
