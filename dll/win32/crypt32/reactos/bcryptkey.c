/*
 * PROJECT:     ReactOS crypt32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Public key export from CNG (BCrypt) key handles
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#include <windef.h>
#include <winbase.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <wine/debug.h>
#include <crypt32_private.h>

WINE_DEFAULT_DEBUG_CHANNEL(crypt);

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) ((LONG)(status) >= 0)
#endif
#ifndef STATUS_INVALID_HANDLE
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008)
#endif
#ifndef STATUS_INVALID_PARAMETER_VALUE
#define STATUS_INVALID_PARAMETER_VALUE ((NTSTATUS)0xC000000D)
#endif

#define ECC_MAGIC_P256_ECDSA 0x31534345
#define ECC_MAGIC_P256_ECDH  0x314B4345
#define ECC_MAGIC_P384_ECDSA 0x33534345
#define ECC_MAGIC_P384_ECDH  0x334B4345
#define ECC_MAGIC_P521_ECDSA 0x35534345
#define ECC_MAGIC_P521_ECDH  0x354B4345

static const char ecc_public_key_oid[] = "1.2.840.10045.2.1";
/* DER encodings of the named curve OIDs: prime256v1, secp384r1, secp521r1 */
static const BYTE curve_p256[] = { 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };
static const BYTE curve_p384[] = { 0x06, 0x05, 0x2b, 0x81, 0x04, 0x00, 0x22 };
static const BYTE curve_p521[] = { 0x06, 0x05, 0x2b, 0x81, 0x04, 0x00, 0x23 };

#define ALIGN8(x) (((x) + 7) & ~(DWORD)7)

/* Lays out CERT_PUBLIC_KEY_INFO, the algorithm OID, the parameters and the key bits in one buffer,
 * each part starting on an 8 byte boundary as on Windows. */
static BOOL fill_public_key_info(const char *oid, const BYTE *params, DWORD params_len,
                                 const BYTE *key, DWORD key_len,
                                 PCERT_PUBLIC_KEY_INFO info, DWORD *size)
{
    DWORD oid_len = ALIGN8(strlen(oid) + 1), par_len = ALIGN8(params_len);
    DWORD needed = ALIGN8(sizeof(CERT_PUBLIC_KEY_INFO)) + oid_len + par_len + key_len;
    BYTE *ptr;

    if (!info)
    {
        *size = needed;
        return TRUE;
    }
    if (*size < needed)
    {
        *size = needed;
        SetLastError(ERROR_MORE_DATA);
        return FALSE;
    }
    *size = needed;

    ptr = (BYTE *)info + ALIGN8(sizeof(CERT_PUBLIC_KEY_INFO));
    info->Algorithm.pszObjId = (char *)ptr;
    lstrcpyA(info->Algorithm.pszObjId, oid);
    ptr += oid_len;

    info->Algorithm.Parameters.cbData = params_len;
    info->Algorithm.Parameters.pbData = params_len ? ptr : NULL;
    if (params_len)
    {
        memcpy(ptr, params, params_len);
        ptr += par_len;
    }

    info->PublicKey.cbData = key_len;
    info->PublicKey.cUnusedBits = 0;
    info->PublicKey.pbData = ptr;
    memcpy(ptr, key, key_len);
    return TRUE;
}

static BOOL export_rsa(const BCRYPT_RSAKEY_BLOB *rsa, ULONG blob_len, DWORD enc, const char *oid,
                       PCERT_PUBLIC_KEY_INFO info, DWORD *size)
{
    const BYTE *exp_be, *mod_be;
    BYTE *capi = NULL, *der = NULL;
    DWORD capi_len, der_len = 0, i;
    BLOBHEADER *hdr;
    RSAPUBKEY *pub;
    BOOL ret = FALSE;

    if (blob_len < sizeof(*rsa) || rsa->cbPublicExp > 4 || !rsa->cbPublicExp || !rsa->cbModulus ||
        blob_len < sizeof(*rsa) + rsa->cbPublicExp + rsa->cbModulus)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    exp_be = (const BYTE *)(rsa + 1);
    mod_be = exp_be + rsa->cbPublicExp;

    /* The CAPI PUBLICKEYBLOB holds the modulus little endian, which CryptEncodeObject turns into RSAPublicKey */
    capi_len = sizeof(*hdr) + sizeof(*pub) + rsa->cbModulus;
    capi = CryptMemAlloc(capi_len);
    if (!capi)
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    hdr = (BLOBHEADER *)capi;
    hdr->bType = PUBLICKEYBLOB;
    hdr->bVersion = CUR_BLOB_VERSION;
    hdr->reserved = 0;
    hdr->aiKeyAlg = CALG_RSA_KEYX;
    pub = (RSAPUBKEY *)(hdr + 1);
    pub->magic = 0x31415352; /* "RSA1" */
    pub->bitlen = rsa->BitLength;
    pub->pubexp = 0;
    for (i = 0; i < rsa->cbPublicExp; i++)
        pub->pubexp = (pub->pubexp << 8) | exp_be[i];
    for (i = 0; i < rsa->cbModulus; i++)
        capi[sizeof(*hdr) + sizeof(*pub) + i] = mod_be[rsa->cbModulus - 1 - i];

    if (CryptEncodeObject(enc, RSA_CSP_PUBLICKEYBLOB, capi, NULL, &der_len))
    {
        der = CryptMemAlloc(der_len);
        if (der && CryptEncodeObject(enc, RSA_CSP_PUBLICKEYBLOB, capi, der, &der_len))
            ret = fill_public_key_info(oid ? oid : szOID_RSA_RSA, NULL, 0, der, der_len, info, size);
        else if (!der)
            SetLastError(ERROR_OUTOFMEMORY);
    }
    CryptMemFree(der);
    CryptMemFree(capi);
    return ret;
}

static BOOL export_ecc(const BCRYPT_ECCKEY_BLOB *ecc, ULONG blob_len, const char *oid,
                       PCERT_PUBLIC_KEY_INFO info, DWORD *size)
{
    const BYTE *params;
    DWORD params_len, key_len;
    BYTE *key;
    BOOL ret;

    if (blob_len < sizeof(*ecc) || !ecc->cbKey || blob_len < sizeof(*ecc) + 2 * ecc->cbKey)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    switch (ecc->dwMagic)
    {
    case ECC_MAGIC_P256_ECDSA: case ECC_MAGIC_P256_ECDH: params = curve_p256; params_len = sizeof(curve_p256); break;
    case ECC_MAGIC_P384_ECDSA: case ECC_MAGIC_P384_ECDH: params = curve_p384; params_len = sizeof(curve_p384); break;
    case ECC_MAGIC_P521_ECDSA: case ECC_MAGIC_P521_ECDH: params = curve_p521; params_len = sizeof(curve_p521); break;
    default:
        SetLastError(NTE_BAD_ALGID);
        return FALSE;
    }

    /* Uncompressed point: 0x04 | X | Y, both coordinates big endian as in the CNG blob */
    key_len = 1 + 2 * ecc->cbKey;
    key = CryptMemAlloc(key_len);
    if (!key)
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    key[0] = 0x04;
    memcpy(key + 1, ecc + 1, 2 * ecc->cbKey);
    ret = fill_public_key_info(oid ? oid : ecc_public_key_oid, params, params_len, key, key_len, info, size);
    CryptMemFree(key);
    return ret;
}

BOOL WINAPI CryptExportPublicKeyInfoFromBCryptKeyHandle(BCRYPT_KEY_HANDLE key, DWORD enc, LPSTR oid,
                                                        DWORD flags, void *aux,
                                                        PCERT_PUBLIC_KEY_INFO info, DWORD *size)
{
    static const LPCWSTR blob_types[] = { BCRYPT_RSAPUBLIC_BLOB, BCRYPT_ECCPUBLIC_BLOB };
    ULONG blob_len = 0, got = 0;
    NTSTATUS status = STATUS_INVALID_PARAMETER_VALUE;
    BYTE *blob;
    BOOL ret;
    UINT i;

    TRACE("(%p, %08lx, %s, %08lx, %p, %p, %p)\n", key, enc, debugstr_a(oid), flags, aux, info, size);

    if (!size)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* The key says what it is by the public blob it can export */
    for (i = 0; i < (sizeof(blob_types) / sizeof(blob_types[0])); i++)
    {
        blob_len = 0;
        status = BCryptExportKey(key, NULL, blob_types[i], NULL, 0, &blob_len, 0);
        if (NT_SUCCESS(status) || status == STATUS_INVALID_HANDLE)
            break;
    }
    if (status == STATUS_INVALID_HANDLE)
    {
        /* A bad handle comes back as its NTSTATUS */
        SetLastError(status);
        return FALSE;
    }
    if (!NT_SUCCESS(status))
    {
        /* Not an asymmetric key that has public key info */
        SetLastError(CRYPT_E_UNKNOWN_ALGO);
        return FALSE;
    }

    blob = CryptMemAlloc(blob_len);
    if (!blob)
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    status = BCryptExportKey(key, NULL, blob_types[i], blob, blob_len, &got, 0);
    if (!NT_SUCCESS(status))
    {
        CryptMemFree(blob);
        SetLastError(status);
        return FALSE;
    }
    if (i == 0)
        ret = export_rsa((const BCRYPT_RSAKEY_BLOB *)blob, got, enc, oid, info, size);
    else
        ret = export_ecc((const BCRYPT_ECCKEY_BLOB *)blob, got, oid, info, size);
    CryptMemFree(blob);
    return ret;
}
