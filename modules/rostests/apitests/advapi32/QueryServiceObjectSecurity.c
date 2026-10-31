/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for QueryServiceObjectSecurity: a NULL buffer asks for the size, the size is the length of the
 *              descriptor
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * WinFsp registers its driver service and then queries the security descriptor of the service with a NULL buffer to
 * find out the size; without that the registration of the WinFsp network provider (DllRegisterServer, done by the
 * MSI) fails.
 */

#include "precomp.h"

static void
test_Query(SC_HANDLE Service)
{
    DWORD Needed, Needed2, Error;
    PSECURITY_DESCRIPTOR Sd;
    BYTE Tiny[8];
    BOOL Ret;

    Needed = 0x12345;
    SetLastError(0xdeadbeef);
    Ret = QueryServiceObjectSecurity(Service, DACL_SECURITY_INFORMATION, NULL, 0, &Needed);
    Error = GetLastError();
    ok(!Ret, "NULL buffer succeeded\n");
    ok(Error == ERROR_INSUFFICIENT_BUFFER, "NULL buffer: error %lu\n", Error);
    ok(Needed > sizeof(SECURITY_DESCRIPTOR_RELATIVE) && Needed < 0x10000, "NULL buffer: needed %lu\n", Needed);

    Needed2 = 0;
    SetLastError(0xdeadbeef);
    Ret = QueryServiceObjectSecurity(Service, DACL_SECURITY_INFORMATION, (PSECURITY_DESCRIPTOR)Tiny, 0, &Needed2);
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "size 0: Ret %d, error %lu\n", Ret, GetLastError());
    ok(Needed2 == Needed, "size 0: needed %lu, expected %lu\n", Needed2, Needed);

    Sd = HeapAlloc(GetProcessHeap(), 0, Needed + 100);
    if (!Sd)
    {
        skip("No memory\n");
        return;
    }

    /* the length that was needed comes back, whatever the size of the buffer */
    Needed2 = 0;
    SetLastError(0xdeadbeef);
    Ret = QueryServiceObjectSecurity(Service, DACL_SECURITY_INFORMATION, Sd, Needed, &Needed2);
    ok(Ret, "exact buffer: error %lu\n", GetLastError());
    ok(Needed2 == Needed, "exact buffer: needed %lu, expected %lu\n", Needed2, Needed);
    if (Ret)
    {
        ok(IsValidSecurityDescriptor(Sd), "the descriptor is not valid\n");
        ok(GetSecurityDescriptorLength(Sd) == Needed, "length %lu, expected %lu\n", GetSecurityDescriptorLength(Sd), Needed);
    }

    Needed2 = 0;
    Ret = QueryServiceObjectSecurity(Service, DACL_SECURITY_INFORMATION, Sd, Needed + 100, &Needed2);
    ok(Ret, "larger buffer: error %lu\n", GetLastError());
    ok(Needed2 == Needed, "larger buffer: needed %lu, expected %lu\n", Needed2, Needed);

    Needed2 = 0;
    SetLastError(0xdeadbeef);
    Ret = QueryServiceObjectSecurity(Service, DACL_SECURITY_INFORMATION, Sd, Needed - 1, &Needed2);
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "short buffer: Ret %d, error %lu\n", Ret, GetLastError());
    ok(Needed2 == Needed, "short buffer: needed %lu, expected %lu\n", Needed2, Needed);

    HeapFree(GetProcessHeap(), 0, Sd);
}

START_TEST(QueryServiceObjectSecurity)
{
    static const WCHAR *Names[] = { L"RpcSs", L"EventLog", L"Dhcp", L"Dnscache", L"Spooler" };
    static const WCHAR *Drivers[] = { L"Null", L"Beep" };
    SC_HANDLE Manager, Service = NULL;
    ULONG i, Tested;

    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!Manager)
    {
        skip("OpenSCManager failed, error %lu\n", GetLastError());
        return;
    }

    for (i = 0; i < ARRAYSIZE(Names) && !Service; i++)
        Service = OpenServiceW(Manager, Names[i], READ_CONTROL | SERVICE_QUERY_CONFIG);
    ok(Service != NULL, "no service to query\n");
    if (Service)
    {
        test_Query(Service);
        CloseServiceHandle(Service);
    }

    /* A driver service has no descriptor of its own on ReactOS: the service manager hands out the default one */
    Tested = 0;
    for (i = 0; i < ARRAYSIZE(Drivers); i++)
    {
        Service = OpenServiceW(Manager, Drivers[i], READ_CONTROL | SERVICE_QUERY_CONFIG);
        if (!Service)
            continue;
        test_Query(Service);
        CloseServiceHandle(Service);
        Tested++;
    }
    ok(Tested != 0, "no driver service to query\n");

    CloseServiceHandle(Manager);
}
