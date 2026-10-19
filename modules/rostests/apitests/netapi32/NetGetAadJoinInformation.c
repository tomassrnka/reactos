/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for netapi32!NetGetAadJoinInformation and NetFreeAadJoinInformation (Windows 10)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2 (x64 and WOW64) on a machine that is not joined to Azure Active Directory.
 */

#include <apitest.h>
#include <winbase.h>
#include <winerror.h>

#define PENDING_ERROR 0xdeadbeef

typedef HRESULT (WINAPI *PFN_NetGetAadJoinInformation)(LPCWSTR, PVOID *);
typedef VOID (WINAPI *PFN_NetFreeAadJoinInformation)(PVOID);

START_TEST(NetGetAadJoinInformation)
{
    HMODULE Netapi32 = LoadLibraryW(L"netapi32.dll");
    PFN_NetGetAadJoinInformation pGet;
    PFN_NetFreeAadJoinInformation pFree;
    PVOID Info;
    HRESULT hr;

    pGet = (PFN_NetGetAadJoinInformation)GetProcAddress(Netapi32, "NetGetAadJoinInformation");
    pFree = (PFN_NetFreeAadJoinInformation)GetProcAddress(Netapi32, "NetFreeAadJoinInformation");
    /* Windows 7 has neither: they came with Windows 10; ReactOS has them from export level 0x601 */
    if (!pGet || !pFree)
    {
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
           "NetGetAadJoinInformation or NetFreeAadJoinInformation is not exported\n");
        skip("the Azure Active Directory join functions are not exported\n");
        return;
    }

    /* the only argument that is checked is the one for the answer */
    Info = (PVOID)(ULONG_PTR)0x55;
    SetLastError(PENDING_ERROR);
    hr = pGet(NULL, NULL);
    ok_hex(hr, E_INVALIDARG);
    ok_hex(GetLastError(), PENDING_ERROR);

    /* a machine that is not joined: no information, "false", and the last error says that nothing was found */
    SetLastError(PENDING_ERROR);
    hr = pGet(NULL, &Info);
    if (!is_reactos() && hr == S_OK)
    {
        skip("this machine is joined to Azure Active Directory\n");
        if (Info)
            pFree(Info);
        pFree(NULL);
        return;
    }
    ok_hex(hr, S_FALSE);
    ok_ptr(Info, NULL);
    ok_hex(GetLastError(), CRYPT_E_NOT_FOUND);

    Info = (PVOID)(ULONG_PTR)0x55;
    SetLastError(PENDING_ERROR);
    hr = pGet(L"", &Info);
    ok_hex(hr, S_FALSE);
    ok_ptr(Info, NULL);
    ok_hex(GetLastError(), CRYPT_E_NOT_FOUND);

    Info = (PVOID)(ULONG_PTR)0x55;
    hr = pGet(L"00000000-0000-0000-0000-000000000000", &Info);
    ok_hex(hr, S_FALSE);
    ok_ptr(Info, NULL);

    /* freeing nothing is fine */
    pFree(NULL);
}
