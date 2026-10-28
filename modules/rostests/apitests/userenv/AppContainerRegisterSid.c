/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the AppContainer SID registration of kernelbase (AppContainerRegisterSid and friends,
 *              Windows 8 and later), with SIDs from userenv!DeriveAppContainerSidFromAppContainerName
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <winreg.h>
#include <sddl.h>

typedef HRESULT (WINAPI *PFN_Derive)(PCWSTR, PSID *);
typedef HRESULT (WINAPI *PFN_Register)(PSID, PCWSTR, PCWSTR);
typedef HRESULT (WINAPI *PFN_Unregister)(PSID);
typedef HRESULT (WINAPI *PFN_LookupMoniker)(PSID, PWSTR *);
typedef BOOLEAN (WINAPI *PFN_FreeMemory)(PVOID);

#define MAPPINGS L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppContainer\\Mappings\\"
#define TEST_NAME L"ReactOS.ApiTest.AppContainerRegisterSid"
#define TEST_DISPLAY L"ReactOS API test AppContainer"

static PFN_Derive pDerive;
static PFN_Register pRegister;
static PFN_Unregister pUnregister;
static PFN_LookupMoniker pLookupMoniker;
static PFN_FreeMemory pFreeMemory;

static void CheckDerivation(void)
{
    PSID Sid1 = NULL, Sid2 = NULL;
    LPWSTR String = NULL;
    HRESULT hr;

    hr = pDerive(TEST_NAME, &Sid1);
    ok(hr == S_OK, "Derive: hr %#lx\n", hr);
    hr = pDerive(L"reactos.apitest.appcontainerregistersid", &Sid2);
    ok(hr == S_OK, "Derive (lower case): hr %#lx\n", hr);
    if (!Sid1 || !Sid2)
        goto Cleanup;

    ok(EqualSid(Sid1, Sid2), "the SID depends on the case of the name\n");
    ok(*GetSidSubAuthorityCount(Sid1) == 8, "%u sub-authorities\n", *GetSidSubAuthorityCount(Sid1));
    ok(ConvertSidToStringSidW(Sid1, &String), "ConvertSidToStringSid failed with %lu\n", GetLastError());
    if (String)
    {
        ok(!wcsncmp(String, L"S-1-15-2-", 9), "got %ls\n", String);
        LocalFree(String);
    }

Cleanup:
    if (Sid1)
        FreeSid(Sid1);
    if (Sid2)
        FreeSid(Sid2);
}

/* The registration as Windows stores it: the moniker and the display name under Mappings\<SID> in HKCU */
static void CheckRegistry(PSID Sid, BOOL Expected)
{
    WCHAR Path[512], Value[100];
    LPWSTR String;
    DWORD Size;
    LONG Error;

    if (!ConvertSidToStringSidW(Sid, &String))
    {
        skip("ConvertSidToStringSid failed with %lu\n", GetLastError());
        return;
    }
    wcscpy(Path, MAPPINGS);
    wcscat(Path, String);
    LocalFree(String);

    Size = sizeof(Value);
    Error = RegGetValueW(HKEY_CURRENT_USER, Path, L"Moniker", RRF_RT_REG_SZ, NULL, Value, &Size);
    if (!Expected)
    {
        ok(Error == ERROR_FILE_NOT_FOUND, "Moniker: error %ld\n", Error);
        return;
    }
    ok(Error == ERROR_SUCCESS, "Moniker: error %ld\n", Error);
    if (Error == ERROR_SUCCESS)
        ok(!wcscmp(Value, TEST_NAME), "Moniker is %ls\n", Value);
    Size = sizeof(Value);
    Error = RegGetValueW(HKEY_CURRENT_USER, Path, L"DisplayName", RRF_RT_REG_SZ, NULL, Value, &Size);
    ok(Error == ERROR_SUCCESS, "DisplayName: error %ld\n", Error);
    if (Error == ERROR_SUCCESS)
        ok(!wcscmp(Value, TEST_DISPLAY), "DisplayName is %ls\n", Value);
}

static void CheckRoundTrip(void)
{
    PSID Sid = NULL;
    PWSTR Moniker;
    HRESULT hr;

    hr = pDerive(TEST_NAME, &Sid);
    ok(hr == S_OK, "Derive: hr %#lx\n", hr);
    if (hr != S_OK)
        return;

    /* Left over by an earlier run that did not finish */
    pUnregister(Sid);

    Moniker = (PWSTR)(ULONG_PTR)0x55;
    hr = pLookupMoniker(Sid, &Moniker);
    ok(FAILED(hr), "LookupMoniker of an unregistered SID: hr %#lx\n", hr);
    if (SUCCEEDED(hr))
        pFreeMemory(Moniker);

    hr = pRegister(Sid, TEST_NAME, TEST_DISPLAY);
    ok(hr == S_OK, "Register: hr %#lx\n", hr);
    CheckRegistry(Sid, TRUE);

    hr = pRegister(Sid, TEST_NAME, TEST_DISPLAY);
    ok(hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), "Register again: hr %#lx\n", hr);

    Moniker = NULL;
    hr = pLookupMoniker(Sid, &Moniker);
    ok(hr == S_OK, "LookupMoniker: hr %#lx\n", hr);
    ok(Moniker != NULL, "no moniker\n");
    if (Moniker)
    {
        /* As registered, with its case */
        ok(!wcscmp(Moniker, TEST_NAME), "moniker %ls\n", Moniker);
        pFreeMemory(Moniker);
    }

    hr = pUnregister(Sid);
    ok(hr == S_OK, "Unregister: hr %#lx\n", hr);
    CheckRegistry(Sid, FALSE);

    Moniker = (PWSTR)(ULONG_PTR)0x55;
    hr = pLookupMoniker(Sid, &Moniker);
    ok(FAILED(hr), "LookupMoniker after Unregister: hr %#lx\n", hr);
    if (SUCCEEDED(hr))
        pFreeMemory(Moniker);

    hr = pUnregister(Sid);
    ok(FAILED(hr), "Unregister again: hr %#lx\n", hr);

    /* A SID can be registered again after it was unregistered */
    hr = pRegister(Sid, TEST_NAME, TEST_DISPLAY);
    ok(hr == S_OK, "Register after Unregister: hr %#lx\n", hr);
    hr = pUnregister(Sid);
    ok(hr == S_OK, "Unregister: hr %#lx\n", hr);

    FreeSid(Sid);
}

static void CheckInvalid(void)
{
    SID_IDENTIFIER_AUTHORITY WorldAuthority = { SECURITY_WORLD_SID_AUTHORITY };
    PSID World = NULL, AppContainer = NULL;
    PWSTR Moniker;
    HRESULT hr;

    hr = pDerive(TEST_NAME, &AppContainer);
    ok(hr == S_OK, "Derive: hr %#lx\n", hr);
    ok(AllocateAndInitializeSid(&WorldAuthority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &World),
       "AllocateAndInitializeSid failed with %lu\n", GetLastError());
    if (!AppContainer || !World)
        goto Cleanup;

    /* Not an AppContainer SID */
    hr = pRegister(World, TEST_NAME, TEST_DISPLAY);
    ok(FAILED(hr), "Register of S-1-1-0: hr %#lx\n", hr);
    hr = pUnregister(World);
    ok(FAILED(hr), "Unregister of S-1-1-0: hr %#lx\n", hr);
    Moniker = NULL;
    hr = pLookupMoniker(World, &Moniker);
    ok(FAILED(hr), "LookupMoniker of S-1-1-0: hr %#lx\n", hr);

    /* No moniker */
    hr = pRegister(AppContainer, NULL, TEST_DISPLAY);
    ok(FAILED(hr), "Register without a moniker: hr %#lx\n", hr);
    hr = pRegister(AppContainer, L"", TEST_DISPLAY);
    ok(FAILED(hr), "Register with an empty moniker: hr %#lx\n", hr);
    Moniker = NULL;
    hr = pLookupMoniker(AppContainer, &Moniker);
    ok(FAILED(hr), "LookupMoniker after failed registrations: hr %#lx\n", hr);

    /* Nothing to free: must not fail or crash */
    pFreeMemory(NULL);

Cleanup:
    if (AppContainer)
        FreeSid(AppContainer);
    if (World)
        FreeSid(World);
}

START_TEST(AppContainerRegisterSid)
{
    HMODULE Userenv = LoadLibraryW(L"userenv.dll");
    HMODULE Kernelbase = LoadLibraryW(L"kernelbase.dll");

    pDerive = (PFN_Derive)GetProcAddress(Userenv, "DeriveAppContainerSidFromAppContainerName");
    pRegister = (PFN_Register)GetProcAddress(Kernelbase, "AppContainerRegisterSid");
    pUnregister = (PFN_Unregister)GetProcAddress(Kernelbase, "AppContainerUnregisterSid");
    pLookupMoniker = (PFN_LookupMoniker)GetProcAddress(Kernelbase, "AppContainerLookupMoniker");
    pFreeMemory = (PFN_FreeMemory)GetProcAddress(Kernelbase, "AppContainerFreeMemory");

    /* Windows 7 has none of them. ReactOS exports the kernelbase ones at every export level. */
    ok(!is_reactos() || (pRegister && pUnregister && pLookupMoniker && pFreeMemory),
       "kernelbase does not export the AppContainer registration functions\n");
    if (!pDerive || !pRegister || !pUnregister || !pLookupMoniker || !pFreeMemory)
    {
        skip("the AppContainer functions are not exported\n");
        return;
    }

    CheckDerivation();
    CheckRoundTrip();
    CheckInvalid();
}
