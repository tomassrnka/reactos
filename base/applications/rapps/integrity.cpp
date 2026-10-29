/*
 * PROJECT:     ReactOS Applications Manager
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Various integrity check mechanisms
 * COPYRIGHT:   Copyright Ismael Ferreras Morezuelas (swyterzone+ros@gmail.com)
 *              Copyright 2016 Mark Jansen <mark.jansen@reactos.org>
 */

#include "rapps.h"

#include <sha1.h>
#include <wincrypt.h>

BOOL
VerifyInteg(LPCWSTR lpSHA1Hash, LPCWSTR lpFileName)
{
    BOOL ret = FALSE;

    /* first off, does it exist at all? */
    HANDLE file = CreateFileW(lpFileName, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);

    if (file == INVALID_HANDLE_VALUE)
        return FALSE;

    /* let's grab the actual file size to organize the mmap'ing rounds */
    LARGE_INTEGER size;
    GetFileSizeEx(file, &size);

    /* retrieve a handle to map the file contents to memory */
    HANDLE map = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (map)
    {
        /* map that thing in address space */
        const unsigned char *file_map = static_cast<const unsigned char *>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
        if (file_map)
        {
            SHA_CTX ctx;
            /* initialize the SHA-1 context */
            A_SHAInit(&ctx);

            /* feed the data to the cookie monster */
            A_SHAUpdate(&ctx, file_map, size.LowPart);

            /* cool, we don't need this anymore */
            UnmapViewOfFile(file_map);

            /* we're done, compute the final hash */
            ULONG sha[5];
            A_SHAFinal(&ctx, sha);

            WCHAR buf[(sizeof(sha) * 2) + 1];
            for (UINT i = 0; i < sizeof(sha); i++)
                _swprintf(buf + 2 * i, L"%02x", ((unsigned char *)sha)[i]);
            /* does the resulting SHA1 match with the provided one? */
            if (!_wcsicmp(buf, lpSHA1Hash))
                ret = TRUE;
        }
        CloseHandle(map);
    }
    CloseHandle(file);
    return ret;
}

/* SHA-256 check for packages whose publisher only offers SHA-256 (RAPPS entry key "SHA256").
 * Fail-closed: any error (no crypto provider, read error, mismatch) is "not verified". */
BOOL
VerifyInteg256(LPCWSTR lpSHA256Hash, LPCWSTR lpFileName)
{
    BOOL ret = FALSE;
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    BYTE buf[64 * 1024];
    BYTE digest[32];
    DWORD cb, cbDigest = sizeof(digest);
    BOOL readOk = TRUE;

    HANDLE file = CreateFileW(lpFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;

    if (CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash))
    {
        for (;;)
        {
            if (!ReadFile(file, buf, sizeof(buf), &cb, NULL))
            {
                readOk = FALSE;
                break;
            }
            if (!cb)
                break;
            if (!CryptHashData(hash, buf, cb, 0))
            {
                readOk = FALSE;
                break;
            }
        }
        if (readOk && CryptGetHashParam(hash, HP_HASHVAL, digest, &cbDigest, 0) && cbDigest == sizeof(digest))
        {
            WCHAR hex[sizeof(digest) * 2 + 1];
            for (UINT i = 0; i < sizeof(digest); i++)
                _swprintf(hex + 2 * i, L"%02x", digest[i]);
            if (!_wcsicmp(hex, lpSHA256Hash))
                ret = TRUE;
        }
    }
    if (hash)
        CryptDestroyHash(hash);
    if (prov)
        CryptReleaseContext(prov, 0);
    CloseHandle(file);
    return ret;
}
