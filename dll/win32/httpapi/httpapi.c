/*
 * PROJECT:     ReactOS HTTP Server API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     HTTP Server API version 2.0 session, URL group and request queue entry points
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * ReactOS has no http.sys, so there is nothing to create a server session or a
 * request queue in. The version 2.0 entry points exist so that programs which
 * import them (LibreOffice links its embedded web server against them) load,
 * and they report ERROR_CALL_NOT_IMPLEMENTED when actually called.
 */

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winerror.h>
#include <http.h>

DWORD WINAPI HttpCreateServerSession(HTTPAPI_VERSION version, HTTP_SERVER_SESSION_ID *session_id, ULONG reserved)
{
    UNREFERENCED_PARAMETER(version);
    UNREFERENCED_PARAMETER(reserved);
    if (!session_id) return ERROR_INVALID_PARAMETER;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI HttpCloseServerSession(HTTP_SERVER_SESSION_ID session_id)
{
    UNREFERENCED_PARAMETER(session_id);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI HttpSetServerSessionProperty(HTTP_SERVER_SESSION_ID session_id, HTTP_SERVER_PROPERTY property,
                                          void *value, ULONG length)
{
    UNREFERENCED_PARAMETER(session_id);
    UNREFERENCED_PARAMETER(property);
    UNREFERENCED_PARAMETER(value);
    UNREFERENCED_PARAMETER(length);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI HttpCreateUrlGroup(HTTP_SERVER_SESSION_ID session_id, HTTP_URL_GROUP_ID *group_id, ULONG reserved)
{
    UNREFERENCED_PARAMETER(session_id);
    UNREFERENCED_PARAMETER(reserved);
    if (!group_id) return ERROR_INVALID_PARAMETER;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI HttpCloseUrlGroup(HTTP_URL_GROUP_ID group_id)
{
    UNREFERENCED_PARAMETER(group_id);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI HttpSetUrlGroupProperty(HTTP_URL_GROUP_ID group_id, HTTP_SERVER_PROPERTY property,
                                     void *value, ULONG length)
{
    UNREFERENCED_PARAMETER(group_id);
    UNREFERENCED_PARAMETER(property);
    UNREFERENCED_PARAMETER(value);
    UNREFERENCED_PARAMETER(length);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI HttpCreateRequestQueue(HTTPAPI_VERSION version, const WCHAR *name, SECURITY_ATTRIBUTES *sa,
                                    ULONG flags, HANDLE *handle)
{
    UNREFERENCED_PARAMETER(version);
    UNREFERENCED_PARAMETER(name);
    UNREFERENCED_PARAMETER(sa);
    UNREFERENCED_PARAMETER(flags);
    if (!handle) return ERROR_INVALID_PARAMETER;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI HttpCloseRequestQueue(HANDLE handle)
{
    UNREFERENCED_PARAMETER(handle);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI HttpSetRequestQueueProperty(HANDLE handle, HTTP_SERVER_PROPERTY property, void *value,
                                         ULONG length, ULONG reserved, void *reserved2)
{
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(property);
    UNREFERENCED_PARAMETER(value);
    UNREFERENCED_PARAMETER(length);
    UNREFERENCED_PARAMETER(reserved);
    UNREFERENCED_PARAMETER(reserved2);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI HttpShutdownRequestQueue(HANDLE handle)
{
    UNREFERENCED_PARAMETER(handle);
    return ERROR_CALL_NOT_IMPLEMENTED;
}
