/*
 * PROJECT:     ReactOS IP Helper API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Interface luids
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "iphlpapi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(iphlpapi);

/*
 * The interface type in the top 16 bits, the interface index in the next 24 bits, zero below. The loopback
 * interface has no index in it: it is 0x0018000000000000 on Windows 7 and 10 whatever its index is.
 */
ULONG64 makeInterfaceLuid(DWORD Index, ULONG Type)
{
  ULONG64 Number = (Type == IF_TYPE_SOFTWARE_LOOPBACK) ? 0 : (Index & 0xffffff);

  return ((ULONG64)Type << 48) | (Number << 24);
}

BOOL getInterfaceLuidByIndex(DWORD Index, PULONG Type, PULONG64 Luid)
{
  MIB_IFROW Row;

  memset(&Row, 0, sizeof(Row));
  Row.dwIndex = Index;
  if (GetIfEntry(&Row) != NO_ERROR)
    return FALSE;

  *Luid = makeInterfaceLuid(Index, Row.dwType);
  if (Type)
    *Type = Row.dwType;
  return TRUE;
}

static BOOL findLoopbackIndex(PDWORD Index)
{
  PMIB_IFTABLE Table = NULL;
  ULONG Size = 0, i, Tries;
  DWORD Result = ERROR_INSUFFICIENT_BUFFER;
  BOOL Found = FALSE;

  for (Tries = 0; Tries < 3 && Result == ERROR_INSUFFICIENT_BUFFER; Tries++)
  {
    free(Table);
    Table = Size ? malloc(Size) : NULL;
    if (Size && !Table)
      return FALSE;
    Result = GetIfTable(Table, &Size, FALSE);
  }

  for (i = 0; Result == NO_ERROR && i < Table->dwNumEntries && !Found; i++)
  {
    if (Table->table[i].dwType == IF_TYPE_SOFTWARE_LOOPBACK)
    {
      *Index = Table->table[i].dwIndex;
      Found = TRUE;
    }
  }
  free(Table);
  return Found;
}

BOOL getInterfaceIndexByLuid(ULONG64 Luid, PDWORD Index)
{
  ULONG Type = (ULONG)(Luid >> 48);
  DWORD Number = (DWORD)((Luid >> 24) & 0xffffff);
  ULONG64 Check;

  if (!Luid || (Luid & 0xffffff))
    return FALSE;

  if (Type == IF_TYPE_SOFTWARE_LOOPBACK)
    return !Number && findLoopbackIndex(Index);

  if (!getInterfaceLuidByIndex(Number, NULL, &Check) || Check != Luid)
    return FALSE;
  *Index = Number;
  return TRUE;
}
