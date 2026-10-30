/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     The service part of getnameinfo: the port number, or the name from the services file.
 *              It was left empty, so OpenSSH for Windows logged and tracked every peer as "port 0".
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected values are what Windows 10 22H2 does.
 */

#include "ws2_32.h"

static void Check(WORD Port, INT Flags, PCSTR Expected)
{
    SOCKADDR_IN Addr;
    CHAR Host[NI_MAXHOST], Serv[NI_MAXSERV];
    INT Error;

    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_port = htons(Port);
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    strcpy(Serv, "unchanged");
    Error = getnameinfo((PSOCKADDR)&Addr, sizeof(Addr), Host, sizeof(Host), Serv, sizeof(Serv),
                        NI_NUMERICHOST | Flags);
    ok(Error == 0, "port %u flags 0x%x: error %d\n", Port, Flags, Error);
    ok(!strcmp(Serv, Expected), "port %u flags 0x%x: service \"%s\", expected \"%s\"\n", Port, Flags, Serv, Expected);
}

START_TEST(getnameinfoserv)
{
    WSADATA Data;
    SOCKADDR_IN Addr;
    CHAR Serv[NI_MAXSERV];
    INT Error;

    ok(WSAStartup(MAKEWORD(2, 2), &Data) == 0, "WSAStartup failed\n");

    Check(49152, NI_NUMERICSERV, "49152");
    Check(49152, 0, "49152");          /* no such service: the number */
    Check(80, NI_NUMERICSERV, "80");
    Check(22, NI_NUMERICSERV, "22");
    Check(22, 0, "ssh");
    Check(80, 0, "http");
    Check(53, NI_DGRAM, "domain");

    /* A service buffer too short for the number */
    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_port = htons(49152);
    Error = getnameinfo((PSOCKADDR)&Addr, sizeof(Addr), NULL, 0, Serv, 2, NI_NUMERICSERV);
    ok(Error == ERROR_INSUFFICIENT_BUFFER, "a 2 character buffer for 49152: error %d\n", Error);
    Error = getnameinfo((PSOCKADDR)&Addr, sizeof(Addr), NULL, 0, Serv, 5, NI_NUMERICSERV);
    ok(Error == ERROR_INSUFFICIENT_BUFFER, "a 5 character buffer for 49152: error %d\n", Error);
    Error = getnameinfo((PSOCKADDR)&Addr, sizeof(Addr), NULL, 0, Serv, 6, NI_NUMERICSERV);
    ok(Error == 0 && !strcmp(Serv, "49152"), "a 6 character buffer for 49152: error %d, \"%s\"\n", Error, Serv);

    WSACleanup();
}
