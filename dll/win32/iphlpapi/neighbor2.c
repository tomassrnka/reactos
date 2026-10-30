/*
 * PROJECT:     ReactOS IP Helper API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     GetIpNetEntry2, ResolveIpNetEntry2 and the thread compartment calls
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "iphlpapi_private.h"
#include <wine/netioapi.h>

WINE_DEFAULT_DEBUG_CHANNEL(iphlpapi);

/* The compartment every interface lives in; the stack here has no other. */
#define PRIMARY_COMPARTMENT 1

#define LOOPBACK_LUID 0x0018000000000000ULL

/*
 * The measured Windows 10 behaviour: a thread starts in compartment 1, and compartment 1 and 0 are accepted
 * by the setter (0 leaves the thread where it is), any other id does not exist.
 */
NET_IF_COMPARTMENT_ID WINAPI GetCurrentThreadCompartmentId(void)
{
  return PRIMARY_COMPARTMENT;
}

DWORD WINAPI SetCurrentThreadCompartmentId(NET_IF_COMPARTMENT_ID CompartmentId)
{
  TRACE("(%lu)\n", CompartmentId);

  if (CompartmentId == PRIMARY_COMPARTMENT || CompartmentId == 0)
    return NO_ERROR;
  return ERROR_NOT_FOUND;
}

/*
 * The interface a row names. A luid wins over an index, a zero luid or index is not a constraint; an unknown luid
 * is ERROR_NOT_FOUND and an unknown index ERROR_FILE_NOT_FOUND, as in GetBestRoute2. *Constrained says whether
 * the row named one.
 */
static DWORD row_interface(const MIB_IPNET_ROW2 *Row, PDWORD Index, PBOOL Constrained)
{
  ULONG64 Luid;

  *Index = 0;
  *Constrained = FALSE;
  if (Row->InterfaceLuid.Value)
  {
    if (!getInterfaceIndexByLuid(Row->InterfaceLuid.Value, Index))
      return ERROR_NOT_FOUND;
    *Constrained = TRUE;
  }
  else if (Row->InterfaceIndex)
  {
    if (!getInterfaceLuidByIndex(Row->InterfaceIndex, NULL, &Luid))
      return ERROR_FILE_NOT_FOUND;
    *Index = Row->InterfaceIndex;
    *Constrained = TRUE;
  }
  return NO_ERROR;
}

static BOOL set_interface(PMIB_IPNET_ROW2 Row, DWORD Index)
{
  ULONG64 Luid;

  if (!getInterfaceLuidByIndex(Index, NULL, &Luid))
    return FALSE;
  Row->InterfaceIndex = Index;
  Row->InterfaceLuid.Value = Luid;
  return TRUE;
}

static void set_neighbor(PMIB_IPNET_ROW2 Row, NL_NEIGHBOR_STATE State, const BYTE *Address, ULONG Length)
{
  memset(Row->PhysicalAddress, 0, sizeof(Row->PhysicalAddress));
  if (Length > sizeof(Row->PhysicalAddress))
    Length = sizeof(Row->PhysicalAddress);
  memcpy(Row->PhysicalAddress, Address, Length);
  Row->PhysicalAddressLength = Length;
  Row->State = State;
  Row->Flags = 0;
  Row->ReachabilityTime.LastReachable = (State == NlnsPermanent) ? 0 : GetTickCount();
}

static BOOL valid_family(const MIB_IPNET_ROW2 *Row)
{
  return Row->Address.si_family == AF_INET || Row->Address.si_family == AF_INET6;
}

static BOOL is_v6_loopback(const MIB_IPNET_ROW2 *Row)
{
  static const IN6_ADDR Loopback = { { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } } };

  return Row->Address.si_family == AF_INET6 && !memcmp(&Row->Address.Ipv6.sin6_addr, &Loopback, sizeof(Loopback));
}

/******************************************************************
 *    GetIpNetEntry2 (IPHLPAPI.@)
 *
 * Look up one neighbor in the cache.
 *
 * NOTES
 *  Only IPv4 neighbors exist, as there is no IPv6 stack: an IPv6 address is not found. The cache is the ARP
 *  table; a dynamic entry is reported Reachable and a static one Permanent. The Windows entries that need no
 *  ARP exchange (the limited broadcast, Permanent with the all-ones address) are not reported. The row is
 *  left alone when nothing is found; without an interface in it, the interface of the entry is filled in.
 */
DWORD WINAPI GetIpNetEntry2(PMIB_IPNET_ROW2 Row)
{
  PMIB_IPNETTABLE Table = NULL;
  ULONG Size = 0, Tries;
  DWORD Error = ERROR_INSUFFICIENT_BUFFER, Index, i;
  BOOL Constrained;

  TRACE("(%p)\n", Row);

  if (!Row || !valid_family(Row))
    return ERROR_INVALID_PARAMETER;

  Error = row_interface(Row, &Index, &Constrained);
  if (Error != NO_ERROR)
    return Error;

  if (Row->Address.si_family != AF_INET)
    return ERROR_NOT_FOUND;

  Error = ERROR_INSUFFICIENT_BUFFER;
  for (Tries = 0; Tries < 5 && Error == ERROR_INSUFFICIENT_BUFFER; Tries++)
  {
    HeapFree(GetProcessHeap(), 0, Table);
    Table = Size ? HeapAlloc(GetProcessHeap(), 0, Size) : NULL;
    if (Size && !Table)
      return ERROR_OUTOFMEMORY;
    Error = GetIpNetTable(Table, &Size, FALSE);
  }
  if (Error != NO_ERROR)
  {
    HeapFree(GetProcessHeap(), 0, Table);
    return Error == ERROR_NO_DATA ? ERROR_NOT_FOUND : Error;
  }

  Error = ERROR_NOT_FOUND;
  for (i = 0; i < Table->dwNumEntries; i++)
  {
    PMIB_IPNETROW Entry = &Table->table[i];

    if (Entry->dwAddr != Row->Address.Ipv4.sin_addr.s_addr)
      continue;
    if (Constrained && Entry->dwIndex != Index)
      continue;
    if (Entry->dwType != MIB_IPNET_TYPE_DYNAMIC && Entry->dwType != MIB_IPNET_TYPE_STATIC)
      continue;
    if (!set_interface(Row, Entry->dwIndex))
      continue;

    set_neighbor(Row, Entry->dwType == MIB_IPNET_TYPE_STATIC ? NlnsPermanent : NlnsReachable, Entry->bPhysAddr,
                 Entry->dwPhysAddrLen);
    Error = NO_ERROR;
    break;
  }
  HeapFree(GetProcessHeap(), 0, Table);
  return Error;
}

/* The interface that owns an IPv4 address of this computer. */
static BOOL find_owner(IPAddr Address, PDWORD Index)
{
  InterfaceIndexTable *Table = getInterfaceIndexTable();
  BOOL Found = FALSE;
  DWORD i;

  if (!Table)
    return FALSE;
  for (i = 0; i < Table->numIndexes && !Found; i++)
  {
    if (getInterfaceIPAddrByIndex(Table->indexes[i]) == Address)
    {
      *Index = Table->indexes[i];
      Found = TRUE;
    }
  }
  free(Table);
  return Found;
}

/******************************************************************
 *    ResolveIpNetEntry2 (IPHLPAPI.@)
 *
 * Resolve a neighbor, sending an ARP request when it is not cached.
 *
 * NOTES
 *  The measured Windows behaviour is followed for the cases below. A loopback address (or ::1) resolves on the
 *  loopback interface, Permanent and without a link address; an address of this computer resolves to the link
 *  address of its interface, Permanent. The ARP failure is ERROR_BAD_NET_NAME, as is any IPv6 address but ::1
 *  (there is no IPv6 stack). 0.0.0.0 is ERROR_INVALID_PARAMETER. Windows also answers the limited broadcast
 *  and multicast addresses with the link address of the interface; here they are ARPed for and fail. A
 *  SourceAddress that is not an address of the interface is ignored. Without an interface in the row the
 *  route to the address picks one; an address without a route is ERROR_BAD_NET_NAME.
 */
DWORD WINAPI ResolveIpNetEntry2(PMIB_IPNET_ROW2 Row, const SOCKADDR_INET *SourceAddress)
{
  DWORD Error, Index, Mode, Own = 0;
  BOOL Constrained;
  IPAddr Dest, Source = 0;
  ULONG Hw[2], HwLen;
  BYTE Link[MAX_INTERFACE_PHYSADDR];

  TRACE("(%p, %p)\n", Row, SourceAddress);

  if (!Row || !valid_family(Row))
    return ERROR_INVALID_PARAMETER;

  Error = row_interface(Row, &Index, &Constrained);
  if (Error != NO_ERROR)
    return Error;

  if (Row->Address.si_family == AF_INET6)
  {
    if (!is_v6_loopback(Row) || !getInterfaceIndexByLuid(LOOPBACK_LUID, &Index) || !set_interface(Row, Index))
      return ERROR_BAD_NET_NAME;
    set_neighbor(Row, NlnsPermanent, NULL, 0);
    return NO_ERROR;
  }

  Dest = Row->Address.Ipv4.sin_addr.s_addr;
  if (!Dest)
    return ERROR_INVALID_PARAMETER;

  /* 127.0.0.0/8 lives on the loopback interface alone */
  if ((Dest & 0xff) == 127)
  {
    if (!getInterfaceIndexByLuid(LOOPBACK_LUID, &Index) || !set_interface(Row, Index))
      return ERROR_BAD_NET_NAME;
    set_neighbor(Row, NlnsPermanent, NULL, 0);
    return NO_ERROR;
  }

  /* an address of this computer is its own neighbor */
  if (find_owner(Dest, &Own) && (!Constrained || Own == Index))
  {
    ULONG Length = sizeof(Link);

    if (getInterfacePhysicalByIndex(Own, &Length, Link, &Mode) == NO_ERROR && set_interface(Row, Own))
    {
      set_neighbor(Row, NlnsPermanent, Link, Length);
      return NO_ERROR;
    }
  }

  if (!Constrained)
  {
    SOCKADDR_INET Best, BestSource;
    MIB_IPFORWARD_ROW2 Route;

    memset(&Best, 0, sizeof(Best));
    Best.Ipv4.sin_family = AF_INET;
    Best.Ipv4.sin_addr.s_addr = Dest;
    if (GetBestRoute2(NULL, 0, NULL, &Best, 0, &Route, &BestSource) != NO_ERROR)
      return ERROR_BAD_NET_NAME;
    Index = Route.InterfaceIndex;
  }

  /* the ARP request leaves with an address of the interface; one the caller names must be it */
  Source = getInterfaceIPAddrByIndex(Index);
  if (SourceAddress && SourceAddress->si_family == AF_INET && SourceAddress->Ipv4.sin_addr.s_addr &&
      SourceAddress->Ipv4.sin_addr.s_addr == Source)
    Source = SourceAddress->Ipv4.sin_addr.s_addr;
  if (!Source || Source == INADDR_NONE)
    return ERROR_BAD_NET_NAME;

  memset(Hw, 0, sizeof(Hw));
  HwLen = 6;
  if (SendARP(Dest, Source, Hw, &HwLen) != NO_ERROR || !HwLen || HwLen > sizeof(Hw) || !set_interface(Row, Index))
    return ERROR_BAD_NET_NAME;

  set_neighbor(Row, NlnsReachable, (const BYTE *)Hw, HwLen);
  return NO_ERROR;
}
