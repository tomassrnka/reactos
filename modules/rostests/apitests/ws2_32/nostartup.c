/*
 *  PROJECT:         ReactOS api tests
 *  LICENSE:         GPLv2+ - See COPYING in the top level directory
 *  PURPOSE:         Test for WSAStartup
 *  PROGRAMMER:      Sylvain Petreolle <sylvain.petreolle@reactos.org>
 */

#include "ws2_32.h"

// This test depends on WSAStartup not having been called
START_TEST(nostartup)
{
    int Error = 0;
    char szName[256];
    unsigned int i;
    ok(WSASocketA(0, 0, 0, NULL, 0, 0) == INVALID_SOCKET, "WSASocketA should have failed\n");

    WSASetLastError(0xdeadbeef);
    getservbyname(NULL, NULL);
    Error = WSAGetLastError();
    ok_dec(Error, WSANOTINITIALISED);

    WSASetLastError(0xdeadbeef);
    getservbyport(0, NULL);
    Error = WSAGetLastError();
    ok_dec(Error, WSANOTINITIALISED);

    WSASetLastError(0xdeadbeef);
    gethostbyname(NULL);
    Error = WSAGetLastError();
    ok_dec(Error, WSANOTINITIALISED);

    /* The host name is not known before WSAStartup; the buffer is left alone */
    memset(szName, 'x', sizeof(szName));
    WSASetLastError(0xdeadbeef);
    ok_dec(gethostname(szName, sizeof(szName)), SOCKET_ERROR);
    Error = WSAGetLastError();
    ok_dec(Error, WSANOTINITIALISED);
    for (i = 0; i < sizeof(szName) && szName[i] == 'x'; i++);
    ok(i == sizeof(szName), "Buffer changed at %u\n", i);

    ok_dec(inet_addr("127.0.0.1"), 0x100007f);
}
