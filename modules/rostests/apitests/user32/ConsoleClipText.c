/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Contract test: console paste must stay inside the clipboard block
 */

#include "precomp.h"

#include "../../../../win32ss/user/winsrv/consrv/frontends/gui/cliptext.h"

START_TEST(ConsoleClipText)
{
    static const WCHAR text[] = L"abcd";
    SYSTEM_INFO si;
    PBYTE pBase;
    DWORD dwOld, cb;
    PWCHAR p;
    HGLOBAL hMem;
    HANDLE hGot;
    PWCHAR pGot;

    /* Bounded cases on an ordinary buffer */
    ok_long((LONG)ConGetBoundedTextLength(L"abc", 4 * sizeof(WCHAR)), 3);
    ok_long((LONG)ConGetBoundedTextLength(L"abc", 2 * sizeof(WCHAR)), 2);
    ok_long((LONG)ConGetBoundedTextLength(L"abc", 1), 0);
    ok_long((LONG)ConGetBoundedTextLength(L"abc", 0), 0);

    /* Unterminated text flush against an inaccessible page: the helper must not touch it */
    GetSystemInfo(&si);
    pBase = VirtualAlloc(NULL, 2 * si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!pBase)
    {
        skip("VirtualAlloc failed\n");
        return;
    }
    VirtualProtect(pBase + si.dwPageSize, si.dwPageSize, PAGE_NOACCESS, &dwOld);
    cb = sizeof(text) - sizeof(WCHAR);  /* no terminator */
    p = (PWCHAR)(pBase + si.dwPageSize - cb);
    memcpy(p, text, cb);
    ok_long((LONG)ConGetBoundedTextLength(p, cb), 4);
    VirtualFree(pBase, 0, MEM_RELEASE);

    /* The clipboard preserves an unterminated block as-is (so consumers must bound it) */
    hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, cb);
    if (!hMem || !OpenClipboard(NULL))
    {
        skip("clipboard unavailable\n");
        if (hMem) GlobalFree(hMem);
        return;
    }
    pGot = GlobalLock(hMem);
    memcpy(pGot, text, cb);
    GlobalUnlock(hMem);
    EmptyClipboard();
    ok(SetClipboardData(CF_UNICODETEXT, hMem) != NULL, "SetClipboardData failed\n");
    hGot = GetClipboardData(CF_UNICODETEXT);
    ok(hGot != NULL, "GetClipboardData failed\n");
    if (hGot)
    {
        pGot = GlobalLock(hGot);
        ok_long((LONG)ConGetBoundedTextLength(pGot, GlobalSize(hGot)), 4);
        GlobalUnlock(hGot);
    }
    EmptyClipboard();
    CloseClipboard();
}
