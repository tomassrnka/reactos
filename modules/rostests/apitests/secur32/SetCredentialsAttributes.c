/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for SetCredentialsAttributesA/W (secur32, also exported by sspicli)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * msquic.dll imports SetCredentialsAttributesW from Secur32.dll. Everything is bound at run time, so
 * the program also runs where the function is absent. The expected values are those of Windows 10
 * 22H2: a handle that names nothing is SEC_E_INVALID_HANDLE, a Schannel credential does not accept
 * attributes (SEC_E_UNSUPPORTED_FUNCTION). NTLM and Negotiate answer with codes of their own on
 * Windows (SEC_E_INVALID_HANDLE and SEC_E_INVALID_TOKEN), so only a failure is required of them.
 * Attribute 2 is not exercised: Windows checks its buffer size before the package is asked.
 */

#define SECURITY_WIN32
#include <apitest.h>
#include <sspi.h>
#include <schannel.h>

typedef SECURITY_STATUS (WINAPI *PFN_Set)(PCredHandle, ULONG, PVOID, ULONG);
typedef SECURITY_STATUS (WINAPI *PFN_Acquire)(SEC_CHAR *, SEC_CHAR *, ULONG, PLUID, PVOID, SEC_GET_KEY_FN, PVOID,
                                              PCredHandle, PTimeStamp);
typedef SECURITY_STATUS (WINAPI *PFN_Free)(PCredHandle);

static const ULONG Attributes[] = { 0, 1, 3, 0x80, 0x1000 };

static void TestHandles(const char *Name, PFN_Set SetW, PFN_Set SetA)
{
    CredHandle Zero = { 0, 0 };
    char Buffer[64] = { 0 };
    SECURITY_STATUS Status;

    Status = SetW(NULL, 1, Buffer, 4);
    ok(Status == SEC_E_INVALID_HANDLE, "%s: W with a NULL handle: 0x%08lx\n", Name, (ULONG)Status);
    Status = SetA(NULL, 1, Buffer, 4);
    ok(Status == SEC_E_INVALID_HANDLE, "%s: A with a NULL handle: 0x%08lx\n", Name, (ULONG)Status);
    Status = SetW(&Zero, 1, Buffer, 4);
    ok(Status == SEC_E_INVALID_HANDLE, "%s: W with a zeroed handle: 0x%08lx\n", Name, (ULONG)Status);
    Status = SetA(&Zero, 1, Buffer, 4);
    ok(Status == SEC_E_INVALID_HANDLE, "%s: A with a zeroed handle: 0x%08lx\n", Name, (ULONG)Status);
}

static void TestPackage(PFN_Set SetW, PFN_Set SetA, PFN_Acquire Acquire, PFN_Free Free, SEC_CHAR *Package,
                        BOOL Schannel)
{
    CredHandle Cred;
    TimeStamp Expiry;
    SCHANNEL_CRED Sc;
    char Buffer[64] = { 0 };
    SECURITY_STATUS Status;
    ULONG i;

    memset(&Sc, 0, sizeof(Sc));
    Sc.dwVersion = SCHANNEL_CRED_VERSION;
    Sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION;

    Status = Acquire(NULL, Package, SECPKG_CRED_OUTBOUND, NULL, Schannel ? &Sc : NULL, NULL, NULL, &Cred, &Expiry);
    if (Status != SEC_E_OK)
    {
        skip("%s: AcquireCredentialsHandle: 0x%08lx\n", Package, (ULONG)Status);
        return;
    }

    for (i = 0; i < ARRAYSIZE(Attributes); i++)
    {
        Status = SetW(&Cred, Attributes[i], Buffer, 8);
        if (Schannel)
            ok(Status == SEC_E_UNSUPPORTED_FUNCTION, "%s: W attribute 0x%lx: 0x%08lx\n", Package, Attributes[i],
               (ULONG)Status);
        else
            ok(FAILED(Status), "%s: W attribute 0x%lx: 0x%08lx\n", Package, Attributes[i], (ULONG)Status);

        Status = SetA(&Cred, Attributes[i], Buffer, 8);
        if (Schannel)
            ok(Status == SEC_E_UNSUPPORTED_FUNCTION, "%s: A attribute 0x%lx: 0x%08lx\n", Package, Attributes[i],
               (ULONG)Status);
        else
            ok(FAILED(Status), "%s: A attribute 0x%lx: 0x%08lx\n", Package, Attributes[i], (ULONG)Status);
    }

    Status = SetW(&Cred, 1, NULL, 0);
    ok(FAILED(Status), "%s: W with no buffer: 0x%08lx\n", Package, (ULONG)Status);

    Free(&Cred);
}

START_TEST(SetCredentialsAttributes)
{
    HMODULE Secur32 = LoadLibraryW(L"secur32.dll");
    HMODULE Sspicli = LoadLibraryW(L"sspicli.dll");
    PFN_Set SetW, SetA;
    PFN_Acquire Acquire;
    PFN_Free Free;

    ok(Secur32 != NULL, "secur32.dll does not load\n");
    if (!Secur32)
        return;

    SetW = (PFN_Set)GetProcAddress(Secur32, "SetCredentialsAttributesW");
    SetA = (PFN_Set)GetProcAddress(Secur32, "SetCredentialsAttributesA");
    ok(SetW != NULL, "secur32 does not export SetCredentialsAttributesW\n");
    ok(SetA != NULL, "secur32 does not export SetCredentialsAttributesA\n");
    if (!SetW || !SetA)
        return;

    if (Sspicli)
    {
        ok(GetProcAddress(Sspicli, "SetCredentialsAttributesW") == (FARPROC)SetW,
           "sspicli's SetCredentialsAttributesW is not secur32's\n");
        ok(GetProcAddress(Sspicli, "SetCredentialsAttributesA") == (FARPROC)SetA,
           "sspicli's SetCredentialsAttributesA is not secur32's\n");
    }

    TestHandles("secur32", SetW, SetA);

    Acquire = (PFN_Acquire)GetProcAddress(Secur32, "AcquireCredentialsHandleA");
    Free = (PFN_Free)GetProcAddress(Secur32, "FreeCredentialsHandle");
    if (Acquire && Free)
    {
        TestPackage(SetW, SetA, Acquire, Free, (SEC_CHAR *)"NTLM", FALSE);
        TestPackage(SetW, SetA, Acquire, Free, (SEC_CHAR *)UNISP_NAME_A, TRUE);
    }
}
