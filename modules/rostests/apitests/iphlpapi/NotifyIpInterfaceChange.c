/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for NotifyIpInterfaceChange and CancelMibChangeNotify2
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <winsock2.h>

typedef VOID (WINAPI *IPINTERFACE_CALLBACK)(PVOID, PVOID, ULONG);
typedef DWORD (WINAPI *FN_NotifyIpInterfaceChange)(ADDRESS_FAMILY, IPINTERFACE_CALLBACK, PVOID, BOOLEAN, HANDLE *);
typedef DWORD (WINAPI *FN_CancelMibChangeNotify2)(HANDLE);

static VOID WINAPI Callback(PVOID Context, PVOID Row, ULONG Type)
{
}

START_TEST(NotifyIpInterfaceChange)
{
    HMODULE hIphlpapi = GetModuleHandleW(L"iphlpapi.dll");
    FN_NotifyIpInterfaceChange pNotifyIpInterfaceChange;
    FN_CancelMibChangeNotify2 pCancelMibChangeNotify2;
    HANDLE Handle;
    DWORD Error;

    pNotifyIpInterfaceChange = (FN_NotifyIpInterfaceChange)GetProcAddress(hIphlpapi, "NotifyIpInterfaceChange");
    pCancelMibChangeNotify2 = (FN_CancelMibChangeNotify2)GetProcAddress(hIphlpapi, "CancelMibChangeNotify2");
    if (!pNotifyIpInterfaceChange && is_reactos() && !GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64"))
    {
        skip("No NT 6 exports\n");
        return;
    }
    ok(pNotifyIpInterfaceChange && pCancelMibChangeNotify2, "NotifyIpInterfaceChange/CancelMibChangeNotify2 are not exported\n");
    if (!pNotifyIpInterfaceChange || !pCancelMibChangeNotify2)
        return;

    Error = pNotifyIpInterfaceChange(99, Callback, NULL, FALSE, &Handle);
    ok(Error == ERROR_INVALID_PARAMETER, "Bad family: %lu\n", Error);
    Error = pNotifyIpInterfaceChange(AF_UNSPEC, Callback, NULL, FALSE, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "No handle pointer: %lu\n", Error);

    Handle = NULL;
    Error = pNotifyIpInterfaceChange(AF_UNSPEC, Callback, NULL, FALSE, &Handle);
    if (is_reactos())
    {
        /* Interface changes cannot be seen, so the registration fails honestly */
        ok(Error == ERROR_NOT_SUPPORTED, "NotifyIpInterfaceChange returned %lu\n", Error);
    }
    else
    {
        ok(Error == NO_ERROR, "NotifyIpInterfaceChange returned %lu\n", Error);
        ok(Handle != NULL, "No handle\n");
        if (Error == NO_ERROR)
        {
            Error = pCancelMibChangeNotify2(Handle);
            ok(Error == NO_ERROR, "CancelMibChangeNotify2 returned %lu\n", Error);
        }
    }

    Error = pCancelMibChangeNotify2(NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "CancelMibChangeNotify2(NULL) returned %lu\n", Error);
}
