/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Astra audit #8 - NtUserGetWOWClass returned the raw PCLS it
 *              looked up, i.e. a kernel VA into the desktop/user heap, straight
 *              to user mode (kernel-pointer info-leak, defeats KASLR). The fix
 *              converts it to the caller's read-only user-mode view of the heap
 *              (or NULL). This test asserts the returned value is never a
 *              kernel-space pointer, while the class lookup still resolves.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "../win32nt.h"

#ifdef _WIN64
#define USER_ADDR_LIMIT ((ULONG_PTR)0x0000800000000000ULL)
#else
#define USER_ADDR_LIMIT ((ULONG_PTR)0x80000000UL)
#endif

static LRESULT CALLBACK
Astra8WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(hWnd, msg, wp, lp);
}

START_TEST(NtUserGetWOWClassPtr)
{
    HINSTANCE hinst = GetModuleHandle(NULL);
    WNDCLASSEXW wc;
    UNICODE_STRING name;
    ATOM atom;
    PCLS cls;
    static const WCHAR szClass[] = L"Astra8WowClass";

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Astra8WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = szClass;
    atom = RegisterClassExW(&wc);
    ok(atom != 0, "RegisterClassExW failed, error %lu\n", GetLastError());
    if (!atom)
        return;

    RtlInitUnicodeString(&name, szClass);

    /* --- Lookup resolves (positive) --- */
    SetLastError(0);
    cls = NtUserGetWOWClass(hinst, &name);
    /*
     * A registered class must resolve to a non-NULL class object view; however
     * the hard security contract is simply: the returned pointer must NOT be a
     * kernel-space address. (NULL is acceptable - it means "not exposed" - and
     * is still safe.)
     */
    ok(!((ULONG_PTR)cls >= USER_ADDR_LIMIT),
       "SECURITY(Astra#8): NtUserGetWOWClass returned a kernel pointer %p "
       "(raw desktop-heap VA leaked to user mode)\n", (PVOID)cls);
    ok(cls != NULL,
       "NtUserGetWOWClass did not resolve a registered class (got NULL)\n");

    /* --- Negative: unknown class -> NULL, no kernel pointer --- */
    {
        UNICODE_STRING bogus;
        RtlInitUnicodeString(&bogus, L"Astra8NoSuchClass");
        cls = NtUserGetWOWClass(hinst, &bogus);
        ok(cls == NULL,
           "NtUserGetWOWClass(unknown) should return NULL, got %p\n", (PVOID)cls);
    }

    UnregisterClassW(szClass, hinst);
}
