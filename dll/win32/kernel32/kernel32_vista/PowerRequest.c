/* ReactOS power-request capability APIs. SPDX-License-Identifier: GPL-2.0-or-later */
#include "k32_vista.h"

/* The power manager has no request-object implementation. Do not create a
 * placeholder handle or claim that a display/system sleep request is active. */
HANDLE WINAPI
PowerCreateRequest(PREASON_CONTEXT Context)
{
    UNREFERENCED_PARAMETER(Context);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return INVALID_HANDLE_VALUE;
}

BOOL WINAPI
PowerSetRequest(HANDLE Request, ULONG Type)
{
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Type);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL WINAPI
PowerClearRequest(HANDLE Request, ULONG Type)
{
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Type);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}
