/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for bcrypt key handles: algorithm name property and export blob types
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2 (x64 and WOW64). CryptExportPublicKeyInfoFromBCryptKeyHandle has to tell
 * RSA, ECC and symmetric keys apart, and exporting a key as the blob of another kind of key must not
 * touch the key as that kind of key.
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <bcrypt.h>

#define STATUS_NOT_SUPPORTED_VALUE ((NTSTATUS)0xC00000BB)

static void CheckKey(BCRYPT_KEY_HANDLE Key, PCWSTR ExpectedName, BOOL Rsa, BOOL Ecc, BOOL Data)
{
    WCHAR Name[40];
    ULONG Size, Length;
    NTSTATUS Status;

    Size = 0;
    Status = BCryptGetProperty(Key, BCRYPT_ALGORITHM_NAME, (PUCHAR)Name, sizeof(Name), &Size, 0);
    ok(Status == 0, "%ls: BCryptGetProperty(ALGORITHM_NAME) returned %08lx\n", ExpectedName, (ULONG)Status);
    if (Status == 0)
        ok(!wcscmp(Name, ExpectedName), "Algorithm name of the key is '%ls', expected '%ls'\n", Name, ExpectedName);

    Length = 0;
    Status = BCryptExportKey(Key, NULL, BCRYPT_RSAPUBLIC_BLOB, NULL, 0, &Length, 0);
    ok(Status == (Rsa ? 0 : STATUS_NOT_SUPPORTED_VALUE), "%ls: RSAPUBLICBLOB returned %08lx\n", ExpectedName, (ULONG)Status);
    Length = 0;
    Status = BCryptExportKey(Key, NULL, BCRYPT_ECCPUBLIC_BLOB, NULL, 0, &Length, 0);
    ok(Status == (Ecc ? 0 : STATUS_NOT_SUPPORTED_VALUE), "%ls: ECCPUBLICBLOB returned %08lx\n", ExpectedName, (ULONG)Status);
    Length = 0;
    Status = BCryptExportKey(Key, NULL, BCRYPT_DH_PUBLIC_BLOB, NULL, 0, &Length, 0);
    ok(Status == STATUS_NOT_SUPPORTED_VALUE, "%ls: DHPUBLICBLOB returned %08lx\n", ExpectedName, (ULONG)Status);
    Length = 0;
    Status = BCryptExportKey(Key, NULL, BCRYPT_DSA_PUBLIC_BLOB, NULL, 0, &Length, 0);
    ok(Status == STATUS_NOT_SUPPORTED_VALUE, "%ls: DSAPUBLICBLOB returned %08lx\n", ExpectedName, (ULONG)Status);
    Length = 0;
    Status = BCryptExportKey(Key, NULL, BCRYPT_KEY_DATA_BLOB, NULL, 0, &Length, 0);
    ok(Status == (Data ? 0 : STATUS_NOT_SUPPORTED_VALUE), "%ls: KEYDATABLOB returned %08lx\n", ExpectedName, (ULONG)Status);
}

START_TEST(BCryptKeyExport)
{
    BCRYPT_ALG_HANDLE Alg;
    BCRYPT_KEY_HANDLE Key;
    BYTE Secret[16] = { 1, 2, 3 };
    NTSTATUS Status;

    if (BCryptOpenAlgorithmProvider(&Alg, BCRYPT_RSA_ALGORITHM, NULL, 0) == 0)
    {
        Status = BCryptGenerateKeyPair(Alg, &Key, 1024, 0);
        if (Status == 0)
            Status = BCryptFinalizeKeyPair(Key, 0);
        if (Status == 0)
        {
            CheckKey(Key, BCRYPT_RSA_ALGORITHM, TRUE, FALSE, FALSE);
            BCryptDestroyKey(Key);
        }
        else
        {
            skip("No RSA key pairs (%08lx)\n", (ULONG)Status);
        }
        BCryptCloseAlgorithmProvider(Alg, 0);
    }
    else
    {
        skip("No RSA provider\n");
    }

    if (BCryptOpenAlgorithmProvider(&Alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) == 0)
    {
        Status = BCryptGenerateKeyPair(Alg, &Key, 256, 0);
        if (Status == 0)
            Status = BCryptFinalizeKeyPair(Key, 0);
        if (Status == 0)
        {
            CheckKey(Key, BCRYPT_ECDSA_P256_ALGORITHM, FALSE, TRUE, FALSE);
            BCryptDestroyKey(Key);
        }
        else
        {
            skip("No ECDSA key pairs (%08lx)\n", (ULONG)Status);
        }
        BCryptCloseAlgorithmProvider(Alg, 0);
    }
    else
    {
        skip("No ECDSA provider\n");
    }

    if (BCryptOpenAlgorithmProvider(&Alg, BCRYPT_AES_ALGORITHM, NULL, 0) == 0)
    {
        Status = BCryptGenerateSymmetricKey(Alg, &Key, NULL, 0, Secret, sizeof(Secret), 0);
        if (Status == 0)
        {
            CheckKey(Key, BCRYPT_AES_ALGORITHM, FALSE, FALSE, TRUE);
            BCryptDestroyKey(Key);
        }
        else
        {
            skip("No AES keys (%08lx)\n", (ULONG)Status);
        }
        BCryptCloseAlgorithmProvider(Alg, 0);
    }
    else
    {
        skip("No AES provider\n");
    }
}
