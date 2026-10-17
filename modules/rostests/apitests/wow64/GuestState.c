/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Wide guest reads of kernel-maintained TEB/PEB state
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */
#include <apitest.h>
#include <user32.h>
#include "GuestState.h"

extern ULONG __cdecl ClientPfnCall64(ULONG, ULONGLONG *, ULONGLONG *);
extern void ClientPfnCallback64(void);
static ULONGLONG Callbacks[32];

ULONG GuestStateCall(ULONG Number, ULONGLONG *Args)
{
    return ClientPfnCall64(Number, Args, Callbacks);
}

DWORD_PTR WINAPI NtUserGetThreadState(DWORD Routine)
{
    ULONGLONG Args[6] = { Routine };
    return GuestStateCall(Native_NtUserGetThreadState, Args);
}

ULONG_PTR WINAPI NtUserCallOneParam(ULONG_PTR Param, DWORD Routine)
{
    ULONGLONG Args[6] = { Param, Routine };
    /* DesktopPtrToUser normally needs no syscall for the current desktop. */
    ok(Routine != ONEPARAM_ROUTINE_GETDESKTOPMAPPING,
       "unexpected cross-desktop mapping in this fixture\n");
    return GuestStateCall(Native_NtUserCallOneParam, Args);
}

START_TEST(GuestState)
{
    BOOL Wow64 = FALSE;
    USERCONNECT Connection = {0};
    ULONGLONG Args[6] = { (ULONGLONG)-1, (ULONG_PTR)&Connection, sizeof(Connection) };
    PTEB64 Teb;
    ULONG i, Status;

    if (!is_reactos() || !IsWow64Process(GetCurrentProcess(), &Wow64) || !Wow64)
    {
        skip("Requires ReactOS WoW64 and its native syscall numbers\n");
        return;
    }
    for (i = 0; i < ARRAYSIZE(Callbacks); ++i)
        Callbacks[i] = (ULONG_PTR)ClientPfnCallback64;

    /* The oracle reads GS in long mode, independently of the guest accessor's
     * subtraction from FS. The kernel initializes all the state under test. */
    Teb = GuestStateNativeTeb();
    ok((ULONG_PTR)Teb < 0x80000000, "native TEB is not below 2 GB: %p\n", Teb);
    ok((PBYTE)NtCurrentTeb() - (PBYTE)Teb == 0x2000, "TEB allocation contract changed\n");
    Connection.ulVersion = USER_VERSION;
    Status = GuestStateCall(Native_NtUserProcessConnect, Args);
    ok(Status == 0, "NtUserProcessConnect: %lx\n", Status);
    if (Status) return;
    NtUserGetThreadState(THREADSTATE_GETTHREADINFO);
    ok(Teb->Win32ThreadInfo != 0, "kernel did not initialize THREADINFO\n");
    ok(Teb->ProcessEnvironmentBlock < 0x80000000, "native PEB is not below 2 GB\n");
    GuestStateUser(Teb, &Connection);
    GuestStateGdi(Teb);
}
