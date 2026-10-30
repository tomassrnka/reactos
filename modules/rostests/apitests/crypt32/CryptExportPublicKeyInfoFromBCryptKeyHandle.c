/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for crypt32!CryptExportPublicKeyInfoFromBCryptKeyHandle
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2 (x64 and WOW64). LibreOffice (libxmlsec-mscng) imports this function.
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <wincrypt.h>
#include <bcrypt.h>

#define STATUS_INVALID_HANDLE_VALUE ((DWORD)0xC0000008)
#define CRYPT_E_UNKNOWN_ALGO_VALUE ((DWORD)0x80091002)

typedef BOOL (WINAPI *PFN_Export)(BCRYPT_KEY_HANDLE, DWORD, LPSTR, DWORD, void *, PCERT_PUBLIC_KEY_INFO, DWORD *);

static BOOL InBuffer(const BYTE *Buffer, DWORD Size, const void *Pointer, DWORD Length)
{
    return (const BYTE *)Pointer >= Buffer && (const BYTE *)Pointer + Length <= Buffer + Size;
}

static void TestRsa(PFN_Export pExport)
{
    BCRYPT_ALG_HANDLE Alg;
    BCRYPT_KEY_HANDLE Key;
    PCERT_PUBLIC_KEY_INFO Info;
    DWORD Size, Size2, Error;
    BYTE *Buffer;
    NTSTATUS Status;
    BOOL Ret;

    Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_RSA_ALGORITHM, NULL, 0);
    if (Status != 0)
    {
        skip("No RSA provider (%08lx)\n", (ULONG)Status);
        return;
    }
    Status = BCryptGenerateKeyPair(Alg, &Key, 1024, 0);
    if (Status == 0)
        Status = BCryptFinalizeKeyPair(Key, 0);
    if (Status != 0)
    {
        skip("Cannot generate an RSA key pair (%08lx)\n", (ULONG)Status);
        BCryptCloseAlgorithmProvider(Alg, 0);
        return;
    }

    /* Size query */
    Size = 0;
    SetLastError(0xdeadbeef);
    Ret = pExport(Key, X509_ASN_ENCODING, NULL, 0, NULL, NULL, &Size);
    ok(Ret, "Size query failed, error %lu\n", GetLastError());
    ok(Size > sizeof(CERT_PUBLIC_KEY_INFO) + 140, "Size is %lu\n", Size);

    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size + 16);
    Info = (PCERT_PUBLIC_KEY_INFO)Buffer;
    Size2 = Size;
    SetLastError(0xdeadbeef);
    Ret = pExport(Key, X509_ASN_ENCODING, NULL, 0, NULL, Info, &Size2);
    ok(Ret, "Export failed, error %lu\n", GetLastError());
    ok(Size2 == Size, "Size is %lu, expected %lu\n", Size2, Size);
    if (Ret)
    {
        ok(InBuffer(Buffer, Size, Info->Algorithm.pszObjId, 1), "OID is outside the buffer\n");
        ok(Info->Algorithm.pszObjId && !strcmp(Info->Algorithm.pszObjId, szOID_RSA_RSA),
           "OID is '%s'\n", Info->Algorithm.pszObjId);
        ok(Info->Algorithm.Parameters.cbData == 0, "Parameters are %lu bytes\n", Info->Algorithm.Parameters.cbData);
        ok(Info->PublicKey.cUnusedBits == 0, "cUnusedBits is %lu\n", Info->PublicKey.cUnusedBits);
        /* RSAPublicKey ::= SEQUENCE { modulus INTEGER (128 bytes + sign byte), publicExponent INTEGER 65537 } */
        ok(Info->PublicKey.cbData == 140, "Public key is %lu bytes\n", Info->PublicKey.cbData);
        ok(InBuffer(Buffer, Size, Info->PublicKey.pbData, Info->PublicKey.cbData), "Key is outside the buffer\n");
        if (Info->PublicKey.cbData == 140)
        {
            ok(Info->PublicKey.pbData[0] == 0x30 && Info->PublicKey.pbData[1] == 0x81 &&
               Info->PublicKey.pbData[2] == 0x89 && Info->PublicKey.pbData[3] == 0x02,
               "Key does not start with a SEQUENCE of INTEGERs\n");
            ok(!memcmp(Info->PublicKey.pbData + 137, "\x01\x00\x01", 3), "Exponent is not 65537\n");
        }
    }

    /* A buffer that is one byte short */
    memset(Buffer, 0, Size + 16);
    Size2 = Size - 1;
    SetLastError(0xdeadbeef);
    Ret = pExport(Key, X509_ASN_ENCODING, NULL, 0, NULL, Info, &Size2);
    Error = GetLastError();
    ok(!Ret, "A short buffer succeeded\n");
    ok(Error == ERROR_MORE_DATA, "Error is %lu\n", Error);
    ok(Size2 == Size, "Size is %lu, expected %lu\n", Size2, Size);

    /* The OID can be given */
    Size2 = Size;
    SetLastError(0xdeadbeef);
    Ret = pExport(Key, X509_ASN_ENCODING, (LPSTR)szOID_RSA_RSA, 0, NULL, Info, &Size2);
    ok(Ret, "Export with the RSA OID failed, error %lu\n", GetLastError());

    /* No encoding type */
    Size2 = Size;
    SetLastError(0xdeadbeef);
    Ret = pExport(Key, 0, NULL, 0, NULL, Info, &Size2);
    ok(!Ret, "Export without an encoding type succeeded\n");

    HeapFree(GetProcessHeap(), 0, Buffer);
    BCryptDestroyKey(Key);
    BCryptCloseAlgorithmProvider(Alg, 0);
}

static void TestEcc(PFN_Export pExport)
{
    BCRYPT_ALG_HANDLE Alg;
    BCRYPT_KEY_HANDLE Key;
    PCERT_PUBLIC_KEY_INFO Info;
    DWORD Size, Size2;
    BYTE *Buffer;
    NTSTATUS Status;
    BOOL Ret;

    Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0);
    if (Status != 0)
    {
        skip("No ECDSA provider (%08lx)\n", (ULONG)Status);
        return;
    }
    Status = BCryptGenerateKeyPair(Alg, &Key, 256, 0);
    if (Status == 0)
        Status = BCryptFinalizeKeyPair(Key, 0);
    if (Status != 0)
    {
        skip("Cannot generate an ECDSA key pair (%08lx)\n", (ULONG)Status);
        BCryptCloseAlgorithmProvider(Alg, 0);
        return;
    }

    Size = 0;
    Ret = pExport(Key, X509_ASN_ENCODING, NULL, 0, NULL, NULL, &Size);
    ok(Ret, "Size query failed, error %lu\n", GetLastError());
    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size + 16);
    Info = (PCERT_PUBLIC_KEY_INFO)Buffer;
    Size2 = Size;
    Ret = pExport(Key, X509_ASN_ENCODING, NULL, 0, NULL, Info, &Size2);
    ok(Ret, "Export failed, error %lu\n", GetLastError());
    if (Ret)
    {
        static const BYTE P256[] = { 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };

        ok(Info->Algorithm.pszObjId && !strcmp(Info->Algorithm.pszObjId, "1.2.840.10045.2.1"),
           "OID is '%s'\n", Info->Algorithm.pszObjId);
        ok(Info->Algorithm.Parameters.cbData == sizeof(P256) &&
           !memcmp(Info->Algorithm.Parameters.pbData, P256, sizeof(P256)), "Curve parameters are wrong\n");
        ok(Info->PublicKey.cbData == 65 && Info->PublicKey.pbData[0] == 4 && Info->PublicKey.cUnusedBits == 0,
           "Key is %lu bytes\n", Info->PublicKey.cbData);
    }
    HeapFree(GetProcessHeap(), 0, Buffer);
    BCryptDestroyKey(Key);
    BCryptCloseAlgorithmProvider(Alg, 0);
}

START_TEST(CryptExportPublicKeyInfoFromBCryptKeyHandle)
{
    PFN_Export pExport;
    BCRYPT_ALG_HANDLE Alg;
    BCRYPT_KEY_HANDLE Key;
    BYTE Secret[16] = { 1, 2, 3, 4 };
    DWORD Size, Error;
    BOOL Ret;

    pExport = (PFN_Export)GetProcAddress(LoadLibraryW(L"crypt32.dll"),
                                         "CryptExportPublicKeyInfoFromBCryptKeyHandle");
    ok(pExport != NULL, "CryptExportPublicKeyInfoFromBCryptKeyHandle is not exported by crypt32.dll\n");
    if (!pExport)
    {
        skip("CryptExportPublicKeyInfoFromBCryptKeyHandle is not exported\n");
        return;
    }

    /* A bad handle comes back as its NTSTATUS */
    Size = 0;
    SetLastError(0xdeadbeef);
    Ret = pExport(NULL, X509_ASN_ENCODING, NULL, 0, NULL, NULL, &Size);
    Error = GetLastError();
    ok(!Ret, "A NULL key succeeded\n");
    ok(Error == STATUS_INVALID_HANDLE_VALUE, "Error is %08lx\n", Error);

    /* A symmetric key has no public key info */
    if (BCryptOpenAlgorithmProvider(&Alg, BCRYPT_AES_ALGORITHM, NULL, 0) == 0)
    {
        if (BCryptGenerateSymmetricKey(Alg, &Key, NULL, 0, Secret, sizeof(Secret), 0) == 0)
        {
            Size = 0;
            SetLastError(0xdeadbeef);
            Ret = pExport(Key, X509_ASN_ENCODING, NULL, 0, NULL, NULL, &Size);
            Error = GetLastError();
            ok(!Ret, "An AES key succeeded\n");
            ok(Error == CRYPT_E_UNKNOWN_ALGO_VALUE, "Error is %08lx\n", Error);
            BCryptDestroyKey(Key);
        }
        else
        {
            skip("No AES keys\n");
        }
        BCryptCloseAlgorithmProvider(Alg, 0);
    }
    else
    {
        skip("No AES provider\n");
    }

    TestRsa(pExport);
    TestEcc(pExport);
}
