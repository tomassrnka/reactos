/*
* Copyright 2026 Alistair Leslie-Hughes
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winsock2.h"

#include "ndfapi.h"

#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(ndfapi);

/*
 * There is no diagnostics service behind this library. An incident can be created, kept and
 * closed, with the argument checks that Windows makes, but it cannot be diagnosed.
 */

/* the error that Windows gives for a missing pointer or string, which is not E_POINTER */
#define NDF_E_BAD_PARAM ((HRESULT)0x8008f905)

struct incident
{
    struct list entry;
};

static CRITICAL_SECTION incident_cs;
static CRITICAL_SECTION_DEBUG incident_cs_debug =
{
    0, 0, &incident_cs,
    { &incident_cs_debug.ProcessLocksList, &incident_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": incident_cs") }
};
static CRITICAL_SECTION incident_cs = { &incident_cs_debug, -1, 0, 0, 0, 0 };

static struct list incident_list = LIST_INIT(incident_list);

static HRESULT create_incident(NDFHANDLE *handle)
{
    struct incident *incident = calloc(1, sizeof(*incident));

    if (!incident) return E_OUTOFMEMORY;

    EnterCriticalSection(&incident_cs);
    list_add_head(&incident_list, &incident->entry);
    LeaveCriticalSection(&incident_cs);

    *handle = incident;
    return S_OK;
}

static BOOL is_incident(NDFHANDLE handle)
{
    struct incident *incident;
    BOOL found = FALSE;

    EnterCriticalSection(&incident_cs);
    LIST_FOR_EACH_ENTRY(incident, &incident_list, struct incident, entry)
    {
        if (incident == handle)
        {
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&incident_cs);
    return found;
}

STDAPI NdfCloseIncident(NDFHANDLE handle)
{
    struct incident *incident, *found = NULL;

    TRACE("%p\n", handle);

    if (!handle) return S_OK;

    EnterCriticalSection(&incident_cs);
    LIST_FOR_EACH_ENTRY(incident, &incident_list, struct incident, entry)
    {
        if (incident == handle)
        {
            list_remove(&incident->entry);
            found = incident;
            break;
        }
    }
    LeaveCriticalSection(&incident_cs);

    if (!found) return E_HANDLE;
    free(found);
    return S_OK;
}

STDAPI NdfCancelIncident(NDFHANDLE handle)
{
    FIXME("%p\n", handle);

    if (!is_incident(handle)) return E_HANDLE;

    /* only a diagnosis that runs can be cancelled, and none does */
    return E_INVALIDARG;
}

STDAPI NdfGetTraceFile(NDFHANDLE handle, LPCWSTR *location)
{
    FIXME("%p, %p\n", handle, location);

    if (!is_incident(handle)) return E_HANDLE;

    /* a trace file belongs to a diagnosis that has run, and none has */
    return E_INVALIDARG;
}

STDAPI NdfCreateConnectivityIncident(NDFHANDLE *handle)
{
    TRACE("%p\n", handle);

    if (!handle) return NDF_E_BAD_PARAM;
    return create_incident(handle);
}

STDAPI NdfCreateDNSIncident(LPCWSTR hostname, WORD query_type, NDFHANDLE *handle)
{
    TRACE("%s, %u, %p\n", debugstr_w(hostname), query_type, handle);

    if (!hostname || !handle) return NDF_E_BAD_PARAM;
    return create_incident(handle);
}

STDAPI NdfCreateSharingIncident(LPCWSTR path, NDFHANDLE *handle)
{
    TRACE("%s, %p\n", debugstr_w(path), handle);

    if (!handle) return NDF_E_BAD_PARAM;
    return create_incident(handle);
}

STDAPI NdfCreateWebIncident(LPCWSTR url, NDFHANDLE *handle)
{
    TRACE("%s, %p\n", debugstr_w(url), handle);

    if (!url || !handle) return NDF_E_BAD_PARAM;
    return create_incident(handle);
}

STDAPI NdfCreateWebIncidentEx(LPCWSTR url, BOOL use_winhttp, LPWSTR module_path, NDFHANDLE *handle)
{
    TRACE("%s, %d, %s, %p\n", debugstr_w(url), use_winhttp, debugstr_w(module_path), handle);

    if (!url || !handle) return NDF_E_BAD_PARAM;
    return create_incident(handle);
}

STDAPI NdfCreateWinSockIncident(SOCKET sock, LPCWSTR host, USHORT port, LPCWSTR app_id, SID *user_id, NDFHANDLE *handle)
{
    struct sockaddr_storage name;
    int len = sizeof(name);

    TRACE("%Ix, %s, %u, %s, %p, %p\n", sock, debugstr_w(host), port, debugstr_w(app_id), user_id, handle);

    /* Windows asks the socket for its local and its remote address, and an incident needs a connection */
    if (getsockname(sock, (struct sockaddr *)&name, &len) == SOCKET_ERROR)
        return HRESULT_FROM_WIN32(WSAGetLastError());
    len = sizeof(name);
    if (getpeername(sock, (struct sockaddr *)&name, &len) == SOCKET_ERROR)
        return HRESULT_FROM_WIN32(WSAGetLastError());
    if (!handle) return NDF_E_BAD_PARAM;
    return create_incident(handle);
}

STDAPI NdfDiagnoseIncident(NDFHANDLE handle, ULONG *count, RootCauseInfo **causes, DWORD wait, DWORD flags)
{
    FIXME("%p, %p, %p, %ld, %ld\n", handle, count, causes, wait, flags);

    if (!is_incident(handle)) return E_HANDLE;
    if (!count || !causes) return E_INVALIDARG;

    /* without the service Windows finds nothing to diagnose with, and says so in the outputs as well */
    *count = 0;
    *causes = NULL;
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

STDAPI NdfExecuteDiagnosis(NDFHANDLE handle, HWND hwnd)
{
    FIXME("%p, %p\n", handle, hwnd);

    if (!is_incident(handle)) return E_HANDLE;

    /* the diagnosis needs the same service as the one that cannot be found for NdfDiagnoseIncident */
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(instance);
        break;
    case DLL_PROCESS_DETACH:
        if (!reserved)
        {
            struct incident *incident, *next;

            LIST_FOR_EACH_ENTRY_SAFE(incident, next, &incident_list, struct incident, entry)
            {
                list_remove(&incident->entry);
                free(incident);
            }
            DeleteCriticalSection(&incident_cs);
        }
        break;
    }
    return TRUE;
}

HRESULT WINAPI DllRegisterServer(void)
{
    FIXME("() stub\n");
    return S_OK;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    FIXME("() stub\n");
    return S_OK;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **obj)
{
    FIXME("(%s, %s, %p) stub\n", debugstr_guid(clsid), debugstr_guid(iid), obj);

    if (!obj) return E_POINTER;
    *obj = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}
