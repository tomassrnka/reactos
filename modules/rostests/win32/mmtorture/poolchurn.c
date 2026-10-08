/*
 * PROJECT:     ReactOS memory manager torture suite
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Workload (e): kernel pool churn from user mode. Objects,
 *              registry keys, files, pipes, timers, ports, sections,
 *              handles and sockets come and go; run it with special pool
 *              on for a chosen tag
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "mmtorture.h"
#include <winsock2.h>

static WCHAR Dir[MAX_PATH];
static BOOL UseNet, NoRegistry;
static LONG Slot;

static VOID
ChurnObjects(MMT_RNG *Rng, ULONG Thread)
{
    HANDLE H[48];
    WCHAR Name[64];
    ULONG i;

    for (i = 0; i < _countof(H); i++)
    {
        BOOL Named = MmtRand(Rng) & 1;
        _snwprintf(Name, _countof(Name), L"MmtPool-%lu-%lu-%lu", GetCurrentProcessId(), Thread, i);
        switch (i % 4)
        {
            case 0: H[i] = CreateEventW(NULL, i & 1, FALSE, Named ? Name : NULL); break;
            case 1: H[i] = CreateSemaphoreW(NULL, 0, 10, Named ? Name : NULL); break;
            case 2: H[i] = CreateMutexW(NULL, FALSE, Named ? Name : NULL); break;
            default: H[i] = CreateWaitableTimerW(NULL, TRUE, Named ? Name : NULL); break;
        }
    }
    for (i = 0; i < _countof(H); i++)
    {
        if (!H[i])
            continue;
        if (i % 4 == 0)
            SetEvent(H[i]);
        else if (i % 4 == 1)
            ReleaseSemaphore(H[i], 1, NULL);
        else if (i % 4 == 3)
        {
            LARGE_INTEGER Due;
            Due.QuadPart = -(LONGLONG)MmtRandRange(Rng, 1, 20000);
            SetWaitableTimer(H[i], &Due, 0, NULL, NULL, FALSE);
            if (MmtRand(Rng) & 1)
                CancelWaitableTimer(H[i]);
        }
        WaitForSingleObject(H[i], 0);
        CloseHandle(H[i]);
    }
}

static VOID
ChurnRegistry(MMT_RNG *Rng, ULONG Thread)
{
    WCHAR Path[128];
    HKEY Key;
    UCHAR Data[16384];
    ULONG i, Values = MmtRandRange(Rng, 1, 24);
    DWORD Size;

    _snwprintf(Path, _countof(Path), L"Software\\MmTorture\\P%lu-T%lu", GetCurrentProcessId(), Thread);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, Path, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &Key, NULL) != ERROR_SUCCESS)
        return;
    memset(Data, (int)Thread, sizeof(Data));
    for (i = 0; i < Values; i++)
    {
        WCHAR Value[16];
        _snwprintf(Value, _countof(Value), L"v%lu", i);
        RegSetValueExW(Key, Value, 0, REG_BINARY, Data, MmtRandRange(Rng, 1, sizeof(Data)));
    }
    for (i = 0; i < Values; i++)
    {
        WCHAR Value[16];
        _snwprintf(Value, _countof(Value), L"v%lu", i);
        Size = sizeof(Data);
        if (RegQueryValueExW(Key, Value, NULL, NULL, Data, &Size) == ERROR_SUCCESS && Size && Data[Size - 1] != (UCHAR)Thread)
            MmtFail("pool: registry value %S content changed", Value);
    }
    RegCloseKey(Key);
    if (RegDeleteKeyW(HKEY_CURRENT_USER, Path) != ERROR_SUCCESS)
        MmtFail("pool: cannot delete key %S", Path);
}

static VOID
ChurnFiles(MMT_RNG *Rng, ULONG Thread)
{
    WCHAR Path[MAX_PATH];
    HANDLE File;
    PUCHAR Buffer = VirtualAlloc(NULL, 65536, MEM_COMMIT, PAGE_READWRITE);
    DWORD Length = MmtRandRange(Rng, 1, 16) * 4096, Done;
    BOOL NoBuffering = MmtRand(Rng) & 1;
    ULONG i;

    _snwprintf(Path, _countof(Path), L"%s\\pool-%lu-%lu.tmp", Dir, GetCurrentProcessId(), Thread);
    if (!Buffer)
        return;
    File = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       NoBuffering ? FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH : 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        MmtFail("pool: cannot create %S: %lu", Path, GetLastError());
        VirtualFree(Buffer, 0, MEM_RELEASE);
        return;
    }
    for (i = 0; i < Length; i++)
        Buffer[i] = (UCHAR)(i * 7 + Thread);
    if (!WriteFile(File, Buffer, Length, &Done, NULL) || Done != Length)
        MmtFail("pool: write %S: %lu", Path, GetLastError());
    SetFilePointer(File, 0, NULL, FILE_BEGIN);
    memset(Buffer, 0, Length);
    if (!ReadFile(File, Buffer, Length, &Done, NULL) || Done != Length)
        MmtFail("pool: read %S: %lu", Path, GetLastError());
    for (i = 0; i < Length; i++)
    {
        if (Buffer[i] != (UCHAR)(i * 7 + Thread))
        {
            MmtFail("pool: %S byte %lu read back %02x (no-buffering %d)", Path, i, Buffer[i], NoBuffering);
            break;
        }
    }
    CloseHandle(File);
    VirtualFree(Buffer, 0, MEM_RELEASE);
    if (!DeleteFileW(Path))
        MmtFail("pool: cannot delete %S: %lu", Path, GetLastError());
}

static DWORD WINAPI
EmptyThread(PVOID Context)
{
    return (DWORD)(ULONG_PTR)Context;
}

static VOID
ChurnPipes(MMT_RNG *Rng, ULONG Thread)
{
    WCHAR Name[64];
    HANDLE Server, Client;
    UCHAR Data[8192];
    DWORD Length = MmtRandRange(Rng, 1, sizeof(Data)), Done = 0;

    _snwprintf(Name, _countof(Name), L"\\\\.\\pipe\\mmtpool-%lu-%lu", GetCurrentProcessId(), Thread);
    Server = CreateNamedPipeW(Name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 16384, 16384, 0, NULL);
    if (Server == INVALID_HANDLE_VALUE)
        return;
    Client = CreateFileW(Name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (Client != INVALID_HANDLE_VALUE)
    {
        memset(Data, 0x3C, Length);
        if (WriteFile(Client, Data, Length, &Done, NULL) && Done == Length)
        {
            memset(Data, 0, Length);
            if (!ReadFile(Server, Data, Length, &Done, NULL) || Done != Length || Data[Length - 1] != 0x3C)
                MmtFail("pool: pipe read %lu of %lu: %lu", Done, Length, GetLastError());
        }
        CloseHandle(Client);
    }
    DisconnectNamedPipe(Server);
    CloseHandle(Server);
}

static VOID
ChurnMisc(MMT_RNG *Rng)
{
    HANDLE Port, Section, Dup[256];
    ULONG i, Count;
    PVOID View;

    /* Completion port packets */
    Port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (Port)
    {
        DWORD Bytes;
        ULONG_PTR Key;
        LPOVERLAPPED Ov;
        for (i = 0; i < 32; i++)
            PostQueuedCompletionStatus(Port, i, i, NULL);
        for (i = 0; i < 16; i++)
            GetQueuedCompletionStatus(Port, &Bytes, &Key, &Ov, 0);
        CloseHandle(Port);
    }
    /* Small sections, mapped and not */
    Section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, MmtRandRange(Rng, 1, 64) * 4096, NULL);
    if (Section)
    {
        View = (MmtRand(Rng) & 1) ? MapViewOfFile(Section, FILE_MAP_WRITE, 0, 0, 0) : NULL;
        if (View)
        {
            *(volatile ULONG *)View = 1;
            UnmapViewOfFile(View);
        }
        CloseHandle(Section);
    }
    /* Handle table growth and shrink */
    Count = MmtRandRange(Rng, 1, _countof(Dup));
    for (i = 0; i < Count; i++)
    {
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &Dup[i], 0, FALSE, DUPLICATE_SAME_ACCESS))
            break;
    }
    Count = i;
    for (i = 0; i < Count; i++)
        CloseHandle(Dup[i]);
}

static VOID
ChurnSockets(MMT_RNG *Rng)
{
    SOCKET Listener, Client, Server;
    struct sockaddr_in Addr;
    int Length = sizeof(Addr);
    char Data[4096];
    int Sent, Got;

    Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Listener == INVALID_SOCKET)
        return;
    ZeroMemory(&Addr, sizeof(Addr));
    Addr.sin_family = AF_INET;
    Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(Listener, (struct sockaddr *)&Addr, sizeof(Addr)) || listen(Listener, 4) ||
        getsockname(Listener, (struct sockaddr *)&Addr, &Length))
    {
        closesocket(Listener);
        return;
    }
    Client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (Client != INVALID_SOCKET && !connect(Client, (struct sockaddr *)&Addr, sizeof(Addr)))
    {
        Server = accept(Listener, NULL, NULL);
        if (Server != INVALID_SOCKET)
        {
            Sent = send(Client, Data, MmtRandRange(Rng, 1, sizeof(Data)), 0);
            Got = recv(Server, Data, sizeof(Data), 0);
            if (Sent <= 0 || Got <= 0)
                MmtLog("pool: loopback send %d recv %d: %d", Sent, Got, WSAGetLastError());
            closesocket(Server);
        }
    }
    if (Client != INVALID_SOCKET)
        closesocket(Client);
    closesocket(Listener);
}

static DWORD WINAPI
PoolThread(PVOID Context)
{
    ULONG Thread = (ULONG)(ULONG_PTR)Context;
    MMT_RNG Rng;

    MmtRngInit(&Rng, GetTickCount() ^ (Thread << 18) ^ GetCurrentProcessId());
    while (!MmtShouldStop())
    {
        switch (MmtRandRange(&Rng, 0, UseNet ? 6 : 5))
        {
            case 0: ChurnObjects(&Rng, Thread); break;
            case 1: if (!NoRegistry) ChurnRegistry(&Rng, Thread); break;
            case 2: ChurnFiles(&Rng, Thread); break;
            case 3: ChurnPipes(&Rng, Thread); break;
            case 4: ChurnMisc(&Rng); break;
            case 5:
            {
                HANDLE T = CreateThread(NULL, 0, EmptyThread, NULL, 0, NULL);
                if (T)
                {
                    WaitForSingleObject(T, 60000);
                    CloseHandle(T);
                }
                break;
            }
            default: ChurnSockets(&Rng); break;
        }
        MmtProgress(Slot);
    }
    return 0;
}

/* pool DIR THREADS [net] [noreg] */
int
MmtPoolMain(int argc, char **argv)
{
    ULONG Threads = min(MmtArgUlong(argc, argv, 3, 8), 32), i, Count = 0;
    HANDLE Handles[32];
    WSADATA Wsa;

    MmtOpenShared(FALSE);
    Slot = MmtAllocSlot("pool");
    MultiByteToWideChar(CP_ACP, 0, argc > 2 ? argv[2] : "C:\\mmt", -1, Dir, _countof(Dir));
    CreateDirectoryW(Dir, NULL);
    for (i = 4; (int)i < argc; i++)
    {
        if (!strcmp(argv[i], "net"))
            UseNet = !WSAStartup(MAKEWORD(2, 2), &Wsa);
        else if (!strcmp(argv[i], "noreg"))
            NoRegistry = TRUE;
    }
    for (i = 0; i < Threads; i++)
    {
        Handles[Count] = CreateThread(NULL, 0, PoolThread, (PVOID)(ULONG_PTR)i, 0, NULL);
        if (Handles[Count])
            Count++;
    }
    WaitForMultipleObjects(Count, Handles, TRUE, INFINITE);
    if (UseNet)
        WSACleanup();
    MmtLog("pool done");
    return 0;
}
