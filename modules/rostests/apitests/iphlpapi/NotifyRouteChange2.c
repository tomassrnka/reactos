/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for NotifyRouteChange2 and NotifyUnicastIpAddressChange
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <winsock2.h>

typedef VOID (WINAPI *CHANGE_CALLBACK)(PVOID, PVOID, ULONG);
typedef DWORD (WINAPI *FN_Notify)(ADDRESS_FAMILY, CHANGE_CALLBACK, PVOID, BOOLEAN, HANDLE *);
typedef DWORD (WINAPI *FN_CancelMibChangeNotify2)(HANDLE);

static VOID WINAPI Callback(PVOID Context, PVOID Row, ULONG Type)
{
}

static void TestNotify(const char *Name, FN_Notify pNotify, FN_CancelMibChangeNotify2 pCancel)
{
    HANDLE Handle;
    DWORD Error;

    Error = pNotify(99, Callback, NULL, FALSE, &Handle);
    ok(Error == ERROR_INVALID_PARAMETER, "%s: bad family: %lu\n", Name, Error);
    Error = pNotify(AF_UNSPEC, NULL, NULL, FALSE, &Handle);
    ok(Error == ERROR_INVALID_PARAMETER, "%s: no callback: %lu\n", Name, Error);
    Error = pNotify(AF_UNSPEC, Callback, NULL, FALSE, NULL);
    ok(Error == ERROR_INVALID_PARAMETER, "%s: no handle pointer: %lu\n", Name, Error);

    Handle = NULL;
    Error = pNotify(AF_UNSPEC, Callback, NULL, FALSE, &Handle);
    if (is_reactos())
    {
        /* Changes cannot be seen, so the registration fails honestly */
        ok(Error == ERROR_NOT_SUPPORTED, "%s returned %lu\n", Name, Error);
    }
    else
    {
        ok(Error == NO_ERROR, "%s returned %lu\n", Name, Error);
        ok(Handle != NULL, "%s: no handle\n", Name);
        if (Error == NO_ERROR)
        {
            Error = pCancel(Handle);
            ok(Error == NO_ERROR, "CancelMibChangeNotify2 returned %lu\n", Error);
        }
    }
}

START_TEST(NotifyRouteChange2)
{
    HMODULE hIphlpapi = GetModuleHandleW(L"iphlpapi.dll");
    FN_Notify pNotifyRouteChange2, pNotifyUnicastIpAddressChange;
    FN_CancelMibChangeNotify2 pCancelMibChangeNotify2;

    pNotifyRouteChange2 = (FN_Notify)GetProcAddress(hIphlpapi, "NotifyRouteChange2");
    pNotifyUnicastIpAddressChange = (FN_Notify)GetProcAddress(hIphlpapi, "NotifyUnicastIpAddressChange");
    pCancelMibChangeNotify2 = (FN_CancelMibChangeNotify2)GetProcAddress(hIphlpapi, "CancelMibChangeNotify2");
    if (!pNotifyRouteChange2 && (!is_reactos() || !GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64")))
    {
        skip("No NT 6 exports\n");
        return;
    }
    ok(pNotifyRouteChange2 != NULL, "NotifyRouteChange2 is not exported\n");
    ok(pNotifyUnicastIpAddressChange != NULL, "NotifyUnicastIpAddressChange is not exported\n");
    if (!pNotifyRouteChange2 || !pNotifyUnicastIpAddressChange || !pCancelMibChangeNotify2)
        return;

    TestNotify("NotifyRouteChange2", pNotifyRouteChange2, pCancelMibChangeNotify2);
    TestNotify("NotifyUnicastIpAddressChange", pNotifyUnicastIpAddressChange, pCancelMibChangeNotify2);
}
