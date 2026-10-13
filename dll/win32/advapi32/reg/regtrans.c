/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Transacted registry functions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <advapi32.h>

/*
 * A transacted registry operation needs a transaction from the kernel transaction manager, which
 * this kernel does not have (CreateTransaction fails), so no caller can hold a valid transaction
 * handle. These functions do nothing and say so.
 */

/*
 * @implemented
 */
LSTATUS
WINAPI
RegCreateKeyTransactedW(
    _In_ HKEY hKey,
    _In_ LPCWSTR lpSubKey,
    _Reserved_ DWORD Reserved,
    _In_opt_ LPWSTR lpClass,
    _In_ DWORD dwOptions,
    _In_ REGSAM samDesired,
    _In_opt_ const LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _Out_ PHKEY phkResult,
    _Out_opt_ LPDWORD lpdwDisposition,
    _In_ HANDLE hTransaction,
    _Reserved_ PVOID pExtendedParemeter)
{
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/*
 * @implemented
 */
LSTATUS
WINAPI
RegCreateKeyTransactedA(
    _In_ HKEY hKey,
    _In_ LPCSTR lpSubKey,
    _Reserved_ DWORD Reserved,
    _In_opt_ LPSTR lpClass,
    _In_ DWORD dwOptions,
    _In_ REGSAM samDesired,
    _In_opt_ const LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _Out_ PHKEY phkResult,
    _Out_opt_ LPDWORD lpdwDisposition,
    _In_ HANDLE hTransaction,
    _Reserved_ PVOID pExtendedParemeter)
{
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/*
 * @implemented
 */
LSTATUS
WINAPI
RegOpenKeyTransactedW(
    _In_ HKEY hKey,
    _In_opt_ LPCWSTR lpSubKey,
    _In_opt_ DWORD ulOptions,
    _In_ REGSAM samDesired,
    _Out_ PHKEY phkResult,
    _In_ HANDLE hTransaction,
    _Reserved_ PVOID pExtendedParemeter)
{
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/*
 * @implemented
 */
LSTATUS
WINAPI
RegOpenKeyTransactedA(
    _In_ HKEY hKey,
    _In_opt_ LPCSTR lpSubKey,
    _In_opt_ DWORD ulOptions,
    _In_ REGSAM samDesired,
    _Out_ PHKEY phkResult,
    _In_ HANDLE hTransaction,
    _Reserved_ PVOID pExtendedParemeter)
{
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/*
 * @implemented
 */
LSTATUS
WINAPI
RegDeleteKeyTransactedW(
    _In_ HKEY hKey,
    _In_ LPCWSTR lpSubKey,
    _In_ REGSAM samDesired,
    _Reserved_ DWORD Reserved,
    _In_ HANDLE hTransaction,
    _Reserved_ PVOID pExtendedParameter)
{
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/*
 * @implemented
 */
LSTATUS
WINAPI
RegDeleteKeyTransactedA(
    _In_ HKEY hKey,
    _In_ LPCSTR lpSubKey,
    _In_ REGSAM samDesired,
    _Reserved_ DWORD Reserved,
    _In_ HANDLE hTransaction,
    _Reserved_ PVOID pExtendedParameter)
{
    return ERROR_CALL_NOT_IMPLEMENTED;
}
