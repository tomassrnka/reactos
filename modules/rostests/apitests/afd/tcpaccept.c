/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     T76: accept() on a listening TCP socket must succeed, for an
 *              Administrator and for a real non-admin user (it used to fail
 *              with WSAEACCES because AfdAccept referenced the accepting
 *              socket handle with generic access bits).
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"
#include <lm.h>

#define T76_USER L"t76user"
#define T76_PASS L"T76-Pass#1x"

static
void
T76Accept(
    _In_ PCSTR Who)
{
    struct sockaddr_in a, peer, cname;
    int len = sizeof(a), peerlen, r, pass;
    SOCKET lst, cli, srv;
    char c = 'x', rc = 0;

    lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(lst != INVALID_SOCKET, "%s: socket failed %d\n", Who, WSAGetLastError());
    if (lst == INVALID_SOCKET)
        return;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    r = bind(lst, (struct sockaddr *)&a, sizeof(a));
    ok(r == 0, "%s: bind failed %d\n", Who, WSAGetLastError());
    if (r != 0)
    {
        closesocket(lst);
        return;
    }
    getsockname(lst, (struct sockaddr *)&a, &len);
    r = listen(lst, 4);
    ok(r == 0, "%s: listen failed %d\n", Who, WSAGetLastError());

    /* pass 0: accept() with the peer address, pass 1: without */
    for (pass = 0; pass < 2; pass++)
    {
        cli = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok(cli != INVALID_SOCKET, "%s: client socket failed %d\n", Who, WSAGetLastError());
        r = connect(cli, (struct sockaddr *)&a, sizeof(a));
        ok(r == 0, "%s: connect failed %d\n", Who, WSAGetLastError());
        if (r != 0)
        {
            closesocket(cli);
            continue;
        }

        peerlen = sizeof(peer);
        memset(&peer, 0, sizeof(peer));
        if (pass == 0)
            srv = accept(lst, (struct sockaddr *)&peer, &peerlen);
        else
            srv = accept(lst, NULL, NULL);
        ok(srv != INVALID_SOCKET, "%s: accept(pass %d) failed %d\n", Who, pass, WSAGetLastError());
        if (srv != INVALID_SOCKET)
        {
            if (pass == 0)
            {
                len = sizeof(cname);
                getsockname(cli, (struct sockaddr *)&cname, &len);
                ok(peerlen == sizeof(peer), "%s: peerlen %d\n", Who, peerlen);
                ok(peer.sin_family == AF_INET &&
                   peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
                   peer.sin_port == cname.sin_port,
                   "%s: wrong peer address (port %u vs %u)\n", Who,
                   ntohs(peer.sin_port), ntohs(cname.sin_port));
            }
            ok(send(cli, &c, 1, 0) == 1, "%s: send failed %d\n", Who, WSAGetLastError());
            rc = 0;
            ok(recv(srv, &rc, 1, 0) == 1 && rc == 'x', "%s: recv failed/mismatch %d\n", Who, WSAGetLastError());
            c = 'y';
            ok(send(srv, &c, 1, 0) == 1, "%s: reply send failed %d\n", Who, WSAGetLastError());
            rc = 0;
            ok(recv(cli, &rc, 1, 0) == 1 && rc == 'y', "%s: reply recv failed/mismatch %d\n", Who, WSAGetLastError());
            c = 'x';
            closesocket(srv);
        }
        closesocket(cli);
    }
    closesocket(lst);
}

START_TEST(tcpaccept)
{
    WSADATA wd;
    HANDLE tok = NULL;
    BYTE sidA[SECURITY_MAX_SID_SIZE];
    DWORD cb;
    BOOL member = TRUE;
    USER_INFO_1 ui;
    DWORD err = 0;
    NET_API_STATUS ns;

    WSAStartup(MAKEWORD(2, 2), &wd);

    cb = sizeof(sidA);
    CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, sidA, &cb);
    CheckTokenMembership(NULL, sidA, &member);
    if (!member)
    {
        skip("Test must run as an Administrator\n");
        return;
    }

    T76Accept("admin");

    memset(&ui, 0, sizeof(ui));
    ui.usri1_name = (LPWSTR)T76_USER;
    ui.usri1_password = (LPWSTR)T76_PASS;
    ui.usri1_priv = USER_PRIV_USER;
    ui.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    NetUserDel(NULL, T76_USER);
    ns = NetUserAdd(NULL, 1, (LPBYTE)&ui, &err);
    if (ns != NERR_Success)
    {
        skip("NetUserAdd failed %lu\n", ns);
        return;
    }
    if (!LogonUserW(T76_USER, L".", T76_PASS, LOGON32_LOGON_INTERACTIVE,
                    LOGON32_PROVIDER_DEFAULT, &tok) ||
        !ImpersonateLoggedOnUser(tok))
    {
        skip("Cannot log on / impersonate the test user (%lu)\n", GetLastError());
        NetUserDel(NULL, T76_USER);
        return;
    }
    member = TRUE;
    CheckTokenMembership(NULL, sidA, &member);
    ok(!member, "token sanity: impersonated token must not be an effective Administrator\n");

    T76Accept("nonadmin");

    RevertToSelf();
    CloseHandle(tok);
    NetUserDel(NULL, T76_USER);
    WSACleanup();
}
