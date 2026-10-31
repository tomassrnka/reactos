/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for RtlGetCurrentTransaction and RtlSetCurrentTransaction
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef PVOID (NTAPI *PFN_RtlGetCurrentTransaction)(VOID);
typedef BOOL (NTAPI *PFN_RtlSetCurrentTransaction)(PVOID);

static HANDLE OtherThreadValue;

static DWORD WINAPI OtherThread(LPVOID Param)
{
    PFN_RtlGetCurrentTransaction pGet = Param;

    OtherThreadValue = pGet();
    return 0;
}

START_TEST(RtlCurrentTransaction)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_RtlGetCurrentTransaction pGet = (PFN_RtlGetCurrentTransaction)GetProcAddress(Ntdll, "RtlGetCurrentTransaction");
    PFN_RtlSetCurrentTransaction pSet = (PFN_RtlSetCurrentTransaction)GetProcAddress(Ntdll, "RtlSetCurrentTransaction");
    HANDLE Thread;

    if (!pGet || !pSet)
    {
        ok(!is_reactos(), "RtlGetCurrentTransaction or RtlSetCurrentTransaction is not exported by ntdll.dll\n");
        skip("the current transaction functions are not available\n");
        return;
    }

    /* no transaction to begin with; the value is the thread's own, whatever it is */
    ok(pGet() == NULL, "initial value %p\n", pGet());

    ok(pSet((PVOID)(ULONG_PTR)0x1234) == TRUE, "setting 0x1234 failed\n");
    ok(pGet() == (PVOID)(ULONG_PTR)0x1234, "got %p, expected 0x1234\n", pGet());

    /* only this thread has it, and it is in the TEB */
    OtherThreadValue = (HANDLE)(ULONG_PTR)0xdeadbeef;
    Thread = CreateThread(NULL, 0, OtherThread, pGet, 0, NULL);
    ok(Thread != NULL, "CreateThread failed with %lu\n", GetLastError());
    if (Thread)
    {
        WaitForSingleObject(Thread, 10000);
        CloseHandle(Thread);
        ok(OtherThreadValue == NULL, "another thread got %p\n", OtherThreadValue);
    }
    ok(*(PVOID *)((PUCHAR)NtCurrentTeb() + (sizeof(PVOID) == 8 ? 0x17B8 : 0xFAC)) == (PVOID)(ULONG_PTR)0x1234,
       "the value is not in the TEB\n");

    ok(pSet(NULL) == TRUE, "clearing failed\n");
    ok(pGet() == NULL, "got %p after clearing\n", pGet());
}
