/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     Inheritance of socket handles: sockets are inheritable unless created with
 *              WSA_FLAG_NO_HANDLE_INHERIT, including the socket WSASocketW makes from the
 *              WSAPROTOCOL_INFO of WSADuplicateSocket. A socket in a PROC_THREAD_ATTRIBUTE_HANDLE_LIST
 *              must be accepted by CreateProcess (OpenSSH's sshd passes its connection to sshd-session so).
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected values are what Windows 10 22H2 does.
 */

#include "ws2_32.h"

#ifndef WSA_FLAG_NO_HANDLE_INHERIT
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80
#endif
#ifndef EXTENDED_STARTUPINFO_PRESENT
#define EXTENDED_STARTUPINFO_PRESENT 0x00080000
#endif
#ifndef PROC_THREAD_ATTRIBUTE_HANDLE_LIST
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x00020002
#endif

typedef BOOL (WINAPI *INITIALIZE_LIST)(PVOID, DWORD, DWORD, PSIZE_T);
typedef BOOL (WINAPI *UPDATE_LIST)(PVOID, DWORD, DWORD_PTR, PVOID, SIZE_T, PVOID, PSIZE_T);
typedef VOID (WINAPI *DELETE_LIST)(PVOID);

static BOOL IsInheritable(SOCKET s, PCSTR What)
{
    DWORD Flags = 0xff;

    ok(GetHandleInformation((HANDLE)s, &Flags), "%s: GetHandleInformation failed: %lu\n", What, GetLastError());
    return (Flags & HANDLE_FLAG_INHERIT) != 0;
}

/* CreateProcess with STARTF_USESTDHANDLES naming the socket in a handle list; the child exits with 3 */
static VOID CheckHandleList(SOCKET s, PCSTR What)
{
    HMODULE Kernel = GetModuleHandleW(L"kernel32.dll");
    INITIALIZE_LIST Initialize = (INITIALIZE_LIST)GetProcAddress(Kernel, "InitializeProcThreadAttributeList");
    UPDATE_LIST Update = (UPDATE_LIST)GetProcAddress(Kernel, "UpdateProcThreadAttribute");
    DELETE_LIST Delete = (DELETE_LIST)GetProcAddress(Kernel, "DeleteProcThreadAttributeList");
    STARTUPINFOEXW Si;
    PROCESS_INFORMATION Pi;
    WCHAR Cmd[] = L"cmd.exe /c exit 3";
    HANDLE List[1];
    SIZE_T Size = 0;
    DWORD Exit = 0, Error;
    BOOL Ok;

    if (!Initialize || !Update || !Delete)
    {
        skip("%s: no process attribute lists\n", What);
        return;
    }
    Initialize(NULL, 1, 0, &Size);
    Si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, Size);
    ok(Si.lpAttributeList != NULL, "%s: out of memory\n", What);
    if (!Si.lpAttributeList)
        return;
    ok(Initialize(Si.lpAttributeList, 1, 0, &Size), "%s: Initialize failed: %lu\n", What, GetLastError());
    List[0] = (HANDLE)s;
    ok(Update(Si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, List, sizeof(List), NULL, NULL),
       "%s: Update failed: %lu\n", What, GetLastError());
    ZeroMemory(&Si.StartupInfo, sizeof(Si.StartupInfo));
    Si.StartupInfo.cb = sizeof(Si);
    Si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    Si.StartupInfo.hStdInput = (HANDLE)s;
    Si.StartupInfo.hStdOutput = (HANDLE)s;
    Si.StartupInfo.hStdError = (HANDLE)s;

    Ok = CreateProcessW(NULL, Cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                        NULL, NULL, &Si.StartupInfo, &Pi);
    Error = GetLastError();
    ok(Ok, "%s: CreateProcess with the socket in the handle list failed: %lu\n", What, Error);
    if (Ok)
    {
        ok(WaitForSingleObject(Pi.hProcess, 30000) == WAIT_OBJECT_0, "%s: child did not exit\n", What);
        GetExitCodeProcess(Pi.hProcess, &Exit);
        ok(Exit == 3, "%s: child exit code %lu\n", What, Exit);
        CloseHandle(Pi.hThread);
        CloseHandle(Pi.hProcess);
    }
    Delete(Si.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, Si.lpAttributeList);
}

START_TEST(handleinherit)
{
    WSADATA Data;
    SOCKET Listener, Client, Server, Dup, DupOverlapped, Plain, Overlapped, NoInherit;
    WSAPROTOCOL_INFOW Info;
    struct sockaddr_in Addr;
    int Length = sizeof(Addr);

    ok(WSAStartup(MAKEWORD(2, 2), &Data) == 0, "WSAStartup failed\n");

    Plain = socket(AF_INET, SOCK_STREAM, 0);
    ok(Plain != INVALID_SOCKET, "socket failed: %d\n", WSAGetLastError());
    ok(IsInheritable(Plain, "socket"), "socket() is not inheritable\n");
    Overlapped = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    ok(Overlapped != INVALID_SOCKET, "WSASocketW failed: %d\n", WSAGetLastError());
    ok(IsInheritable(Overlapped, "overlapped"), "an overlapped WSASocketW socket is not inheritable\n");
    NoInherit = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    ok(NoInherit != INVALID_SOCKET, "WSASocketW(NO_HANDLE_INHERIT) failed: %d\n", WSAGetLastError());
    ok(!IsInheritable(NoInherit, "no inherit"), "WSA_FLAG_NO_HANDLE_INHERIT socket is inheritable\n");

    Listener = socket(AF_INET, SOCK_STREAM, 0);
    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok(bind(Listener, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "bind failed: %d\n", WSAGetLastError());
    ok(getsockname(Listener, (struct sockaddr *)&Addr, &Length) == 0, "getsockname failed\n");
    ok(listen(Listener, 1) == 0, "listen failed\n");
    Client = socket(AF_INET, SOCK_STREAM, 0);
    ok(connect(Client, (struct sockaddr *)&Addr, sizeof(Addr)) == 0, "connect failed: %d\n", WSAGetLastError());
    ok(IsInheritable(Client, "client"), "a connected client socket is not inheritable\n");
    Server = accept(Listener, NULL, NULL);
    ok(Server != INVALID_SOCKET, "accept failed: %d\n", WSAGetLastError());
    ok(IsInheritable(Server, "accepted"), "an accepted socket is not inheritable\n");

    /* The socket a process makes from the info of WSADuplicateSocket (here for the same process) */
    ok(WSADuplicateSocketW(Server, GetCurrentProcessId(), &Info) == 0, "WSADuplicateSocket failed: %d\n", WSAGetLastError());
    Dup = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &Info, 0, 0);
    ok(Dup != INVALID_SOCKET, "WSASocketW(FROM_PROTOCOL_INFO) failed: %d\n", WSAGetLastError());
    if (Dup != INVALID_SOCKET)
    {
        ok(IsInheritable(Dup, "duplicated"), "the socket made from WSADuplicateSocket is not inheritable\n");
        CheckHandleList(Dup, "duplicated socket");
    }
    ok(WSADuplicateSocketW(Server, GetCurrentProcessId(), &Info) == 0, "second WSADuplicateSocket failed\n");
    DupOverlapped = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &Info, 0, WSA_FLAG_OVERLAPPED);
    ok(DupOverlapped != INVALID_SOCKET, "overlapped WSASocketW(FROM_PROTOCOL_INFO) failed: %d\n", WSAGetLastError());
    if (DupOverlapped != INVALID_SOCKET)
        ok(IsInheritable(DupOverlapped, "duplicated overlapped"), "the overlapped duplicated socket is not inheritable\n");

    CheckHandleList(Server, "accepted socket");

    closesocket(Plain);
    closesocket(Overlapped);
    closesocket(NoInherit);
    closesocket(Listener);
    closesocket(Client);
    closesocket(Server);
    if (Dup != INVALID_SOCKET) closesocket(Dup);
    if (DupOverlapped != INVALID_SOCKET) closesocket(DupOverlapped);
    WSACleanup();
}
