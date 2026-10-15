/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GetCurrentThreadStackLimits
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef VOID (WINAPI *FN_GetCurrentThreadStackLimits)(PULONG_PTR, PULONG_PTR);

static FN_GetCurrentThreadStackLimits pGetCurrentThreadStackLimits;

static void
CheckLimits(const char *What)
{
    ULONG_PTR Low = 0, High = 0;
    ULONG_PTR Here = (ULONG_PTR)&Low;
    MEMORY_BASIC_INFORMATION Info;
    NT_TIB *Tib = (NT_TIB *)NtCurrentTeb();

    pGetCurrentThreadStackLimits(&Low, &High);

    ok(Low == (ULONG_PTR)NtCurrentTeb()->DeallocationStack,
       "%s: low is %p, DeallocationStack is %p\n", What, (PVOID)Low, NtCurrentTeb()->DeallocationStack);
    ok(High == (ULONG_PTR)Tib->StackBase,
       "%s: high is %p, StackBase is %p\n", What, (PVOID)High, Tib->StackBase);
    ok(Low != 0 && Low < High, "%s: limits %p and %p are not an ordered range\n", What, (PVOID)Low, (PVOID)High);
    ok(Here >= Low && Here < High, "%s: a local variable at %p is outside %p to %p\n", What, (PVOID)Here, (PVOID)Low, (PVOID)High);

    ok(VirtualQuery((PVOID)Here, &Info, sizeof(Info)) == sizeof(Info), "%s: VirtualQuery failed\n", What);
    ok((ULONG_PTR)Info.AllocationBase == Low, "%s: allocation base %p, low limit %p\n",
       What, Info.AllocationBase, (PVOID)Low);
}

static DWORD WINAPI
ThreadProc(PVOID Param)
{
    CheckLimits("secondary thread");
    return 0;
}

START_TEST(GetCurrentThreadStackLimits)
{
    HANDLE Thread;

    pGetCurrentThreadStackLimits = (FN_GetCurrentThreadStackLimits)
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentThreadStackLimits");
    if (!pGetCurrentThreadStackLimits)
    {
        HKEY Key;

        /* Windows 7 and earlier lack it, ReactOS has it */
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ReactOS", 0, KEY_READ, &Key) == ERROR_SUCCESS)
        {
            RegCloseKey(Key);
            ok(0, "kernel32 does not export GetCurrentThreadStackLimits\n");
        }
        else
        {
            skip("GetCurrentThreadStackLimits is not available\n");
        }
        return;
    }

    CheckLimits("main thread");

    Thread = CreateThread(NULL, 0, ThreadProc, NULL, 0, NULL);
    ok(Thread != NULL, "CreateThread failed, error %lu\n", GetLastError());
    if (Thread)
    {
        WaitForSingleObject(Thread, INFINITE);
        CloseHandle(Thread);
    }
}
