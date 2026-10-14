/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for gzip response bodies that are cut or damaged, and for the receive timeout
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#include <windef.h>
#include <winsock2.h>
#include <wininet.h>
#include <stdio.h>
#include <string.h>

#ifndef INTERNET_OPTION_HTTP_DECODING
#define INTERNET_OPTION_HTTP_DECODING 65
#endif

#define TIMEOUT_MS 6000

typedef enum
{
    EXPECT_DATA,        /* the bytes of the case, then the end of the body */
    EXPECT_DECODE,      /* ERROR_INTERNET_DECODING_FAILED and no data */
    EXPECT_TIMEOUT,     /* ERROR_INTERNET_TIMEOUT after the receive timeout */
    EXPECT_STOP         /* an error before the receive timeout is over, or the timeout; never a wait for longer */
} EXPECT;

/* The server hands one connection the bytes of one case, then closes it or keeps it open */
typedef struct
{
    PCSTR Name;
    const BYTE *Body;
    UINT BodyLen;
    BOOL Open;              /* announce more than is sent and keep the connection open (up to 20 seconds) */
    BOOL Plain;             /* no Content-Encoding header */
    BOOL DefaultTimeout;    /* leave the receive timeout at its default */
    EXPECT Expect;
    UINT ExpectedBytes;
} CASE;

static HMODULE s_hWininet;
static HMODULE s_hWs2;

static HINTERNET (WINAPI *pInternetOpenA)(LPCSTR, DWORD, LPCSTR, LPCSTR, DWORD);
static HINTERNET (WINAPI *pInternetConnectA)(HINTERNET, LPCSTR, INTERNET_PORT, LPCSTR, LPCSTR, DWORD, DWORD, DWORD_PTR);
static HINTERNET (WINAPI *pHttpOpenRequestA)(HINTERNET, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR *, DWORD, DWORD_PTR);
static BOOL (WINAPI *pHttpSendRequestA)(HINTERNET, LPCSTR, DWORD, LPVOID, DWORD);
static BOOL (WINAPI *pInternetReadFile)(HINTERNET, LPVOID, DWORD, LPDWORD);
static BOOL (WINAPI *pInternetSetOptionA)(HINTERNET, DWORD, LPVOID, DWORD);
static BOOL (WINAPI *pInternetCloseHandle)(HINTERNET);

static int (WSAAPI *pWSAStartup)(WORD, LPWSADATA);
static int (WSAAPI *pWSACleanup)(void);
static SOCKET (WSAAPI *psocket)(int, int, int);
static int (WSAAPI *pbind)(SOCKET, const struct sockaddr *, int);
static int (WSAAPI *plisten)(SOCKET, int);
static SOCKET (WSAAPI *paccept)(SOCKET, struct sockaddr *, int *);
static int (WSAAPI *pgetsockname)(SOCKET, struct sockaddr *, int *);
static int (WSAAPI *precv)(SOCKET, char *, int, int);
static int (WSAAPI *psend)(SOCKET, const char *, int, int);
static int (WSAAPI *pclosesocket)(SOCKET);
static int (WSAAPI *pshutdown)(SOCKET, int);
static unsigned long (WSAAPI *pinet_addr)(const char *);

static BOOL LoadFunctions(void)
{
    s_hWininet = LoadLibraryA("wininet.dll");
    s_hWs2 = LoadLibraryA("ws2_32.dll");
    if (!s_hWininet || !s_hWs2)
        return FALSE;
#define GET_INET(name) if (!(p##name = (void *)GetProcAddress(s_hWininet, #name))) return FALSE
    GET_INET(InternetOpenA); GET_INET(InternetConnectA); GET_INET(HttpOpenRequestA); GET_INET(HttpSendRequestA);
    GET_INET(InternetReadFile); GET_INET(InternetSetOptionA); GET_INET(InternetCloseHandle);
#undef GET_INET
#define GET_WS(name) if (!(p##name = (void *)GetProcAddress(s_hWs2, #name))) return FALSE
    GET_WS(WSAStartup); GET_WS(WSACleanup); GET_WS(socket); GET_WS(bind); GET_WS(listen); GET_WS(accept);
    GET_WS(getsockname); GET_WS(recv); GET_WS(send); GET_WS(closesocket); GET_WS(shutdown); GET_WS(inet_addr);
#undef GET_WS
    return TRUE;
}

typedef struct
{
    SOCKET Listener;
    const CASE *Case;
} SERVER;

static DWORD WINAPI ServerThread(PVOID pParam)
{
    SERVER *pServer = pParam;
    const CASE *pCase = pServer->Case;
    SOCKET Client = paccept(pServer->Listener, NULL, NULL);
    char Request[2048], Header[256];
    int cbRequest = 0, cbHeader, cb;
    UINT Sent;

    if (Client == INVALID_SOCKET)
        return 1;
    while (cbRequest < (int)sizeof(Request) - 1)
    {
        cb = precv(Client, Request + cbRequest, sizeof(Request) - 1 - cbRequest, 0);
        if (cb <= 0)
            break;
        cbRequest += cb;
        Request[cbRequest] = 0;
        if (strstr(Request, "\r\n\r\n"))
            break;
    }
    cbHeader = sprintf(Header,
                       "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n%s"
                       "Content-Length: %u\r\nConnection: close\r\n\r\n",
                       pCase->Plain ? "" : "Content-Encoding: gzip\r\n",
                       pCase->BodyLen + (pCase->Open ? 100 : 0));
    psend(Client, Header, cbHeader, 0);
    for (Sent = 0; Sent < pCase->BodyLen; Sent += cb)
    {
        cb = psend(Client, (const char *)pCase->Body + Sent, pCase->BodyLen - Sent, 0);
        if (cb <= 0)
            break;
    }
    if (pCase->Open)
    {
        /* Wait for the client to give up, at most 20 seconds */
        DWORD dwStart = GetTickCount();
        char Byte;

        while (GetTickCount() - dwStart < 20000)
        {
            Sleep(100);
            if (precv(Client, &Byte, 1, MSG_PEEK) <= 0)
                break;
        }
    }
    pshutdown(Client, 2);
    pclosesocket(Client);
    return 0;
}

/* A stream is the header, deflate blocks and a trailer of the CRC-32 and the size of the data.
 * The blocks here are stored ones: a flag byte (1 final, 0 not), the length, its complement, the bytes */
#define GZ_HEADER 0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0x03
#define BLOCK_HELLO_FINAL 0x01, 0x05, 0x00, 0xfa, 0xff, 'h', 'e', 'l', 'l', 'o'
#define BLOCK_HELLO 0x00, 0x05, 0x00, 0xfa, 0xff, 'h', 'e', 'l', 'l', 'o'
#define TRAILER_HELLO 0x86, 0xa6, 0x10, 0x36, 0x05, 0x00, 0x00, 0x00

static const BYTE GzValid[] = { GZ_HEADER, BLOCK_HELLO_FINAL, TRAILER_HELLO };
static const BYTE GzBadCrc[] = { GZ_HEADER, BLOCK_HELLO_FINAL, 0xde, 0xad, 0xbe, 0xef, 0x05, 0x00, 0x00, 0x00 };
static const BYTE GzBadSize[] = { GZ_HEADER, BLOCK_HELLO_FINAL, 0x86, 0xa6, 0x10, 0x36, 0x06, 0x00, 0x00, 0x00 };
static const BYTE GzNoTrailer[] = { GZ_HEADER, BLOCK_HELLO_FINAL };
static const BYTE GzHalfTrailer[] = { GZ_HEADER, BLOCK_HELLO_FINAL, 0x86, 0xa6, 0x10 };
static const BYTE GzTrailingGarbage[] = { GZ_HEADER, BLOCK_HELLO_FINAL, TRAILER_HELLO, 'j', 'u', 'n', 'k', 'j', 'u', 'n', 'k' };
/* A block that is not final, then bytes that are no block header */
static const BYTE GzBlockThenGarbage[] =
{
    GZ_HEADER, BLOCK_HELLO,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};
/* A block that is not final, then the body ends */
static const BYTE GzBlockThenEnd[] = { GZ_HEADER, BLOCK_HELLO };
/* A block that announces 100 bytes and carries 5, then the body ends */
static const BYTE GzBlockCut[] = { GZ_HEADER, 0x01, 0x64, 0x00, 0x9b, 0xff, 'h', 'e', 'l', 'l', 'o' };
static const BYTE GzHeaderOnly[] = { GZ_HEADER };
static const BYTE NotGzip[] = "this is plain text and no gzip stream at all, 0123456789";

/* All of these behave the same on Windows Server 2008 R2 and Windows 10 */
static const CASE Cases[] =
{
    { "valid", GzValid, sizeof(GzValid), FALSE, FALSE, FALSE, EXPECT_DATA, 5 },
    { "bad-crc", GzBadCrc, sizeof(GzBadCrc), FALSE, FALSE, FALSE, EXPECT_DATA, 5 },
    { "bad-size", GzBadSize, sizeof(GzBadSize), FALSE, FALSE, FALSE, EXPECT_DECODE, 0 },
    { "no-trailer", GzNoTrailer, sizeof(GzNoTrailer), FALSE, FALSE, FALSE, EXPECT_DATA, 5 },
    { "half-trailer", GzHalfTrailer, sizeof(GzHalfTrailer), FALSE, FALSE, FALSE, EXPECT_DATA, 5 },
    { "trailing-garbage", GzTrailingGarbage, sizeof(GzTrailingGarbage), FALSE, FALSE, FALSE, EXPECT_DATA, 5 },
    { "block-then-garbage", GzBlockThenGarbage, sizeof(GzBlockThenGarbage), FALSE, FALSE, FALSE, EXPECT_DECODE, 0 },
    { "block-then-end", GzBlockThenEnd, sizeof(GzBlockThenEnd), FALSE, FALSE, FALSE, EXPECT_DATA, 5 },
    { "block-cut", GzBlockCut, sizeof(GzBlockCut), FALSE, FALSE, FALSE, EXPECT_DATA, 5 },
    { "header-only", GzHeaderOnly, sizeof(GzHeaderOnly), FALSE, FALSE, FALSE, EXPECT_DATA, 0 },
    { "not-gzip", NotGzip, sizeof(NotGzip) - 1, FALSE, FALSE, FALSE, EXPECT_DECODE, 0 },
    /* The body is not complete and the server keeps the connection open */
    { "plain-open", NotGzip, sizeof(NotGzip) - 1, TRUE, TRUE, FALSE, EXPECT_TIMEOUT, 0 },
    { "block-cut-open", GzBlockCut, sizeof(GzBlockCut), TRUE, FALSE, FALSE, EXPECT_TIMEOUT, 0 },
    { "block-then-garbage-open", GzBlockThenGarbage, sizeof(GzBlockThenGarbage), TRUE, FALSE, FALSE, EXPECT_STOP, 0 },
    { "block-cut-open-default", GzBlockCut, sizeof(GzBlockCut), TRUE, FALSE, TRUE, EXPECT_TIMEOUT, 0 },
};

typedef struct
{
    UINT Bytes;
    BOOL Ok;            /* the last InternetReadFile succeeded */
    DWORD Error;        /* its error when it failed */
    DWORD TotalMs;
    UINT Calls;
    BOOL RequestFailed;
    DWORD RequestError;
} OUTCOME;

static void RunCase(const CASE *pCase, OUTCOME *pOutcome)
{
    SERVER Server = { INVALID_SOCKET, pCase };
    struct sockaddr_in Addr;
    int cbAddr = sizeof(Addr);
    HANDLE hThread;
    HINTERNET hInternet, hConnect, hRequest;
    BOOL bDecode = TRUE;
    DWORD dwTimeout = TIMEOUT_MS, dwStart;
    BYTE Buffer[64];

    ZeroMemory(pOutcome, sizeof(*pOutcome));
    Server.Listener = psocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = pinet_addr("127.0.0.1");
    pbind(Server.Listener, (struct sockaddr *)&Addr, sizeof(Addr));
    plisten(Server.Listener, 1);
    pgetsockname(Server.Listener, (struct sockaddr *)&Addr, &cbAddr);
    hThread = CreateThread(NULL, 0, ServerThread, &Server, 0, NULL);

    hInternet = pInternetOpenA("gzip-test", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    pInternetSetOptionA(hInternet, INTERNET_OPTION_HTTP_DECODING, &bDecode, sizeof(bDecode));
    hConnect = pInternetConnectA(hInternet, "127.0.0.1", (INTERNET_PORT)((Addr.sin_port >> 8) | ((Addr.sin_port & 0xff) << 8)),
                                 NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    hRequest = pHttpOpenRequestA(hConnect, "GET", "/gz", NULL, NULL, NULL,
                                 INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_KEEP_CONNECTION, 0);
    if (!pCase->DefaultTimeout)
        pInternetSetOptionA(hRequest, INTERNET_OPTION_RECEIVE_TIMEOUT, &dwTimeout, sizeof(dwTimeout));
    dwStart = GetTickCount();
    if (!pHttpSendRequestA(hRequest, "Accept-Encoding: gzip\r\n", (DWORD)-1, NULL, 0))
    {
        pOutcome->RequestFailed = TRUE;
        pOutcome->RequestError = GetLastError();
    }
    else
    {
        for (;;)
        {
            DWORD cbRead = 0;

            pOutcome->Ok = pInternetReadFile(hRequest, Buffer, sizeof(Buffer), &cbRead);
            pOutcome->Error = pOutcome->Ok ? 0 : GetLastError();
            pOutcome->Calls++;
            if (!pOutcome->Ok || !cbRead || pOutcome->Calls > 50)
                break;
            pOutcome->Bytes += cbRead;
        }
    }
    pOutcome->TotalMs = GetTickCount() - dwStart;

    pInternetCloseHandle(hRequest);
    pInternetCloseHandle(hConnect);
    pInternetCloseHandle(hInternet);
    pclosesocket(Server.Listener);
    WaitForSingleObject(hThread, 25000);
    CloseHandle(hThread);
}

static void CheckOutcome(const CASE *pCase, const OUTCOME *pOutcome)
{
    DWORD dwExpected = pCase->DefaultTimeout ? 30000 : TIMEOUT_MS;

    ok(!pOutcome->RequestFailed, "%s: HttpSendRequest failed: %lu\n", pCase->Name, pOutcome->RequestError);
    if (pOutcome->RequestFailed)
        return;
    switch (pCase->Expect)
    {
        case EXPECT_DATA:
            ok(pOutcome->Ok, "%s: InternetReadFile failed: %lu\n", pCase->Name, pOutcome->Error);
            ok(pOutcome->Bytes == pCase->ExpectedBytes, "%s: %u bytes, expected %u\n", pCase->Name, pOutcome->Bytes, pCase->ExpectedBytes);
            break;
        case EXPECT_DECODE:
            ok(!pOutcome->Ok && pOutcome->Error == ERROR_INTERNET_DECODING_FAILED,
               "%s: InternetReadFile returned %d, error %lu, expected ERROR_INTERNET_DECODING_FAILED\n",
               pCase->Name, pOutcome->Ok, pOutcome->Error);
            ok(pOutcome->Bytes == 0, "%s: %u bytes before the error\n", pCase->Name, pOutcome->Bytes);
            break;
        case EXPECT_TIMEOUT:
            ok(!pOutcome->Ok && pOutcome->Error == ERROR_INTERNET_TIMEOUT,
               "%s: InternetReadFile returned %d, error %lu, expected ERROR_INTERNET_TIMEOUT\n",
               pCase->Name, pOutcome->Ok, pOutcome->Error);
            /* Windows waits a little longer than the timeout */
            ok(pOutcome->TotalMs + 500 >= dwExpected && pOutcome->TotalMs <= dwExpected + 3500,
               "%s: Failed after %lu ms, expected about %lu ms\n", pCase->Name, pOutcome->TotalMs, dwExpected);
            break;
        case EXPECT_STOP:
            ok(!pOutcome->Ok && (pOutcome->Error == ERROR_INTERNET_TIMEOUT || pOutcome->Error == ERROR_INTERNET_DECODING_FAILED),
               "%s: InternetReadFile returned %d, error %lu\n", pCase->Name, pOutcome->Ok, pOutcome->Error);
            ok(pOutcome->TotalMs <= dwExpected + 3500, "%s: Failed after %lu ms\n", pCase->Name, pOutcome->TotalMs);
            break;
    }
}

START_TEST(HttpGzip)
{
    WSADATA WsaData;
    UINT i;
    PCSTR pszOnly = strstr(GetCommandLineA(), "HttpGzip ");

    if (!LoadFunctions() || pWSAStartup(MAKEWORD(2, 2), &WsaData))
    {
        skip("wininet or winsock is not available\n");
        return;
    }
    for (i = 0; i < _countof(Cases); i++)
    {
        OUTCOME Outcome;

        /* A case name after the test name runs that case alone */
        if (pszOnly && strcmp(pszOnly + 9, Cases[i].Name) != 0)
            continue;
        RunCase(&Cases[i], &Outcome);
        trace("%s: %u bytes in %u calls, last call %s, error %lu, %lu ms\n", Cases[i].Name, Outcome.Bytes,
              Outcome.Calls, Outcome.Ok ? "ok" : "failed", Outcome.Error, Outcome.TotalMs);
        CheckOutcome(&Cases[i], &Outcome);
    }
    pWSACleanup();
}
