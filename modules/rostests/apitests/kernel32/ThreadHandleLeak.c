/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Threads must not leak kernel handles (per-thread DLL state released at thread exit)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define THREAD_COUNT    500
#define MAX_HANDLE_GROWTH 10

typedef UINT_PTR (WINAPI *PFN_SOCKET)(int, int, int);
typedef int (WINAPI *PFN_CLOSESOCKET)(UINT_PTR);
typedef int (WINAPI *PFN_WSASTARTUP)(WORD, PVOID);

static PFN_SOCKET g_pfnSocket;
static PFN_CLOSESOCKET g_pfnCloseSocket;

static
DWORD
WINAPI
PlainThread(
    _In_ PVOID Parameter)
{
    return 0;
}

/* Typical runtime-thread work: TLS, events, alertable wait, the thread's own handle duplicated */
static
DWORD
WINAPI
RuntimeThread(
    _In_ PVOID Parameter)
{
    DWORD dwTls = TlsAlloc();
    HANDLE hEvent, hDup;

    TlsSetValue(dwTls, (PVOID)1);
    hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    SleepEx(0, TRUE);
    WaitForSingleObjectEx(hEvent, 0, TRUE);
    CloseHandle(hEvent);
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        CloseHandle(hDup);
    TlsFree(dwTls);
    return 0;
}

/* The first Winsock call of a thread creates per-thread state in ws2_32 */
static
DWORD
WINAPI
WinsockThread(
    _In_ PVOID Parameter)
{
    UINT_PTR s = g_pfnSocket(2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);

    if (s != (UINT_PTR)-1)
        g_pfnCloseSocket(s);
    return 0;
}

/* Shell folder resolution expands %VARIABLES% through the ProfileList registry key */
typedef HRESULT (WINAPI *PFN_SHGETFOLDERPATHW)(HWND, int, HANDLE, DWORD, LPWSTR);
static PFN_SHGETFOLDERPATHW g_pfnGetFolderPath;

static
DWORD
WINAPI
ShellThread(
    _In_ PVOID Parameter)
{
    static const int s_Csidl[] = { 0x1a /* APPDATA */, 0x1c /* LOCAL_APPDATA */, 0x23 /* COMMON_APPDATA */, 0x28 /* PROFILE */ };
    WCHAR szPath[MAX_PATH];
    UINT i;

    for (i = 0; i < ARRAYSIZE(s_Csidl); i++)
        g_pfnGetFolderPath(NULL, s_Csidl[i], NULL, 0, szPath);
    return 0;
}

static
LONG
RunThreads(
    _In_ LPTHREAD_START_ROUTINE Routine,
    _In_ BOOL Suspended)
{
    DWORD dwBefore = 0, dwAfter = 0;
    INT i;

    GetProcessHandleCount(GetCurrentProcess(), &dwBefore);
    for (i = 0; i < THREAD_COUNT; i++)
    {
        HANDLE hThread = CreateThread(NULL, 0, Routine, NULL, Suspended ? CREATE_SUSPENDED : 0, NULL);

        if (!hThread)
        {
            skip("CreateThread failed with %lu\n", GetLastError());
            break;
        }
        if (Suspended)
            ResumeThread(hThread);
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
    }
    Sleep(100);
    GetProcessHandleCount(GetCurrentProcess(), &dwAfter);
    return (LONG)dwAfter - (LONG)dwBefore;
}

START_TEST(ThreadHandleLeak)
{
    HMODULE hWs2 = LoadLibraryW(L"ws2_32.dll");
    HMODULE hShell = LoadLibraryW(L"shell32.dll");
    PFN_WSASTARTUP pfnStartup;
    BYTE WsaData[1024];
    LONG lDelta;

    /* Warm up lazy per-process state */
    RunThreads(PlainThread, FALSE);

    lDelta = RunThreads(PlainThread, FALSE);
    ok(lDelta < MAX_HANDLE_GROWTH, "plain threads leaked %ld handles over %d threads\n", lDelta, THREAD_COUNT);

    lDelta = RunThreads(PlainThread, TRUE);
    ok(lDelta < MAX_HANDLE_GROWTH, "suspended+resumed threads leaked %ld handles over %d threads\n", lDelta, THREAD_COUNT);

    lDelta = RunThreads(RuntimeThread, FALSE);
    ok(lDelta < MAX_HANDLE_GROWTH, "runtime-style threads leaked %ld handles over %d threads\n", lDelta, THREAD_COUNT);

    pfnStartup = hWs2 ? (PFN_WSASTARTUP)GetProcAddress(hWs2, "WSAStartup") : NULL;
    g_pfnSocket = hWs2 ? (PFN_SOCKET)GetProcAddress(hWs2, "socket") : NULL;
    g_pfnCloseSocket = hWs2 ? (PFN_CLOSESOCKET)GetProcAddress(hWs2, "closesocket") : NULL;
    if (pfnStartup && g_pfnSocket && g_pfnCloseSocket && pfnStartup(MAKEWORD(2, 2), WsaData) == 0)
    {
        /* Warm up Winsock process state (providers, catalog) */
        RunThreads(WinsockThread, FALSE);

        lDelta = RunThreads(WinsockThread, FALSE);
        ok(lDelta < MAX_HANDLE_GROWTH, "Winsock threads leaked %ld handles over %d threads\n", lDelta, THREAD_COUNT);
    }
    else
    {
        skip("Winsock not available\n");
    }

    g_pfnGetFolderPath = hShell ? (PFN_SHGETFOLDERPATHW)GetProcAddress(hShell, "SHGetFolderPathW") : NULL;
    if (g_pfnGetFolderPath)
    {
        RunThreads(ShellThread, FALSE);

        lDelta = RunThreads(ShellThread, FALSE);
        ok(lDelta < MAX_HANDLE_GROWTH, "shell folder threads leaked %ld handles over %d threads\n", lDelta, THREAD_COUNT);
    }
    else
    {
        skip("SHGetFolderPathW not available\n");
    }
}
