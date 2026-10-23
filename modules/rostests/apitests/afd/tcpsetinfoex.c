/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     T89: IOCTL_TCP_SET_INFORMATION_EX with INFO_TYPE_ADDRESS_OBJECT
 *              or INFO_TYPE_CONNECTION must resolve its target only from an
 *              address-file (transport) entity. A caller - unprivileged since
 *              the non-admin socket change (T66) - that names an interface or
 *              network-layer entity (IF/AT/CL_NL/CO_NL) must be rejected with
 *              STATUS_INVALID_PARAMETER instead of having the interface context
 *              mis-used as an ADDRESS_FILE (kernel pool corruption). Legitimate
 *              per-socket options (TCP_NODELAY, SO_KEEPALIVE) must keep working
 *              for a non-admin. Defensive test: the type-confusion requests use
 *              a valid option id and a small buffer; on a fixed kernel they are
 *              rejected before any write, so nothing is modified.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"
#include <tdiinfo.h>
#include <lm.h>

#define T89_USER L"t89user"
#define T89_PASS L"T89-Pass#1x"

static
NTSTATUS
T89Ioctl(
    _In_ HANDLE File,
    _In_ ULONG Code,
    _In_ PVOID In,
    _In_ ULONG InLen)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK Iosb;
    HANDLE Event;

    Status = NtCreateEvent(&Event, EVENT_ALL_ACCESS, NULL, NotificationEvent, FALSE);
    if (!NT_SUCCESS(Status))
        return Status;
    Iosb.Status = STATUS_PENDING;
    Status = NtDeviceIoControlFile(File, Event, NULL, NULL, &Iosb, Code, In, InLen, NULL, 0);
    if (Status == STATUS_PENDING)
    {
        NtWaitForSingleObject(Event, FALSE, NULL);
        Status = Iosb.Status;
    }
    NtClose(Event);
    return Status;
}

static
NTSTATUS
T89OpenDevice(
    _In_ PCWSTR Name,
    _Out_ PHANDLE Handle)
{
    UNICODE_STRING Us;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Iosb;

    RtlInitUnicodeString(&Us, Name);
    InitializeObjectAttributes(&Oa, &Us, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return NtCreateFile(Handle, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE, &Oa, &Iosb,
                        NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0);
}

/* The core contract: a SET_INFORMATION_EX whose toi_type is ADDRESS_OBJECT or
 * CONNECTION but whose toi_entity is an interface / network-layer entity (whose
 * context is a PIP_INTERFACE, not a PADDRESS_FILE) must be refused outright.
 * Before the fix the driver resolved the context by the caller-chosen entity
 * with no type check and wrote the option through it as an ADDRESS_FILE, which
 * for an interface entity corrupts kernel pool. The fix rejects it, so each
 * request must return STATUS_INVALID_PARAMETER and never STATUS_SUCCESS. */
static
void
T89TypeConfusion(
    _In_ PCSTR Who)
{
    HANDLE Dev;
    NTSTATUS Status;
    PTCP_REQUEST_SET_INFORMATION_EX Req;
    ULONG Size = FIELD_OFFSET(TCP_REQUEST_SET_INFORMATION_EX, Buffer) + sizeof(ULONG);
    ULONG WrongEnt[4] = { IF_ENTITY, AT_ENTITY, CL_NL_ENTITY, CO_NL_ENTITY };
    ULONG Types[2] = { INFO_TYPE_ADDRESS_OBJECT, INFO_TYPE_CONNECTION };
    ULONG e, t, inst;

    Status = T89OpenDevice(L"\\Device\\Tcp", &Dev);
    ok(Status == STATUS_SUCCESS, "%s: open \\Device\\Tcp failed 0x%lx\n", Who, Status);
    if (!NT_SUCCESS(Status))
        return;

    Req = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (Req == NULL)
    {
        NtClose(Dev);
        return;
    }

    for (t = 0; t < 2; t++)
    {
        for (e = 0; e < 4; e++)
        {
            for (inst = 0; inst < 2; inst++)
            {
                RtlZeroMemory(Req, Size);
                Req->ID.toi_entity.tei_entity = WrongEnt[e];
                Req->ID.toi_entity.tei_instance = inst;
                Req->ID.toi_class = INFO_CLASS_PROTOCOL;
                Req->ID.toi_type = Types[t];
                Req->ID.toi_id = AO_OPTION_TTL;
                Req->BufferSize = sizeof(ULONG);
                *(PULONG)Req->Buffer = 0x41; /* a mis-typed write would land here */

                Status = T89Ioctl(Dev, IOCTL_TCP_SET_INFORMATION_EX, Req, Size);
                trace("%s: SET type=0x%lx entity=0x%lx inst=%lu -> 0x%lx\n",
                      Who, Types[t], WrongEnt[e], inst, Status);
                ok(Status == STATUS_INVALID_PARAMETER,
                   "%s: mistyped SET (type 0x%lx entity 0x%lx inst %lu) must be rejected, got 0x%lx\n",
                   Who, Types[t], WrongEnt[e], inst, Status);
                ok(Status != STATUS_SUCCESS,
                   "%s: mistyped SET (type 0x%lx entity 0x%lx) must never succeed (type confusion)\n",
                   Who, Types[t], WrongEnt[e]);
            }
        }
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, Req);
    NtClose(Dev);
}

/* Legitimate per-socket options route through wshtcpip, which only ever selects
 * a transport (CO_TL/CL_TL/ER) entity, so the type check must leave them intact
 * for a non-admin: establish a loopback connection and exercise TCP_NODELAY and
 * SO_KEEPALIVE. */
static
void
T89LegitOptions(
    _In_ PCSTR Who)
{
    struct sockaddr_in a;
    int len = sizeof(a), r, val, vlen;
    SOCKET lst, cli, srv;
    int one = 1;

    lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(lst != INVALID_SOCKET, "%s: socket(TCP) failed %d\n", Who, WSAGetLastError());
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
    ok(listen(lst, 1) == 0, "%s: listen failed %d\n", Who, WSAGetLastError());

    cli = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(cli != INVALID_SOCKET, "%s: client socket failed\n", Who);
    r = connect(cli, (struct sockaddr *)&a, sizeof(a));
    ok(r == 0, "%s: connect failed %d\n", Who, WSAGetLastError());

    srv = accept(lst, NULL, NULL);
    ok(srv != INVALID_SOCKET, "%s: accept failed %d\n", Who, WSAGetLastError());

    /* Connection option (INFO_TYPE_CONNECTION on a CO_TL entity) */
    r = setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one));
    ok(r == 0, "%s: setsockopt(TCP_NODELAY) must work for non-admin, got %d (%d)\n",
       Who, r, WSAGetLastError());
    /* ReactOS wshtcpip WSHGetSocketInformation is UNIMPLEMENTED, so getsockopt
     * does not round-trip the value and never reaches the kernel query path that
     * this fix touches; trace only. The setsockopt above is the option write
     * that actually exercises the fixed SET path. */
    val = 0;
    vlen = sizeof(val);
    r = getsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (char *)&val, &vlen);
    trace("%s: getsockopt(TCP_NODELAY) r=%d val=%d (%d)\n", Who, r, val, WSAGetLastError());

    /* Socket keepalive */
    r = setsockopt(cli, SOL_SOCKET, SO_KEEPALIVE, (char *)&one, sizeof(one));
    ok(r == 0, "%s: setsockopt(SO_KEEPALIVE) must work for non-admin, got %d (%d)\n",
       Who, r, WSAGetLastError());

    if (srv != INVALID_SOCKET)
        closesocket(srv);
    closesocket(cli);
    closesocket(lst);
}

START_TEST(tcpsetinfoex)
{
    WSADATA wd;
    HANDLE tok = NULL;
    BYTE sidA[SECURITY_MAX_SID_SIZE];
    DWORD cb;
    BOOL member = TRUE;

    WSAStartup(MAKEWORD(2, 2), &wd);

    cb = sizeof(sidA);
    CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, sidA, &cb);
    CheckTokenMembership(NULL, sidA, &member);
    if (!member)
    {
        skip("Test must run as an Administrator\n");
        WSACleanup();
        return;
    }

    /* admin: the type-confusion must already be refused */
    T89TypeConfusion("admin");
    T89LegitOptions("admin");

    /* real non-admin user: NetUserAdd + LogonUser + impersonate */
    {
        USER_INFO_1 ui;
        DWORD err = 0;
        NET_API_STATUS ns;

        memset(&ui, 0, sizeof(ui));
        ui.usri1_name = (LPWSTR)T89_USER;
        ui.usri1_password = (LPWSTR)T89_PASS;
        ui.usri1_priv = USER_PRIV_USER;
        ui.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
        NetUserDel(NULL, T89_USER);
        ns = NetUserAdd(NULL, 1, (LPBYTE)&ui, &err);
        if (ns != NERR_Success)
        {
            skip("NetUserAdd failed %lu\n", ns);
            WSACleanup();
            return;
        }
        if (!LogonUserW(T89_USER, L".", T89_PASS, LOGON32_LOGON_INTERACTIVE,
                        LOGON32_PROVIDER_DEFAULT, &tok) ||
            !ImpersonateLoggedOnUser(tok))
        {
            skip("Cannot log on / impersonate the test user (%lu)\n", GetLastError());
            NetUserDel(NULL, T89_USER);
            WSACleanup();
            return;
        }
    }

    member = TRUE;
    CheckTokenMembership(NULL, sidA, &member);
    ok(!member, "token sanity: impersonated token must not be an effective Administrator\n");

    T89TypeConfusion("nonadmin");
    T89LegitOptions("nonadmin");

    RevertToSelf();
    CloseHandle(tok);
    NetUserDel(NULL, T89_USER);
    WSACleanup();
}
