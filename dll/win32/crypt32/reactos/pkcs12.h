/*
 * PROJECT:     ReactOS crypt32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     PKCS #12 (RFC 7292) reader and writer, independent of the Windows headers
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#ifndef _P12_PKCS12_H
#define _P12_PKCS12_H

#include <stddef.h>
#include <stdint.h>

#define P12_OK              0
#define P12_ERR_FORMAT      1   /* malformed or not a PFX */
#define P12_ERR_PASSWORD    2   /* MAC mismatch or bad decryption */
#define P12_ERR_UNSUPPORTED 3   /* well-formed, but uses something not implemented */
#define P12_ERR_NOMEM       4
#define P12_ERR_LIMIT       5   /* iteration count, size or item count beyond the limits */
#define P12_ERR_RANDOM      6
#define P12_ERR_DECRYPT     7   /* decryption produced invalid padding / structure (no MAC to say which) */

/* limits applied to untrusted input */
#define P12_MAX_ITERATIONS         600000u  /* one KDF / MAC invocation: Windows 10 rejects more, also for PBKDF2 */
#define P12_MAX_TOTAL_ITERATIONS  8000000u  /* sum over the whole file */
#define P12_MAX_SALT              4096
#define P12_MAX_BAGS              4096
#define P12_MAX_DEPTH             16

struct p12_item
{
    uint8_t  *data;         /* certificate DER, or PKCS #8 PrivateKeyInfo DER */
    size_t    len;
    uint8_t  *id;           /* localKeyId, may be NULL */
    size_t    id_len;
    uint16_t *name;         /* friendlyName, NUL-terminated UTF-16, may be NULL */
};

struct p12_contents
{
    struct p12_item *certs;
    size_t           ncerts;
    struct p12_item *keys;
    size_t           nkeys;
};

/* Platform hooks: random bytes for salts and IVs; return 0 on success. */
int p12_random(uint8_t *buf, size_t len);

/* password: NUL-terminated UTF-16, or NULL for "no password". An empty and a NULL password are
 * tried in turn when the MAC does not verify with the one given. */
int  p12_parse(const uint8_t *buf, size_t len, const uint16_t *password, struct p12_contents *out);
int  p12_verify_password(const uint8_t *buf, size_t len, const uint16_t *password);
void p12_free_contents(struct p12_contents *c);
void p12_wipe(void *p, size_t n);

/* RSA PKCS #8 PrivateKeyInfo <-> CryptoAPI PRIVATEKEYBLOB. Anything not RSA is P12_ERR_UNSUPPORTED. */
int p12_pkcs8_to_rsa_blob(const uint8_t *pkcs8, size_t len, uint8_t **blob, size_t *blob_len);
int p12_rsa_blob_to_pkcs8(const uint8_t *blob, size_t len, uint8_t **pkcs8, size_t *pkcs8_len);
/* modulus (big endian, no leading zeros) of an RSA PKCS #8 key or an X.509 certificate */
int p12_pkcs8_modulus(const uint8_t *pkcs8, size_t len, const uint8_t **mod, size_t *mod_len);
int p12_cert_modulus(const uint8_t *cert, size_t len, const uint8_t **mod, size_t *mod_len);

/* Writer. Items are read, not copied or freed. */
#define P12_WRITE_PBES2      0x1 /* PBES2 / PBKDF2-HMAC-SHA256 / AES-256-CBC and HMAC-SHA256 MAC */
#define P12_WRITE_PLAIN_CERTS 0x2 /* certificates in an unencrypted ContentInfo */
struct p12_write_params
{
    unsigned flags;
    uint32_t iterations;    /* 0: default */
};
int p12_write(const struct p12_item *certs, size_t ncerts, const struct p12_item *keys, size_t nkeys,
              const uint16_t *password, const struct p12_write_params *params, uint8_t **out, size_t *out_len);
void p12_free(void *p);

/* Symmetric content encryption for CMS (crypt32 msg.c). *out is allocated, release it with p12_free.
 * Block ciphers use CBC with PKCS #7 padding (decryption checks it); RC4 is the plain stream cipher.
 * The IV is 8 bytes (DES, 3DES, RC2) or 16 bytes (AES) and is not used for RC4. */
#define P12_CIPHER_3DES 1
#define P12_CIPHER_DES  2
#define P12_CIPHER_RC2  3
#define P12_CIPHER_RC4  4
#define P12_CIPHER_AES  5
int p12_cms_crypt(int cipher, const uint8_t *key, size_t keylen, unsigned rc2bits, const uint8_t *iv,
                  int encrypt, const uint8_t *in, size_t inlen, uint8_t **out, size_t *outlen);

#endif
