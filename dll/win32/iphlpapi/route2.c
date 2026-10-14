/*
 * PROJECT:     ReactOS IP Helper API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     GetBestRoute2
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "iphlpapi_private.h"
#include <wine/netioapi.h>

WINE_DEFAULT_DEBUG_CHANNEL(iphlpapi);

/*
 * The route metric Windows gives on-link and host routes. The legacy route table of the stack here holds route
 * and interface cost as one number and the interface cost is not available, so routes with a gateway report
 * that number as it is.
 */
#define ON_LINK_METRIC 256

static BOOL if_index_to_luid(DWORD Index, PULONG Type, NET_LUID *Luid)
{
  Luid->Value = 0;
  return getInterfaceLuidByIndex(Index, Type, &Luid->Value);
}

/*
 * The interface that owns an address of this computer, or with Broadcast also the interface whose
 * subnet the address is the broadcast address of. With Constrained the given interface is the only
 * candidate. Addresses are in network byte order.
 */
static BOOL find_local_address(IPAddr Dest, BOOL Broadcast, BOOL Constrained, DWORD ConstrainedIndex, PDWORD Index)
{
  InterfaceIndexTable *Table;
  BOOL Found = FALSE;
  DWORD i;

  if (!Dest || Dest == INADDR_NONE || !(Table = getInterfaceIndexTable()))
    return FALSE;

  for (i = 0; i < Table->numIndexes && !Found; i++)
  {
    IPAddr Address = getInterfaceIPAddrByIndex(Table->indexes[i]);
    IPAddr Mask = getInterfaceMaskByIndex(Table->indexes[i]);

    if (Constrained && Table->indexes[i] != ConstrainedIndex)
      continue;
    if (!Address || Address == INADDR_NONE)
      continue;
    if (Dest == Address || (Broadcast && Mask && Mask != INADDR_NONE && Dest == (Address | ~Mask)))
    {
      *Index = Table->indexes[i];
      Found = TRUE;
    }
  }
  free(Table);
  return Found;
}

/* The interface of a limited broadcast that names none: the first with an address, a real one before the loopback. */
static BOOL find_broadcast_interface(PDWORD Index)
{
  InterfaceIndexTable *Table = getInterfaceIndexTable();
  BOOL Found = FALSE;
  ULONG Type;
  NET_LUID Luid;
  DWORD i, Loopback = 0;
  BOOL HaveLoopback = FALSE;

  if (!Table)
    return FALSE;

  for (i = 0; i < Table->numIndexes && !Found; i++)
  {
    IPAddr Address = getInterfaceIPAddrByIndex(Table->indexes[i]);

    if (!Address || Address == INADDR_NONE || !if_index_to_luid(Table->indexes[i], &Type, &Luid))
      continue;
    if (Type == IF_TYPE_SOFTWARE_LOOPBACK)
    {
      Loopback = Table->indexes[i];
      HaveLoopback = TRUE;
    }
    else
    {
      *Index = Table->indexes[i];
      Found = TRUE;
    }
  }
  if (!Found && HaveLoopback)
  {
    *Index = Loopback;
    Found = TRUE;
  }
  free(Table);
  return Found;
}

static ULONG mask_prefix_length(ULONG Mask)
{
  ULONG Length = 0;

  for (Mask = ntohl(Mask); Mask & 0x80000000; Mask <<= 1)
    Length++;
  return Length;
}

static BOOL fill_best_route(PMIB_IPFORWARD_ROW2 Row, SOCKADDR_INET *BestSource, DWORD IfIndex, IPAddr Dest,
                            ULONG PrefixLength, IPAddr NextHop, ULONG Metric, BOOLEAN Loopback, IPAddr Source)
{
  NET_LUID Luid;

  if (!if_index_to_luid(IfIndex, NULL, &Luid))
    return FALSE;

  memset(Row, 0, sizeof(*Row));
  Row->InterfaceLuid = Luid;
  Row->InterfaceIndex = IfIndex;
  Row->DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
  Row->DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr = Dest;
  Row->DestinationPrefix.PrefixLength = PrefixLength;
  Row->NextHop.Ipv4.sin_family = AF_INET;
  Row->NextHop.Ipv4.sin_addr.s_addr = NextHop;
  /* the stack here keeps no route lifetime or age, so a route never expires and has no age */
  Row->ValidLifetime = Row->PreferredLifetime = 0xffffffff;
  Row->Metric = Metric;
  Row->Protocol = MIB_IPPROTO_NETMGMT;
  Row->Loopback = Loopback;
  Row->AutoconfigureAddress = TRUE;

  memset(BestSource, 0, sizeof(*BestSource));
  BestSource->Ipv4.sin_family = AF_INET;
  BestSource->Ipv4.sin_addr.s_addr = Source ? Source : getInterfaceIPAddrByIndex(IfIndex);
  return TRUE;
}

/******************************************************************
 *    GetBestRoute2 (IPHLPAPI.@)
 *
 * Find the route a packet to a destination takes, and the source address it carries.
 *
 * NOTES
 *  Only IPv4 is routed, as there is no IPv6 stack: every IPv6 destination has no route.
 *  The measured Windows behaviour is followed. A luid wins over an index, a zero luid
 *  or index is not a constraint, and a source address confines the route to the
 *  interface that owns it. A destination that is an address of this computer, or the
 *  broadcast address of one of its subnets, gets a host route over that interface.
 *  Windows also answers multicast and link-local destinations with an empty row and
 *  NO_ERROR; here they follow the routing table like any other address.
 */
DWORD WINAPI GetBestRoute2(NET_LUID *InterfaceLuid, NET_IFINDEX InterfaceIndex,
                           const SOCKADDR_INET *SourceAddress, const SOCKADDR_INET *DestinationAddress,
                           ULONG AddressSortOptions, PMIB_IPFORWARD_ROW2 BestRoute,
                           SOCKADDR_INET *BestSourceAddress)
{
  DWORD IfIndex = 0, SourceIf, OwnerIf, Matched = 0, i;
  ULONG Type = 0, MatchedLength = 0;
  IPAddr Dest, Source = 0;
  RouteTable *Table;
  BOOL Constrained = FALSE, Found = FALSE, Filled, Local;
  NET_LUID Luid;

  TRACE("(%p, %lu, %p, %p, 0x%lx, %p, %p)\n", InterfaceLuid, InterfaceIndex, SourceAddress,
        DestinationAddress, AddressSortOptions, BestRoute, BestSourceAddress);

  if (!DestinationAddress || !BestRoute || !BestSourceAddress)
    return ERROR_INVALID_PARAMETER;
  if (DestinationAddress->si_family != AF_INET && DestinationAddress->si_family != AF_INET6)
    return ERROR_INVALID_PARAMETER;

  if (DestinationAddress->si_family == AF_INET6)
  {
    static const IN6_ADDR any = { { { 0 } } };

    return memcmp(&DestinationAddress->Ipv6.sin6_addr, &any, sizeof(any)) ? ERROR_NETWORK_UNREACHABLE
                                                                           : ERROR_NOT_FOUND;
  }

  if (InterfaceLuid && InterfaceLuid->Value)
  {
    if (!getInterfaceIndexByLuid(InterfaceLuid->Value, &IfIndex))
      return ERROR_NOT_FOUND;
    Constrained = TRUE;
  }
  else if (InterfaceIndex)
  {
    IfIndex = InterfaceIndex;
    if (!if_index_to_luid(IfIndex, NULL, &Luid))
      return ERROR_FILE_NOT_FOUND;
    Constrained = TRUE;
  }

  if (SourceAddress && SourceAddress->si_family == AF_INET && SourceAddress->Ipv4.sin_addr.s_addr)
  {
    Source = SourceAddress->Ipv4.sin_addr.s_addr;
    if (!find_local_address(Source, FALSE, FALSE, 0, &SourceIf))
      return ERROR_NOT_FOUND;
    if (Constrained && SourceIf != IfIndex)
      return ERROR_NETWORK_UNREACHABLE;
    IfIndex = SourceIf;
    Constrained = TRUE;
  }

  Dest = DestinationAddress->Ipv4.sin_addr.s_addr;

  /* an address or broadcast of the interface asked for is found first, then any other's */
  Local = Constrained && find_local_address(Dest, TRUE, TRUE, IfIndex, &OwnerIf);
  if (!Local)
    Local = find_local_address(Dest, TRUE, FALSE, 0, &OwnerIf);
  if (!Local && Dest == INADDR_NONE)
  {
    /* the limited broadcast goes out over the interface asked for, else over the first one with an address */
    if (Constrained)
    {
      OwnerIf = IfIndex;
      Local = TRUE;
    }
    else
      Local = find_broadcast_interface(&OwnerIf);
  }

  if (Local)
  {
    if (Constrained && OwnerIf != IfIndex)
    {
      /* the 127.0.0.0/8 addresses exist on the loopback interface alone */
      if (if_index_to_luid(IfIndex, &Type, &Luid) && (Dest & 0xff) == 127 && Type != IF_TYPE_SOFTWARE_LOOPBACK)
        return ERROR_INVALID_PARAMETER;
      return ERROR_NETWORK_UNREACHABLE;
    }
    Filled = fill_best_route(BestRoute, BestSourceAddress, OwnerIf, Dest, 32, 0, ON_LINK_METRIC, TRUE,
                             Source ? Source : getInterfaceIPAddrByIndex(OwnerIf));
    return Filled ? NO_ERROR : ERROR_NOT_FOUND;
  }

  Table = getRouteTable();
  if (!Table)
    return ERROR_OUTOFMEMORY;

  /* the longest matching prefix wins, then the lowest metric */
  for (i = 0; i < Table->numRoutes; i++)
  {
    RouteEntry *Route = &Table->routes[i];
    ULONG Length = mask_prefix_length(Route->mask);

    if ((Dest & Route->mask) != (Route->dest & Route->mask))
      continue;
    if (Constrained && Route->ifIndex != IfIndex)
      continue;
    if (!Found || Length > MatchedLength || (Length == MatchedLength && Route->metric < Table->routes[Matched].metric))
    {
      Found = TRUE;
      Matched = i;
      MatchedLength = Length;
    }
  }

  Filled = FALSE;
  if (Found)
  {
    RouteEntry *Route = &Table->routes[Matched];
    IPAddr Own = getInterfaceIPAddrByIndex(Route->ifIndex);
    BOOL OnLink = !Route->gateway || Route->gateway == Own;

    /* a gateway that is the interface address, or none, means the destination is on the link */
    Filled = fill_best_route(BestRoute, BestSourceAddress, Route->ifIndex, Route->dest & Route->mask, MatchedLength,
                             OnLink ? 0 : Route->gateway, OnLink ? ON_LINK_METRIC : Route->metric, FALSE, Source);
  }
  HeapFree(GetProcessHeap(), 0, Table);

  if (!Found)
    return ERROR_NETWORK_UNREACHABLE;
  return Filled ? NO_ERROR : ERROR_NOT_FOUND;
}
