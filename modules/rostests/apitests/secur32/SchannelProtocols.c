/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     schannel negotiates TLS 1.2 and TLS 1.3 with servers that allow only one of them
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The servers come from the environment as host:port and are not part of the tree: run
 * "tlsserver.py PORT 1.2 1.2 CERT KEY" for SCHANNEL_TEST_TLS12 and "tlsserver.py PORT 1.3 1.3 CERT KEY" for
 * SCHANNEL_TEST_TLS13 (modules/rostests/apitests/secur32/tlsserver.py; a self-signed certificate is fine,
 * the test asks for manual validation). Without them the test skips.
 */

#define SECURITY_WIN32
#include <apitest.h>
#include <winsock2.h>
#include <wincrypt.h>
#include <sspi.h>
#include <schannel.h>

#ifndef SP_PROT_TLS1_2_CLIENT
#define SP_PROT_TLS1_2_CLIENT 0x800
#endif
#ifndef SP_PROT_TLS1_3_CLIENT
#define SP_PROT_TLS1_3_CLIENT 0x2000
#endif

#define BUFFER_SIZE 0x20000

typedef struct _CONNECTION
{
    CredHandle Cred;
    CtxtHandle Ctx;
    SOCKET Socket;
    BYTE *Buffer;
    ULONG Used;
    BOOL HaveContext;
} CONNECTION;

typedef struct _RESULT
{
    SECURITY_STATUS Status;
    DWORD Protocol;
    BOOL GotResponse;
} RESULT;

static BOOL IsReactOS(void)
{
    return !strcmp(winetest_platform, "reactos");
}

static SOCKET ConnectTo(PCSTR Address)
{
    CHAR Host[128];
    PSTR Colon;
    struct sockaddr_in Peer;
    SOCKET Socket;

    lstrcpynA(Host, Address, sizeof(Host));
    Colon = strchr(Host, ':');
    if (!Colon)
        return INVALID_SOCKET;
    *Colon = 0;

    ZeroMemory(&Peer, sizeof(Peer));
    Peer.sin_family = AF_INET;
    Peer.sin_port = htons((USHORT)atoi(Colon + 1));
    Peer.sin_addr.s_addr = inet_addr(Host);

    Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Socket == INVALID_SOCKET)
        return INVALID_SOCKET;
    if (connect(Socket, (struct sockaddr *)&Peer, sizeof(Peer)))
    {
        closesocket(Socket);
        return INVALID_SOCKET;
    }
    return Socket;
}

static BOOL SendAll(SOCKET Socket, const BYTE *Data, ULONG Size)
{
    while (Size)
    {
        int Sent = send(Socket, (const char *)Data, Size, 0);
        if (Sent <= 0)
            return FALSE;
        Data += Sent;
        Size -= Sent;
    }
    return TRUE;
}

/* The handshake loop of a client; the last token is sent also when the call returns SEC_E_OK, as TLS 1.3 needs */
static SECURITY_STATUS Handshake(CONNECTION *Conn, PCSTR Target, DWORD Protocols)
{
    const ULONG Flags = ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_USE_SESSION_KEY | ISC_REQ_CONFIDENTIALITY |
                        ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_STREAM |
                        ISC_REQ_MANUAL_CRED_VALIDATION;
    SCHANNEL_CRED Auth;
    SecBuffer InBuffers[2], OutBuffer;
    SecBufferDesc InDesc, OutDesc;
    SECURITY_STATUS Status;
    TimeStamp Expiry;
    BOOL First = TRUE;
    ULONG Attr;
    int Received;

    ZeroMemory(&Auth, sizeof(Auth));
    Auth.dwVersion = SCHANNEL_CRED_VERSION;
    Auth.grbitEnabledProtocols = Protocols;
    Auth.dwFlags = SCH_CRED_NO_DEFAULT_CREDS;
    Status = AcquireCredentialsHandleA(NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND,
                                       NULL, &Auth, NULL, NULL, &Conn->Cred, &Expiry);
    if (Status != SEC_E_OK)
        return Status;

    OutDesc.ulVersion = SECBUFFER_VERSION;
    OutDesc.cBuffers = 1;
    OutDesc.pBuffers = &OutBuffer;
    InDesc.ulVersion = SECBUFFER_VERSION;
    InDesc.cBuffers = 2;
    InDesc.pBuffers = InBuffers;

    for (;;)
    {
        OutBuffer.BufferType = SECBUFFER_TOKEN;
        OutBuffer.cbBuffer = 0;
        OutBuffer.pvBuffer = NULL;

        if (First)
        {
            Status = InitializeSecurityContextA(&Conn->Cred, NULL, (SEC_CHAR *)Target, Flags, 0, 0, NULL, 0,
                                                &Conn->Ctx, &OutDesc, &Attr, &Expiry);
        }
        else
        {
            InBuffers[0].BufferType = SECBUFFER_TOKEN;
            InBuffers[0].cbBuffer = Conn->Used;
            InBuffers[0].pvBuffer = Conn->Buffer;
            InBuffers[1].BufferType = SECBUFFER_EMPTY;
            InBuffers[1].cbBuffer = 0;
            InBuffers[1].pvBuffer = NULL;
            Status = InitializeSecurityContextA(&Conn->Cred, &Conn->Ctx, (SEC_CHAR *)Target, Flags, 0, 0, &InDesc,
                                                0, NULL, &OutDesc, &Attr, &Expiry);
        }

        if (OutBuffer.cbBuffer && OutBuffer.pvBuffer)
        {
            BOOL Sent = SendAll(Conn->Socket, OutBuffer.pvBuffer, OutBuffer.cbBuffer);

            FreeContextBuffer(OutBuffer.pvBuffer);
            if (!Sent)
                return SEC_E_INTERNAL_ERROR;
        }

        if (!First && Status != SEC_E_INCOMPLETE_MESSAGE)
        {
            /* What the handshake did not consume stays in the buffer */
            if (InBuffers[1].BufferType == SECBUFFER_EXTRA && InBuffers[1].cbBuffer)
            {
                memmove(Conn->Buffer, Conn->Buffer + Conn->Used - InBuffers[1].cbBuffer, InBuffers[1].cbBuffer);
                Conn->Used = InBuffers[1].cbBuffer;
            }
            else
            {
                Conn->Used = 0;
            }
        }
        if (First && (Status == SEC_I_CONTINUE_NEEDED || Status == SEC_E_OK))
            Conn->HaveContext = TRUE;
        First = FALSE;

        if (Status == SEC_E_OK)
            return Status;

        if (Status != SEC_I_CONTINUE_NEEDED && Status != SEC_E_INCOMPLETE_MESSAGE)
            return Status;

        /* Leftover records go to the next call before the socket is read again */
        if (Status == SEC_I_CONTINUE_NEEDED && Conn->Used)
            continue;

        Received = recv(Conn->Socket, (char *)Conn->Buffer + Conn->Used, BUFFER_SIZE - Conn->Used, 0);
        if (Received <= 0)
            return SEC_E_INTERNAL_ERROR;
        Conn->Used += Received;
    }
}

/* One GET over the established connection; the response must be a 200 */
static BOOL GetPage(CONNECTION *Conn)
{
    static const char Request[] = "GET / HTTP/1.1\r\nHost: schannel.test\r\nConnection: close\r\n\r\n";
    SecPkgContext_StreamSizes Sizes;
    SecBuffer Buffers[4];
    SecBufferDesc Desc = { SECBUFFER_VERSION, 4, Buffers };
    BYTE *Send, *Plain;
    ULONG PlainSize = 0, i;
    SECURITY_STATUS Status;
    BOOL Result = FALSE;
    int Received;

    if (QueryContextAttributesA(&Conn->Ctx, SECPKG_ATTR_STREAM_SIZES, &Sizes) != SEC_E_OK)
        return FALSE;

    Send = HeapAlloc(GetProcessHeap(), 0, Sizes.cbHeader + sizeof(Request) + Sizes.cbTrailer);
    Plain = HeapAlloc(GetProcessHeap(), 0, BUFFER_SIZE * 4);
    if (!Send || !Plain)
        goto done;

    memcpy(Send + Sizes.cbHeader, Request, sizeof(Request) - 1);
    Buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
    Buffers[0].cbBuffer = Sizes.cbHeader;
    Buffers[0].pvBuffer = Send;
    Buffers[1].BufferType = SECBUFFER_DATA;
    Buffers[1].cbBuffer = sizeof(Request) - 1;
    Buffers[1].pvBuffer = Send + Sizes.cbHeader;
    Buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
    Buffers[2].cbBuffer = Sizes.cbTrailer;
    Buffers[2].pvBuffer = Send + Sizes.cbHeader + sizeof(Request) - 1;
    Buffers[3].BufferType = SECBUFFER_EMPTY;
    Buffers[3].cbBuffer = 0;
    Buffers[3].pvBuffer = NULL;
    if (EncryptMessage(&Conn->Ctx, 0, &Desc, 0) != SEC_E_OK)
        goto done;
    if (!SendAll(Conn->Socket, Send, Buffers[0].cbBuffer + Buffers[1].cbBuffer + Buffers[2].cbBuffer))
        goto done;

    /* Decrypt until the server closes; records without data (session tickets) just move on */
    for (;;)
    {
        if (!Conn->Used)
        {
            Received = recv(Conn->Socket, (char *)Conn->Buffer, BUFFER_SIZE, 0);
            if (Received <= 0)
                break;
            Conn->Used = Received;
        }

        ZeroMemory(Buffers, sizeof(Buffers));
        Buffers[0].BufferType = SECBUFFER_DATA;
        Buffers[0].cbBuffer = Conn->Used;
        Buffers[0].pvBuffer = Conn->Buffer;
        Status = DecryptMessage(&Conn->Ctx, &Desc, 0, NULL);

        if (Status == SEC_E_INCOMPLETE_MESSAGE)
        {
            Received = recv(Conn->Socket, (char *)Conn->Buffer + Conn->Used, BUFFER_SIZE - Conn->Used, 0);
            if (Received <= 0)
                break;
            Conn->Used += Received;
            continue;
        }
        if (Status == SEC_I_CONTEXT_EXPIRED)
            break;
        if (Status != SEC_E_OK)
            goto done;

        Conn->Used = 0;
        for (i = 0; i < 4; i++)
        {
            if (Buffers[i].BufferType == SECBUFFER_DATA && PlainSize + Buffers[i].cbBuffer <= BUFFER_SIZE * 4)
            {
                memcpy(Plain + PlainSize, Buffers[i].pvBuffer, Buffers[i].cbBuffer);
                PlainSize += Buffers[i].cbBuffer;
            }
            else if (Buffers[i].BufferType == SECBUFFER_EXTRA && Buffers[i].cbBuffer)
            {
                /* the extra bytes lie behind the record in the same buffer */
                memmove(Conn->Buffer, Buffers[i].pvBuffer, Buffers[i].cbBuffer);
                Conn->Used = Buffers[i].cbBuffer;
            }
        }
    }

    Result = PlainSize > 12 && !memcmp(Plain, "HTTP/1.1 200", 12);

done:
    HeapFree(GetProcessHeap(), 0, Send);
    HeapFree(GetProcessHeap(), 0, Plain);
    return Result;
}

static void Connect(PCSTR Address, DWORD Protocols, RESULT *Result)
{
    CONNECTION Conn;
    SecPkgContext_ConnectionInfo Info;

    ZeroMemory(&Conn, sizeof(Conn));
    ZeroMemory(Result, sizeof(*Result));
    Conn.Socket = ConnectTo(Address);
    Conn.Buffer = HeapAlloc(GetProcessHeap(), 0, BUFFER_SIZE);
    if (Conn.Socket == INVALID_SOCKET || !Conn.Buffer)
    {
        ok(0, "cannot connect to %s (%d)\n", Address, WSAGetLastError());
        Result->Status = SEC_E_INTERNAL_ERROR;
        goto done;
    }

    Result->Status = Handshake(&Conn, "schannel.test", Protocols);
    if (Result->Status == SEC_E_OK)
    {
        ZeroMemory(&Info, sizeof(Info));
        if (QueryContextAttributesA(&Conn.Ctx, SECPKG_ATTR_CONNECTION_INFO, &Info) == SEC_E_OK)
            Result->Protocol = Info.dwProtocol;
        Result->GotResponse = GetPage(&Conn);
    }
    /* a failed handshake leaves a context behind as well */
    if (Conn.HaveContext)
        DeleteSecurityContext(&Conn.Ctx);
    if (Conn.Cred.dwLower || Conn.Cred.dwUpper)
        FreeCredentialsHandle(&Conn.Cred);

done:
    if (Conn.Socket != INVALID_SOCKET)
        closesocket(Conn.Socket);
    HeapFree(GetProcessHeap(), 0, Conn.Buffer);
}

#define WORKERS 8
#define ROUNDS 5

typedef struct _WORKER
{
    PCSTR Address;
    DWORD Protocols;
    LONG Failures;
} WORKER;

static BOOL Tls13Supported(void)
{
    SCHANNEL_CRED Auth;
    CredHandle Cred;
    BOOL Supported;

    ZeroMemory(&Auth, sizeof(Auth));
    Auth.dwVersion = SCHANNEL_CRED_VERSION;
    Auth.grbitEnabledProtocols = SP_PROT_TLS1_3_CLIENT;
    Auth.dwFlags = SCH_CRED_NO_DEFAULT_CREDS;
    Supported = AcquireCredentialsHandleA(NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &Auth,
                                          NULL, NULL, &Cred, NULL) == SEC_E_OK;
    if (Supported)
        FreeCredentialsHandle(&Cred);
    return Supported;
}

/* Sessions of several threads share the crypto state of the library */
static DWORD WINAPI ConnectionWorker(void *Param)
{
    WORKER *Worker = Param;
    RESULT Result;
    ULONG i;

    for (i = 0; i < ROUNDS; i++)
    {
        Connect(Worker->Address, Worker->Protocols, &Result);
        if (Result.Status != SEC_E_OK || Result.Protocol != Worker->Protocols || !Result.GotResponse)
            InterlockedIncrement(&Worker->Failures);
    }

    return 0;
}

static void TestParallel(PCSTR Tls12, PCSTR Tls13)
{
    WORKER Workers[WORKERS];
    HANDLE Threads[WORKERS];
    BOOL Have13 = Tls13 && Tls13Supported();
    ULONG i, Started = 0;
    LONG Failures = 0;

    if (!Tls12 && !Have13)
        return;

    for (i = 0; i < WORKERS; i++)
    {
        BOOL Use13 = Have13 && (!Tls12 || (i & 1));

        Workers[i].Address = Use13 ? Tls13 : Tls12;
        Workers[i].Protocols = Use13 ? SP_PROT_TLS1_3_CLIENT : SP_PROT_TLS1_2_CLIENT;
        Workers[i].Failures = 0;
        Threads[i] = CreateThread(NULL, 0, ConnectionWorker, &Workers[i], 0, NULL);
        if (Threads[i])
            Started++;
    }

    for (i = 0; i < WORKERS; i++)
    {
        if (!Threads[i])
            continue;
        WaitForSingleObject(Threads[i], 120000);
        CloseHandle(Threads[i]);
        Failures += Workers[i].Failures;
    }

    ok(Started == WORKERS, "%lu workers started\n", Started);
    ok(Failures == 0, "%ld of %lu connections of parallel sessions failed\n", Failures, Started * ROUNDS);
}

START_TEST(SchannelProtocols)
{
    PCSTR Tls12 = getenv("SCHANNEL_TEST_TLS12");
    PCSTR Tls13 = getenv("SCHANNEL_TEST_TLS13");
    WSADATA Data;
    RESULT Result;

    if (!Tls12 && !Tls13)
    {
        skip("SCHANNEL_TEST_TLS12 and SCHANNEL_TEST_TLS13 name no servers\n");
        return;
    }

    ok(!WSAStartup(MAKEWORD(2, 2), &Data), "WSAStartup failed\n");

    if (Tls12)
    {
        Connect(Tls12, SP_PROT_TLS1_2_CLIENT, &Result);
        ok(Result.Status == SEC_E_OK, "TLS 1.2 against a TLS 1.2 server: 0x%08lx\n", Result.Status);
        ok(Result.Protocol == SP_PROT_TLS1_2_CLIENT, "TLS 1.2 protocol is 0x%lx\n", Result.Protocol);
        ok(Result.GotResponse, "TLS 1.2: no response\n");

        /* Either the credentials refuse the protocol (Windows without TLS 1.3) or the handshake fails */
        Connect(Tls12, SP_PROT_TLS1_3_CLIENT, &Result);
        ok(Result.Status != SEC_E_OK, "TLS 1.3 against a TLS 1.2 server worked\n");
    }

    if (Tls13)
    {
        Connect(Tls13, SP_PROT_TLS1_3_CLIENT, &Result);
        if (Result.Status == SEC_E_ALGORITHM_MISMATCH && !IsReactOS())
        {
            win_skip("TLS 1.3 is not supported by this Windows\n");
        }
        else
        {
            ok(Result.Status == SEC_E_OK, "TLS 1.3 against a TLS 1.3 server: 0x%08lx\n", Result.Status);
            ok(Result.Protocol == SP_PROT_TLS1_3_CLIENT, "TLS 1.3 protocol is 0x%lx\n", Result.Protocol);
            ok(Result.GotResponse, "TLS 1.3: no response\n");
        }

        Connect(Tls13, SP_PROT_TLS1_2_CLIENT, &Result);
        ok(Result.Status != SEC_E_OK, "TLS 1.2 against a TLS 1.3 server worked\n");
    }

    TestParallel(Tls12, Tls13);

    WSACleanup();
}
