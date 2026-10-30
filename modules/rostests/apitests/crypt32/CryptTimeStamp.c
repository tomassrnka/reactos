/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for crypt32!CryptRetrieveTimeStamp and CryptVerifyTimeStampSignature
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2 (x64 and WOW64). LibreOffice (mergedlo.dll) imports both functions.
 * Only the parameter checks are tested: retrieving a time stamp needs a time stamping server.
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <wincrypt.h>

typedef BOOL (WINAPI *PFN_Retrieve)(LPCWSTR, DWORD, DWORD, LPCSTR, const void *, const BYTE *, DWORD,
                                    void **, PCCERT_CONTEXT *, HCERTSTORE *);
typedef BOOL (WINAPI *PFN_Verify)(const BYTE *, DWORD, const BYTE *, DWORD, HCERTSTORE,
                                  void **, PCCERT_CONTEXT *, HCERTSTORE *);

START_TEST(CryptTimeStamp)
{
    HMODULE Crypt32 = LoadLibraryW(L"crypt32.dll");
    PFN_Retrieve pRetrieve = (PFN_Retrieve)GetProcAddress(Crypt32, "CryptRetrieveTimeStamp");
    PFN_Verify pVerify = (PFN_Verify)GetProcAddress(Crypt32, "CryptVerifyTimeStampSignature");
    static const BYTE Data[4] = { 1, 2, 3, 4 };
    void *Context;
    PCCERT_CONTEXT Signer;
    HCERTSTORE Store;
    DWORD Error;
    BOOL Ret;

    ok(pRetrieve != NULL, "CryptRetrieveTimeStamp is not exported by crypt32.dll\n");
    ok(pVerify != NULL, "CryptVerifyTimeStampSignature is not exported by crypt32.dll\n");
    if (!pRetrieve || !pVerify)
    {
        skip("The time stamp functions are not exported\n");
        return;
    }

    /* Nothing given: the answers are left alone */
    Context = (void *)(ULONG_PTR)1;
    Signer = (PCCERT_CONTEXT)(ULONG_PTR)1;
    Store = (HCERTSTORE)(ULONG_PTR)1;
    SetLastError(0xdeadbeef);
    Ret = pRetrieve(NULL, 0, 0, NULL, NULL, NULL, 0, &Context, &Signer, &Store);
    Error = GetLastError();
    ok(!Ret, "CryptRetrieveTimeStamp without arguments succeeded\n");
    ok(Error == ERROR_INVALID_PARAMETER, "Error is %lu\n", Error);
    ok(Context == (void *)(ULONG_PTR)1, "Context is %p\n", Context);
    ok(Signer == (PCCERT_CONTEXT)(ULONG_PTR)1, "Signer is %p\n", Signer);
    ok(Store == (HCERTSTORE)(ULONG_PTR)1, "Store is %p\n", Store);

    /* The context is required */
    SetLastError(0xdeadbeef);
    Ret = pRetrieve(L"http://127.0.0.1:9/", 0, 1000, "2.16.840.1.101.3.4.2.1", NULL, Data, sizeof(Data),
                    NULL, NULL, NULL);
    Error = GetLastError();
    ok(!Ret, "CryptRetrieveTimeStamp without a context succeeded\n");
    ok(Error == ERROR_INVALID_PARAMETER, "Error is %lu\n", Error);

    /* Verify: nothing given */
    Context = (void *)(ULONG_PTR)1;
    SetLastError(0xdeadbeef);
    Ret = pVerify(NULL, 0, NULL, 0, NULL, &Context, NULL, NULL);
    Error = GetLastError();
    ok(!Ret, "CryptVerifyTimeStampSignature without arguments succeeded\n");
    ok(Error == ERROR_INVALID_PARAMETER, "Error is %lu\n", Error);
    ok(Context == (void *)(ULONG_PTR)1, "Context is %p\n", Context);

    /* Verify: not a time stamp token; the context is cleared */
    Context = (void *)(ULONG_PTR)1;
    SetLastError(0xdeadbeef);
    Ret = pVerify(Data, sizeof(Data), NULL, 0, NULL, &Context, NULL, NULL);
    Error = GetLastError();
    ok(!Ret, "CryptVerifyTimeStampSignature of garbage succeeded\n");
    ok(Context == NULL, "Context is %p\n", Context);
    if (!is_reactos())
        ok(Error == 0x8009310b, "Error is %08lx\n", Error); /* CRYPT_E_ASN1_BADTAG; ReactOS does not parse the token yet */
}
