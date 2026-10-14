/* ReactOS transaction capability entry points. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <windef.h>
#include <winbase.h>
#include <winerror.h>

/* The kernel transaction manager is not implemented. Allow callers to detect
 * this through the documented failure returns and select a nontransactional
 * operation, without manufacturing a transaction handle or reporting success. */
HANDLE WINAPI
CreateTransaction(SECURITY_ATTRIBUTES *Attributes, GUID *Guid, DWORD Options,
                  DWORD IsolationLevel, DWORD IsolationFlags, DWORD Timeout,
                  WCHAR *Description)
{
    UNREFERENCED_PARAMETER(Attributes);
    UNREFERENCED_PARAMETER(Guid);
    UNREFERENCED_PARAMETER(Options);
    UNREFERENCED_PARAMETER(IsolationLevel);
    UNREFERENCED_PARAMETER(IsolationFlags);
    UNREFERENCED_PARAMETER(Timeout);
    UNREFERENCED_PARAMETER(Description);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return INVALID_HANDLE_VALUE;
}

BOOL WINAPI
CommitTransaction(HANDLE Transaction)
{
    UNREFERENCED_PARAMETER(Transaction);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL WINAPI
RollbackTransaction(HANDLE Transaction)
{
    UNREFERENCED_PARAMETER(Transaction);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}
