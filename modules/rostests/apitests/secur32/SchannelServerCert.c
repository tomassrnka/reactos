/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     schannel validates the server certificate of a client handshake itself
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define SECURITY_WIN32
#include <apitest.h>
#include <wincrypt.h>
#include <sspi.h>
#include <schannel.h>

#include "SchannelServerCertData.h"

#define TEST_HOST       L"schannel-test.reactos.test"
#define TEST_OTHER_HOST L"another.reactos.test"

/* ServerHello (TLS 1.2, TLS_RSA_WITH_AES_128_CBC_SHA, no extensions) */
static const BYTE ServerHelloRecord[] =
{
    0x16, 0x03, 0x03, 0x00, 0x2a,
    0x02, 0x00, 0x00, 0x26, 0x03, 0x03,
    0x52, 0x65, 0x61, 0x63, 0x74, 0x4f, 0x53, 0x20, 0x73, 0x63, 0x68, 0x61, 0x6e, 0x6e, 0x65, 0x6c,
    0x20, 0x74, 0x65, 0x73, 0x74, 0x20, 0x73, 0x65, 0x72, 0x76, 0x65, 0x72, 0x20, 0x72, 0x6e, 0x64,
    0x00, 0x00, 0x2f, 0x00
};

static const BYTE ServerHelloDoneRecord[] = { 0x16, 0x03, 0x03, 0x00, 0x04, 0x0e, 0x00, 0x00, 0x00 };

typedef struct _CERT_BLOB_REF
{
    const BYTE *Data;
    ULONG Size;
} CERT_BLOB_REF;

#define BLOB(x) { x, sizeof(x) }

static const CERT_BLOB_REF NoCert = { NULL, 0 };

typedef struct _TEST_CASE
{
    PCSTR Name;
    CERT_BLOB_REF Leaf;
    CERT_BLOB_REF Extra;
    PCWSTR Target;
    DWORD CredFlags;
    DWORD ContextFlags;
    SECURITY_STATUS Expected;
    BOOL OwnOutput;
    BOOL TodoRos;
} TEST_CASE;

#define CONTINUE SEC_I_CONTINUE_NEEDED

static const TEST_CASE TestCases[] =
{
    /* Default validation (no manual flag): the handshake itself fails for a bad chain */
    { "valid", BLOB(CertLeafValid), NoCert, TEST_HOST, 0, 0, CONTINUE, FALSE, FALSE },
    { "valid through an intermediate sent by the server", BLOB(CertLeafViaIntermediate), BLOB(CertIntermediate), TEST_HOST, 0, 0, CONTINUE, FALSE, FALSE },
    { "intermediate not sent", BLOB(CertLeafViaIntermediate), NoCert, TEST_HOST, 0, 0, SEC_E_UNTRUSTED_ROOT, FALSE, FALSE },
    { "expired", BLOB(CertLeafExpired), NoCert, TEST_HOST, 0, 0, SEC_E_CERT_EXPIRED, FALSE, FALSE },
    { "wrong host name", BLOB(CertLeafWrongHost), NoCert, TEST_HOST, 0, 0, SEC_E_WRONG_PRINCIPAL, FALSE, FALSE },
    { "valid, other host asked for", BLOB(CertLeafValid), NoCert, TEST_OTHER_HOST, 0, 0, SEC_E_WRONG_PRINCIPAL, FALSE, FALSE },
    { "self-signed", BLOB(CertLeafSelfSigned), NoCert, TEST_HOST, 0, 0, SEC_E_UNTRUSTED_ROOT, FALSE, FALSE },
    { "untrusted root sent", BLOB(CertLeafUntrustedRoot), BLOB(CertUntrustedRoot), TEST_HOST, 0, 0, SEC_E_UNTRUSTED_ROOT, FALSE, FALSE },
    { "untrusted root not sent", BLOB(CertLeafUntrustedRoot), NoCert, TEST_HOST, 0, 0, SEC_E_UNTRUSTED_ROOT, FALSE, FALSE },
    { "valid, no target name", BLOB(CertLeafValid), NoCert, NULL, 0, 0, SEC_E_WRONG_PRINCIPAL, FALSE, FALSE },
    { "wrong host name, no target name", BLOB(CertLeafWrongHost), NoCert, NULL, 0, 0, SEC_E_WRONG_PRINCIPAL, FALSE, FALSE },
    { "expired, no target name", BLOB(CertLeafExpired), NoCert, NULL, 0, 0, SEC_E_WRONG_PRINCIPAL, FALSE, FALSE },
    { "expired, other host asked for", BLOB(CertLeafExpired), NoCert, TEST_OTHER_HOST, 0, 0, SEC_E_CERT_EXPIRED, FALSE, FALSE },
    { "self-signed, other host asked for", BLOB(CertLeafSelfSigned), NoCert, TEST_OTHER_HOST, 0, 0, SEC_E_UNTRUSTED_ROOT, FALSE, FALSE },
    { "untrusted root sent, other host asked for", BLOB(CertLeafUntrustedRoot), BLOB(CertUntrustedRoot), TEST_OTHER_HOST, 0, 0, SEC_E_UNTRUSTED_ROOT, FALSE, FALSE },
    { "no target name, no server name check", BLOB(CertLeafValid), NoCert, NULL, SCH_CRED_NO_SERVERNAME_CHECK, 0, CONTINUE, FALSE, FALSE },
    { "intermediate not sent, other host asked for", BLOB(CertLeafViaIntermediate), NoCert, TEST_OTHER_HOST, 0, 0, SEC_E_UNTRUSTED_ROOT, FALSE, FALSE },
    { "expired, issuer unknown", BLOB(CertLeafExpiredUntrusted), NoCert, TEST_HOST, 0, 0, SEC_E_UNTRUSTED_ROOT, FALSE, FALSE },
    { "expired, untrusted root sent", BLOB(CertLeafExpiredUntrusted), BLOB(CertUntrustedRoot), TEST_HOST, 0, 0, SEC_E_UNTRUSTED_ROOT, FALSE, FALSE },
    { "signed by a trusted certificate that is not a CA", BLOB(CertLeafFromNonCa), BLOB(CertSignerNotCa), TEST_HOST, 0, 0, SEC_E_CERT_WRONG_USAGE, FALSE, FALSE },
    { "own output buffer: valid", BLOB(CertLeafValid), NoCert, TEST_HOST, 0, 0, CONTINUE, TRUE, FALSE },
    { "own output buffer: expired", BLOB(CertLeafExpired), NoCert, TEST_HOST, 0, 0, SEC_E_CERT_EXPIRED, TRUE, FALSE },
    { "revocation asked for, no revocation information", BLOB(CertLeafValid), NoCert, TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN_END_CERT, 0, CRYPT_E_NO_REVOCATION_CHECK, FALSE, FALSE },
    { "revocation asked for, no revocation information, check ignored", BLOB(CertLeafValid), NoCert, TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN_END_CERT | SCH_CRED_IGNORE_NO_REVOCATION_CHECK, 0, CONTINUE, FALSE, FALSE },
    { "revocation: ignore-no-check flag alone", BLOB(CertLeafValid), NoCert, TEST_HOST, SCH_CRED_IGNORE_NO_REVOCATION_CHECK, 0, CONTINUE, FALSE, FALSE },
    { "revocation asked for, offline ignored only", BLOB(CertLeafValid), NoCert, TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN_END_CERT | SCH_CRED_IGNORE_REVOCATION_OFFLINE, 0, CRYPT_E_NO_REVOCATION_CHECK, FALSE, FALSE },
    { "revocation asked for, both ignored", BLOB(CertLeafValid), NoCert, TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN_END_CERT | SCH_CRED_IGNORE_NO_REVOCATION_CHECK | SCH_CRED_IGNORE_REVOCATION_OFFLINE, 0, CONTINUE, FALSE, FALSE },
    { "revocation of the chain asked for", BLOB(CertLeafValid), NoCert, TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN, 0, CRYPT_E_NO_REVOCATION_CHECK, FALSE, FALSE },
    { "revocation of the chain without the root asked for", BLOB(CertLeafValid), NoCert, TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT, 0, CRYPT_E_NO_REVOCATION_CHECK, FALSE, FALSE },
    { "revocation of the chain asked for, check ignored", BLOB(CertLeafValid), NoCert, TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN | SCH_CRED_IGNORE_NO_REVOCATION_CHECK, 0, CONTINUE, FALSE, FALSE },
    { "mixed chain: leaf revocation offline, no information above", BLOB(CertLeafOfflineRevocation), BLOB(CertIntermediate), TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN, 0, CRYPT_E_REVOCATION_OFFLINE, FALSE, FALSE },
    { "mixed chain, no-check ignored only", BLOB(CertLeafOfflineRevocation), BLOB(CertIntermediate), TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN | SCH_CRED_IGNORE_NO_REVOCATION_CHECK, 0, CRYPT_E_REVOCATION_OFFLINE, FALSE, FALSE },
    { "mixed chain, offline ignored only", BLOB(CertLeafOfflineRevocation), BLOB(CertIntermediate), TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN | SCH_CRED_IGNORE_REVOCATION_OFFLINE, 0, CONTINUE, FALSE, FALSE },
    { "mixed chain, both ignored", BLOB(CertLeafOfflineRevocation), BLOB(CertIntermediate), TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN | SCH_CRED_IGNORE_NO_REVOCATION_CHECK | SCH_CRED_IGNORE_REVOCATION_OFFLINE, 0, CONTINUE, FALSE, FALSE },
    { "leaf revocation offline, end certificate checked", BLOB(CertLeafOfflineRevocation), BLOB(CertIntermediate), TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN_END_CERT, 0, CRYPT_E_REVOCATION_OFFLINE, FALSE, FALSE },
    { "leaf revocation offline, end certificate checked, no-check ignored only", BLOB(CertLeafOfflineRevocation), BLOB(CertIntermediate), TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN_END_CERT | SCH_CRED_IGNORE_NO_REVOCATION_CHECK, 0, CRYPT_E_REVOCATION_OFFLINE, FALSE, FALSE },
    { "leaf revocation offline, end certificate checked, offline ignored", BLOB(CertLeafOfflineRevocation), BLOB(CertIntermediate), TEST_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN_END_CERT | SCH_CRED_IGNORE_REVOCATION_OFFLINE, 0, CONTINUE, FALSE, FALSE },
    { "wrong host, revocation offline, chain checked", BLOB(CertLeafOfflineRevocation), BLOB(CertIntermediate), TEST_OTHER_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN, 0, SEC_E_WRONG_PRINCIPAL, FALSE, FALSE },
    { "wrong host, revocation offline, end certificate checked", BLOB(CertLeafOfflineRevocation), BLOB(CertIntermediate), TEST_OTHER_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN_END_CERT, 0, SEC_E_WRONG_PRINCIPAL, FALSE, FALSE },
    { "wrong host, revocation offline ignored, chain checked", BLOB(CertLeafOfflineRevocation), BLOB(CertIntermediate), TEST_OTHER_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN | SCH_CRED_IGNORE_REVOCATION_OFFLINE, 0, SEC_E_WRONG_PRINCIPAL, FALSE, FALSE },
    { "wrong host, revocation offline ignored, end certificate checked", BLOB(CertLeafOfflineRevocation), BLOB(CertIntermediate), TEST_OTHER_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN_END_CERT | SCH_CRED_IGNORE_REVOCATION_OFFLINE, 0, SEC_E_WRONG_PRINCIPAL, FALSE, FALSE },
    { "wrong host, revocation offline, no-check ignored only", BLOB(CertLeafOfflineRevocation), BLOB(CertIntermediate), TEST_OTHER_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN | SCH_CRED_IGNORE_NO_REVOCATION_CHECK, 0, SEC_E_WRONG_PRINCIPAL, FALSE, FALSE },
    { "wrong host, revocation offline, both ignored", BLOB(CertLeafOfflineRevocation), BLOB(CertIntermediate), TEST_OTHER_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN | SCH_CRED_IGNORE_NO_REVOCATION_CHECK | SCH_CRED_IGNORE_REVOCATION_OFFLINE, 0, SEC_E_WRONG_PRINCIPAL, FALSE, FALSE },
    { "wrong host, no revocation information", BLOB(CertLeafValid), NoCert, TEST_OTHER_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN_END_CERT, 0, SEC_E_WRONG_PRINCIPAL, FALSE, FALSE },
    { "wrong host, no revocation information, check ignored", BLOB(CertLeafValid), NoCert, TEST_OTHER_HOST, SCH_CRED_REVOCATION_CHECK_CHAIN_END_CERT | SCH_CRED_IGNORE_NO_REVOCATION_CHECK, 0, SEC_E_WRONG_PRINCIPAL, FALSE, FALSE },
    { "explicit automatic validation: expired", BLOB(CertLeafExpired), NoCert, TEST_HOST, SCH_CRED_AUTO_CRED_VALIDATION, 0, SEC_E_CERT_EXPIRED, FALSE, FALSE },

    /* The caller validates: ISC_REQ_MANUAL_CRED_VALIDATION */
    { "manual (context): valid", BLOB(CertLeafValid), NoCert, TEST_HOST, 0, ISC_REQ_MANUAL_CRED_VALIDATION, CONTINUE, FALSE, FALSE },
    { "manual (context): expired", BLOB(CertLeafExpired), NoCert, TEST_HOST, 0, ISC_REQ_MANUAL_CRED_VALIDATION, CONTINUE, FALSE, FALSE },
    { "manual (context): wrong host name", BLOB(CertLeafWrongHost), NoCert, TEST_HOST, 0, ISC_REQ_MANUAL_CRED_VALIDATION, CONTINUE, FALSE, FALSE },
    { "manual (context): self-signed", BLOB(CertLeafSelfSigned), NoCert, TEST_HOST, 0, ISC_REQ_MANUAL_CRED_VALIDATION, CONTINUE, FALSE, FALSE },
    { "manual (context): untrusted root", BLOB(CertLeafUntrustedRoot), BLOB(CertUntrustedRoot), TEST_HOST, 0, ISC_REQ_MANUAL_CRED_VALIDATION, CONTINUE, FALSE, FALSE },

    /* The caller validates: SCH_CRED_MANUAL_CRED_VALIDATION */
    { "manual (credential): valid", BLOB(CertLeafValid), NoCert, TEST_HOST, SCH_CRED_MANUAL_CRED_VALIDATION, 0, CONTINUE, FALSE, FALSE },
    { "manual (credential): expired", BLOB(CertLeafExpired), NoCert, TEST_HOST, SCH_CRED_MANUAL_CRED_VALIDATION, 0, CONTINUE, FALSE, FALSE },
    { "manual (credential): wrong host name", BLOB(CertLeafWrongHost), NoCert, TEST_HOST, SCH_CRED_MANUAL_CRED_VALIDATION, 0, CONTINUE, FALSE, FALSE },
    { "manual (credential): self-signed", BLOB(CertLeafSelfSigned), NoCert, TEST_HOST, SCH_CRED_MANUAL_CRED_VALIDATION, 0, CONTINUE, FALSE, FALSE },

    /* Only the name check is switched off */
    { "no server name check: wrong host name", BLOB(CertLeafWrongHost), NoCert, TEST_HOST, SCH_CRED_NO_SERVERNAME_CHECK, 0, CONTINUE, FALSE, FALSE },
    { "no server name check: expired", BLOB(CertLeafExpired), NoCert, TEST_HOST, SCH_CRED_NO_SERVERNAME_CHECK, 0, SEC_E_CERT_EXPIRED, FALSE, FALSE },
    { "no server name check: self-signed", BLOB(CertLeafSelfSigned), NoCert, TEST_HOST, SCH_CRED_NO_SERVERNAME_CHECK, 0, SEC_E_UNTRUSTED_ROOT, FALSE, FALSE },
};

static HCERTSTORE RootStore;
static PCCERT_CONTEXT RootContext;

static BOOL InstallRoot(void)
{
    PCCERT_CONTEXT Local, Stale;

    RootStore = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                              CERT_SYSTEM_STORE_LOCAL_MACHINE, L"Root");
    if (!RootStore)
    {
        skip("Cannot open the local machine Root store (%lu)\n", GetLastError());
        return FALSE;
    }

    /* A crashed earlier run may have left the test root behind */
    Local = CertCreateCertificateContext(X509_ASN_ENCODING, CertRoot, sizeof(CertRoot));
    if (Local)
    {
        Stale = CertFindCertificateInStore(RootStore, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, Local, NULL);
        if (Stale)
            CertDeleteCertificateFromStore(Stale);
        CertFreeCertificateContext(Local);
    }

    if (!CertAddEncodedCertificateToStore(RootStore, X509_ASN_ENCODING, CertRoot, sizeof(CertRoot),
                                          CERT_STORE_ADD_REPLACE_EXISTING, &RootContext))
    {
        skip("Cannot add the test root to the local machine Root store (%lu)\n", GetLastError());
        CertCloseStore(RootStore, 0);
        RootStore = NULL;
        return FALSE;
    }

    /* The store file is written on commit, and the chain engine reads it from there */
    ok(CertControlStore(RootStore, 0, CERT_STORE_CTRL_COMMIT, NULL), "CertControlStore failed (%lu)\n", GetLastError());
    return TRUE;
}

static void RemoveRoot(void)
{
    if (RootContext)
    {
        ok(CertDeleteCertificateFromStore(RootContext), "CertDeleteCertificateFromStore failed (%lu)\n", GetLastError());
        RootContext = NULL;
    }
    if (RootStore)
    {
        CertControlStore(RootStore, 0, CERT_STORE_CTRL_COMMIT, NULL);
        CertCloseStore(RootStore, 0);
        RootStore = NULL;
    }
}

static BYTE *PutRecord(BYTE *Out, const BYTE *Data, ULONG Size)
{
    memcpy(Out, Data, Size);
    return Out + Size;
}

static BYTE *PutLength24(BYTE *Out, ULONG Value)
{
    *Out++ = (BYTE)(Value >> 16);
    *Out++ = (BYTE)(Value >> 8);
    *Out++ = (BYTE)Value;
    return Out;
}

/* ServerHello, Certificate (the leaf and optionally one more certificate), ServerHelloDone */
static BYTE *BuildServerFlight(const TEST_CASE *Case, ULONG *Size)
{
    ULONG ListSize = 3 + Case->Leaf.Size + (Case->Extra.Data ? 3 + Case->Extra.Size : 0);
    ULONG RecordSize = 4 + 3 + ListSize;
    ULONG Total = sizeof(ServerHelloRecord) + 5 + RecordSize + sizeof(ServerHelloDoneRecord);
    BYTE *Flight = HeapAlloc(GetProcessHeap(), 0, Total);
    BYTE *p;

    if (!Flight)
        return NULL;

    p = PutRecord(Flight, ServerHelloRecord, sizeof(ServerHelloRecord));
    *p++ = 0x16; *p++ = 0x03; *p++ = 0x03;
    *p++ = (BYTE)(RecordSize >> 8);
    *p++ = (BYTE)RecordSize;
    *p++ = 0x0b;
    p = PutLength24(p, 3 + ListSize);
    p = PutLength24(p, ListSize);
    p = PutLength24(p, Case->Leaf.Size);
    p = PutRecord(p, Case->Leaf.Data, Case->Leaf.Size);
    if (Case->Extra.Data)
    {
        p = PutLength24(p, Case->Extra.Size);
        p = PutRecord(p, Case->Extra.Data, Case->Extra.Size);
    }
    p = PutRecord(p, ServerHelloDoneRecord, sizeof(ServerHelloDoneRecord));
    *Size = (ULONG)(p - Flight);
    return Flight;
}

static SECURITY_STATUS RunHandshake(const TEST_CASE *Case, PULONG OutSize, PULONG RetAttr)
{
    const ULONG BaseFlags = ISC_REQ_USE_SESSION_KEY | ISC_REQ_CONFIDENTIALITY |
                            ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_STREAM;
    SCHANNEL_CRED Auth;
    CredHandle Cred;
    CtxtHandle Ctx;
    TimeStamp Expiry;
    SecBuffer OutBuffer, InBuffers[2];
    BYTE OwnBuffer[16384];
    ULONG OutFlags = Case->OwnOutput ? 0 : ISC_REQ_ALLOCATE_MEMORY;
    SecBufferDesc OutDesc, InDesc;
    ULONG Attr;
    SECURITY_STATUS Status;
    BYTE *Flight;
    ULONG FlightSize;

    *OutSize = 0;
    *RetAttr = 0;

    ZeroMemory(&Auth, sizeof(Auth));
    Auth.dwVersion = SCHANNEL_CRED_VERSION;
    Auth.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    Auth.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | Case->CredFlags;
    Status = AcquireCredentialsHandleW(NULL, (SEC_WCHAR *)UNISP_NAME_W, SECPKG_CRED_OUTBOUND,
                                       NULL, &Auth, NULL, NULL, &Cred, &Expiry);
    ok(Status == SEC_E_OK, "AcquireCredentialsHandle returned 0x%08lx\n", Status);
    if (Status != SEC_E_OK)
        return Status;

    OutDesc.ulVersion = SECBUFFER_VERSION;
    OutDesc.cBuffers = 1;
    OutDesc.pBuffers = &OutBuffer;
    OutBuffer.BufferType = SECBUFFER_TOKEN;
    OutBuffer.cbBuffer = Case->OwnOutput ? sizeof(OwnBuffer) : 0;
    OutBuffer.pvBuffer = Case->OwnOutput ? OwnBuffer : NULL;
    Status = InitializeSecurityContextW(&Cred, NULL, (SEC_WCHAR *)Case->Target, BaseFlags | OutFlags | Case->ContextFlags,
                                        0, 0, NULL, 0, &Ctx, &OutDesc, &Attr, &Expiry);
    ok(Status == SEC_I_CONTINUE_NEEDED, "%s: ClientHello step returned 0x%08lx\n", Case->Name, Status);
    if (Status != SEC_I_CONTINUE_NEEDED)
    {
        FreeCredentialsHandle(&Cred);
        return Status;
    }
    ok(OutBuffer.cbBuffer >= 5 && OutBuffer.pvBuffer && ((BYTE *)OutBuffer.pvBuffer)[0] == 0x16,
       "%s: no ClientHello (%lu bytes)\n", Case->Name, OutBuffer.cbBuffer);
    if (!Case->OwnOutput)
        FreeContextBuffer(OutBuffer.pvBuffer);

    Flight = BuildServerFlight(Case, &FlightSize);
    if (!Flight)
    {
        DeleteSecurityContext(&Ctx);
        FreeCredentialsHandle(&Cred);
        return SEC_E_INSUFFICIENT_MEMORY;
    }

    InDesc.ulVersion = SECBUFFER_VERSION;
    InDesc.cBuffers = 2;
    InDesc.pBuffers = InBuffers;
    InBuffers[0].BufferType = SECBUFFER_TOKEN;
    InBuffers[0].cbBuffer = FlightSize;
    InBuffers[0].pvBuffer = Flight;
    InBuffers[1].BufferType = SECBUFFER_EMPTY;
    InBuffers[1].cbBuffer = 0;
    InBuffers[1].pvBuffer = NULL;
    OutBuffer.BufferType = SECBUFFER_TOKEN;
    OutBuffer.cbBuffer = Case->OwnOutput ? sizeof(OwnBuffer) : 0;
    OutBuffer.pvBuffer = Case->OwnOutput ? OwnBuffer : NULL;
    Status = InitializeSecurityContextW(&Cred, &Ctx, (SEC_WCHAR *)Case->Target, BaseFlags | OutFlags | Case->ContextFlags,
                                        0, 0, &InDesc, 0, &Ctx, &OutDesc, &Attr, &Expiry);
    trace("%s: 0x%08lx, %lu output bytes, attributes 0x%lx\n", Case->Name, Status, OutBuffer.cbBuffer, Attr);
    *OutSize = OutBuffer.cbBuffer;
    *RetAttr = Attr;
    if (OutBuffer.pvBuffer && !Case->OwnOutput)
        FreeContextBuffer(OutBuffer.pvBuffer);

    HeapFree(GetProcessHeap(), 0, Flight);
    DeleteSecurityContext(&Ctx);
    FreeCredentialsHandle(&Cred);
    return Status;
}

static void RunCases(void)
{
    ULONG i;

    for (i = 0; i < ARRAYSIZE(TestCases); i++)
    {
        const TEST_CASE *Case = &TestCases[i];
        SECURITY_STATUS Status;
        ULONG OutSize, RetAttr;

        Status = RunHandshake(Case, &OutSize, &RetAttr);

        todo_ros_if(Case->TodoRos)
            ok(Status == Case->Expected, "%s: expected 0x%08lx, got 0x%08lx\n",
               Case->Name, (ULONG)Case->Expected, Status);

        if (Case->Expected == SEC_I_CONTINUE_NEEDED)
        {
            todo_ros_if(Case->TodoRos)
                ok(OutSize > 0, "%s: no key exchange flight\n", Case->Name);
        }
        else
            ok(OutSize == 0, "%s: %lu bytes of output with a failed handshake\n", Case->Name, OutSize);

        /* Only the context flag is reported back */
        ok(!!(RetAttr & ISC_RET_MANUAL_CRED_VALIDATION) == !!(Case->ContextFlags & ISC_REQ_MANUAL_CRED_VALIDATION),
           "%s: attributes 0x%08lx\n", Case->Name, RetAttr);
    }
}

START_TEST(SchannelServerCert)
{
    if (!InstallRoot())
        return;

    RunCases();
    RemoveRoot();
}
