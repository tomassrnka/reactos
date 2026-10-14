/*
 * Lightweight mbedTLS-based implementation of the schannel (SSL/TLS) provider.
 *
 * Copyright 2015 Peter Hater
 * Copyright 2015 Ismael Ferreras Morezuelas <swyterzone+ros@gmail.com>
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

#include "config.h"
#include "wine/port.h"

#ifdef __REACTOS__
 #include "precomp.h"
#else
 #include <stdarg.h>
 #include <errno.h>

 #include "windef.h"
 #include "winbase.h"
 #include "sspi.h"
 #include "schannel.h"
 #include "wine/debug.h"
 #include "wine/library.h"
#endif

WINE_DEFAULT_DEBUG_CHANNEL(schannel);

#if defined(SONAME_LIBMBEDTLS) && !defined(HAVE_SECURITY_SECURITY_H) && !defined(SONAME_LIBGNUTLS)

/* the DLL reads the connection state of Mbed TLS 3 directly */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/cipher.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/psa_util.h>
#include <psa/crypto.h>

/* Chain errors that make a server certificate unusable and that the SSL policy of crypt32 does not report */
#define SCHAN_FATAL_CHAIN_ERRORS (CERT_TRUST_IS_NOT_SIGNATURE_VALID | CERT_TRUST_IS_UNTRUSTED_ROOT | \
                                  CERT_TRUST_IS_CYCLIC | CERT_TRUST_IS_PARTIAL_CHAIN | \
                                  CERT_TRUST_INVALID_BASIC_CONSTRAINTS | CERT_TRUST_INVALID_NAME_CONSTRAINTS | \
                                  CERT_TRUST_HAS_NOT_SUPPORTED_NAME_CONSTRAINT | \
                                  CERT_TRUST_HAS_NOT_DEFINED_NAME_CONSTRAINT | \
                                  CERT_TRUST_HAS_NOT_PERMITTED_NAME_CONSTRAINT | \
                                  CERT_TRUST_HAS_EXCLUDED_NAME_CONSTRAINT | CERT_TRUST_INVALID_POLICY_CONSTRAINTS | \
                                  CERT_TRUST_IS_EXPLICIT_DISTRUST)

/* wininet.h values of the chain policy checks */
#define SECURITY_FLAG_IGNORE_REVOCATION      0x00000080
#define SECURITY_FLAG_IGNORE_CERT_CN_INVALID 0x00001000

#ifndef __REACTOS__
 /* WINE defines the back-end glue in here */
 #include "secur32_priv.h"

 /* in ReactOS we use schannel instead of secur32 */
 WINE_DEFAULT_DEBUG_CHANNEL(secur32);

 /* WINE prefers to keep it optional, disable this to link explicitly */
 #include "schannel_mbedtls_lazyload.h"

 /* WINE does not define this standard win32 macro for some reason */
 #ifndef _countof
  #define _countof(a) (sizeof(a)/sizeof(*(a)))
 #endif
#endif

typedef struct
{
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    struct schan_transport  *transport;
    WCHAR                   *target;
    DWORD                    cred_flags;
    BOOL                     manual_validation;
    BOOL                     cert_checked;
} MBEDTLS_SESSION, *PMBEDTLS_SESSION;

/* BIO callbacks of Mbed TLS: pull and push move bytes between the session and the SSPI buffers; a transport
   that has nothing to give or no room left answers EAGAIN, which Mbed TLS wants as WANT_READ or WANT_WRITE */
static int schan_pull_adapter(void *session, unsigned char *buff, size_t buff_len)
{
    MBEDTLS_SESSION *s = session;
    int status;

    TRACE("MBEDTLS schan_pull_adapter: (%p/%p, %p, %u)\n", s, s->transport, buff, buff_len);

    status = schan_pull(s->transport, buff, &buff_len);

    TRACE("MBEDTLS schan_pull_adapter: (%p/%p, %p, %u) status: %#x\n", s, s->transport, buff, buff_len, status);

    if (status == NO_ERROR && buff_len)
        return (int)buff_len;

    if (status == NO_ERROR || status == EAGAIN)
        return MBEDTLS_ERR_SSL_WANT_READ;

    ERR("Unknown status code from schan_pull: %d\n", status);
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int schan_push_adapter(void *session, const unsigned char *buff, size_t buff_len)
{
    MBEDTLS_SESSION *s = session;
    int status;

    TRACE("MBEDTLS schan_push_adapter: (%p/%p, %p, %u)\n", s, s->transport, buff, buff_len);

    status = schan_push(s->transport, buff, &buff_len);

    TRACE("MBEDTLS schan_push_adapter: (%p/%p, %p, %u) status: %#x\n", s, s->transport, buff, buff_len, status);

    if (status == NO_ERROR && buff_len)
        return (int)buff_len;

    if (status == NO_ERROR || status == EAGAIN)
        return MBEDTLS_ERR_SSL_WANT_WRITE;

    ERR("Unknown status code from schan_push: %d\n", status);
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

DWORD schan_imp_enabled_protocols(void)
{
    /* NOTE: Mbed TLS 3 has no SSL 2.0, SSL 3.0, TLS 1.0 or TLS 1.1 */
    TRACE("MBEDTLS schan_imp_enabled_protocols()\n");

    return 0
#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
        | SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_2_SERVER
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
        | SP_PROT_TLS1_3_CLIENT | SP_PROT_TLS1_3_SERVER
#endif
        ;
}

/* The PSA crypto module of Mbed TLS 3 that TLS 1.3 uses has to be started once, before the first session */
static volatile LONG schan_psa_state;

static BOOL schan_psa_start(void)
{
    for (;;)
    {
        LONG old = InterlockedCompareExchange(&schan_psa_state, 1, 0);

        if (old == 2)
            return TRUE;

        if (!old)
        {
            if (psa_crypto_init() == PSA_SUCCESS)
            {
                InterlockedExchange(&schan_psa_state, 2);
                return TRUE;
            }

            InterlockedExchange(&schan_psa_state, 0);
            return FALSE;
        }

        Sleep(1);
    }
}

BOOL schan_imp_create_session(schan_imp_session *session, schan_credentials *cred)
{
    MBEDTLS_SESSION *s;
    BOOL tls12 = (cred->enabled_protocols & SP_PROT_TLS1_2_CLIENT) != 0;
    BOOL tls13 = (cred->enabled_protocols & SP_PROT_TLS1_3_CLIENT) != 0;

    WARN("MBEDTLS schan_imp_create_session: %p %p %p\n", session, *session, cred);

    if (!schan_psa_start())
    {
        ERR("Could not start the PSA crypto module\n");
        return FALSE;
    }

    s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MBEDTLS_SESSION));
    if (!(*session = (schan_imp_session)s))
    {
        ERR("Not enough memory to create session\n");
        return FALSE;
    }

    WARN("MBEDTLS init ssl\n");
    mbedtls_ssl_init(&s->ssl);

    WARN("MBEDTLS init conf\n");
    mbedtls_ssl_config_init(&s->conf);
    if (mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT))
    {
        ERR("mbedtls_ssl_config_defaults failed\n");
        goto fail;
    }

    /* Only the versions the credentials ask for are offered, and the server's pick is limited to them */
    mbedtls_ssl_conf_min_tls_version(&s->conf, tls12 ? MBEDTLS_SSL_VERSION_TLS1_2 : MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&s->conf, tls13 ? MBEDTLS_SSL_VERSION_TLS1_3 : MBEDTLS_SSL_VERSION_TLS1_2);

    TRACE("MBEDTLS set BIO callbacks\n");
    mbedtls_ssl_set_bio(&s->ssl, s, schan_push_adapter, schan_pull_adapter, NULL);

    /* The certificate is judged by crypt32 in schan_imp_handshake, so Mbed TLS verifies nothing */
    TRACE("MBEDTLS set authmode\n");
    mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_NONE);

    TRACE("MBEDTLS set rng\n");
    mbedtls_ssl_conf_rng(&s->conf, mbedtls_psa_get_random, MBEDTLS_PSA_RANDOM_STATE);

    TRACE("MBEDTLS setup\n");
    if (mbedtls_ssl_setup(&s->ssl, &s->conf))
    {
        ERR("mbedtls_ssl_setup failed\n");
        goto fail;
    }

    TRACE("MBEDTLS schan_imp_create_session END!\n");
    return TRUE;

fail:
    mbedtls_ssl_free(&s->ssl);
    mbedtls_ssl_config_free(&s->conf);
    HeapFree(GetProcessHeap(), 0, s);
    *session = NULL;
    return FALSE;
}

void schan_imp_dispose_session(schan_imp_session session)
{
    MBEDTLS_SESSION *s = (MBEDTLS_SESSION *)session;
    WARN("MBEDTLS schan_imp_dispose_session: %p\n", session);

    HeapFree(GetProcessHeap(), 0, s->target);
    mbedtls_ssl_free(&s->ssl);
    mbedtls_ssl_config_free(&s->conf);

    /* safely overwrite the freed context with zeroes */
    HeapFree(GetProcessHeap(), HEAP_ZERO_MEMORY, s);
}

void schan_imp_set_session_transport(schan_imp_session session,
                                     struct schan_transport *t)
{
    MBEDTLS_SESSION *s = (MBEDTLS_SESSION *)session;

    TRACE("MBEDTLS schan_imp_set_session_transport: %p %p\n", session, t);

    s->transport = t;
}

void schan_imp_set_session_target(schan_imp_session session, const char *target)
{
    MBEDTLS_SESSION *s = (MBEDTLS_SESSION *)session;

    TRACE("MBEDTLS schan_imp_set_session_target: sess: %p hostname: %s\n", session, target);

    /* FIXME: WINE tests do not pass when we set the hostname because in the test cases
     * contacting 'www.winehq.org' the hostname is defined as 'localhost' so the server
     * sends a non-fatal alert which preemptively forces mbedTLS to close connection. */

    mbedtls_ssl_set_hostname(&s->ssl, target);
}

void schan_imp_set_session_validation(schan_imp_session session, const WCHAR *target, DWORD flags, BOOL manual)
{
    MBEDTLS_SESSION *s = (MBEDTLS_SESSION *)session;

    s->cred_flags = flags;
    s->manual_validation = manual || (flags & SCH_CRED_MANUAL_CRED_VALIDATION);

    if (target)
    {
        SIZE_T size = (lstrlenW(target) + 1) * sizeof(WCHAR);

        s->target = HeapAlloc(GetProcessHeap(), 0, size);
        if (s->target)
            memcpy(s->target, target, size);
    }
}

static SECURITY_STATUS schan_add_peer_chain(const mbedtls_x509_crt *peer_cert, HCERTSTORE store,
                                            PCCERT_CONTEXT *ret);

static const mbedtls_x509_crt *schan_handshake_chain(const MBEDTLS_SESSION *s)
{
    if (mbedtls_ssl_is_handshake_over((mbedtls_ssl_context *)&s->ssl))
        return mbedtls_ssl_get_peer_cert(&s->ssl);

    return s->ssl.session_negotiate ? s->ssl.session_negotiate->peer_cert : NULL;
}

static SECURITY_STATUS schan_status_from_chain_error(DWORD error)
{
    switch (error)
    {
        case CERT_E_EXPIRED:
            return SEC_E_CERT_EXPIRED;

        case CERT_E_CN_NO_MATCH:
            return SEC_E_WRONG_PRINCIPAL;

        case CERT_E_UNTRUSTEDROOT:
        case CERT_E_UNTRUSTEDTESTROOT:
        case CERT_E_CHAINING:
        case TRUST_E_CERT_SIGNATURE:
            return SEC_E_UNTRUSTED_ROOT;

        default:
            return SEC_E_CERT_UNKNOWN;
    }
}

/* Windows schannel checks the chain of a server itself unless the caller asked to do it */
static SECURITY_STATUS schan_validate_server_chain(MBEDTLS_SESSION *s)
{
    static char server_auth_oid[] = szOID_PKIX_KP_SERVER_AUTH;
    char *usages[] = { server_auth_oid };
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl_para;
    CERT_CHAIN_POLICY_PARA policy_para;
    CERT_CHAIN_POLICY_STATUS policy_status;
    CERT_CHAIN_PARA chain_para;
    PCCERT_CHAIN_CONTEXT chain = NULL;
    PCCERT_CONTEXT leaf = NULL;
    HCERTSTORE store;
    SECURITY_STATUS status;
    DWORD chain_flags = 0;

    /* Without a target name there is nothing to match, which Windows reports before any chain error */
    if (!s->target && !(s->cred_flags & SCH_CRED_NO_SERVERNAME_CHECK))
        return SEC_E_WRONG_PRINCIPAL;

    store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, NULL);
    if (!store)
        return SEC_E_INTERNAL_ERROR;

    /* during the handshake the chain sits in the session that is being negotiated */
    status = schan_add_peer_chain(schan_handshake_chain(s), store, &leaf);
    if (status != SEC_E_OK || !leaf)
    {
        CertCloseStore(store, 0);
        return SEC_E_CERT_UNKNOWN;
    }

    memset(&chain_para, 0, sizeof(chain_para));
    chain_para.cbSize = sizeof(chain_para);
    chain_para.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
    chain_para.RequestedUsage.Usage.cUsageIdentifier = 1;
    chain_para.RequestedUsage.Usage.rgpszUsageIdentifier = usages;

    if (s->cred_flags & SCH_CRED_REVOCATION_CHECK_CHAIN_END_CERT)
        chain_flags = CERT_CHAIN_REVOCATION_CHECK_END_CERT;
    else if (s->cred_flags & SCH_CRED_REVOCATION_CHECK_CHAIN)
        chain_flags = CERT_CHAIN_REVOCATION_CHECK_CHAIN;
    else if (s->cred_flags & SCH_CRED_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT)
        chain_flags = CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;

    if (!CertGetCertificateChain(NULL, leaf, NULL, store, &chain_para, chain_flags, NULL, &chain))
    {
        status = SEC_E_CERT_UNKNOWN;
        goto done;
    }

    memset(&ssl_para, 0, sizeof(ssl_para));
    ssl_para.cbSize = sizeof(ssl_para);
    ssl_para.dwAuthType = AUTHTYPE_SERVER;
    /* The policy would also drop a known revocation, so revocation is judged below from the chain status */
    ssl_para.fdwChecks = SECURITY_FLAG_IGNORE_REVOCATION;
    if (s->cred_flags & SCH_CRED_NO_SERVERNAME_CHECK)
        ssl_para.fdwChecks |= SECURITY_FLAG_IGNORE_CERT_CN_INVALID;
    ssl_para.pwszServerName = s->target;

    memset(&policy_para, 0, sizeof(policy_para));
    policy_para.cbSize = sizeof(policy_para);
    policy_para.pvExtraPolicyPara = &ssl_para;

    memset(&policy_status, 0, sizeof(policy_status));
    policy_status.cbSize = sizeof(policy_status);

    if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy_para, &policy_status))
    {
        status = SEC_E_CERT_UNKNOWN;
        goto done;
    }

    status = SEC_E_OK;
    if (policy_status.dwError && policy_status.dwError != CERT_E_CN_NO_MATCH)
    {
        WARN("MBEDTLS server certificate rejected: %#x\n", policy_status.dwError);
        status = schan_status_from_chain_error(policy_status.dwError);
    }
    /* crypt32's SSL policy ignores these, so a missing issuer, a signer that is not a CA or a name constraint pass it */
    else if (chain->TrustStatus.dwErrorStatus & CERT_TRUST_INVALID_BASIC_CONSTRAINTS)
        status = SEC_E_CERT_WRONG_USAGE;
    else if (chain->TrustStatus.dwErrorStatus & SCHAN_FATAL_CHAIN_ERRORS)
        status = SEC_E_UNTRUSTED_ROOT;
    else if (chain_flags && (chain->TrustStatus.dwErrorStatus & CERT_TRUST_IS_REVOKED))
        status = CRYPT_E_REVOKED;
    else if (chain_flags && (chain->TrustStatus.dwErrorStatus & CERT_TRUST_REVOCATION_STATUS_UNKNOWN) &&
             !(s->cred_flags & SCH_CRED_IGNORE_NO_REVOCATION_CHECK))
        status = CRYPT_E_NO_REVOCATION_CHECK;
    /* crypt32 sets the offline bit together with the unknown one when there is no revocation information, which the
     * unknown check above already judged */
    else if (chain_flags && (chain->TrustStatus.dwErrorStatus & CERT_TRUST_IS_OFFLINE_REVOCATION) &&
             !(chain->TrustStatus.dwErrorStatus & CERT_TRUST_REVOCATION_STATUS_UNKNOWN) &&
             !(s->cred_flags & SCH_CRED_IGNORE_REVOCATION_OFFLINE))
        status = CRYPT_E_REVOCATION_OFFLINE;
    else if (policy_status.dwError == CERT_E_CN_NO_MATCH)
        status = SEC_E_WRONG_PRINCIPAL;

done:
    if (chain)
        CertFreeCertificateChain(chain);
    CertFreeCertificateContext(leaf);
    CertCloseStore(store, 0);
    return status;
}

SECURITY_STATUS schan_imp_handshake(schan_imp_session session)
{
    MBEDTLS_SESSION *s = (MBEDTLS_SESSION *)session;
    int err = 0;

    /* Step through the handshake and judge the server certificate as soon as it is there, before the
     * client answers it. TLS 1.3 numbers its states differently, so the chain itself is the trigger. */
    while (!mbedtls_ssl_is_handshake_over(&s->ssl))
    {
        if (!s->cert_checked && schan_handshake_chain(s))
        {
            s->cert_checked = TRUE;

            /* Windows answers a rejected server with the status alone, no alert record */
            if (!s->manual_validation)
            {
                SECURITY_STATUS status = schan_validate_server_chain(s);

                if (status != SEC_E_OK)
                    return status;
            }
        }

        err = mbedtls_ssl_handshake_step(&s->ssl);
        if (err != 0)
            break;
    }

    TRACE("MBEDTLS schan_imp_handshake: %p  err: %#x \n", session, err);

    if (err == MBEDTLS_ERR_SSL_WANT_READ || err == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
        TRACE("Received ERR_SSL_WANT_READ/WRITE... let's try again!\n");
        return SEC_I_CONTINUE_NEEDED;
    }
    else if (err == MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE)
    {
        ERR("schan_imp_handshake: SSL Feature unavailable...\n");
        return SEC_E_UNSUPPORTED_FUNCTION;
    }
    else if (err != 0)
    {
        ERR("schan_imp_handshake: Oops! mbedtls_ssl_handshake_step returned the following error code: -%#x...\n", -err);
        return SEC_E_INTERNAL_ERROR;
    }

    /* A handshake that ends without a certificate having been judged must not pass either */
    if (!s->cert_checked)
    {
        s->cert_checked = TRUE;

        if (!s->manual_validation)
        {
            SECURITY_STATUS status = schan_validate_server_chain(s);

            if (status != SEC_E_OK)
                return status;
        }
    }

    WARN("schan_imp_handshake: Handshake completed!\n");
    WARN("schan_imp_handshake: Protocol is %s, Cipher suite is %s\n", mbedtls_ssl_get_version(&s->ssl),
                                                                      mbedtls_ssl_get_ciphersuite(&s->ssl));
    return SEC_E_OK;
}

static unsigned int schannel_get_cipher_key_size(int ciphersuite_id)
{
    const mbedtls_ssl_ciphersuite_t *ssl_cipher_suite = mbedtls_ssl_ciphersuite_from_id(ciphersuite_id);
    unsigned int key_bitlen = (unsigned int)mbedtls_ssl_ciphersuite_get_cipher_key_bitlen(ssl_cipher_suite);

    TRACE("MBEDTLS schannel_get_cipher_key_size: cipher %#x, returning %u\n", ciphersuite_id, key_bitlen);

    return key_bitlen;
}

static unsigned int schannel_get_mac_key_size(int ciphersuite_id)
{
    const mbedtls_ssl_ciphersuite_t *ssl_cipher_suite = mbedtls_ssl_ciphersuite_from_id(ciphersuite_id);
    const mbedtls_md_info_t                  *md_info = mbedtls_md_info_from_type(ssl_cipher_suite->mac);

    int md_size = mbedtls_md_get_size(md_info) * CHAR_BIT; /* return the size in bits, as the secur32:schannel winetest shows */

    TRACE("MBEDTLS schannel_get_mac_key_size: returning %i\n", md_size);

    return md_size;
}

static unsigned int schannel_get_kx_key_size(const mbedtls_ssl_context *ssl, int ciphersuite_id)
{
    const mbedtls_ssl_ciphersuite_t *ssl_ciphersuite = mbedtls_ssl_ciphersuite_from_id(ciphersuite_id);
    const mbedtls_x509_crt *server_cert = mbedtls_ssl_get_peer_cert(ssl);

    /* TLS 1.3 suites say nothing about the key exchange */
    if (ssl_ciphersuite->key_exchange != MBEDTLS_KEY_EXCHANGE_NONE && server_cert)
        return (unsigned int)mbedtls_pk_get_len(&server_cert->pk);

    TRACE("MBEDTLS schannel_get_kx_key_size: kx %#x, returning 0\n", ssl_ciphersuite->key_exchange);

    return 0;
}

static DWORD schannel_get_protocol(const mbedtls_ssl_context *ssl)
{
    /* schannel only runs client connections here */
    switch (mbedtls_ssl_get_version_number(ssl))
    {
        case MBEDTLS_SSL_VERSION_TLS1_2:
            return SP_PROT_TLS1_2_CLIENT;

        case MBEDTLS_SSL_VERSION_TLS1_3:
            return SP_PROT_TLS1_3_CLIENT;

        default:
        {
            FIXME("MBEDTLS schannel_get_protocol: unknown protocol %d\n", (int)mbedtls_ssl_get_version_number(ssl));
            return 0;
        }
    }
}

static ALG_ID schannel_get_cipher_algid(int ciphersuite_id)
{
    const mbedtls_ssl_ciphersuite_t *cipher_suite = mbedtls_ssl_ciphersuite_from_id(ciphersuite_id);

    switch (cipher_suite->cipher)
    {
        case MBEDTLS_CIPHER_NONE:
        case MBEDTLS_CIPHER_NULL:
            return 0;

        case MBEDTLS_CIPHER_AES_128_ECB:
        case MBEDTLS_CIPHER_AES_128_CBC:
        case MBEDTLS_CIPHER_AES_128_CTR:
        case MBEDTLS_CIPHER_AES_128_GCM:
        case MBEDTLS_CIPHER_AES_128_CCM:
            return CALG_AES_128;

        case MBEDTLS_CIPHER_AES_192_ECB:
        case MBEDTLS_CIPHER_AES_192_CBC:
        case MBEDTLS_CIPHER_AES_192_CTR:
        case MBEDTLS_CIPHER_AES_192_GCM:
        case MBEDTLS_CIPHER_AES_192_CCM:
            return CALG_AES_192;

        case MBEDTLS_CIPHER_AES_256_ECB:
        case MBEDTLS_CIPHER_AES_256_CBC:
        case MBEDTLS_CIPHER_AES_256_CTR:
        case MBEDTLS_CIPHER_AES_256_GCM:
        case MBEDTLS_CIPHER_AES_256_CCM:
            return CALG_AES_256;

        /* nothing to show, ChaCha20-Poly1305 has no ALG_ID in wincrypt.h */
        default:
        {
            FIXME("MBEDTLS schannel_get_cipher_algid: unknown algorithm %d\n", ciphersuite_id);
            return 0;
        }
    }
}

static ALG_ID schannel_get_mac_algid(int ciphersuite_id)
{
    const mbedtls_ssl_ciphersuite_t *cipher_suite = mbedtls_ssl_ciphersuite_from_id(ciphersuite_id);

    switch (cipher_suite->mac)
    {
        case MBEDTLS_MD_NONE:      return 0;
        case MBEDTLS_MD_MD5:       return CALG_MD5;
        case MBEDTLS_MD_SHA1:      return CALG_SHA1;
        case MBEDTLS_MD_SHA224:    return CALG_SHA;
        case MBEDTLS_MD_SHA256:    return CALG_SHA_256;
        case MBEDTLS_MD_SHA384:    return CALG_SHA_384;
        case MBEDTLS_MD_SHA512:    return CALG_SHA_512;
        case MBEDTLS_MD_RIPEMD160: return (ALG_CLASS_HASH | ALG_TYPE_ANY | ALG_SID_RIPEMD160); /* there's no CALG_RIPEMD or CALG_RIPEMD160 defined in <wincrypt.h> yet */

        default:
        {
            FIXME("MBEDTLS schannel_get_mac_algid: unknown algorithm %d\n", cipher_suite->mac);
            return 0;
        }
    }
}

static ALG_ID schannel_get_kx_algid(const mbedtls_ssl_context *ssl, int ciphersuite_id)
{
    const mbedtls_ssl_ciphersuite_t *cipher_suite = mbedtls_ssl_ciphersuite_from_id(ciphersuite_id);

    switch (cipher_suite->key_exchange)
    {
        case MBEDTLS_KEY_EXCHANGE_NONE:
            /* TLS 1.3 suites: the exchange is ephemeral ECDH or DH, not part of the suite */
            return mbedtls_ssl_get_version_number(ssl) == MBEDTLS_SSL_VERSION_TLS1_3 ? CALG_ECDH_EPHEM : 0;

        case MBEDTLS_KEY_EXCHANGE_PSK: /* the original implementation does not support    */
            return 0;                  /* any PSK, and does not define any `CALG_PSK` :)  */

        case MBEDTLS_KEY_EXCHANGE_RSA:
        case MBEDTLS_KEY_EXCHANGE_RSA_PSK:
            return CALG_RSA_KEYX;

        case MBEDTLS_KEY_EXCHANGE_DHE_RSA:
        case MBEDTLS_KEY_EXCHANGE_DHE_PSK:
            return CALG_DH_EPHEM;

        case MBEDTLS_KEY_EXCHANGE_ECDH_RSA:
        case MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA:
            return CALG_ECDH;

        case MBEDTLS_KEY_EXCHANGE_ECDHE_RSA:
        case MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA:
        case MBEDTLS_KEY_EXCHANGE_ECDHE_PSK:
            return CALG_ECDH_EPHEM;

        default:
        {
            FIXME("MBEDTLS schannel_get_kx_algid: unknown algorithm %d\n", cipher_suite->key_exchange);
            return 0;
        }
    }
}

unsigned int schan_imp_get_session_cipher_block_size(schan_imp_session session)
{
    MBEDTLS_SESSION *s = (MBEDTLS_SESSION *)session;
    const mbedtls_ssl_ciphersuite_t *suite = mbedtls_ssl_ciphersuite_from_id(mbedtls_ssl_get_ciphersuite_id_from_ssl(&s->ssl));
    const mbedtls_cipher_info_t *info = suite ? mbedtls_cipher_info_from_type(suite->cipher) : NULL;
    unsigned int cipher_block_size = info ? (unsigned int)mbedtls_cipher_info_get_block_size(info) : 0;

    TRACE("MBEDTLS schan_imp_get_session_cipher_block_size %p returning %u.\n", session, cipher_block_size);

    return cipher_block_size;
}

unsigned int schan_imp_get_max_message_size(schan_imp_session session)
{
    MBEDTLS_SESSION *s = (MBEDTLS_SESSION *)session;
    int payload = mbedtls_ssl_get_max_out_record_payload(&s->ssl);
    unsigned int max_frag_len = payload > 0 ? (unsigned int)payload : MBEDTLS_SSL_OUT_CONTENT_LEN;

    TRACE("MBEDTLS schan_imp_get_max_message_size %p returning %u.\n", session, max_frag_len);

    return max_frag_len;
}

SECURITY_STATUS schan_imp_get_connection_info(schan_imp_session session,
                                              SecPkgContext_ConnectionInfo *info)
{
    MBEDTLS_SESSION *s = (MBEDTLS_SESSION *)session;

    int ciphersuite_id = mbedtls_ssl_get_ciphersuite_id_from_ssl(&s->ssl);

    TRACE("MBEDTLS schan_imp_get_connection_info %p %p.\n", session, info);

    if (!mbedtls_ssl_ciphersuite_from_id(ciphersuite_id))
        return SEC_E_INVALID_HANDLE;

    info->dwProtocol       = schannel_get_protocol(&s->ssl);
    info->aiCipher         = schannel_get_cipher_algid(ciphersuite_id);
    info->dwCipherStrength = schannel_get_cipher_key_size(ciphersuite_id);
    info->aiHash           = schannel_get_mac_algid(ciphersuite_id);
    info->dwHashStrength   = schannel_get_mac_key_size(ciphersuite_id);
    info->aiExch           = schannel_get_kx_algid(&s->ssl, ciphersuite_id);
    info->dwExchStrength   = schannel_get_kx_key_size(&s->ssl, ciphersuite_id);

    return SEC_E_OK;
}

static SECURITY_STATUS schan_add_peer_chain(const mbedtls_x509_crt *peer_cert, HCERTSTORE store,
                                            PCCERT_CONTEXT *ret)
{
    PCCERT_CONTEXT cert_context = NULL;
    const mbedtls_x509_crt *next_cert;

    if (!peer_cert)
        return SEC_E_INTERNAL_ERROR;

    for (next_cert = peer_cert; next_cert != NULL; next_cert = next_cert->next)
    {
        if (!CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, next_cert->raw.p, next_cert->raw.len,
            CERT_STORE_ADD_REPLACE_EXISTING, (next_cert != peer_cert) ? NULL : &cert_context))
        {
            if (next_cert != peer_cert)
                CertFreeCertificateContext(cert_context);
            return GetLastError();
        }
    }

    *ret = cert_context;
    return SEC_E_OK;
}

SECURITY_STATUS schan_imp_get_session_peer_certificate(schan_imp_session session, HCERTSTORE store,
                                                       PCCERT_CONTEXT *ret)
{
    MBEDTLS_SESSION *s = (MBEDTLS_SESSION *)session;

    TRACE("MBEDTLS schan_imp_get_session_peer_certificate %p %p %p %p.\n", session, store, ret, ret != NULL ? *ret : NULL);

    return schan_add_peer_chain(mbedtls_ssl_get_peer_cert(&s->ssl), store, ret);
}

SECURITY_STATUS schan_imp_send(schan_imp_session session, const void *buffer,
                               SIZE_T *length)
{
    MBEDTLS_SESSION *s = (MBEDTLS_SESSION *)session;
    int ret;

    TRACE("MBEDTLS schan_imp_send: (%p, %p, %p/%lu)\n", s, buffer, length, *length);

    ret = mbedtls_ssl_write(&s->ssl, (unsigned char *)buffer, *length);

    if (ret >= 0)
    {
        TRACE("MBEDTLS schan_imp_send: ret=%i.\n", ret);

        *length = ret;
    }
    else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ)
    {
        TRACE("MBEDTLS schan_imp_send: ret=MBEDTLS_ERR_SSL_WANT_WRITE -> SEC_I_CONTINUE_NEEDED\n");

        *length = 0;
        return SEC_I_CONTINUE_NEEDED;
    }
    else
    {
        ERR("MBEDTLS schan_imp_send: mbedtls_ssl_write failed with -%x\n", -ret);
        return SEC_E_INTERNAL_ERROR;
    }

    return SEC_E_OK;
}

SECURITY_STATUS schan_imp_recv(schan_imp_session session, void *buffer,
                               SIZE_T *length)
{
    PMBEDTLS_SESSION s = (PMBEDTLS_SESSION)session;
    int ret;

    TRACE("MBEDTLS schan_imp_recv: (%p, %p, %p/%lu)\n", s, buffer, length, *length);

    ret = mbedtls_ssl_read(&s->ssl, (unsigned char *)buffer, *length);

    TRACE("MBEDTLS schan_imp_recv: (%p, %p, %p/%lu) ret= %#x\n", s, buffer, length, *length, ret);

    if (ret >= 0)
    {
        TRACE("MBEDTLS schan_imp_recv: ret == %i.\n", ret);

        *length = ret;
    }
    else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
        TRACE("MBEDTLS schan_imp_recv: ret=MBEDTLS_ERR_SSL_WANT_READ -> SEC_I_CONTINUE_NEEDED\n");

        *length = 0;
        return SEC_I_CONTINUE_NEEDED;
    }
    else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
    {
        *length = 0;
        TRACE("MBEDTLS schan_imp_recv: ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY -> SEC_E_OK\n");
        return SEC_E_OK;
    }
    else
    {
        ERR("MBEDTLS schan_imp_recv: mbedtls_ssl_read failed with -%x\n", -ret);
        return SEC_E_INTERNAL_ERROR;
    }

    return SEC_E_OK;
}

BOOL schan_imp_allocate_certificate_credentials(schan_credentials *c)
{
    TRACE("MBEDTLS schan_imp_allocate_certificate_credentials %p %p %d\n", c, c->credentials, c->credential_use);

    /* in our case credentials aren't really used for anything, so just stub them */
    c->credentials = NULL;
    return TRUE;
}

void schan_imp_free_certificate_credentials(schan_credentials *c)
{
    TRACE("MBEDTLS schan_imp_free_certificate_credentials %p %p %d\n", c, c->credentials, c->credential_use);
}

BOOL schan_imp_init(void)
{
    TRACE("Schannel MBEDTLS schan_imp_init\n");
    return TRUE;
}

void schan_imp_deinit(void)
{
    WARN("Schannel MBEDTLS schan_imp_deinit\n");
}

#endif /* SONAME_LIBMBEDTLS && !HAVE_SECURITY_SECURITY_H && !SONAME_LIBGNUTLS */
