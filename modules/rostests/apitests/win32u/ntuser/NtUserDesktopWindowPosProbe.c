/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ASTRA8-FU contract test: DesktopWindowProc must not dereference an
 *              unprobed user pointer for WM_WINDOWPOSCHANGING. A bad lParam must
 *              fail gracefully instead of faulting in the kernel.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "../win32nt.h"

START_TEST(NtUserDesktopWindowPosProbe)
{
    HWND hDesktop = GetDesktopWindow();
    WINDOWPOS Pos;
    ULONG_PTR Result;
    BOOL bRet;
    UINT i;
    static const ULONG_PTR BadPointers[] =
    {
        1,
        0x10,
        0xdeadbee0,
#ifdef _WIN64
        0x00007ffffffffff0ULL,
        0xffff800000001000ULL,
#else
        0x7ffffff0,
        0x80001000,
        0xfffffff0,
#endif
    };

    ok(hDesktop != NULL, "GetDesktopWindow failed\n");
    if (!hDesktop)
        return;

    /* Positive control: a valid, harmless WINDOWPOS is accepted */
    ZeroMemory(&Pos, sizeof(Pos));
    Pos.hwnd = hDesktop;
    Result = 0;
    bRet = NtUserMessageCall(hDesktop, WM_WINDOWPOSCHANGING, 0, (LPARAM)&Pos,
                             (ULONG_PTR)&Result, FNID_DESKTOP, FALSE);
    ok(bRet != FALSE, "valid WM_WINDOWPOSCHANGING failed\n");

    /* Bad pointers: must not fault the kernel. The result value is not
     * asserted, reaching the next line (no bugcheck) is the contract. */
    for (i = 0; i < _countof(BadPointers); i++)
    {
        Result = 0;
        bRet = NtUserMessageCall(hDesktop, WM_WINDOWPOSCHANGING, 0, (LPARAM)BadPointers[i],
                                 (ULONG_PTR)&Result, FNID_DESKTOP, FALSE);
        ok(TRUE, "survived bad lParam %p (ret %d)\n", (PVOID)BadPointers[i], bRet);
    }

    /* The same through the ordinary send path */
    for (i = 0; i < _countof(BadPointers); i++)
    {
        SendMessageTimeoutW(hDesktop, WM_WINDOWPOSCHANGING, 0, (LPARAM)BadPointers[i],
                            SMTO_ABORTIFHUNG, 1000, &Result);
        ok(TRUE, "survived SendMessageTimeout with lParam %p\n", (PVOID)BadPointers[i]);
    }
}
