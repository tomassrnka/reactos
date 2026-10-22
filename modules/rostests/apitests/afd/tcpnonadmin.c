/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     T66: \Device\Tcp must be usable by non-admin users (socket
 *              create/bind/listen/connect/send/recv, UDP bind) while privileged
 *              control requests (route/ARP table writes via
 *              IOCTL_TCP_SET_INFORMATION_EX, IOCTL_SET_IP_ADDRESS,
 *              IOCTL_DELETE_IP_ADDRESS) stay denied to non-admins.
 *              Defensive test only; the privileged requests use deliberately
 *              invalid/nonexistent targets so they never change system state.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"
#include <tdiinfo.h>
#include <lm.h>

#define T66_USER L"t66user"
#define T66_PASS L"T66-Pass#1x"

typedef struct { ULONG NteIndex; ULONG Address; ULONG Netmask; } T66_IP_SET_ADDRESS;

static
NTSTATUS
T66Ioctl(
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
T66OpenDevice(
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

/* Socket round trip: TCP listen + connect + accept + send/recv, UDP bind. */
static
void
T66SocketWork(
    _In_ PCSTR Who)
{
    struct sockaddr_in a;
    int len = sizeof(a), r;
    SOCKET lst, cli, srv, udp;
    char c = 'x', rc = 0;

    lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ok(lst != INVALID_SOCKET, "%s: socket(TCP) failed %d\n", Who, WSAGetLastError());
    if (lst == INVALID_SOCKET)
        return;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    r = bind(lst, (struct sockaddr *)&a, sizeof(a));
    ok(r == 0, "%s: TCP bind failed %d\n", Who, WSAGetLastError());
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
    if (r == 0)
    {
        struct sockaddr_in peer;
        int peerlen = sizeof(peer);

        srv = accept(lst, (struct sockaddr *)&peer, &peerlen);
        ok(srv != INVALID_SOCKET, "%s: accept failed %d\n", Who, WSAGetLastError());
        if (srv != INVALID_SOCKET)
        {
            ok(send(cli, &c, 1, 0) == 1, "%s: send failed %d\n", Who, WSAGetLastError());
            ok(recv(srv, &rc, 1, 0) == 1 && rc == 'x', "%s: recv failed/mismatch\n", Who);
            closesocket(srv);
        }
    }
    closesocket(cli);
    closesocket(lst);

    udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ok(udp != INVALID_SOCKET, "%s: socket(UDP) failed\n", Who);
    if (udp != INVALID_SOCKET)
    {
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ok(bind(udp, (struct sockaddr *)&a, sizeof(a)) == 0, "%s: UDP bind failed %d\n", Who, WSAGetLastError());
        len = sizeof(a);
        getsockname(udp, (struct sockaddr *)&a, &len);
        ok(sendto(udp, &c, 1, 0, (struct sockaddr *)&a, sizeof(a)) == 1, "%s: UDP sendto failed %d\n", Who, WSAGetLastError());
        rc = 0;
        r = recv(udp, &rc, 1, 0);
        ok(r == 1 && rc == 'x', "%s: UDP recv returned %d (%d)\n", Who, r, WSAGetLastError());
        closesocket(udp);
    }
}

/* Privileged control requests. Admin must pass the gate (any status except
 * ACCESS_DENIED; the targets are invalid so nothing is modified); non-admin
 * must get STATUS_ACCESS_DENIED. */
static
void
T66Privileged(
    _In_ PCSTR Who,
    _In_ BOOLEAN IsAdmin)
{
    HANDLE Dev;
    NTSTATUS Status;
    PTCP_REQUEST_SET_INFORMATION_EX Req;
    T66_IP_SET_ADDRESS Ip;
    ULONG Size = sizeof(*Req);
    ULONG Ent[3] = { AT_ENTITY, CL_NL_ENTITY, CO_NL_ENTITY };
    ULONG i;

    Status = T66OpenDevice(L"\\Device\\Tcp", &Dev);
    ok(Status == STATUS_SUCCESS, "%s: open \\Device\\Tcp failed 0x%lx\n", Who, Status);
    if (!NT_SUCCESS(Status))
        return;

    Req = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    for (i = 0; i < 3; i++)
    {
        Req->ID.toi_entity.tei_entity = Ent[i];
        Req->ID.toi_entity.tei_instance = 0;
        Req->ID.toi_class = INFO_CLASS_PROTOCOL;
        Req->ID.toi_type = INFO_TYPE_PROVIDER;
        Req->ID.toi_id = IP_MIB_ARPTABLE_ENTRY_ID;
        Req->BufferSize = 0;
        Status = T66Ioctl(Dev, IOCTL_TCP_SET_INFORMATION_EX, Req, Size);
        trace("%s: SET_INFORMATION_EX(provider, entity %lx) = 0x%lx\n", Who, Ent[i], Status);
        if (IsAdmin)
            ok(Status != STATUS_ACCESS_DENIED, "%s: privileged SET_INFORMATION_EX must pass the gate (0x%lx)\n", Who, Status);
        else
            ok(Status == STATUS_ACCESS_DENIED, "%s: privileged SET_INFORMATION_EX must be denied, got 0x%lx\n", Who, Status);
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Req);

    /* per-socket (address object) class is NOT privileged: must not be denied */
    Req = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    Req->ID.toi_class = INFO_CLASS_PROTOCOL;
    Req->ID.toi_type = INFO_TYPE_ADDRESS_OBJECT;
    Status = T66Ioctl(Dev, IOCTL_TCP_SET_INFORMATION_EX, Req, Size);
    ok(Status != STATUS_ACCESS_DENIED, "%s: address-object SET_INFORMATION_EX must not be denied (0x%lx)\n", Who, Status);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Req);

    /* too-short input must be rejected before anything is read */
    Status = T66Ioctl(Dev, IOCTL_TCP_SET_INFORMATION_EX, &Ip, 4);
    ok(Status == STATUS_INVALID_PARAMETER, "%s: short SET_INFORMATION_EX gave 0x%lx\n", Who, Status);

    memset(&Ip, 0, sizeof(Ip));
    Ip.NteIndex = 0x7fffffff; /* nonexistent interface: no-op for admin */
    Status = T66Ioctl(Dev, IOCTL_SET_IP_ADDRESS, &Ip, sizeof(Ip));
    trace("%s: SET_IP_ADDRESS = 0x%lx\n", Who, Status);
    if (IsAdmin)
        ok(Status != STATUS_ACCESS_DENIED, "%s: SET_IP_ADDRESS must pass the gate (0x%lx)\n", Who, Status);
    else
        ok(Status == STATUS_ACCESS_DENIED, "%s: SET_IP_ADDRESS must be denied, got 0x%lx\n", Who, Status);

    Status = T66Ioctl(Dev, IOCTL_DELETE_IP_ADDRESS, &Ip, sizeof(Ip));
    trace("%s: DELETE_IP_ADDRESS = 0x%lx\n", Who, Status);
    if (IsAdmin)
        ok(Status != STATUS_ACCESS_DENIED, "%s: DELETE_IP_ADDRESS must pass the gate (0x%lx)\n", Who, Status);
    else
        ok(Status == STATUS_ACCESS_DENIED, "%s: DELETE_IP_ADDRESS must be denied, got 0x%lx\n", Who, Status);

    NtClose(Dev);
}

START_TEST(tcpnonadmin)
{
    WSADATA wd;
    HANDLE tok = NULL, Dev;
    BYTE sidA[SECURITY_MAX_SID_SIZE];
    DWORD cb;
    BOOL member = TRUE;
    NTSTATUS Status;

    WSAStartup(MAKEWORD(2, 2), &wd);

    cb = sizeof(sidA);
    CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, sidA, &cb);
    CheckTokenMembership(NULL, sidA, &member);
    if (!member)
    {
        skip("Test must run as an Administrator\n");
        return;
    }

    /* admin: full */
    Status = T66OpenDevice(L"\\Device\\Tcp", &Dev);
    ok(Status == STATUS_SUCCESS, "admin: open \\Device\\Tcp failed 0x%lx\n", Status);
    if (NT_SUCCESS(Status))
        NtClose(Dev);
    T66SocketWork("admin");
    T66Privileged("admin", TRUE);

    /* real non-admin user: NetUserAdd + LogonUser + impersonate */
    {
        USER_INFO_1 ui;
        DWORD err = 0;
        NET_API_STATUS ns;

        memset(&ui, 0, sizeof(ui));
        ui.usri1_name = (LPWSTR)T66_USER;
        ui.usri1_password = (LPWSTR)T66_PASS;
        ui.usri1_priv = USER_PRIV_USER;
        ui.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
        NetUserDel(NULL, T66_USER);
        ns = NetUserAdd(NULL, 1, (LPBYTE)&ui, &err);
        if (ns != NERR_Success)
        {
            skip("NetUserAdd failed %lu\n", ns);
            return;
        }
        if (!LogonUserW(T66_USER, L".", T66_PASS, LOGON32_LOGON_INTERACTIVE,
                        LOGON32_PROVIDER_DEFAULT, &tok) ||
            !ImpersonateLoggedOnUser(tok))
        {
            skip("Cannot log on / impersonate the test user (%lu)\n", GetLastError());
            NetUserDel(NULL, T66_USER);
            return;
        }
    }
    member = TRUE;
    CheckTokenMembership(NULL, sidA, &member);
    ok(!member, "token sanity: impersonated token must not be an effective Administrator\n");

    Status = T66OpenDevice(L"\\Device\\Tcp", &Dev);
    ok(Status == STATUS_SUCCESS, "nonadmin: open \\Device\\Tcp must succeed, got 0x%lx\n", Status);
    if (NT_SUCCESS(Status))
        NtClose(Dev);
    T66SocketWork("nonadmin");
    T66Privileged("nonadmin", FALSE);

    RevertToSelf();
    CloseHandle(tok);
    NetUserDel(NULL, T66_USER);
    WSACleanup();
}
