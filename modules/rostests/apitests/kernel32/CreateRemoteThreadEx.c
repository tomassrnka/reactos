/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for CreateRemoteThreadEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef HANDLE (WINAPI *FN_CreateRemoteThreadEx)(HANDLE, LPSECURITY_ATTRIBUTES, SIZE_T,
                                                 LPTHREAD_START_ROUTINE, LPVOID, DWORD,
                                                 LPPROC_THREAD_ATTRIBUTE_LIST, LPDWORD);
typedef BOOL (WINAPI *FN_InitializeProcThreadAttributeList)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD, PSIZE_T);
typedef BOOL (WINAPI *FN_UpdateProcThreadAttribute)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR,
                                                    PVOID, SIZE_T, PVOID, PSIZE_T);
typedef VOID (WINAPI *FN_DeleteProcThreadAttributeList)(LPPROC_THREAD_ATTRIBUTE_LIST);

#ifndef PROC_THREAD_ATTRIBUTE_IDEAL_PROCESSOR
#define PROC_THREAD_ATTRIBUTE_IDEAL_PROCESSOR 0x00030005
#endif

typedef DWORD (WINAPI *FN_GetThreadId)(HANDLE);

static FN_CreateRemoteThreadEx pCreateRemoteThreadEx;
static FN_GetThreadId pGetThreadId, pGetProcessIdOfThread;

/* Thread id and owner, through GetProcAddress so that the test still loads on NT 5.1 */
static void CheckThreadIds(HANDLE hThread, DWORD Tid, DWORD Pid)
{
    if (!pGetThreadId || !pGetProcessIdOfThread)
    {
        ok(Tid != 0, "No thread id\n");
        return;
    }
    ok_long(Tid, pGetThreadId(hThread));
    ok_long(pGetProcessIdOfThread(hThread), Pid);
}
static LONG s_Runs;

static DWORD WINAPI ThreadProc(LPVOID Param)
{
    InterlockedIncrement(&s_Runs);
    return (DWORD)(ULONG_PTR)Param;
}

#ifdef _WIN64
/* Saves the size of the stack reservation of the thread */
static SIZE_T s_StackReserve;
static DWORD WINAPI StackSizeProc(LPVOID Param)
{
    MEMORY_BASIC_INFORMATION Info;
    if (VirtualQuery(&Info, &Info, sizeof(Info)))
        s_StackReserve = (ULONG_PTR)((PNT_TIB)NtCurrentTeb())->StackBase - (ULONG_PTR)Info.AllocationBase;
    return 0;
}
#endif

static DWORD WaitThread(HANDLE hThread)
{
    DWORD ExitCode = 0xdeadbeef;
    ok_long(WaitForSingleObject(hThread, 10000), WAIT_OBJECT_0);
    ok(GetExitCodeThread(hThread, &ExitCode), "GetExitCodeThread failed: %lu\n", GetLastError());
    return ExitCode;
}

static void Test_CurrentProcess(void)
{
    HANDLE hThread;
    DWORD Tid = 0;

    /* Plain call, same as CreateThread */
    s_Runs = 0;
    hThread = pCreateRemoteThreadEx(GetCurrentProcess(), NULL, 0, ThreadProc,
                                    (LPVOID)(ULONG_PTR)0x1234, 0, NULL, &Tid);
    ok(hThread != NULL, "CreateRemoteThreadEx failed: %lu\n", GetLastError());
    if (hThread)
    {
        CheckThreadIds(hThread, Tid, GetCurrentProcessId());
        ok_long(WaitThread(hThread), 0x1234);
        ok_long(s_Runs, 1);
        CloseHandle(hThread);
    }

    /* No thread id buffer, a stack reservation */
    s_Runs = 0;
    hThread = pCreateRemoteThreadEx(GetCurrentProcess(), NULL, 1024 * 1024, ThreadProc,
                                    (LPVOID)(ULONG_PTR)0x55, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                    NULL, NULL);
    ok(hThread != NULL, "CreateRemoteThreadEx failed: %lu\n", GetLastError());
    if (hThread)
    {
        ok_long(WaitThread(hThread), 0x55);
        ok_long(s_Runs, 1);
        CloseHandle(hThread);
    }

#ifdef _WIN64
    /* A reservation above 4 GB is kept whole */
    s_StackReserve = 0;
    hThread = pCreateRemoteThreadEx(GetCurrentProcess(), NULL, (SIZE_T)0x100100000ULL, StackSizeProc,
                                    NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL, NULL);
    ok(hThread != NULL, "CreateRemoteThreadEx failed with a 4 GB stack: %lu\n", GetLastError());
    if (hThread)
    {
        ok_long(WaitThread(hThread), 0);
        ok(s_StackReserve >= (SIZE_T)0x100100000ULL, "Stack reservation 0x%Ix\n", s_StackReserve);
        CloseHandle(hThread);
    }
#endif

    /* Suspended until resumed */
    s_Runs = 0;
    hThread = pCreateRemoteThreadEx(GetCurrentProcess(), NULL, 0, ThreadProc,
                                    (LPVOID)(ULONG_PTR)0x77, CREATE_SUSPENDED, NULL, &Tid);
    ok(hThread != NULL, "CreateRemoteThreadEx failed: %lu\n", GetLastError());
    if (hThread)
    {
        ok_long(WaitForSingleObject(hThread, 200), WAIT_TIMEOUT);
        ok_long(s_Runs, 0);
        ok_long(ResumeThread(hThread), 1);
        ok_long(WaitThread(hThread), 0x77);
        ok_long(s_Runs, 1);
        CloseHandle(hThread);
    }

    /* Invalid process handle */
    SetLastError(0xdeadbeef);
    hThread = pCreateRemoteThreadEx(NULL, NULL, 0, ThreadProc, NULL, 0, NULL, &Tid);
    ok(hThread == NULL, "CreateRemoteThreadEx succeeded\n");
    ok_long(GetLastError(), ERROR_INVALID_HANDLE);
    if (hThread) CloseHandle(hThread);
}

static void Test_RemoteProcess(void)
{
    WCHAR Path[MAX_PATH], CmdLine[MAX_PATH + 64];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    LPTHREAD_START_ROUTINE pExitThread;
    HANDLE hThread, hReady;
    DWORD Tid = 0;

    /* ExitThread has the same address in a child of the same image type */
    pExitThread = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitThread");
    ok(pExitThread != NULL, "No ExitThread\n");
    if (!pExitThread)
        return;

    hReady = CreateEventW(NULL, TRUE, FALSE, L"CreateRemoteThreadEx_child_ready");
    ok(hReady != NULL, "CreateEventW failed: %lu\n", GetLastError());
    if (!hReady)
        return;

    GetModuleFileNameW(NULL, Path, _countof(Path));
    StringCchPrintfW(CmdLine, _countof(CmdLine), L"\"%s\" CreateRemoteThreadEx child", Path);
    if (!CreateProcessW(Path, CmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        skip("CreateProcessW failed: %lu\n", GetLastError());
        CloseHandle(hReady);
        return;
    }
    ok_long(WaitForSingleObject(hReady, 10000), WAIT_OBJECT_0);
    CloseHandle(hReady);

    hThread = pCreateRemoteThreadEx(pi.hProcess, NULL, 0, pExitThread,
                                    (LPVOID)(ULONG_PTR)0x4321, 0, NULL, &Tid);
    ok(hThread != NULL, "CreateRemoteThreadEx failed: %lu\n", GetLastError());
    if (hThread)
    {
        CheckThreadIds(hThread, Tid, pi.dwProcessId);
        ok_long(WaitThread(hThread), 0x4321);
        CloseHandle(hThread);
    }

    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

static void Test_AttributeList(void)
{
    FN_InitializeProcThreadAttributeList pInit;
    FN_UpdateProcThreadAttribute pUpdate;
    FN_DeleteProcThreadAttributeList pDelete;
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    LPPROC_THREAD_ATTRIBUTE_LIST List;
    PROCESSOR_NUMBER Ideal = { 0 };
    SIZE_T Size = 0;
    HANDLE hThread;
    ULONG_PTR Dummy[16] = { 0 };

    pInit = (FN_InitializeProcThreadAttributeList)GetProcAddress(hKernel32, "InitializeProcThreadAttributeList");
    pUpdate = (FN_UpdateProcThreadAttribute)GetProcAddress(hKernel32, "UpdateProcThreadAttribute");
    pDelete = (FN_DeleteProcThreadAttributeList)GetProcAddress(hKernel32, "DeleteProcThreadAttributeList");

    if (pInit && pUpdate && pDelete)
        pInit(NULL, 1, 0, &Size);
    if (Size == 0 || Size > sizeof(Dummy))
    {
        /* ReactOS: InitializeProcThreadAttributeList is not implemented, no list can exist */
        SetLastError(0xdeadbeef);
        hThread = pCreateRemoteThreadEx(GetCurrentProcess(), NULL, 0, ThreadProc, NULL, 0,
                                        (LPPROC_THREAD_ATTRIBUTE_LIST)Dummy, NULL);
        ok(hThread == NULL, "CreateRemoteThreadEx succeeded with a list\n");
        ok_long(GetLastError(), ERROR_NOT_SUPPORTED);
        if (hThread)
        {
            WaitForSingleObject(hThread, 10000);
            CloseHandle(hThread);
        }
        return;
    }

    /* Windows: an empty list and an ideal processor attribute are accepted */
    List = (LPPROC_THREAD_ATTRIBUTE_LIST)Dummy;
    ok(pInit(List, 1, 0, &Size), "InitializeProcThreadAttributeList failed: %lu\n", GetLastError());
    s_Runs = 0;
    hThread = pCreateRemoteThreadEx(GetCurrentProcess(), NULL, 0, ThreadProc,
                                    (LPVOID)(ULONG_PTR)0x99, 0, List, NULL);
    ok(hThread != NULL, "CreateRemoteThreadEx failed with an empty list: %lu\n", GetLastError());
    if (hThread)
    {
        ok_long(WaitThread(hThread), 0x99);
        CloseHandle(hThread);
    }

    ok(pUpdate(List, 0, PROC_THREAD_ATTRIBUTE_IDEAL_PROCESSOR, &Ideal, sizeof(Ideal), NULL, NULL),
       "UpdateProcThreadAttribute failed: %lu\n", GetLastError());
    hThread = pCreateRemoteThreadEx(GetCurrentProcess(), NULL, 0, ThreadProc,
                                    (LPVOID)(ULONG_PTR)0x9a, 0, List, NULL);
    ok(hThread != NULL, "CreateRemoteThreadEx failed with an ideal processor: %lu\n", GetLastError());
    if (hThread)
    {
        ok_long(WaitThread(hThread), 0x9a);
        CloseHandle(hThread);
    }
    pDelete(List);
}

START_TEST(CreateRemoteThreadEx)
{
    int argc;
    char **argv;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "child"))
    {
        HANDLE hReady = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"CreateRemoteThreadEx_child_ready");
        if (hReady)
        {
            SetEvent(hReady);
            CloseHandle(hReady);
        }
        Sleep(30000);
        return;
    }

    pCreateRemoteThreadEx = (FN_CreateRemoteThreadEx)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                    "CreateRemoteThreadEx");
#if defined(__REACTOS__) && (DLL_EXPORT_VERSION < 0x601)
    if (!pCreateRemoteThreadEx)
    {
        skip("CreateRemoteThreadEx is exported from 0x601 on\n");
        return;
    }
#endif
    ok(pCreateRemoteThreadEx != NULL, "CreateRemoteThreadEx is not exported\n");
    if (!pCreateRemoteThreadEx)
        return;

    pGetThreadId = (FN_GetThreadId)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadId");
    pGetProcessIdOfThread = (FN_GetThreadId)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessIdOfThread");

    Test_CurrentProcess();
    Test_RemoteProcess();
    Test_AttributeList();
}
