/* Cross-bitness socket I/O. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <winsock2.h>
#include <windows.h>
#include <winternl.h>
#include <winioctl.h>
#include <tdi.h>
#include <drivers/afd/shared.h>
#include <stdio.h>
#include <string.h>

static unsigned Checks, Failures;
#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; printf("FAIL line %u: %s error=%d\n", __LINE__, #x, WSAGetLastError()); } } while (0)

static DWORD WINAPI Watchdog(void *Done)
{
    if (WaitForSingleObject(Done, 15000) != WAIT_OBJECT_0)
    {
        puts("FAIL socket test timed out");
        ExitProcess(124);
    }
    return 0;
}

/* ReactOS's private AFD contract; enabled explicitly, never on Windows. */
static void CheckHandleReply(SOCKET Socket)
{
    typedef LONG (WINAPI *IOCTL)(HANDLE, HANDLE, PVOID, PVOID, PVOID, ULONG, PVOID, ULONG, PVOID, ULONG);
    IOCTL Ioctl = (IOCTL)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDeviceIoControlFile");
    struct { ULONG_PTR Status, Information; ULONG Guard; } Io = {0, 0, 0x12345678};
    ULONG Words[4] = {0xabcdef01, 0, 0x76543210, 0xabcdef01};
    ULONG Flags = AFD_ADDRESS_HANDLE;
    LONG Status;
    BOOL Wow = FALSE;

    CHECK(Ioctl != NULL && IsWow64Process(GetCurrentProcess(), &Wow));
    if (!Ioctl || !Wow) return;
    Status = Ioctl((HANDLE)Socket, NULL, NULL, NULL, &Io, IOCTL_AFD_GET_TDI_HANDLES,
                   &Flags, sizeof(Flags), &Words[1], 2 * sizeof(ULONG));
    CHECK(Status == 0 && Io.Status == 0 && Words[1] != 0);
    CHECK(Words[0] == 0xabcdef01 && Words[2] == 0x76543210 && Words[3] == 0xabcdef01);
    CHECK(Io.Guard == 0x12345678);
    Status = Ioctl((HANDLE)Socket, NULL, NULL, NULL, &Io, IOCTL_AFD_GET_TDI_HANDLES,
                   &Flags, sizeof(Flags), &Words[1], 2 * sizeof(ULONG) - 1);
    CHECK(Status == (LONG)0xc0000023); /* STATUS_BUFFER_TOO_SMALL */
    Status = Ioctl((HANDLE)Socket, NULL, NULL, NULL, &Io, IOCTL_AFD_GET_TDI_HANDLES,
                   &Flags, 0, &Words[1], 2 * sizeof(ULONG));
    CHECK(Status == (LONG)0xc0000023);
    Status = Ioctl((HANDLE)Socket, NULL, NULL, NULL, &Io, IOCTL_AFD_GET_TDI_HANDLES,
                   &Flags, sizeof(Flags), NULL, 2 * sizeof(ULONG));
    CHECK(Status == (LONG)0xc0000005); /* STATUS_ACCESS_VIOLATION */
    CHECK(Words[0] == 0xabcdef01 && Words[2] == 0x76543210 && Words[3] == 0xabcdef01);
    CHECK(Io.Guard == 0x12345678);
}

int main(int argc, char **argv)
{
    WSADATA Data;
    SOCKET Listener = INVALID_SOCKET, Client = INVALID_SOCKET, Server = INVALID_SOCKET;
    struct sockaddr_in Address = {0};
    struct { char Text[8]; DWORD Guard; } Reply = {{0}, 0x12345678};
    WSABUF Buffers[2] = {{2, "Wo"}, {3, "W64"}};
    DWORD Count = 0, Timeout = 2000;
    int Length = sizeof(Address);
    HANDLE Done, Thread;
    fd_set Read;
    struct timeval Wait = {2, 0};

    setvbuf(stdout, NULL, _IONBF, 0);
    Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    Thread = CreateThread(NULL, 0, Watchdog, Done, 0, NULL);
    CHECK(Done && Thread);
    if (!Done || !Thread) return 1;
    CHECK(WSAStartup(MAKEWORD(2, 2), &Data) == 0);
    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(Listener != INVALID_SOCKET);
    if (Listener == INVALID_SOCKET) goto End;
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(Listener, (struct sockaddr *)&Address, sizeof(Address)) == 0);
    CHECK(listen(Listener, 1) == 0);
    CHECK(getsockname(Listener, (struct sockaddr *)&Address, &Length) == 0);
    CHECK(Length == sizeof(Address) && Address.sin_port != 0);
    if (Failures) goto End;
    if (argc == 2 && !strcmp(argv[1], "--bind-abi"))
    {
        CheckHandleReply(Listener);
        goto End;
    }
    if (argc == 2 && !strcmp(argv[1], "--bind-only")) goto End;
    Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(Client != INVALID_SOCKET);
    if (Client == INVALID_SOCKET) goto End;
    CHECK(connect(Client, (struct sockaddr *)&Address, sizeof(Address)) == 0);
    if (Failures) goto End;
    Server = accept(Listener, NULL, NULL);
    CHECK(Server != INVALID_SOCKET);
    if (Server == INVALID_SOCKET) goto End;
    CHECK(setsockopt(Server, SOL_SOCKET, SO_RCVTIMEO, (char *)&Timeout, sizeof(Timeout)) == 0);
    CHECK(setsockopt(Client, SOL_SOCKET, SO_RCVTIMEO, (char *)&Timeout, sizeof(Timeout)) == 0);
    CHECK(WSASend(Client, Buffers, 2, &Count, 0, NULL, NULL) == 0 && Count == 5);
    FD_ZERO(&Read);
    FD_SET(Server, &Read);
    CHECK(select(0, &Read, NULL, NULL, &Wait) == 1 && FD_ISSET(Server, &Read));
    CHECK(recv(Server, Reply.Text, 5, MSG_WAITALL) == 5);
    CHECK(!memcmp(Reply.Text, "WoW64", 5) && Reply.Guard == 0x12345678);
    CHECK(send(Server, Reply.Text, 5, 0) == 5);
    memset(Reply.Text, 0, sizeof(Reply.Text));
    CHECK(recv(Client, Reply.Text, 5, MSG_WAITALL) == 5);
    CHECK(!memcmp(Reply.Text, "WoW64", 5) && Reply.Guard == 0x12345678);
    CHECK(shutdown(Client, SD_SEND) == 0);
    CHECK(recv(Server, Reply.Text, sizeof(Reply.Text), 0) == 0);
End:
    if (Server != INVALID_SOCKET) CHECK(closesocket(Server) == 0);
    if (Client != INVALID_SOCKET) CHECK(closesocket(Client) == 0);
    if (Listener != INVALID_SOCKET) CHECK(closesocket(Listener) == 0);
    CHECK(WSACleanup() == 0);
    SetEvent(Done);
    WaitForSingleObject(Thread, INFINITE);
    CloseHandle(Thread);
    CloseHandle(Done);
    printf("SOCKET checks=%u failures=%u\n", Checks, Failures);
    return Failures != 0;
}
