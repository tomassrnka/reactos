/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for applying an impersonation token (ThreadImpersonationToken)
 *              to a thread of another process and to an exiting thread
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ndk/pstypes.h>

typedef BOOL (WINAPI *FN_OpenProcessToken)(HANDLE, DWORD, PHANDLE);
typedef BOOL (WINAPI *FN_DuplicateTokenEx)(HANDLE, DWORD, LPSECURITY_ATTRIBUTES,
                                           SECURITY_IMPERSONATION_LEVEL, TOKEN_TYPE, PHANDLE);
typedef BOOL (WINAPI *FN_SetThreadToken)(PHANDLE, HANDLE);
typedef BOOL (WINAPI *FN_OpenThreadToken)(HANDLE, DWORD, BOOL, PHANDLE);

static FN_OpenProcessToken pOpenProcessToken;
static FN_DuplicateTokenEx pDuplicateTokenEx;
static FN_SetThreadToken pSetThreadToken;
static FN_OpenThreadToken pOpenThreadToken;

static HANDLE s_ExitEvent;

#define CHILD_THREADS 48

/* Make an impersonation token from the current process token */
static HANDLE MakeImpersonationToken(void)
{
    HANDLE hProcessToken = NULL, hImpToken = NULL;

    if (!pOpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &hProcessToken))
    {
        skip("OpenProcessToken failed: %lu\n", GetLastError());
        return NULL;
    }

    if (!pDuplicateTokenEx(hProcessToken, TOKEN_IMPERSONATE | TOKEN_QUERY, NULL,
                           SecurityImpersonation, TokenImpersonation, &hImpToken))
    {
        skip("DuplicateTokenEx failed: %lu\n", GetLastError());
        hImpToken = NULL;
    }

    CloseHandle(hProcessToken);
    return hImpToken;
}

/* The caller is not impersonating, so its own TEB must say so */
static void CheckOwnTeb(const char *What)
{
    ok(NtCurrentTeb()->IsImpersonating == 0, "%s: caller TEB IsImpersonating = %lu\n",
       What, (ULONG)NtCurrentTeb()->IsImpersonating);
    ok(NtCurrentTeb()->ImpersonationLocale == 0, "%s: caller TEB ImpersonationLocale = %ld\n",
       What, (LONG)NtCurrentTeb()->ImpersonationLocale);
}

/*
 * Applying an impersonation token to a thread of another process must write that
 * thread's TEB in its own process. The kernel used to write the target's TEB
 * address in the caller's address space instead: for the primary thread that
 * address is the caller's own primary TEB (silent corruption), for a later thread
 * it is usually unmapped in the caller (bugcheck 0x1E, STATUS_ACCESS_VIOLATION).
 */
static void Test_RemoteThread(void)
{
    WCHAR Path[MAX_PATH], CmdLine[MAX_PATH + 64];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    LPTHREAD_START_ROUTINE pSleep;
    HANDLE hReady, hImpToken, hRemote, hThreadToken;
    BOOL Success;

    hImpToken = MakeImpersonationToken();
    if (!hImpToken)
        return;

    /* Sleep has the same address in a child of the same image type */
    pSleep = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Sleep");
    ok(pSleep != NULL, "No Sleep\n");

    hReady = CreateEventW(NULL, TRUE, FALSE, L"ThreadImpersonationToken_child_ready");
    ok(hReady != NULL, "CreateEventW failed: %lu\n", GetLastError());
    if (!hReady || !pSleep)
    {
        if (hReady) CloseHandle(hReady);
        CloseHandle(hImpToken);
        return;
    }

    GetModuleFileNameW(NULL, Path, _countof(Path));
    StringCchPrintfW(CmdLine, _countof(CmdLine), L"\"%s\" ThreadImpersonationToken child", Path);
    if (!CreateProcessW(Path, CmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        skip("CreateProcessW failed: %lu\n", GetLastError());
        CloseHandle(hReady);
        CloseHandle(hImpToken);
        return;
    }
    ok_long(WaitForSingleObject(hReady, 10000), WAIT_OBJECT_0);
    CloseHandle(hReady);

    CheckOwnTeb("before");

    /* The child's primary thread */
    SetLastError(0xdeadbeef);
    Success = pSetThreadToken(&pi.hThread, hImpToken);
    ok(Success, "SetThreadToken on the remote primary thread failed: %lu\n", GetLastError());
    CheckOwnTeb("remote primary thread");
    hThreadToken = NULL;
    ok(pOpenThreadToken(pi.hThread, TOKEN_QUERY, TRUE, &hThreadToken),
       "Remote primary thread has no token: %lu\n", GetLastError());
    if (hThreadToken) CloseHandle(hThreadToken);

    /* A thread created after the child's CHILD_THREADS threads: its TEB lies below
     * anything the caller has mapped */
    hRemote = CreateRemoteThread(pi.hProcess, NULL, 0, pSleep, (LPVOID)(ULONG_PTR)30000, 0, NULL);
    ok(hRemote != NULL, "CreateRemoteThread failed: %lu\n", GetLastError());
    if (hRemote)
    {
        SetLastError(0xdeadbeef);
        Success = pSetThreadToken(&hRemote, hImpToken);
        ok(Success, "SetThreadToken on a later remote thread failed: %lu\n", GetLastError());
        CheckOwnTeb("later remote thread");
        CloseHandle(hRemote);
    }

    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(hImpToken);
}

static DWORD WINAPI ExitingProc(LPVOID Param)
{
    WaitForSingleObject(s_ExitEvent, INFINITE);
    return 0;
}

/*
 * A thread that is terminating may have its TEB torn down while impersonation is
 * applied; the kernel must take rundown protection and guard the write, so this
 * never crashes whether the thread is mid-exit or already gone.
 */
static void Test_ExitingThread(void)
{
    HANDLE hImpToken;
    int i;

    hImpToken = MakeImpersonationToken();
    if (!hImpToken)
        return;

    s_ExitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok(s_ExitEvent != NULL, "CreateEventW failed: %lu\n", GetLastError());
    if (!s_ExitEvent)
    {
        CloseHandle(hImpToken);
        return;
    }

    for (i = 0; i < 64; i++)
    {
        HANDLE hThread = CreateThread(NULL, 0, ExitingProc, NULL, 0, NULL);
        ok(hThread != NULL, "CreateThread failed: %lu\n", GetLastError());
        if (!hThread)
            break;

        /* Let the thread run down and race SetThreadToken against its teardown */
        SetEvent(s_ExitEvent);
        pSetThreadToken(&hThread, hImpToken);
        WaitForSingleObject(hThread, 5000);
        /* Now fully exited: TEB is gone, must still not crash */
        pSetThreadToken(&hThread, hImpToken);
        ResetEvent(s_ExitEvent);
        CloseHandle(hThread);
    }
    ok(TRUE, "Exiting-thread impersonation did not crash\n");

    CloseHandle(s_ExitEvent);
    CloseHandle(hImpToken);
}

START_TEST(ThreadImpersonationToken)
{
    HMODULE hAdvapi32;
    int argc;
    char **argv;

    /* Child instance: just signal readiness and stay alive as the remote target */
    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "child"))
    {
        HANDLE hReady;
        int i;

        /* Grow the child's TEB area well past the parent's */
        for (i = 0; i < CHILD_THREADS; i++)
        {
            HANDLE hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)Sleep, (LPVOID)(ULONG_PTR)30000, 0, NULL);
            if (hThread) CloseHandle(hThread);
        }
        hReady = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"ThreadImpersonationToken_child_ready");
        if (hReady)
        {
            SetEvent(hReady);
            CloseHandle(hReady);
        }
        Sleep(30000);
        return;
    }

    hAdvapi32 = LoadLibraryW(L"advapi32.dll");
    ok(hAdvapi32 != NULL, "LoadLibraryW(advapi32) failed: %lu\n", GetLastError());
    if (!hAdvapi32)
        return;

    pOpenProcessToken = (FN_OpenProcessToken)GetProcAddress(hAdvapi32, "OpenProcessToken");
    pDuplicateTokenEx = (FN_DuplicateTokenEx)GetProcAddress(hAdvapi32, "DuplicateTokenEx");
    pSetThreadToken = (FN_SetThreadToken)GetProcAddress(hAdvapi32, "SetThreadToken");
    pOpenThreadToken = (FN_OpenThreadToken)GetProcAddress(hAdvapi32, "OpenThreadToken");
    if (!pOpenProcessToken || !pDuplicateTokenEx || !pSetThreadToken || !pOpenThreadToken)
    {
        skip("advapi32 token APIs not available\n");
        return;
    }

    Test_RemoteThread();
    Test_ExitingThread();
}
