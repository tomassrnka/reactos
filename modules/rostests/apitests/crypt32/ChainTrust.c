/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the trust of certificate chains: usage properties, partial chains
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 *
 * A root may be trusted for some purposes only: its CERT_ENHKEY_USAGE_PROP_ID property in the
 * store lists them (the code signing roots of the AuthRoot store are restricted this way). A
 * chain ending at such a root is not valid for any other requested usage, and when another path
 * (through a cross certificate) ends at a root trusted for all purposes, CertGetCertificateChain
 * picks that one.
 *
 * A chain that does not reach any root (the issuer of its last certificate is unknown) is not
 * trusted: the base and Authenticode policies fail it with CERT_E_CHAINING, the SSL policy with
 * CERT_E_UNTRUSTEDROOT, as on Windows.
 *
 * The certificates come from gen_usage_fixtures.py. They are only trusted through a chain engine
 * with an exclusive root store in memory: nothing is installed.
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <wincrypt.h>

#include "usage_fixtures.h"

static char ServerAuth[] = szOID_PKIX_KP_SERVER_AUTH;
static char CodeSigning[] = szOID_PKIX_KP_CODE_SIGNING;

static PCCERT_CONTEXT Decode(const char *Text)
{
    BYTE Der[4096];
    DWORD Length = sizeof(Der);

    if (!CryptStringToBinaryA(Text, 0, CRYPT_STRING_BASE64, Der, &Length, NULL, NULL))
        return NULL;
    return CertCreateCertificateContext(X509_ASN_ENCODING, Der, Length);
}

/* Adds Cert to Store; Usage: the only usage the copy in the store is trusted for, or NULL */
static BOOL AddCert(HCERTSTORE Store, PCCERT_CONTEXT Cert, char *Usage)
{
    PCCERT_CONTEXT Added;
    BOOL Ret;

    if (!CertAddCertificateContextToStore(Store, Cert, CERT_STORE_ADD_ALWAYS, &Added))
        return FALSE;
    Ret = TRUE;
    if (Usage)
    {
        CERT_ENHKEY_USAGE EnhKeyUsage = { 1, &Usage };

        Ret = CertSetEnhancedKeyUsage(Added, &EnhKeyUsage);
    }
    CertFreeCertificateContext(Added);
    return Ret;
}

/* Builds the chain of Leaf with the roots of RootStore only; Usage: the requested usage or NULL.
 * Returns the chain's error status, and in Top the certificate it ends at. Policies: when not
 * NULL, receives the results of the base, Authenticode and SSL policies for the chain. */
static DWORD BuildChainEx(HCERTSTORE RootStore, HCERTSTORE Extra, PCCERT_CONTEXT Leaf, char *Usage,
                          PCCERT_CONTEXT *Top, DWORD *Policies)
{
    CERT_CHAIN_ENGINE_CONFIG Config = { sizeof(Config) };
    CERT_CHAIN_PARA Para = { sizeof(Para) };
    HCERTCHAINENGINE Engine;
    PCCERT_CHAIN_CONTEXT Chain;
    DWORD Status = ~0u;

    *Top = NULL;
    Config.hExclusiveRoot = RootStore;
    if (!CertCreateCertificateChainEngine(&Config, &Engine))
    {
        skip("CertCreateCertificateChainEngine failed: %lu\n", GetLastError());
        return Status;
    }
    if (Usage)
    {
        Para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        Para.RequestedUsage.Usage.cUsageIdentifier = 1;
        Para.RequestedUsage.Usage.rgpszUsageIdentifier = &Usage;
    }
    if (CertGetCertificateChain(Engine, Leaf, NULL, Extra, &Para, CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL,
                                NULL, &Chain))
    {
        PCERT_SIMPLE_CHAIN Simple = Chain->rgpChain[Chain->cChain - 1];

        Status = Chain->TrustStatus.dwErrorStatus;
        *Top = CertDuplicateCertificateContext(Simple->rgpElement[Simple->cElement - 1]->pCertContext);
        if (Policies)
        {
            static const LPCSTR Oids[] = { CERT_CHAIN_POLICY_BASE, CERT_CHAIN_POLICY_AUTHENTICODE,
                                           CERT_CHAIN_POLICY_SSL };
            SSL_EXTRA_CERT_CHAIN_POLICY_PARA Ssl = { { sizeof(Ssl) } };
            DWORD i;

            Ssl.dwAuthType = AUTHTYPE_SERVER;
            for (i = 0; i < ARRAYSIZE(Oids); i++)
            {
                CERT_CHAIN_POLICY_PARA PolicyPara = { sizeof(PolicyPara) };
                CERT_CHAIN_POLICY_STATUS PolicyStatus = { sizeof(PolicyStatus) };

                if (Oids[i] == CERT_CHAIN_POLICY_SSL)
                    PolicyPara.pvExtraPolicyPara = &Ssl;
                Policies[i] = CertVerifyCertificateChainPolicy(Oids[i], Chain, &PolicyPara, &PolicyStatus) ?
                              PolicyStatus.dwError : ~0u;
            }
        }
        CertFreeCertificateChain(Chain);
    }
    else
        ok(0, "CertGetCertificateChain failed: %lu\n", GetLastError());
    CertFreeCertificateChainEngine(Engine);
    return Status;
}

static DWORD BuildChain(HCERTSTORE RootStore, HCERTSTORE Extra, PCCERT_CONTEXT Leaf, char *Usage,
                        PCCERT_CONTEXT *Top)
{
    return BuildChainEx(RootStore, Extra, Leaf, Usage, Top, NULL);
}

START_TEST(ChainTrust)
{
    PCCERT_CONTEXT RootA = Decode(UsRootA), RootB = Decode(UsRootB), CrossB = Decode(UsCrossB),
                   Leaf = Decode(UsLeaf), Top;
    HCERTSTORE Extra, Roots, Peer;
    DWORD Status, Policies[3];

    ok(RootA && RootB && CrossB && Leaf, "Decoding the certificates failed\n");
    if (!RootA || !RootB || !CrossB || !Leaf)
        return;
    Extra = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
    ok(Extra != NULL && AddCert(Extra, CrossB, NULL), "Cannot set up the additional store\n");

    /* only root A, trusted for all purposes: the chain goes through the cross certificate */
    Roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
    ok(AddCert(Roots, RootA, NULL), "Adding root A failed\n");
    Status = BuildChain(Roots, Extra, Leaf, ServerAuth, &Top);
    ok(Status == 0, "unrestricted root, server authentication: status %#lx\n", Status);
    ok(Top && CertCompareCertificate(X509_ASN_ENCODING, Top->pCertInfo, RootA->pCertInfo),
       "unrestricted root: the chain does not end at root A\n");
    if (Top) CertFreeCertificateContext(Top);
    CertCloseStore(Roots, 0);

    /* only root A, trusted for code signing only */
    Roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
    ok(AddCert(Roots, RootA, CodeSigning), "Adding root A failed\n");
    Status = BuildChain(Roots, Extra, Leaf, ServerAuth, &Top);
    ok(Status != ~0u && (Status & CERT_TRUST_IS_NOT_VALID_FOR_USAGE),
       "code signing root, server authentication: status %#lx\n", Status);
    ok(Status != ~0u && !(Status & CERT_TRUST_IS_UNTRUSTED_ROOT),
       "code signing root, server authentication: status %#lx\n", Status);
    if (Top) CertFreeCertificateContext(Top);
    Status = BuildChain(Roots, Extra, Leaf, CodeSigning, &Top);
    ok(Status == 0, "code signing root, code signing: status %#lx\n", Status);
    if (Top) CertFreeCertificateContext(Top);
    Status = BuildChain(Roots, Extra, Leaf, NULL, &Top);
    ok(Status == 0, "code signing root, no usage requested: status %#lx\n", Status);
    if (Top) CertFreeCertificateContext(Top);

    /* the root also comes with the certificate to verify, as a server or a signature may send it:
     * the restriction of the trusted copy still applies */
    Peer = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
    ok(Peer != NULL && AddCert(Peer, CrossB, NULL) && AddCert(Peer, RootA, NULL),
       "Cannot set up the peer's store\n");
    Status = BuildChain(Roots, Peer, Leaf, ServerAuth, &Top);
    ok(Status != ~0u && (Status & CERT_TRUST_IS_NOT_VALID_FOR_USAGE),
       "code signing root sent by the peer, server authentication: status %#lx\n", Status);
    if (Top) CertFreeCertificateContext(Top);
    CertCloseStore(Peer, 0);

    /* plus root B, trusted for all purposes: the chain for server authentication ends there */
    ok(AddCert(Roots, RootB, NULL), "Adding root B failed\n");
    Status = BuildChain(Roots, Extra, Leaf, ServerAuth, &Top);
    ok(Status == 0, "both roots, server authentication: status %#lx\n", Status);
    ok(Top && CertCompareCertificate(X509_ASN_ENCODING, Top->pCertInfo, RootB->pCertInfo),
       "both roots, server authentication: the chain does not end at root B\n");
    if (Top) CertFreeCertificateContext(Top);
    CertCloseStore(Roots, 0);

    /* no cross certificate: the issuer of the leaf is unknown, the chain ends nowhere */
    Roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, NULL);
    ok(AddCert(Roots, RootA, NULL), "Adding root A failed\n");
    Status = BuildChainEx(Roots, NULL, Leaf, NULL, &Top, Policies);
    ok(Status == CERT_TRUST_IS_PARTIAL_CHAIN, "partial chain: status %#lx\n", Status);
    if (Status != ~0u)
    {
        ok(Policies[0] == CERT_E_CHAINING, "partial chain, base policy: %#lx\n", Policies[0]);
        ok(Policies[1] == CERT_E_CHAINING, "partial chain, Authenticode policy: %#lx\n", Policies[1]);
        ok(Policies[2] == CERT_E_UNTRUSTEDROOT, "partial chain, SSL policy: %#lx\n", Policies[2]);
    }
    if (Top) CertFreeCertificateContext(Top);
    CertCloseStore(Roots, 0);

    CertCloseStore(Extra, 0);
    CertFreeCertificateContext(Leaf);
    CertFreeCertificateContext(CrossB);
    CertFreeCertificateContext(RootB);
    CertFreeCertificateContext(RootA);
}
