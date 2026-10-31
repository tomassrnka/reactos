/*
 * PKCS #12 (PFXImportCertStore, PFXExportCertStoreEx, PFXVerifyPassword) tests
 *
 * Copyright 2026 Tomas Srnka
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdio.h>
#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winerror.h>
#include <wincrypt.h>

#include "wine/test.h"

/* The fixtures come from gen_pkcs12_fixtures.py: openssl 3 (PBES2/AES-256 + HMAC-SHA256, and the
 * legacy RC2-40/3DES + HMAC-SHA1 set), python-cryptography (no MAC, no encryption) and, for
 * 'windows', Windows 10's own PFXExportCertStoreEx. All keys are throw-away test keys. */
#include "pkcs12_fixtures.h"

#ifndef PKCS12_NO_PERSIST_KEY
#define PKCS12_NO_PERSIST_KEY 0x8000
#endif
#ifndef REPORT_NO_PRIVATE_KEY
#define REPORT_NO_PRIVATE_KEY 0x1
#define REPORT_NOT_ABLE_TO_EXPORT_PRIVATE_KEY 0x2
#define EXPORT_PRIVATE_KEYS 0x4
#endif
#ifndef PKCS12_DISABLE_ENCRYPT_CERTIFICATES
#define PKCS12_DISABLE_ENCRYPT_CERTIFICATES 0x100
#endif
#ifndef ERROR_INVALID_PASSWORD
#define ERROR_INVALID_PASSWORD 86
#endif

static const WCHAR pw[] = {'p','w',0};
static const WCHAR pw2[] = {'s','e','c','o','n','d',0};
static const WCHAR bad[] = {'b','a','d',0};
static const WCHAR empty[] = {0};

#define LEAF_NAME "ReactOS PKCS12 test"
#define NO_PERSIST_EXP (CRYPT_EXPORTABLE | PKCS12_NO_PERSIST_KEY)

struct fixture
{
    const char *name;
    const BYTE *data;
    DWORD len;
    const WCHAR *password;
    int ncerts;
    BOOL friendly;
};

#define FIX(n, pwd, count, fr) { #n, pfx_##n, sizeof(pfx_##n), pwd, count, fr }

static const struct fixture fixtures[] =
{
    FIX(aes256sha256, pw, 2, TRUE),
    FIX(legacy, pw, 2, TRUE),
    FIX(des3, pw, 1, FALSE),
    FIX(emptypw, empty, 1, FALSE),
    FIX(nomac, pw, 1, FALSE),
    FIX(plain, NULL, 1, TRUE),
    FIX(itermax, pw, 1, FALSE),
    FIX(windows, pw, 2, TRUE),
};

static PCCERT_CONTEXT find_leaf(HCERTSTORE store)
{
    PCCERT_CONTEXT cert = NULL;
    char name[128];

    while ((cert = CertEnumCertificatesInStore(store, cert)))
    {
        name[0] = 0;
        CertGetNameStringA(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, sizeof(name));
        if (!strcmp(name, LEAF_NAME)) return cert;
    }
    return NULL;
}

static int count_certs(HCERTSTORE store)
{
    PCCERT_CONTEXT cert = NULL;
    int n = 0;
    while ((cert = CertEnumCertificatesInStore(store, cert))) n++;
    return n;
}

/* sign with the key and verify with the certificate's public key */
static BOOL sign_and_verify(HCRYPTPROV prov, DWORD spec, PCCERT_CONTEXT cert)
{
    BYTE sig[512];
    DWORD len = sizeof(sig);
    HCRYPTHASH hash;
    HCRYPTKEY pub;
    BOOL ret = FALSE;

    if (!CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) return FALSE;
    CryptHashData(hash, (const BYTE *)"abc", 3, 0);
    if (CryptSignHashA(hash, spec, NULL, 0, sig, &len))
    {
        HCRYPTHASH hash2;
        CryptDestroyHash(hash);
        hash = 0;
        if (CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash2))
        {
            CryptHashData(hash2, (const BYTE *)"abc", 3, 0);
            if (CryptImportPublicKeyInfo(prov, X509_ASN_ENCODING, &cert->pCertInfo->SubjectPublicKeyInfo, &pub))
            {
                ret = CryptVerifySignatureA(hash2, sig, len, pub, NULL, 0);
                CryptDestroyKey(pub);
            }
            CryptDestroyHash(hash2);
        }
    }
    if (hash) CryptDestroyHash(hash);
    return ret;
}

/* Does the certificate have a usable private key? Reports through the key context (ephemeral
 * import) or the key provider info (persisted). */
static BOOL cert_has_working_key(PCCERT_CONTEXT cert, BOOL *persisted, BOOL delete_key)
{
    BYTE buf[512];
    DWORD size = sizeof(buf);
    BOOL ret = FALSE;

    *persisted = FALSE;
    if (CertGetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, buf, &size))
    {
        CRYPT_KEY_PROV_INFO *info = (CRYPT_KEY_PROV_INFO *)buf;
        HCRYPTPROV prov;

        *persisted = TRUE;
        /* Windows uses the type 1 provider, ReactOS the one with SHA-2 */
        ok(info->dwProvType == PROV_RSA_FULL || info->dwProvType == PROV_RSA_AES, "got provider type %lu\n", info->dwProvType);
        ok(info->dwKeySpec == AT_KEYEXCHANGE, "got key spec %lu\n", info->dwKeySpec);
        if (CryptAcquireContextW(&prov, info->pwszContainerName, info->pwszProvName, info->dwProvType, info->dwFlags))
        {
            ret = sign_and_verify(prov, info->dwKeySpec, cert);
            CryptReleaseContext(prov, 0);
            if (delete_key)
            {
                ok(CryptAcquireContextW(&prov, info->pwszContainerName, info->pwszProvName, info->dwProvType,
                                        info->dwFlags | CRYPT_DELETEKEYSET), "delete failed %08lx\n", GetLastError());
            }
        }
        return ret;
    }
    size = sizeof(buf);
    if (CertGetCertificateContextProperty(cert, CERT_KEY_CONTEXT_PROP_ID, buf, &size))
    {
        CERT_KEY_CONTEXT *kc = (CERT_KEY_CONTEXT *)buf;
        ok(kc->dwKeySpec == AT_KEYEXCHANGE, "got key spec %lu\n", kc->dwKeySpec);
        return kc->hCryptProv && sign_and_verify(kc->hCryptProv, kc->dwKeySpec, cert);
    }
    return FALSE;
}

static void check_store(const char *name, HCERTSTORE store, int ncerts, BOOL has_key, BOOL friendly, BOOL delete_key)
{
    PCCERT_CONTEXT leaf, cert = NULL;
    BOOL persisted;

    ok(count_certs(store) == ncerts, "%s: got %d certificates\n", name, count_certs(store));
    leaf = find_leaf(store);
    ok(leaf != NULL, "%s: leaf certificate not found\n", name);
    if (!leaf) return;
    if (has_key)
    {
        ok(cert_has_working_key(leaf, &persisted, delete_key), "%s: the key does not work (%08lx)\n", name, GetLastError());
    }
    else
    {
        BYTE buf[64];
        DWORD size = sizeof(buf);
        ok(!CertGetCertificateContextProperty(leaf, CERT_KEY_CONTEXT_PROP_ID, buf, &size) &&
           !CertGetCertificateContextProperty(leaf, CERT_KEY_PROV_INFO_PROP_ID, buf, &size),
           "%s: unexpected key\n", name);
    }
    if (friendly)
    {
        WCHAR str[64];
        DWORD size = sizeof(str);
        BOOL ret = CertGetCertificateContextProperty(leaf, CERT_FRIENDLY_NAME_PROP_ID, str, &size);
        ok(ret, "%s: no friendly name\n", name);
    }
    /* only the leaf has a key */
    while ((cert = CertEnumCertificatesInStore(store, cert)))
    {
        BYTE buf[64];
        DWORD size = sizeof(buf);
        if (cert == leaf || cert->cbCertEncoded == leaf->cbCertEncoded) continue;
        ok(!CertGetCertificateContextProperty(cert, CERT_KEY_PROV_INFO_PROP_ID, buf, &size) &&
           !CertGetCertificateContextProperty(cert, CERT_KEY_CONTEXT_PROP_ID, buf, &size),
           "%s: key attached to another certificate\n", name);
    }
    CertFreeCertificateContext(leaf);
}

static void test_import(void)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(fixtures); i++)
    {
        const struct fixture *f = &fixtures[i];
        CRYPT_DATA_BLOB blob = { f->len, (BYTE *)f->data };
        HCERTSTORE store;
        BOOL ret;

        ok(PFXIsPFXBlob(&blob), "%s: not recognized\n", f->name);
        ret = PFXVerifyPassword(&blob, f->password, 0);
        ok(ret, "%s: PFXVerifyPassword failed\n", f->name);

        SetLastError(0xdeadbeef);
        store = PFXImportCertStore(&blob, f->password, NO_PERSIST_EXP);
        ok(store != NULL, "%s: import failed %08lx\n", f->name, GetLastError());
        if (store)
        {
            check_store(f->name, store, f->ncerts, TRUE, f->friendly, FALSE);
            CertCloseStore(store, 0);
        }

        /* the key goes into a container */
        store = PFXImportCertStore(&blob, f->password, CRYPT_EXPORTABLE | CRYPT_USER_KEYSET);
        ok(store != NULL, "%s: persisting import failed %08lx\n", f->name, GetLastError());
        if (store)
        {
            check_store(f->name, store, f->ncerts, TRUE, f->friendly, TRUE);
            CertCloseStore(store, 0);
        }
    }
}

static void test_passwords(void)
{
    CRYPT_DATA_BLOB blob;
    HCERTSTORE store;
    BOOL ret;

    blob.pbData = (BYTE *)pfx_aes256sha256; blob.cbData = sizeof(pfx_aes256sha256);
    ret = PFXVerifyPassword(&blob, bad, 0);
    ok(!ret, "wrong password accepted\n");
    ret = PFXVerifyPassword(&blob, NULL, 0);
    ok(!ret, "NULL password accepted\n");
    SetLastError(0xdeadbeef);
    store = PFXImportCertStore(&blob, bad, NO_PERSIST_EXP);
    ok(!store && GetLastError() == ERROR_INVALID_PASSWORD, "got %p, %08lx\n", store, GetLastError());
    SetLastError(0xdeadbeef);
    store = PFXImportCertStore(&blob, NULL, NO_PERSIST_EXP);
    ok(!store && GetLastError() == ERROR_INVALID_PASSWORD, "got %p, %08lx\n", store, GetLastError());

    /* a MAC made from the empty password is found with NULL as well as with "" */
    blob.pbData = (BYTE *)pfx_emptypw; blob.cbData = sizeof(pfx_emptypw);
    ok(PFXVerifyPassword(&blob, NULL, 0), "NULL password failed\n");
    ok(PFXVerifyPassword(&blob, empty, 0), "empty password failed\n");
    ok(!PFXVerifyPassword(&blob, bad, 0), "wrong password accepted\n");
    store = PFXImportCertStore(&blob, NULL, NO_PERSIST_EXP);
    ok(store != NULL, "import with NULL password failed %08lx\n", GetLastError());
    if (store) CertCloseStore(store, 0);
    store = PFXImportCertStore(&blob, empty, NO_PERSIST_EXP);
    ok(store != NULL, "import with empty password failed %08lx\n", GetLastError());
    if (store) CertCloseStore(store, 0);

    /* no MAC and nothing encrypted: a password cannot be right */
    blob.pbData = (BYTE *)pfx_plain; blob.cbData = sizeof(pfx_plain);
    ok(PFXVerifyPassword(&blob, NULL, 0), "NULL password failed\n");
    ok(PFXVerifyPassword(&blob, empty, 0), "empty password failed\n");
    ok(!PFXVerifyPassword(&blob, pw, 0), "password accepted\n");
    SetLastError(0xdeadbeef);
    store = PFXImportCertStore(&blob, pw, NO_PERSIST_EXP);
    ok(!store && GetLastError() == ERROR_INVALID_PASSWORD, "got %p, %08lx\n", store, GetLastError());

    /* no MAC but encrypted: the padding tells */
    blob.pbData = (BYTE *)pfx_nomac; blob.cbData = sizeof(pfx_nomac);
    SetLastError(0xdeadbeef);
    store = PFXImportCertStore(&blob, bad, NO_PERSIST_EXP);
    ok(!store && GetLastError() == NTE_BAD_DATA, "got %p, %08lx\n", store, GetLastError());

    /* Windows 10 refuses more than 600000 iterations (600000 itself works, see the fixtures) */
    blob.pbData = (BYTE *)pfx_iterover; blob.cbData = sizeof(pfx_iterover);
    ok(PFXIsPFXBlob(&blob), "not recognized\n");
    ok(!PFXVerifyPassword(&blob, pw, 0), "too many iterations accepted\n");
    SetLastError(0xdeadbeef);
    store = PFXImportCertStore(&blob, pw, NO_PERSIST_EXP);
    ok(!store && GetLastError() == CRYPT_E_BAD_ENCODE, "got %p, %08lx\n", store, GetLastError());

    /* tampered MAC */
    blob.pbData = (BYTE *)pfx_badmac; blob.cbData = sizeof(pfx_badmac);
    ok(PFXIsPFXBlob(&blob), "not recognized\n");
    ok(!PFXVerifyPassword(&blob, pw, 0), "tampered MAC accepted\n");
    SetLastError(0xdeadbeef);
    store = PFXImportCertStore(&blob, pw, NO_PERSIST_EXP);
    ok(!store && GetLastError() == ERROR_INVALID_PASSWORD, "got %p, %08lx\n", store, GetLastError());
}

static void test_malformed(void)
{
    CRYPT_DATA_BLOB blob;
    HCERTSTORE store;
    BYTE junk[300];
    DWORD i;

    SetLastError(0xdeadbeef);
    store = PFXImportCertStore(NULL, pw, 0);
    ok(!store && GetLastError() == ERROR_INVALID_PARAMETER, "got %p, %08lx\n", store, GetLastError());

    blob.pbData = (BYTE *)pfx_legacy; blob.cbData = sizeof(pfx_legacy);
    SetLastError(0xdeadbeef);
    store = PFXImportCertStore(&blob, pw, 0x4);
    ok(!store && GetLastError() == ERROR_INVALID_PARAMETER, "got %p, %08lx\n", store, GetLastError());

    for (i = 0; i < sizeof(junk); i++) junk[i] = (BYTE)(i * 7 + 3);
    blob.pbData = junk; blob.cbData = sizeof(junk);
    ok(!PFXIsPFXBlob(&blob), "junk recognized\n");
    ok(!PFXVerifyPassword(&blob, pw, 0), "junk accepted\n");
    SetLastError(0xdeadbeef);
    store = PFXImportCertStore(&blob, pw, NO_PERSIST_EXP);
    ok(!store && GetLastError() == CRYPT_E_BAD_ENCODE, "got %p, %08lx\n", store, GetLastError());

    /* truncated at various places, none must crash or succeed */
    for (i = 0; i < sizeof(pfx_legacy); i += 97)
    {
        blob.pbData = (BYTE *)pfx_legacy; blob.cbData = i;
        store = PFXImportCertStore(&blob, pw, NO_PERSIST_EXP);
        ok(!store, "truncated at %lu imported\n", i);
        if (store) CertCloseStore(store, 0);
    }
}

static void test_export_roundtrip(const char *name, DWORD import_flags, DWORD export_flags, const WCHAR *export_pw,
                                  BOOL expect_ok, DWORD expect_err, BOOL expect_key)
{
    CRYPT_DATA_BLOB src = { sizeof(pfx_aes256sha256), (BYTE *)pfx_aes256sha256 }, out = {0}, small;
    HCERTSTORE store, store2;
    DWORD size;
    BOOL ret;

    store = PFXImportCertStore(&src, pw, import_flags);
    ok(store != NULL, "%s: import failed %08lx\n", name, GetLastError());
    if (!store) return;

    SetLastError(0xdeadbeef);
    ret = PFXExportCertStoreEx(store, &out, export_pw, NULL, export_flags);
    if (!expect_ok)
    {
        ok(!ret && GetLastError() == expect_err, "%s: got %d, %08lx\n", name, ret, GetLastError());
        CertCloseStore(store, 0);
        return;
    }
    ok(ret, "%s: size query failed %08lx\n", name, GetLastError());
    ok(out.cbData > 0, "%s: no size\n", name);
    if (!ret) { CertCloseStore(store, 0); return; }
    size = out.cbData;
    out.pbData = HeapAlloc(GetProcessHeap(), 0, size);

    small = out;
    small.cbData = size - 1;
    SetLastError(0xdeadbeef);
    ret = PFXExportCertStoreEx(store, &small, export_pw, NULL, export_flags);
    /* Windows fails without setting an error code, and leaves the size alone */
    ok(!ret && GetLastError() == 0, "%s: got %d, %08lx\n", name, ret, GetLastError());
    ok(small.cbData == size - 1 || small.cbData == size, "%s: size changed to %lu\n", name, small.cbData);
    trace("small buffer: size now %lu (needed %lu)\n", small.cbData, size);

    ret = PFXExportCertStoreEx(store, &out, export_pw, NULL, export_flags);
    ok(ret, "%s: export failed %08lx\n", name, GetLastError());
    ok(out.cbData == size, "%s: size %lu, expected %lu\n", name, out.cbData, size);
    ok(PFXIsPFXBlob(&out), "%s: result is not a PFX\n", name);
    ok(PFXVerifyPassword(&out, export_pw, 0), "%s: password does not verify\n", name);
    ok(!PFXVerifyPassword(&out, bad, 0), "%s: wrong password verifies\n", name);

    store2 = PFXImportCertStore(&out, export_pw, NO_PERSIST_EXP);
    ok(store2 != NULL, "%s: re-import failed %08lx\n", name, GetLastError());
    if (store2)
    {
        check_store(name, store2, 2, expect_key, TRUE, FALSE);
        CertCloseStore(store2, 0);
    }
    HeapFree(GetProcessHeap(), 0, out.pbData);
    CertCloseStore(store, 0);
}

static void test_export(void)
{
    CRYPT_DATA_BLOB blob = {0};
    HCERTSTORE store;
    BOOL ret;

    test_export_roundtrip("keys", NO_PERSIST_EXP, EXPORT_PRIVATE_KEYS, pw2, TRUE, 0, TRUE);
    test_export_roundtrip("keys-persisted", CRYPT_EXPORTABLE | CRYPT_USER_KEYSET, EXPORT_PRIVATE_KEYS, pw2, TRUE, 0, TRUE);
    test_export_roundtrip("nullpw", NO_PERSIST_EXP, EXPORT_PRIVATE_KEYS, NULL, TRUE, 0, TRUE);
    test_export_roundtrip("emptypw", NO_PERSIST_EXP, EXPORT_PRIVATE_KEYS, empty, TRUE, 0, TRUE);
    test_export_roundtrip("plaincerts", NO_PERSIST_EXP, EXPORT_PRIVATE_KEYS | PKCS12_DISABLE_ENCRYPT_CERTIFICATES, pw2,
                          TRUE, 0, TRUE);
    test_export_roundtrip("certsonly", NO_PERSIST_EXP, 0, pw2, TRUE, 0, FALSE);
    /* a key that cannot be exported is left out unless the caller asks to hear about it */
    test_export_roundtrip("nonexportable", PKCS12_NO_PERSIST_KEY, EXPORT_PRIVATE_KEYS, pw2, TRUE, 0, FALSE);
    test_export_roundtrip("report-notable", PKCS12_NO_PERSIST_KEY, EXPORT_PRIVATE_KEYS | REPORT_NOT_ABLE_TO_EXPORT_PRIVATE_KEY,
                          pw2, FALSE, NTE_BAD_KEY_STATE, FALSE);
    test_export_roundtrip("badflags", NO_PERSIST_EXP, 0x8000, pw2, FALSE, ERROR_INVALID_PARAMETER, FALSE);

    SetLastError(0xdeadbeef);
    ret = PFXExportCertStoreEx(NULL, &blob, pw, NULL, 0);
    ok(!ret && GetLastError() == ERROR_INVALID_PARAMETER, "got %d, %08lx\n", ret, GetLastError());

    /* an empty store gives a valid empty PFX */
    store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
    ok(store != NULL, "CertOpenStore failed %08lx\n", GetLastError());
    ret = PFXExportCertStoreEx(store, &blob, pw, NULL, EXPORT_PRIVATE_KEYS);
    ok(ret, "export of an empty store failed %08lx\n", GetLastError());
    CertCloseStore(store, 0);
}

/* .NET marks the certificates of a PFX it loads without PersistKeySet using a private property */
static void test_private_properties(void)
{
    PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING, pfx_leaf_der, sizeof(pfx_leaf_der));
    CRYPT_DATA_BLOB blob = { 4, (BYTE *)"abcd" }, empty = { 0, NULL };
    BYTE buf[16];
    DWORD size;
    BOOL ret;
    static const DWORD ids[] = { 0x8001, 125 };
    unsigned i;

    ok(cert != NULL, "CertCreateCertificateContext failed %08lx\n", GetLastError());
    if (!cert) return;
    for (i = 0; i < ARRAY_SIZE(ids); i++)
    {
        ret = CertSetCertificateContextProperty(cert, ids[i], CERT_SET_PROPERTY_INHIBIT_PERSIST_FLAG, &blob);
        ok(ret, "%lu: set failed %08lx\n", ids[i], GetLastError());
        size = sizeof(buf);
        ret = CertGetCertificateContextProperty(cert, ids[i], buf, &size);
        ok(ret && size == 4 && !memcmp(buf, "abcd", 4), "%lu: got %d, %lu\n", ids[i], ret, size);
        ret = CertSetCertificateContextProperty(cert, ids[i], CERT_SET_PROPERTY_INHIBIT_PERSIST_FLAG, &empty);
        ok(ret, "%lu: set empty failed %08lx\n", ids[i], GetLastError());
        ret = CertSetCertificateContextProperty(cert, ids[i], 0, NULL);
        ok(ret, "%lu: delete failed %08lx\n", ids[i], GetLastError());
        size = sizeof(buf);
        SetLastError(0xdeadbeef);
        ret = CertGetCertificateContextProperty(cert, ids[i], buf, &size);
        ok(!ret && GetLastError() == CRYPT_E_NOT_FOUND, "%lu: got %d, %08lx\n", ids[i], ret, GetLastError());
    }
    CertFreeCertificateContext(cert);
}

START_TEST(pkcs12)
{
    test_private_properties();
    test_import();
    test_passwords();
    test_malformed();
    test_export();
}
