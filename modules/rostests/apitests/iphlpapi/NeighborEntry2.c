/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for GetIpNetEntry2, ResolveIpNetEntry2 and the thread compartment calls
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * msquic.dll imports GetIpNetEntry2, ResolveIpNetEntry2 and SetCurrentThreadCompartmentId from
 * IPHLPAPI.DLL. The expected values were probed on Windows 10 22H2 (x64, native and as a 32-bit
 * process). Everything is bound at run time, so the program also runs where a function is absent
 * (and is then a plain failure of the check for it).
 */

#include <apitest.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>

/* The row of the Windows SDK, under a private name: the headers of this tree do not declare it. */
typedef struct
{
    SOCKADDR_INET Address;
    ULONG InterfaceIndex;
    NET_LUID InterfaceLuid;
    UCHAR PhysicalAddress[32];
    ULONG PhysicalAddressLength;
    ULONG State;
    UCHAR Flags;
    ULONG ReachabilityTime;
} NEIGHBOR_ROW;

#define NlnsIncomplete 1
#define NlnsProbe     2
#define NlnsReachable 5
#define NlnsPermanent 6

#define LOOPBACK_LUID 0x0018000000000000ULL

typedef ULONG (WINAPI *PFN_GetComp)(void);
typedef DWORD (WINAPI *PFN_SetComp)(ULONG);
typedef DWORD (WINAPI *PFN_Entry)(NEIGHBOR_ROW *);
typedef DWORD (WINAPI *PFN_Resolve)(NEIGHBOR_ROW *, const SOCKADDR_INET *);
typedef DWORD (WINAPI *PFN_Adapters)(PIP_ADAPTER_INFO, PULONG);
typedef DWORD (WINAPI *PFN_Arp)(IPAddr, IPAddr, PULONG, PULONG);
typedef DWORD (WINAPI *PFN_ToLuid)(ULONG, NET_LUID *);

static PFN_GetComp pGet;
static PFN_SetComp pSet;
static PFN_Entry pEntry;
static PFN_Resolve pResolve;

static IPAddr Ip(UCHAR a, UCHAR b, UCHAR c, UCHAR d)
{
    return (IPAddr)a | ((IPAddr)b << 8) | ((IPAddr)c << 16) | ((IPAddr)d << 24);
}

static IPAddr ParseIp(const char *Text)
{
    unsigned a, b, c, d;

    if (sscanf(Text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 0;
    return Ip(a, b, c, d);
}

static void V4(NEIGHBOR_ROW *Row, IPAddr Address, ULONG Index, ULONG64 Luid)
{
    memset(Row, 0, sizeof(*Row));
    Row->Address.Ipv4.sin_family = AF_INET;
    Row->Address.Ipv4.sin_addr.s_addr = Address;
    Row->InterfaceIndex = Index;
    Row->InterfaceLuid.Value = Luid;
}

static void V6(NEIGHBOR_ROW *Row, const USHORT *Words, ULONG Index)
{
    int i;

    memset(Row, 0, sizeof(*Row));
    Row->Address.Ipv6.sin6_family = AF_INET6;
    for (i = 0; i < 8; i++)
        Row->Address.Ipv6.sin6_addr.u.Word[i] = (USHORT)((Words[i] << 8) | (Words[i] >> 8));
    Row->InterfaceIndex = Index;
}

/*
 * Nothing is cached for an address. Windows keeps the entry of a failed resolution for a while, Incomplete or
 * Unreachable, and GetIpNetEntry2 reports it with NO_ERROR, so an earlier probe of the same address is
 * tolerated.
 */
static BOOL IsAbsent(DWORD Error, const NEIGHBOR_ROW *Row)
{
    return Error == ERROR_NOT_FOUND || (Error == NO_ERROR && Row->State <= NlnsIncomplete);
}

static DWORD WINAPI CompartmentThread(void *Context)
{
    *(ULONG *)Context = pGet();
    return 0;
}

static void TestCompartment(void)
{
    ULONG InThread = 0;
    HANDLE Thread;
    DWORD Error;

    if (!pGet || !pSet)
        return;

    ok(pGet() == 1, "initial compartment %lu\n", pGet());

    Error = pSet(1);
    ok(Error == NO_ERROR, "Set(1): %lu\n", Error);
    ok(pGet() == 1, "compartment %lu after Set(1)\n", pGet());
    Error = pSet(0);
    ok(Error == NO_ERROR, "Set(0): %lu\n", Error);
    ok(pGet() == 1, "compartment %lu after Set(0)\n", pGet());
    Error = pSet(2);
    ok(Error == ERROR_NOT_FOUND, "Set(2): %lu\n", Error);
    ok(pGet() == 1, "compartment %lu after Set(2)\n", pGet());
    Error = pSet(0xffffffff);
    ok(Error == ERROR_NOT_FOUND, "Set(-1): %lu\n", Error);
    ok(pGet() == 1, "compartment %lu after Set(-1)\n", pGet());

    Thread = CreateThread(NULL, 0, CompartmentThread, &InThread, 0, NULL);
    if (Thread)
    {
        WaitForSingleObject(Thread, 10000);
        CloseHandle(Thread);
        ok(InThread == 1, "compartment %lu in a new thread\n", InThread);
    }
}

static void TestParameters(void)
{
    NEIGHBOR_ROW Row;
    DWORD Error;

    Error = pEntry(NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "GetIpNetEntry2(NULL): %lu\n", Error);
    Error = pResolve(NULL, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "ResolveIpNetEntry2(NULL): %lu\n", Error);

    memset(&Row, 0, sizeof(Row));
    Error = pEntry(&Row);
    ok(Error == ERROR_INVALID_PARAMETER, "GetIpNetEntry2(zero row): %lu\n", Error);
    Error = pResolve(&Row, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "ResolveIpNetEntry2(zero row): %lu\n", Error);

    memset(&Row, 0, sizeof(Row));
    Row.Address.si_family = 99;
    Row.InterfaceIndex = 1;
    Error = pEntry(&Row);
    ok(Error == ERROR_INVALID_PARAMETER, "GetIpNetEntry2(family 99): %lu\n", Error);
    Error = pResolve(&Row, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "ResolveIpNetEntry2(family 99): %lu\n", Error);

    V4(&Row, 0, 1, 0);
    Error = pResolve(&Row, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "ResolveIpNetEntry2(0.0.0.0): %lu\n", Error);
}

static void TestLoopback(void)
{
    static const USHORT Loop6[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
    NEIGHBOR_ROW Row;
    DWORD Error;

    V4(&Row, Ip(127, 0, 0, 1), 0, 0);
    Error = pEntry(&Row);
    ok(Error == ERROR_NOT_FOUND, "GetIpNetEntry2(127.0.0.1): %lu\n", Error);

    V4(&Row, Ip(127, 0, 0, 1), 0, 0);
    Error = pResolve(&Row, NULL);
    ok(Error == NO_ERROR, "ResolveIpNetEntry2(127.0.0.1): %lu\n", Error);
    if (Error == NO_ERROR)
    {
        /* the index is not checked: the loopback interface of this stack has index 0, Windows 1 */
        ok(Row.InterfaceLuid.Value == LOOPBACK_LUID, "luid 0x%I64x\n", Row.InterfaceLuid.Value);
        ok(Row.State == NlnsPermanent, "state %lu\n", Row.State);
        ok(Row.PhysicalAddressLength == 0, "link address length %lu\n", Row.PhysicalAddressLength);
    }

    V6(&Row, Loop6, 1);
    Error = pResolve(&Row, NULL);
    ok(Error == NO_ERROR, "ResolveIpNetEntry2(::1): %lu\n", Error);
    if (Error == NO_ERROR)
    {
        ok(Row.InterfaceLuid.Value == LOOPBACK_LUID, "::1: luid 0x%I64x\n", Row.InterfaceLuid.Value);
        ok(Row.State == NlnsPermanent, "::1: state %lu\n", Row.State);
    }
}

static void TestV6(ULONG Index)
{
    static const USHORT LinkLocal[8] = { 0xfe80, 0, 0, 0, 0, 0, 0, 1 };
    NEIGHBOR_ROW Row;
    DWORD Error;

    V6(&Row, LinkLocal, Index);
    Error = pEntry(&Row);
    ok(IsAbsent(Error, &Row), "GetIpNetEntry2(fe80::1): %lu, state %lu\n", Error, Row.State);

    V6(&Row, LinkLocal, Index);
    Error = pResolve(&Row, NULL);
    ok(Error == ERROR_BAD_NET_NAME, "ResolveIpNetEntry2(fe80::1): %lu\n", Error);
}

static void CheckNeighbor(const char *Name, const NEIGHBOR_ROW *Row, ULONG Index, const NET_LUID *Luid,
                          const ULONG *Hw)
{
    ok(Row->InterfaceIndex == Index, "%s: interface %lu, expected %lu\n", Name, Row->InterfaceIndex, Index);
    ok(Row->InterfaceLuid.Value == Luid->Value, "%s: luid 0x%I64x, expected 0x%I64x\n", Name,
       Row->InterfaceLuid.Value, Luid->Value);
    ok(Row->PhysicalAddressLength == 6, "%s: link address length %lu\n", Name, Row->PhysicalAddressLength);
    ok(!memcmp(Row->PhysicalAddress, Hw, 6), "%s: link address differs from the SendARP answer\n", Name);
    /* a fresh neighbor is Reachable, an older one Stale, Delay or Probe, depending on the time */
    ok(Row->State >= NlnsProbe && Row->State <= NlnsPermanent, "%s: state %lu\n", Name, Row->State);
}

static void TestNeighbors(void)
{
    PFN_Adapters pAdapters;
    PFN_Arp pArp;
    PFN_ToLuid pToLuid;
    HMODULE Iphlpapi = GetModuleHandleW(L"iphlpapi.dll");
    PIP_ADAPTER_INFO Adapters = NULL, A, Found = NULL;
    ULONG Size = 0, Hw[2] = { 0, 0 }, HwLen = 6;
    IPAddr Gateway = 0, Source = 0;
    NET_LUID Luid;
    NEIGHBOR_ROW Row;
    DWORD Error;
    ULONG Index;

    pAdapters = (PFN_Adapters)GetProcAddress(Iphlpapi, "GetAdaptersInfo");
    pArp = (PFN_Arp)GetProcAddress(Iphlpapi, "SendARP");
    pToLuid = (PFN_ToLuid)GetProcAddress(Iphlpapi, "ConvertInterfaceIndexToLuid");
    if (!pAdapters || !pArp || !pToLuid)
    {
        skip("no adapter functions\n");
        return;
    }

    Error = pAdapters(NULL, &Size);
    if (Error != ERROR_BUFFER_OVERFLOW || !(Adapters = HeapAlloc(GetProcessHeap(), 0, Size)) ||
        pAdapters(Adapters, &Size) != NO_ERROR)
    {
        skip("no adapter information\n");
        HeapFree(GetProcessHeap(), 0, Adapters);
        return;
    }

    for (A = Adapters; A; A = A->Next)
    {
        Source = ParseIp(A->IpAddressList.IpAddress.String);
        Gateway = ParseIp(A->GatewayList.IpAddress.String);
        if (Source && Gateway && A->AddressLength == 6)
        {
            Found = A;
            break;
        }
    }
    if (!Found)
    {
        skip("no adapter with an address and a gateway\n");
        HeapFree(GetProcessHeap(), 0, Adapters);
        return;
    }

    Index = Found->Index;
    memset(&Luid, 0, sizeof(Luid));
    Error = pToLuid(Index, &Luid);
    ok(Error == NO_ERROR, "ConvertInterfaceIndexToLuid: %lu\n", Error);

    TestV6(Index);

    /* an address nobody has is not in the cache, and not on a bad interface either */
    V4(&Row, Ip(192, 0, 2, 99), Index, 0);
    Error = pEntry(&Row);
    ok(IsAbsent(Error, &Row), "GetIpNetEntry2(192.0.2.99): %lu, state %lu\n", Error, Row.State);
    if (Error == ERROR_NOT_FOUND)
        ok(Row.InterfaceLuid.Value == 0 && Row.PhysicalAddressLength == 0, "a failed lookup changed the row\n");

    V4(&Row, Gateway, 0xfff0, 0);
    Error = pEntry(&Row);
    ok(Error == ERROR_FILE_NOT_FOUND, "GetIpNetEntry2(unknown interface index): %lu\n", Error);

    V4(&Row, Gateway, Index, Luid.Value + 1);
    Error = pEntry(&Row);
    ok(Error == ERROR_NOT_FOUND, "GetIpNetEntry2(unknown luid): %lu\n", Error);

    /* the gateway, after an ARP exchange */
    Error = pArp(Gateway, Source, Hw, &HwLen);
    if (Error != NO_ERROR)
    {
        skip("SendARP to the gateway: %lu\n", Error);
        HeapFree(GetProcessHeap(), 0, Adapters);
        return;
    }

    V4(&Row, Gateway, Index, 0);
    Error = pEntry(&Row);
    ok(Error == NO_ERROR, "GetIpNetEntry2(gateway, index): %lu\n", Error);
    if (Error == NO_ERROR)
        CheckNeighbor("GetIpNetEntry2 by index", &Row, Index, &Luid, Hw);

    V4(&Row, Gateway, 0, Luid.Value);
    Error = pEntry(&Row);
    ok(Error == NO_ERROR, "GetIpNetEntry2(gateway, luid): %lu\n", Error);
    if (Error == NO_ERROR)
        CheckNeighbor("GetIpNetEntry2 by luid", &Row, Index, &Luid, Hw);

    V4(&Row, Gateway, 0, 0);
    Error = pEntry(&Row);
    ok(Error == NO_ERROR, "GetIpNetEntry2(gateway, no interface): %lu\n", Error);
    if (Error == NO_ERROR)
        CheckNeighbor("GetIpNetEntry2 any interface", &Row, Index, &Luid, Hw);

    V4(&Row, Gateway, Index, 0);
    Error = pResolve(&Row, NULL);
    ok(Error == NO_ERROR, "ResolveIpNetEntry2(gateway, index): %lu\n", Error);
    if (Error == NO_ERROR)
        CheckNeighbor("ResolveIpNetEntry2 by index", &Row, Index, &Luid, Hw);

    V4(&Row, Gateway, 0, Luid.Value);
    Error = pResolve(&Row, NULL);
    ok(Error == NO_ERROR, "ResolveIpNetEntry2(gateway, luid): %lu\n", Error);
    if (Error == NO_ERROR)
        CheckNeighbor("ResolveIpNetEntry2 by luid", &Row, Index, &Luid, Hw);

    V4(&Row, Gateway, 0, 0);
    Error = pResolve(&Row, NULL);
    ok(Error == NO_ERROR, "ResolveIpNetEntry2(gateway, no interface): %lu\n", Error);
    if (Error == NO_ERROR)
        CheckNeighbor("ResolveIpNetEntry2 any interface", &Row, Index, &Luid, Hw);

    /* the address of this computer is its own neighbor: Permanent, with its own link address */
    V4(&Row, Source, Index, 0);
    Error = pResolve(&Row, NULL);
    ok(Error == NO_ERROR, "ResolveIpNetEntry2(own address): %lu\n", Error);
    if (Error == NO_ERROR)
    {
        ok(Row.State == NlnsPermanent, "own address: state %lu\n", Row.State);
        ok(Row.PhysicalAddressLength == 6, "own address: link address length %lu\n", Row.PhysicalAddressLength);
        ok(!memcmp(Row.PhysicalAddress, Found->Address, 6), "own address: not the adapter's link address\n");
        ok(Row.InterfaceIndex == Index && Row.InterfaceLuid.Value == Luid.Value, "own address: wrong interface\n");
    }

    /* an on-link address nobody answers for: the ARP failure is ERROR_BAD_NET_NAME, and the row stays as it was */
    {
        IPAddr Mask = ParseIp(Found->IpAddressList.IpMask.String);
        IPAddr Host = (Source & Mask) | (Ip(0, 0, 0, 200) & ~Mask);

        if (Mask && Host != Source && Host != Gateway && Host != ((Source & Mask) | ~Mask))
        {
            V4(&Row, Host, Index, 0);
            Error = pResolve(&Row, NULL);
            ok(Error == ERROR_BAD_NET_NAME, "ResolveIpNetEntry2(absent neighbor): %lu\n", Error);
            ok(Row.InterfaceLuid.Value == 0 && Row.PhysicalAddressLength == 0, "a failed resolution changed the row\n");
        }
        else
            skip("no address for the absent neighbor\n");
    }

    HeapFree(GetProcessHeap(), 0, Adapters);
}

START_TEST(NeighborEntry2)
{
    HMODULE Iphlpapi = LoadLibraryW(L"iphlpapi.dll");

    ok(Iphlpapi != NULL, "iphlpapi.dll does not load\n");
    if (!Iphlpapi)
        return;

    pGet = (PFN_GetComp)GetProcAddress(Iphlpapi, "GetCurrentThreadCompartmentId");
    pSet = (PFN_SetComp)GetProcAddress(Iphlpapi, "SetCurrentThreadCompartmentId");
    pEntry = (PFN_Entry)GetProcAddress(Iphlpapi, "GetIpNetEntry2");
    pResolve = (PFN_Resolve)GetProcAddress(Iphlpapi, "ResolveIpNetEntry2");
    ok(pGet != NULL, "no GetCurrentThreadCompartmentId\n");
    ok(pSet != NULL, "no SetCurrentThreadCompartmentId\n");
    ok(pEntry != NULL, "no GetIpNetEntry2\n");
    ok(pResolve != NULL, "no ResolveIpNetEntry2\n");

    TestCompartment();
    if (!pEntry || !pResolve)
        return;

    TestParameters();
    TestLoopback();
    TestNeighbors();
}
