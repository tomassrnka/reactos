/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Defensive contract tests for NtUserSetWindowsHookEx (T70)
 * PROGRAMMERS:     reactos-nitro security lane
 *
 * win32ss/user/ntuser/hook.c captured the caller's ModuleName UNICODE_STRING
 * and allocated/copied exactly MaximumLength bytes, but then set
 * Hook->ModuleName.Length = ModuleName.Length verbatim. A caller-supplied
 * Length greater than MaximumLength made the stored counted string claim more
 * bytes than its backing buffer, causing later counted-string uses to over-
 * read up to ~64KB of kernel pool. The fix rejects Length > MaximumLength.
 *
 * These tests use a thread-local hook on the current thread (safe: never
 * installed globally, and the kernel does not load the module here) with a
 * non-NULL Mod so the ModuleName capture path is exercised.
 */

#include "../win32nt.h"

static LRESULT CALLBACK
TestHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    return CallNextHookEx(NULL, code, wParam, lParam);
}

START_TEST(NtUserSetWindowsHookExModule)
{
    HINSTANCE hInst = (HINSTANCE)GetModuleHandleW(NULL);
    DWORD tid = GetCurrentThreadId();
    static WCHAR moduleBuf[] = L"dummy.dll";
    UNICODE_STRING mod;
    HHOOK hHook;

    /* --- Positive: a well-formed counted string (Length <= MaximumLength)
     *     still installs the hook. --- */
    mod.Buffer = moduleBuf;
    mod.MaximumLength = (USHORT)sizeof(moduleBuf);
    mod.Length = (USHORT)(sizeof(moduleBuf) - sizeof(WCHAR)); /* excl. NUL */

    hHook = NtUserSetWindowsHookEx(hInst, &mod, tid, WH_GETMESSAGE,
                                   TestHookProc, FALSE);
    TEST(hHook != NULL);
    if (hHook)
        TEST(NtUserUnhookWindowsHookEx(hHook) == TRUE);

    /* --- T70: Length > MaximumLength must be rejected. ---
     * The backing buffer is only MaximumLength bytes; a larger Length would
     * make the stored string over-read the pool. Expect a clean NULL return
     * with ERROR_INVALID_PARAMETER and no leaked hook. */
    mod.Buffer = moduleBuf;
    mod.MaximumLength = (USHORT)sizeof(moduleBuf);
    mod.Length = 0x4000; /* 16KB claimed, backing buffer is tiny */

    SetLastError(0xdeadbeef);
    hHook = NtUserSetWindowsHookEx(hInst, &mod, tid, WH_GETMESSAGE,
                                   TestHookProc, FALSE);
    TEST(hHook == NULL);
    ok(GetLastError() == ERROR_INVALID_PARAMETER,
       "expected ERROR_INVALID_PARAMETER, got %lu\n", GetLastError());

    if (hHook)
    {
        /* Should not happen on a fixed kernel; clean up if it did. */
        NtUserUnhookWindowsHookEx(hHook);
    }

    /* --- Boundary: Length == MaximumLength is still accepted. --- */
    mod.Buffer = moduleBuf;
    mod.MaximumLength = (USHORT)sizeof(moduleBuf);
    mod.Length = (USHORT)sizeof(moduleBuf);

    hHook = NtUserSetWindowsHookEx(hInst, &mod, tid, WH_GETMESSAGE,
                                   TestHookProc, FALSE);
    TEST(hHook != NULL);
    if (hHook)
        TEST(NtUserUnhookWindowsHookEx(hHook) == TRUE);
}
