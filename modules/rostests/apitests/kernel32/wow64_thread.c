/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <windows.h>
static LONG Attached, Detached;
static DWORD Tls = TLS_OUT_OF_INDEXES;

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, PVOID Reserved)
{
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Reserved);
    switch (Reason)
    {
    case DLL_PROCESS_ATTACH:
        Tls = TlsAlloc();
        return Tls != TLS_OUT_OF_INDEXES;
    case DLL_THREAD_ATTACH:
        InterlockedIncrement(&Attached);
        TlsSetValue(Tls, (PVOID)(ULONG_PTR)GetCurrentThreadId());
        break;
    case DLL_THREAD_DETACH:
        if ((DWORD)(ULONG_PTR)TlsGetValue(Tls) == GetCurrentThreadId())
            InterlockedIncrement(&Detached);
        break;
    case DLL_PROCESS_DETACH:
        TlsFree(Tls);
        break;
    }
    return TRUE;
}

VOID WINAPI ThreadNotifications(PLONG Attach, PLONG Detach)
{
    *Attach = Attached;
    *Detach = Detached;
}
