/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for RegisterSuspendResumeNotification and UnregisterSuspendResumeNotification (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows 10 22H2 (x64 and WOW64): the flags are DEVICE_NOTIFY_WINDOW_HANDLE (0) or
 * DEVICE_NOTIFY_CALLBACK (2), the recipient is not looked at when the registration is made, and a registration
 * is ended once.
 */

#include "precomp.h"

typedef PVOID (WINAPI *PFN_Register)(HANDLE, DWORD);
typedef BOOL (WINAPI *PFN_Unregister)(PVOID);

#define PENDING_ERROR 0xdeadbeef

static ULONG CALLBACK PowerCallback(PVOID Context, ULONG Type, PVOID Setting)
{
    return 0;
}

START_TEST(SuspendResumeNotification)
{
    HMODULE User32 = GetModuleHandleW(L"user32.dll");
    PFN_Register pRegister;
    PFN_Unregister pUnregister;
    struct { PVOID Callback; PVOID Context; } Params = { PowerCallback, NULL };
    static const DWORD BadFlags[] = { 1, 3, 4, 0x80000000 };
    HWND Window;
    PVOID Handle;
    BOOL Result;
    DWORD Error;
    ULONG i;

    pRegister = (PFN_Register)GetProcAddress(User32, "RegisterSuspendResumeNotification");
    pUnregister = (PFN_Unregister)GetProcAddress(User32, "UnregisterSuspendResumeNotification");
    /* Windows 7 and older have none; ReactOS has them from export level 0x601, which a kernel32 with the Windows 7 functions shows */
    if (!pRegister || !pUnregister)
    {
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
           "the suspend and resume notification functions are not exported\n");
        skip("the suspend and resume notification functions are not exported\n");
        return;
    }

    Window = CreateWindowExW(0, L"STATIC", L"power", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    ok(Window != NULL, "CreateWindowEx failed\n");

    /* a registration for a window, ended once */
    SetLastError(PENDING_ERROR);
    Handle = pRegister(Window, 0);
    Error = GetLastError();
    ok(Handle != NULL, "no registration\n");
    ok(Error == PENDING_ERROR, "the last error became %lu\n", Error);
    if (Handle)
    {
        SetLastError(PENDING_ERROR);
        Result = pUnregister(Handle);
        Error = GetLastError();
        ok(Result == TRUE, "the registration was not ended\n");
        ok(Error == PENDING_ERROR, "the last error became %lu\n", Error);
        SetLastError(PENDING_ERROR);
        Result = pUnregister(Handle);
        Error = GetLastError();
        ok(Result == FALSE, "the registration was ended twice\n");
        ok(Error == ERROR_INVALID_PARAMETER, "ended twice: last error %lu\n", Error);
    }

    /* a callback registration takes the parameters as the recipient */
    Handle = pRegister(&Params, 2);
    ok(Handle != NULL, "no callback registration\n");
    if (Handle)
        ok(pUnregister(Handle), "the callback registration was not ended\n");

    /* the recipient is not looked at */
    Handle = pRegister(NULL, 0);
    ok(Handle != NULL, "no registration without a recipient\n");
    if (Handle)
        ok(pUnregister(Handle), "the registration without a recipient was not ended\n");
    Handle = pRegister((HANDLE)(ULONG_PTR)0x1234, 0);
    ok(Handle != NULL, "no registration for a bad window\n");
    if (Handle)
        ok(pUnregister(Handle), "the registration for a bad window was not ended\n");

    /* only the two flags are allowed */
    for (i = 0; i < RTL_NUMBER_OF(BadFlags); i++)
    {
        SetLastError(PENDING_ERROR);
        Handle = pRegister(Window, BadFlags[i]);
        Error = GetLastError();
        ok(Handle == NULL, "flags %#lx: a registration was made\n", BadFlags[i]);
        ok(Error == ERROR_INVALID_PARAMETER, "flags %#lx: last error %lu\n", BadFlags[i], Error);
        if (Handle)
            pUnregister(Handle);
    }

    /* no registration to end */
    SetLastError(PENDING_ERROR);
    Result = pUnregister(NULL);
    Error = GetLastError();
    ok(Result == FALSE, "no registration was ended\n");
    ok(Error == ERROR_INVALID_PARAMETER, "no registration: last error %lu\n", Error);

    if (Window)
        DestroyWindow(Window);
}
