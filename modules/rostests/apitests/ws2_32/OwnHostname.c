/*
 * PROJECT:     ws2_32.dll API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The machine's own name resolves locally to its own addresses (or loopback), and its
 *              addresses (loopback included) resolve back to its name, as on Windows
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "ws2_32.h"
#include <winnls.h>

static CHAR OwnName[256];

static BOOL has_ipv4(const ADDRINFOA *Ai, ULONG *FirstAddr)
{
    for (; Ai; Ai = Ai->ai_next)
    {
        if (Ai->ai_family == AF_INET)
        {
            if (FirstAddr)
                *FirstAddr = ((const SOCKADDR_IN *)Ai->ai_addr)->sin_addr.S_un.S_addr;
            return TRUE;
        }
    }
    return FALSE;
}

static void test_name(const char *Name, const char *Label)
{
    ADDRINFOA Hints, *Result = NULL;
    ADDRINFOW HintsW, *ResultW = NULL;
    WCHAR NameW[256];
    struct hostent *Host;
    ULONG Addr = 0;
    int Error;

    Host = gethostbyname(Name);
    ok(Host != NULL, "[%s] gethostbyname failed: %d\n", Label, WSAGetLastError());
    if (Host)
    {
        ok(Host->h_addrtype == AF_INET, "[%s] h_addrtype %d\n", Label, Host->h_addrtype);
        ok(Host->h_addr_list[0] != NULL, "[%s] no address\n", Label);
        ok(Host->h_name && !lstrcmpiA(Host->h_name, OwnName), "[%s] h_name '%s', expected '%s'\n", Label, Host->h_name, OwnName);
    }

    ZeroMemory(&Hints, sizeof(Hints));
    Hints.ai_family = AF_UNSPEC;
    Error = getaddrinfo(Name, NULL, &Hints, &Result);
    ok(Error == 0, "[%s] getaddrinfo returned %d\n", Label, Error);
    if (Error == 0)
    {
        ok(has_ipv4(Result, &Addr), "[%s] no IPv4 address\n", Label);
        ok(Addr != 0, "[%s] address 0.0.0.0\n", Label);
        freeaddrinfo(Result);
    }

    ZeroMemory(&Hints, sizeof(Hints));
    Hints.ai_family = AF_INET;
    Hints.ai_flags = AI_CANONNAME;
    Result = NULL;
    Error = getaddrinfo(Name, NULL, &Hints, &Result);
    ok(Error == 0, "[%s] getaddrinfo(AF_INET, AI_CANONNAME) returned %d\n", Label, Error);
    if (Error == 0)
    {
        ok(Result->ai_canonname && !lstrcmpiA(Result->ai_canonname, OwnName), "[%s] canonical name '%s', expected '%s'\n",
           Label, Result->ai_canonname, OwnName);
        freeaddrinfo(Result);
    }

    MultiByteToWideChar(CP_ACP, 0, Name, -1, NameW, RTL_NUMBER_OF(NameW));
    ZeroMemory(&HintsW, sizeof(HintsW));
    HintsW.ai_family = AF_INET;
    Error = GetAddrInfoW(NameW, NULL, &HintsW, &ResultW);
    ok(Error == 0, "[%s] GetAddrInfoW returned %d\n", Label, Error);
    if (Error == 0)
        FreeAddrInfoW(ResultW);
}

static void test_reverse(const char *Ip, int Flags, const char *Label)
{
    SOCKADDR_IN Sin;
    CHAR Host[NI_MAXHOST] = "";
    int Error;

    ZeroMemory(&Sin, sizeof(Sin));
    Sin.sin_family = AF_INET;
    Sin.sin_addr.S_un.S_addr = inet_addr(Ip);
    Error = getnameinfo((PSOCKADDR)&Sin, sizeof(Sin), Host, sizeof(Host), NULL, 0, Flags);
    ok(Error == 0, "[%s] getnameinfo(%s, 0x%x) returned %d\n", Label, Ip, Flags, Error);
    if (Error == 0)
        ok(!lstrcmpiA(Host, OwnName), "[%s] getnameinfo(%s, 0x%x) named '%s', expected '%s'\n", Label, Ip, Flags, Host, OwnName);
}

START_TEST(OwnHostname)
{
    WSADATA WsaData;
    ADDRINFOA Hints, *Result = NULL;
    SOCKADDR_IN6 Sin6;
    CHAR Host[NI_MAXHOST] = "";
    CHAR Ip[32];
    ULONG Addr = 0;
    CHAR NameDot[300];
    int Error;

    Error = WSAStartup(MAKEWORD(2, 2), &WsaData);
    ok_dec(Error, 0);

    Error = gethostname(OwnName, sizeof(OwnName));
    ok_dec(Error, 0);

    /* the own name, with the root label's dot, and the empty name (= the own name) */
    test_name(OwnName, "own name");
    sprintf(NameDot, "%s.", OwnName);
    test_name(NameDot, "own name with dot");
    {
        struct hostent *Host2 = gethostbyname("");
        ok(Host2 != NULL, "gethostbyname(\"\") failed: %d\n", WSAGetLastError());
        ZeroMemory(&Hints, sizeof(Hints));
        Error = getaddrinfo("", NULL, &Hints, &Result);
        ok(Error == 0, "getaddrinfo(\"\") returned %d\n", Error);
        if (Error == 0)
            freeaddrinfo(Result);
    }

    /* loopback and the machine's own address resolve back to its name */
    test_reverse("127.0.0.1", 0, "loopback");
    test_reverse("127.0.0.1", NI_NAMEREQD, "loopback NAMEREQD");
    test_reverse("127.0.0.1", NI_NOFQDN, "loopback NOFQDN");

    ZeroMemory(&Hints, sizeof(Hints));
    Hints.ai_family = AF_INET;
    Result = NULL;
    if (getaddrinfo(OwnName, NULL, &Hints, &Result) == 0 && has_ipv4(Result, &Addr))
    {
        struct in_addr In;
        In.S_un.S_addr = Addr;
        lstrcpyA(Ip, inet_ntoa(In));
        test_reverse(Ip, 0, "own address");
        test_reverse(Ip, NI_NAMEREQD, "own address NAMEREQD");
    }
    if (Result)
        freeaddrinfo(Result);

    ZeroMemory(&Sin6, sizeof(Sin6));
    Sin6.sin6_family = AF_INET6;
    Sin6.sin6_addr.u.Byte[15] = 1;
    Error = getnameinfo((PSOCKADDR)&Sin6, sizeof(Sin6), Host, sizeof(Host), NULL, 0, NI_NAMEREQD);
    ok(Error == 0, "getnameinfo(::1) returned %d\n", Error);
    if (Error == 0)
        ok(!lstrcmpiA(Host, OwnName), "getnameinfo(::1) named '%s', expected '%s'\n", Host, OwnName);

    /* an address that is not ours is still given numerically on request */
    {
        SOCKADDR_IN Sin;
        ZeroMemory(&Sin, sizeof(Sin));
        Sin.sin_family = AF_INET;
        Sin.sin_addr.S_un.S_addr = inet_addr("192.0.2.77"); /* TEST-NET-1 */
        Error = getnameinfo((PSOCKADDR)&Sin, sizeof(Sin), Host, sizeof(Host), NULL, 0, NI_NUMERICHOST);
        ok(Error == 0, "getnameinfo(NI_NUMERICHOST) returned %d\n", Error);
        ok(!lstrcmpiA(Host, "192.0.2.77"), "numeric name '%s'\n", Host);
    }

    WSACleanup();
}
