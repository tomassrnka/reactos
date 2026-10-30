/*
 * PROJECT:     ReactOS IP Helper API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     NotifyIpInterfaceChange, NotifyUnicastIpAddressChange, NotifyRouteChange2,
 *              CancelMibChangeNotify2
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "iphlpapi_private.h"
#include <wine/netioapi.h>

WINE_DEFAULT_DEBUG_CHANNEL(iphlpapi);

/*
 * Each registration has a worker thread. tcpip.sys completes a pending request when the IPv4 address table
 * (also an interface or a link coming or going) or the route table changes; the worker then takes a new
 * snapshot of the table, compares it with the last one and calls back for each row that was added, deleted
 * or changed. The request is queued again before the snapshot is taken, so a change in between is not lost.
 *
 * As on Windows 10, the initial notification is made by the caller's thread before the Notify function
 * returns, and CancelMibChangeNotify2 waits for a callback that is running. Windows deadlocks when a callback
 * cancels its own registration; here that call returns without waiting and the registration is freed when the
 * callback returns.
 */

enum { KIND_INTERFACE, KIND_UNICAST, KIND_ROUTE };

typedef VOID (WINAPI *MIB_CALLBACK)(PVOID Context, PVOID Row, MIB_NOTIFICATION_TYPE Type);

typedef struct _MIB_NOTIFY
{
  struct _MIB_NOTIFY *Next;
  int Kind;
  MIB_CALLBACK Callback;
  PVOID Context;
  HANDLE Thread;
  HANDLE Stop;
  HANDLE File;
  HANDLE IoEvent;
  DWORD ThreadId;
  HMODULE Module;               /* this DLL, held for the worker */
  BOOL Detached;                /* a callback cancelled its own registration: the worker frees it */
  PVOID Rows;                   /* the last snapshot */
  ULONG Count;
} MIB_NOTIFY;

static MIB_NOTIFY *NotifyHead;
static CRITICAL_SECTION RegistrationLock;

void NotifyRegistrationInit(void)
{
  InitializeCriticalSection(&RegistrationLock);
}

void NotifyRegistrationFree(void)
{
  DeleteCriticalSection(&RegistrationLock);
}

/* ---- the tables ---- */

typedef struct _KIND_OPS
{
  SIZE_T RowSize;
  DWORD (*Snapshot)(PVOID *Rows, ULONG *Count);
  BOOL (*SameKey)(const void *A, const void *B);
  BOOL (*SameData)(const void *A, const void *B);
  ULONG IoControl;
} KIND_OPS;

static DWORD snapshot_interfaces(PVOID *Rows, ULONG *Count)
{
  PMIB_IPINTERFACE_TABLE Table;
  DWORD Result = GetIpInterfaceTable(AF_INET, &Table);

  if (Result != NO_ERROR)
    return Result;
  *Count = Table->NumEntries;
  *Rows = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)(Table->NumEntries + 1) * sizeof(MIB_IPINTERFACE_ROW));
  if (*Rows)
    memcpy(*Rows, Table->Table, (SIZE_T)Table->NumEntries * sizeof(MIB_IPINTERFACE_ROW));
  FreeMibTable(Table);
  return *Rows ? NO_ERROR : ERROR_NOT_ENOUGH_MEMORY;
}

static BOOL same_interface_key(const void *A, const void *B)
{
  const MIB_IPINTERFACE_ROW *X = A, *Y = B;
  return X->Family == Y->Family && X->InterfaceLuid.Value == Y->InterfaceLuid.Value;
}

static BOOL same_interface_data(const void *A, const void *B)
{
  return !memcmp(A, B, sizeof(MIB_IPINTERFACE_ROW));
}

static DWORD snapshot_unicast(PVOID *Rows, ULONG *Count)
{
  PMIB_UNICASTIPADDRESS_TABLE Table;
  DWORD Result = GetUnicastIpAddressTable(AF_INET, &Table);

  if (Result != NO_ERROR)
    return Result;
  *Count = Table->NumEntries;
  *Rows = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)(Table->NumEntries + 1) * sizeof(MIB_UNICASTIPADDRESS_ROW));
  if (*Rows)
    memcpy(*Rows, Table->Table, (SIZE_T)Table->NumEntries * sizeof(MIB_UNICASTIPADDRESS_ROW));
  FreeMibTable(Table);
  return *Rows ? NO_ERROR : ERROR_NOT_ENOUGH_MEMORY;
}

static BOOL same_unicast_key(const void *A, const void *B)
{
  const MIB_UNICASTIPADDRESS_ROW *X = A, *Y = B;
  return X->InterfaceLuid.Value == Y->InterfaceLuid.Value &&
         X->Address.Ipv4.sin_addr.s_addr == Y->Address.Ipv4.sin_addr.s_addr;
}

/* The lifetimes of a lease and the time stamp are not compared: they change with the clock. */
static BOOL same_unicast_data(const void *A, const void *B)
{
  const MIB_UNICASTIPADDRESS_ROW *X = A, *Y = B;
  return X->PrefixOrigin == Y->PrefixOrigin && X->SuffixOrigin == Y->SuffixOrigin &&
         X->OnLinkPrefixLength == Y->OnLinkPrefixLength && X->SkipAsSource == Y->SkipAsSource &&
         X->DadState == Y->DadState && X->InterfaceIndex == Y->InterfaceIndex;
}

static UINT8 mask_length(DWORD Mask)
{
  DWORD Host = ntohl(Mask);
  UINT8 Length = 0;

  for (; Host & 0x80000000UL; Host <<= 1)
    Length++;
  return Length;
}

static DWORD snapshot_routes(PVOID *Rows, ULONG *Count)
{
  PMIB_IPFORWARDTABLE Table = NULL;
  PMIB_IPFORWARD_ROW2 Out;
  ULONG Size = 0, Tries, i;
  DWORD Result = ERROR_INSUFFICIENT_BUFFER;
  DWORD LastIndex = 0xffffffff;
  ULONG64 LastLuid = 0;

  for (Tries = 0; Tries < 3 && Result == ERROR_INSUFFICIENT_BUFFER; Tries++)
  {
    HeapFree(GetProcessHeap(), 0, Table);
    Table = Size ? HeapAlloc(GetProcessHeap(), 0, Size) : NULL;
    if (Size && !Table)
      return ERROR_NOT_ENOUGH_MEMORY;
    Result = GetIpForwardTable(Table, &Size, FALSE);
  }
  if (Result == ERROR_NO_DATA)
  {
    /* no routes at all */
    HeapFree(GetProcessHeap(), 0, Table);
    *Count = 0;
    *Rows = HeapAlloc(GetProcessHeap(), 0, sizeof(*Out));
    return *Rows ? NO_ERROR : ERROR_NOT_ENOUGH_MEMORY;
  }
  if (Result != NO_ERROR)
  {
    HeapFree(GetProcessHeap(), 0, Table);
    return Result;
  }

  Out = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)(Table->dwNumEntries + 1) * sizeof(*Out));
  if (!Out)
  {
    HeapFree(GetProcessHeap(), 0, Table);
    return ERROR_NOT_ENOUGH_MEMORY;
  }
  for (i = 0; i < Table->dwNumEntries; i++)
  {
    const MIB_IPFORWARDROW *Row = &Table->table[i];
    PMIB_IPFORWARD_ROW2 New = &Out[i];

    if (Row->dwForwardIfIndex != LastIndex)
    {
      LastIndex = Row->dwForwardIfIndex;
      if (!getInterfaceLuidByIndex(LastIndex, NULL, &LastLuid))
        LastLuid = 0;
    }
    New->InterfaceLuid.Value = LastLuid;
    New->InterfaceIndex = Row->dwForwardIfIndex;
    New->DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
    New->DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr = Row->dwForwardDest;
    New->DestinationPrefix.PrefixLength = mask_length(Row->dwForwardMask);
    New->NextHop.Ipv4.sin_family = AF_INET;
    New->NextHop.Ipv4.sin_addr.s_addr = Row->dwForwardNextHop;
    New->ValidLifetime = 0xffffffff;
    New->PreferredLifetime = 0xffffffff;
    New->Metric = Row->dwForwardMetric1;
    New->Protocol = Row->dwForwardProto;
    New->Age = Row->dwForwardAge;
  }
  *Count = Table->dwNumEntries;
  *Rows = Out;
  HeapFree(GetProcessHeap(), 0, Table);
  return NO_ERROR;
}

static BOOL same_route_key(const void *A, const void *B)
{
  const MIB_IPFORWARD_ROW2 *X = A, *Y = B;
  return X->InterfaceIndex == Y->InterfaceIndex &&
         X->DestinationPrefix.PrefixLength == Y->DestinationPrefix.PrefixLength &&
         X->DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr == Y->DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr &&
         X->NextHop.Ipv4.sin_addr.s_addr == Y->NextHop.Ipv4.sin_addr.s_addr &&
         X->Metric == Y->Metric;
}

/* A route whose metric changes is a deleted route and an added one: the metric is part of the key. */
static BOOL same_route_data(const void *A, const void *B)
{
  const MIB_IPFORWARD_ROW2 *X = A, *Y = B;
  return X->Protocol == Y->Protocol;
}

static const KIND_OPS Kinds[] =
{
  { sizeof(MIB_IPINTERFACE_ROW), snapshot_interfaces, same_interface_key, same_interface_data, IOCTL_IP_ADDCHANGE_NOTIFY_REQUEST },
  { sizeof(MIB_UNICASTIPADDRESS_ROW), snapshot_unicast, same_unicast_key, same_unicast_data, IOCTL_IP_ADDCHANGE_NOTIFY_REQUEST },
  { sizeof(MIB_IPFORWARD_ROW2), snapshot_routes, same_route_key, same_route_data, IOCTL_IP_ROUTECHANGE_NOTIFY_REQUEST },
};

/* ---- the worker ---- */

static BOOL is_stopping(MIB_NOTIFY *Notify)
{
  return WaitForSingleObject(Notify->Stop, 0) == WAIT_OBJECT_0;
}

/* Calls back for what differs between the last snapshot and a new one, and keeps the new one. */
static void report_changes(MIB_NOTIFY *Notify)
{
  const KIND_OPS *Ops = &Kinds[Notify->Kind];
  PVOID NewRows = NULL;
  ULONG NewCount = 0, i, j;
  BYTE *Old, *New;

  if (Ops->Snapshot(&NewRows, &NewCount) != NO_ERROR)
    return;

  Old = Notify->Rows;
  New = NewRows;

  /* deleted rows first, then the new and the changed ones */
  for (i = 0; i < Notify->Count && !is_stopping(Notify); i++)
  {
    for (j = 0; j < NewCount; j++)
      if (Ops->SameKey(Old + i * Ops->RowSize, New + j * Ops->RowSize))
        break;
    if (j == NewCount)
      Notify->Callback(Notify->Context, Old + i * Ops->RowSize, MibDeleteInstance);
  }
  for (j = 0; j < NewCount && !is_stopping(Notify); j++)
  {
    for (i = 0; i < Notify->Count; i++)
      if (Ops->SameKey(Old + i * Ops->RowSize, New + j * Ops->RowSize))
        break;
    if (i == Notify->Count)
      Notify->Callback(Notify->Context, New + j * Ops->RowSize, MibAddInstance);
    else if (!Ops->SameData(Old + i * Ops->RowSize, New + j * Ops->RowSize))
      Notify->Callback(Notify->Context, New + j * Ops->RowSize, MibParameterNotification);
  }

  HeapFree(GetProcessHeap(), 0, Notify->Rows);
  Notify->Rows = NewRows;
  Notify->Count = NewCount;
}

static void free_notify(MIB_NOTIFY *Notify)
{
  if (Notify->File)
    NtClose(Notify->File);
  if (Notify->IoEvent)
    CloseHandle(Notify->IoEvent);
  if (Notify->Stop)
    CloseHandle(Notify->Stop);
  if (Notify->Thread)
    CloseHandle(Notify->Thread);
  HeapFree(GetProcessHeap(), 0, Notify->Rows);
  HeapFree(GetProcessHeap(), 0, Notify);
}

static void notify_worker_main(MIB_NOTIFY *Notify)
{
  const KIND_OPS *Ops = &Kinds[Notify->Kind];
  IO_STATUS_BLOCK Iosb;
  NTSTATUS Status;
  HANDLE Wait[2];
  BOOL Detached;

  Wait[0] = Notify->IoEvent;
  Wait[1] = Notify->Stop;

  while (!is_stopping(Notify))
  {
    /* queue the request first: a change after it is queued completes it, however soon the snapshot is taken */
    ResetEvent(Notify->IoEvent);
    Iosb.Status = STATUS_PENDING;
    Status = NtDeviceIoControlFile(Notify->File, Notify->IoEvent, NULL, NULL, &Iosb,
                                   Ops->IoControl, NULL, 0, NULL, 0);
    if (Status == STATUS_NOT_IMPLEMENTED || Status == STATUS_NOT_SUPPORTED || Status == STATUS_INVALID_DEVICE_REQUEST)
    {
      /* an older tcpip.sys without this request: there is nothing to wait for, so do not poll */
      WaitForSingleObject(Notify->Stop, INFINITE);
      break;
    }
    if (!NT_SUCCESS(Status))
    {
      /* nothing to wait on: look again in a second, or leave when stopped */
      if (WaitForSingleObject(Notify->Stop, 1000) == WAIT_OBJECT_0)
        break;
      report_changes(Notify);
      continue;
    }

    report_changes(Notify);

    if (WaitForMultipleObjects(2, Wait, FALSE, INFINITE) != WAIT_OBJECT_0)
    {
      /* stopped with the request still queued: complete it before Iosb goes away */
      NtDeviceIoControlFile(Notify->File, NULL, NULL, NULL, &Iosb,
                            IOCTL_IP_ADDCHANGE_CANCEL_REQUEST, NULL, 0, NULL, 0);
      WaitForSingleObject(Notify->IoEvent, INFINITE);
      break;
    }
  }

  EnterCriticalSection(&RegistrationLock);
  Detached = Notify->Detached;
  LeaveCriticalSection(&RegistrationLock);
  if (Detached)
    free_notify(Notify);
}

/*
 * The worker holds a reference of this DLL until it is done, and leaves through FreeLibraryAndExitThread: a
 * FreeLibrary of the application cannot unmap the code of a worker that still runs. A registration that is not
 * cancelled therefore keeps the DLL loaded.
 */
static DWORD WINAPI notify_worker(PVOID Parameter)
{
  HMODULE Module = ((MIB_NOTIFY *)Parameter)->Module;

  notify_worker_main(Parameter);
  FreeLibraryAndExitThread(Module, 0);
  return 0;
}

static VOID WINAPI no_callback(PVOID Context, PVOID Row, MIB_NOTIFICATION_TYPE Type)
{
}

static DWORD register_notify(int Kind, ADDRESS_FAMILY Family, PVOID Callback, PVOID Context,
                             BOOLEAN InitialNotification, HANDLE *NotificationHandle)
{
  MIB_NOTIFY *Notify;
  UNICODE_STRING Name;
  OBJECT_ATTRIBUTES Attrs;
  IO_STATUS_BLOCK Iosb;
  NTSTATUS Status;
  DWORD Result;

  if ((Family != AF_UNSPEC && Family != AF_INET && Family != AF_INET6) || !NotificationHandle)
    return ERROR_INVALID_PARAMETER;

  Notify = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Notify));
  if (!Notify)
    return ERROR_NOT_ENOUGH_MEMORY;
  Notify->Kind = Kind;
  /* Windows accepts no callback and faults on the first change; this never calls one */
  Notify->Callback = Callback ? Callback : no_callback;
  Notify->Context = Context;

  /* only IPv4 exists: an IPv6 registration never sees a change */
  if (Family != AF_INET6)
  {
    Notify->Stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    Notify->IoEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    RtlInitUnicodeString(&Name, L"\\Device\\Tcp");
    InitializeObjectAttributes(&Attrs, &Name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = NtOpenFile(&Notify->File, FILE_READ_DATA | SYNCHRONIZE, &Attrs, &Iosb,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
    if (!Notify->Stop || !Notify->IoEvent || !NT_SUCCESS(Status))
    {
      Result = NT_SUCCESS(Status) ? ERROR_NOT_ENOUGH_MEMORY : RtlNtStatusToDosError(Status);
      if (!NT_SUCCESS(Status))
        Notify->File = NULL;
      free_notify(Notify);
      return Result;
    }
    Result = Kinds[Kind].Snapshot(&Notify->Rows, &Notify->Count);
    if (Result != NO_ERROR)
    {
      free_notify(Notify);
      return Result;
    }
  }

  /* the initial notification comes from this thread, before the call returns */
  if (InitialNotification)
    Notify->Callback(Context, NULL, MibInitialNotification);

  if (Notify->File)
  {
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)notify_worker, &Notify->Module))
    {
      Result = GetLastError();
      free_notify(Notify);
      return Result;
    }
    Notify->Thread = CreateThread(NULL, 0, notify_worker, Notify, CREATE_SUSPENDED, &Notify->ThreadId);
    if (!Notify->Thread)
    {
      Result = GetLastError();
      FreeLibrary(Notify->Module);
      free_notify(Notify);
      return Result;
    }
  }

  EnterCriticalSection(&RegistrationLock);
  Notify->Next = NotifyHead;
  NotifyHead = Notify;
  LeaveCriticalSection(&RegistrationLock);

  if (Notify->Thread)
    ResumeThread(Notify->Thread);

  *NotificationHandle = Notify;
  return NO_ERROR;
}

DWORD WINAPI NotifyIpInterfaceChange(ADDRESS_FAMILY Family, PVOID Callback, PVOID CallerContext,
                                     BOOLEAN InitialNotification, HANDLE *NotificationHandle)
{
  return register_notify(KIND_INTERFACE, Family, Callback, CallerContext, InitialNotification, NotificationHandle);
}

DWORD WINAPI NotifyUnicastIpAddressChange(ADDRESS_FAMILY Family, PVOID Callback, PVOID CallerContext,
                                          BOOLEAN InitialNotification, HANDLE *NotificationHandle)
{
  return register_notify(KIND_UNICAST, Family, Callback, CallerContext, InitialNotification, NotificationHandle);
}

/*
 * Returns the unicast address table once it is stable, or ERROR_IO_PENDING and the table to the callback
 * when it is not (an address still being configured). Addresses here have no tentative state: the table is
 * always stable, so it is returned at once, with no notification handle and no callback.
 */
DWORD WINAPI NotifyStableUnicastIpAddressTable(ADDRESS_FAMILY Family, PMIB_UNICASTIPADDRESS_TABLE *Table,
                                               PVOID Callback, PVOID CallerContext, HANDLE *NotificationHandle)
{
  if (!Table || !NotificationHandle)
    return ERROR_INVALID_PARAMETER;

  *NotificationHandle = NULL;
  return GetUnicastIpAddressTable(Family, Table);
}

DWORD WINAPI NotifyRouteChange2(ADDRESS_FAMILY Family, PVOID Callback, PVOID CallerContext,
                                BOOLEAN InitialNotification, HANDLE *NotificationHandle)
{
  return register_notify(KIND_ROUTE, Family, Callback, CallerContext, InitialNotification, NotificationHandle);
}

/*
 * Waits for a callback that is running, so none runs after this returns. A callback that cancels its own
 * registration does not wait for itself.
 */
DWORD WINAPI CancelMibChangeNotify2(HANDLE NotificationHandle)
{
  MIB_NOTIFY *Notify, **Link;
  BOOL Self = FALSE;

  /* Windows answers ERROR_INVALID_HANDLE for no handle; a handle that was cancelled is unknown as well */
  if (!NotificationHandle)
    return ERROR_INVALID_HANDLE;

  EnterCriticalSection(&RegistrationLock);
  for (Link = &NotifyHead; *Link && *Link != NotificationHandle; Link = &(*Link)->Next)
    ;
  Notify = *Link;
  if (Notify)
  {
    *Link = Notify->Next;
    if (Notify->Thread && Notify->ThreadId == GetCurrentThreadId())
    {
      Notify->Detached = TRUE;
      Self = TRUE;
    }
  }
  LeaveCriticalSection(&RegistrationLock);

  if (!Notify)
    return ERROR_INVALID_HANDLE;

  if (Notify->Stop)
    SetEvent(Notify->Stop);
  if (Self)
    return NO_ERROR;

  if (Notify->Thread)
    WaitForSingleObject(Notify->Thread, INFINITE);
  free_notify(Notify);
  return NO_ERROR;
}
