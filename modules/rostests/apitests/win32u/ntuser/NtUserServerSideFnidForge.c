/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ASTRA8-FU contract test: user mode must not be able to get an
 *              arbitrary (user-class) window treated as a server-side system
 *              window, which would run DesktopWindowProc / PopupMenuWndProc /
 *              UserMessageWindowProc on a mismatched class -> type-confusion /
 *              OOB write past the WND.
 *
 *              NtUserMessageCall(dwType=FNID_*) lets a caller choose BOTH the
 *              target window AND which server-side wndproc runs. Three earlier
 *              per-window/class discriminators were all user-forgeable:
 *                - the instance fnid (accepted unchecked),
 *                - the class fnid (NtUserRegisterClassExWOW stored a caller fnID),
 *                - CSF_SERVERSIDEPROC (auto-granted when lpfnWndProc matched the
 *                  public client-proc table, e.g. DefWindowProcW).
 *              The fix gates every route on CSF_SYSTEMCLASS, set only by
 *              UserRegisterSystemClasses and impossible to set from user mode
 *              (NtUserRegisterClassExWOW masks Flags to CSF_ANSIPROC), plus a
 *              class-fnid==dwType match. This test drives each forge route and
 *              asserts rejection without a bugcheck; positive controls confirm
 *              that legitimate menus / message windows / normal windows still work.
 *
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "../win32nt.h"

static const WCHAR szForgeClass[]  = L"Astra8FuForgeClass";
static const WCHAR szServerProc[]  = L"Astra8FuServerProcClass";

/* Register a class directly through the syscall so we can supply an arbitrary
   fnID, exactly as user32's RegisterClassExWOWW does. Returns the atom (0 on
   rejection). */
static ATOM
RegisterClassWithFnid(LPCWSTR pszName, WNDPROC proc, int cbWndExtra, DWORD fnID)
{
    WNDCLASSEXW wc;
    UNICODE_STRING ClassName, ClassVersion, MenuName;
    CLSMENUNAME clsMenuName;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = 0;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.cbWndExtra = cbWndExtra;
    wc.lpszClassName = pszName;

    RtlInitUnicodeString(&ClassName, pszName);
    ClassVersion = ClassName;
    MenuName.Buffer = NULL;      /* no menu: Length 0 + NULL buffer passes the
                                    NtUserRegisterClassExWOW menu-name check */
    MenuName.Length = 0;
    MenuName.MaximumLength = 0;

    clsMenuName.pszClientAnsiMenuName = NULL;
    clsMenuName.pwszClientUnicodeMenuName = NULL;
    clsMenuName.pusMenuName = &MenuName;

    return (ATOM)NtUserRegisterClassExWOW(&wc,
                                          &ClassName,
                                          &ClassVersion,
                                          &clsMenuName,
                                          fnID,
                                          0,
                                          NULL);
}

/* A user-mode write of a positive extra-byte index on a genuine #32768 window
   must fail with ERROR_ACCESS_DENIED, return 0, and leave the slot unchanged. */
static void
CheckRefusedPtr(HWND hWnd, int index, LONG_PTR planted, BOOL ansi)
{
    LONG_PTR before, ret, after;
    DWORD err;

    before = GetWindowLongPtrW(hWnd, index);
    ok(before != planted, "slot %d already equals planted value\n", index);

    SetLastError(0xdeadf00d);
    ret = ansi ? SetWindowLongPtrA(hWnd, index, planted)
               : SetWindowLongPtrW(hWnd, index, planted);
    err = GetLastError();
    ok(err == ERROR_ACCESS_DENIED,
       "SetWindowLongPtr%c(#32768, %d) should fail ACCESS_DENIED, err=%lu\n",
       ansi ? 'A' : 'W', index, err);
    ok(ret == 0, "SetWindowLongPtr%c(#32768, %d) returned %p, expected 0\n",
       ansi ? 'A' : 'W', index, (void *)ret);

    after = GetWindowLongPtrW(hWnd, index);
    ok(after == before,
       "SECURITY: SetWindowLongPtr%c(#32768, %d) changed the slot (%p -> %p)\n",
       ansi ? 'A' : 'W', index, (void *)before, (void *)after);
}

static void
CheckRefusedLong(HWND hWnd, int index, BOOL ansi)
{
    LONG before, planted, ret, after;
    DWORD err;

    before = GetWindowLongW(hWnd, index);
    planted = before ^ 0x51515151;   /* guaranteed different from the current value */

    SetLastError(0xdeadf00d);
    ret = ansi ? SetWindowLongA(hWnd, index, planted)
               : SetWindowLongW(hWnd, index, planted);
    err = GetLastError();
    ok(err == ERROR_ACCESS_DENIED,
       "SetWindowLong%c(#32768, %d) should fail ACCESS_DENIED, err=%lu\n",
       ansi ? 'A' : 'W', index, err);
    ok(ret == 0, "SetWindowLong%c(#32768, %d) returned 0x%lx, expected 0\n",
       ansi ? 'A' : 'W', index, ret);

    after = GetWindowLongW(hWnd, index);
    ok(after == before,
       "SECURITY: SetWindowLong%c(#32768, %d) changed the slot (0x%lx -> 0x%lx)\n",
       ansi ? 'A' : 'W', index, before, after);
}

static void
CheckRefusedWord(HWND hWnd, int index)
{
    WORD before, after, ret;
    DWORD err;

    before = GetWindowWord(hWnd, index);
    SetLastError(0xdeadf00d);
    ret = SetWindowWord(hWnd, index, (WORD)(before ^ 0x5252));
    err = GetLastError();
    ok(err == ERROR_ACCESS_DENIED,
       "SetWindowWord(#32768, %d) should fail ACCESS_DENIED, err=%lu\n", index, err);
    ok(ret == 0, "SetWindowWord(#32768, %d) returned 0x%x, expected 0\n", index, ret);
    after = GetWindowWord(hWnd, index);
    ok(after == before,
       "SECURITY: SetWindowWord(#32768, %d) changed the slot (0x%x -> 0x%x)\n",
       index, before, after);
}

/* Negative (non-extra-byte) indices on a genuine #32768 window must SUCCEED. */
static void
CheckAllowedNegativeIndex(HWND hWnd, BOOL ansi)
{
    LONG_PTR userdata = (LONG_PTR)(ULONG_PTR)0x1234ABCD;
    LONG style, newstyle;
    DWORD err;

    /* GWLP_USERDATA */
    SetLastError(0xdeadf00d);
    if (ansi)
        SetWindowLongPtrA(hWnd, GWLP_USERDATA, userdata);
    else
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, userdata);
    err = GetLastError();
    ok(err != ERROR_ACCESS_DENIED,
       "SetWindowLongPtr%c(#32768, GWLP_USERDATA) wrongly refused, err=%lu\n",
       ansi ? 'A' : 'W', err);
    ok(GetWindowLongPtrW(hWnd, GWLP_USERDATA) == userdata,
       "SetWindowLongPtr%c(#32768, GWLP_USERDATA) did not take effect (read back %p)\n",
       ansi ? 'A' : 'W', (void *)GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);

    /* GWL_STYLE: toggle a harmless bit, check it took, then restore. */
    style = GetWindowLongW(hWnd, GWL_STYLE);
    newstyle = style ^ WS_BORDER;
    SetLastError(0xdeadf00d);
    if (ansi)
        SetWindowLongA(hWnd, GWL_STYLE, newstyle);
    else
        SetWindowLongW(hWnd, GWL_STYLE, newstyle);
    err = GetLastError();
    ok(err != ERROR_ACCESS_DENIED,
       "SetWindowLong%c(#32768, GWL_STYLE) wrongly refused, err=%lu\n",
       ansi ? 'A' : 'W', err);
    ok((GetWindowLongW(hWnd, GWL_STYLE) & WS_BORDER) == (newstyle & WS_BORDER),
       "SetWindowLong%c(#32768, GWL_STYLE) did not take effect (style 0x%lx -> 0x%lx, wanted 0x%lx)\n",
       ansi ? 'A' : 'W', style, GetWindowLongW(hWnd, GWL_STYLE), newstyle);
    SetWindowLongW(hWnd, GWL_STYLE, style);
}

START_TEST(NtUserServerSideFnidForge)
{
    HINSTANCE hinst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    ATOM atom;
    HWND hWnd, hMsgWnd;
    BOOL bRet;
    ULONG_PTR Result;
    MSG msg;

    /* ===== Forge route 1: NtUserRegisterClassExWOW with a server-side fnID ===== */
    /* The syscall must reject server-side class fnIDs (FNID_FIRST..FNID_SWITCH)
       from user mode. These classes are kernel-only (UserRegisterSystemClasses);
       user32 only ever registers control classes (FNID_BUTTON and above). */
    atom = RegisterClassWithFnid(L"Astra8FuMenuFnid", DefWindowProcW,
                                 sizeof(PVOID), FNID_MENU);
    ok(atom == 0, "RegisterClassExWOW(fnID=FNID_MENU) should be rejected, atom=0x%x\n", atom);

    atom = RegisterClassWithFnid(L"Astra8FuDeskFnid", DefWindowProcW,
                                 sizeof(PVOID), FNID_DESKTOP);
    ok(atom == 0, "RegisterClassExWOW(fnID=FNID_DESKTOP) should be rejected, atom=0x%x\n", atom);

    atom = RegisterClassWithFnid(L"Astra8FuMsgFnid", DefWindowProcW,
                                 sizeof(PVOID), FNID_MESSAGEWND);
    ok(atom == 0, "RegisterClassExWOW(fnID=FNID_MESSAGEWND) should be rejected, atom=0x%x\n", atom);

    /* Positive control: a user class whose wndproc is DefWindowProcW still
       registers normally (fnID==0). The kernel auto-sets CSF_SERVERSIDEPROC for
       it (DefWindowProcW matches the client-proc table) - the old forgeable
       discriminator - but the class is still NOT a system class, as the FNID_MENU
       dispatch on its window proves below. */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hinst;
    wc.cbWndExtra = sizeof(PVOID);
    wc.lpszClassName = szServerProc;
    atom = RegisterClassExW(&wc);
    ok(atom != 0, "RegisterClassExW(DefWindowProcW) should succeed, err %lu\n", GetLastError());

    /* ===== Build a plain user window to attack ===== */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hinst;
    wc.cbWndExtra = sizeof(PVOID);   /* enough room to tempt the OOB-store path */
    wc.lpszClassName = szForgeClass;
    atom = RegisterClassExW(&wc);
    ok(atom != 0, "RegisterClassExW(forge class) failed, err %lu\n", GetLastError());

    hWnd = CreateWindowExW(0, szForgeClass, L"forge", WS_OVERLAPPEDWINDOW,
                           0, 0, 50, 30, NULL, NULL, hinst, NULL);
    ok(hWnd != NULL, "CreateWindowExW(forge class) failed, err %lu\n", GetLastError());

    if (hWnd)
    {
        /* ===== Forge route 2: NtUserSetWindowFNID with a server-side fnID ===== */
        /* Flipping a user window to FNID_MENU/FNID_DESKTOP would route it into the
           kernel server procs. Only a genuine system-class window may carry one. */
        SetLastError(0xdeadf00d);
        bRet = NtUserSetWindowFNID(hWnd, FNID_MENU);
        ok(bRet == FALSE, "NtUserSetWindowFNID(FNID_MENU) on user window should fail, got %d\n", bRet);

        bRet = NtUserSetWindowFNID(hWnd, FNID_DESKTOP);
        ok(bRet == FALSE, "NtUserSetWindowFNID(FNID_DESKTOP) on user window should fail, got %d\n", bRet);

        bRet = NtUserSetWindowFNID(hWnd, FNID_MESSAGEWND);
        ok(bRet == FALSE, "NtUserSetWindowFNID(FNID_MESSAGEWND) on user window should fail, got %d\n", bRet);

        /* ===== Forge route 3: SetClassLongPtr(GCLP_WNDPROC) to a server proc =====
           Re-pointing the class wndproc at DefWindowProcW re-triggers the kernel's
           CSF_SERVERSIDEPROC auto-detect. It must still not grant server-side
           treatment (no CSF_SYSTEMCLASS). */
        SetClassLongPtrW(hWnd, GCLP_WNDPROC, (LONG_PTR)DefWindowProcW);

        /* ===== The dispatch hatch: NtUserMessageCall(dwType=FNID_*) =====
           Each of these would, on the unfixed kernel, run the server proc on this
           user-class window. The strongest proof is the absence of a bugcheck. */

        /* (a) FNID_MENU WM_NCCREATE would flip the window to FNID_MENU and store a
               POPUPMENU pointer past the WND; must be rejected. */
        SetLastError(0xdeadf00d);
        Result = 0;
        bRet = NtUserMessageCall(hWnd, WM_NCCREATE, 0, 0,
                                 (ULONG_PTR)&Result, FNID_MENU, FALSE);
        ok(bRet == FALSE, "NtUserMessageCall(FNID_MENU, WM_NCCREATE) should be rejected, got %d\n", bRet);

        /* (b) Plant a fake "ppopupmenu" in the window extra bytes. On the unfixed
               kernel the WM_NCCREATE above already flipped the window to FNID_MENU,
               so this FNID_MENU/WM_CREATE dispatch takes the else-branch and reads
               the planted pointer out of the extra bytes as a POPUPMENU* (type
               confusion); with a crafted non-NULL CREATESTRUCT it becomes an
               arbitrary kernel write via pPopupMenu->spmenu. We deliberately pass
               lParam==0 (contract test, not a weaponised PoC): the assertion is
               simply that the gate refuses the dispatch, which also prevents the
               window from ever being flipped in the first place. */
        SetWindowLongPtrW(hWnd, 0, (LONG_PTR)(ULONG_PTR)0x4142434445464748ULL);
        Result = 0;
        bRet = NtUserMessageCall(hWnd, WM_CREATE, 0, 0,
                                 (ULONG_PTR)&Result, FNID_MENU, FALSE);
        ok(bRet == FALSE, "NtUserMessageCall(FNID_MENU, WM_CREATE) should be rejected, got %d\n", bRet);

        /* (c) FNID_DESKTOP / FNID_MESSAGEWND on the user window must also be refused. */
        Result = 0;
        bRet = NtUserMessageCall(hWnd, WM_NCCREATE, 0, 0,
                                 (ULONG_PTR)&Result, FNID_DESKTOP, FALSE);
        ok(bRet == FALSE, "NtUserMessageCall(FNID_DESKTOP) should be rejected, got %d\n", bRet);

        Result = 0;
        bRet = NtUserMessageCall(hWnd, WM_NCCREATE, 0, 0,
                                 (ULONG_PTR)&Result, FNID_MESSAGEWND, FALSE);
        ok(bRet == FALSE, "NtUserMessageCall(FNID_MESSAGEWND) should be rejected, got %d\n", bRet);

        /* The window must still be an ordinary, working window after all attacks. */
        ok(IsWindow(hWnd) == TRUE, "forge window destroyed by a rejected dispatch\n");
        bRet = SetWindowPos(hWnd, NULL, 7, 9, 40, 20, SWP_NOZORDER | SWP_NOACTIVATE);
        ok(bRet == TRUE, "normal window op broken after forge attempts, err %lu\n", GetLastError());

        DestroyWindow(hWnd);
    }

    /* ===== Forge route via the CSF_SERVERSIDEPROC class, exercised as a window ===== */
    hWnd = CreateWindowExW(0, szServerProc, L"srv", WS_OVERLAPPEDWINDOW,
                           0, 0, 50, 30, NULL, NULL, hinst, NULL);
    ok(hWnd != NULL, "CreateWindowExW(server-proc class) failed, err %lu\n", GetLastError());
    if (hWnd)
    {
        /* CSF_SERVERSIDEPROC is set on this class (DefWindowProcW matched the
           client table), the old bypassed discriminator - but it is not a system
           class, so FNID_MENU dispatch must be refused. */
        Result = 0;
        bRet = NtUserMessageCall(hWnd, WM_NCCREATE, 0, 0,
                                 (ULONG_PTR)&Result, FNID_MENU, FALSE);
        ok(bRet == FALSE,
           "NtUserMessageCall(FNID_MENU) on a CSF_SERVERSIDEPROC user class should be rejected, got %d\n",
           bRet);
        bRet = NtUserSetWindowFNID(hWnd, FNID_MENU);
        ok(bRet == FALSE,
           "NtUserSetWindowFNID(FNID_MENU) on a CSF_SERVERSIDEPROC user class should fail, got %d\n",
           bRet);
        DestroyWindow(hWnd);
    }

    /* ===== The genuine-system-class exploit (ASTRA8-FU follow-up hole) =====
       A process may legitimately OWN a real WC_MENU (#32768) window. It passes
       every class gate above (CSF_SYSTEMCLASS, pcls->fnid==FNID_MENU), and the
       kernel stores a real PPOPUPMENU in its extra bytes at offset 0. The extra
       bytes must still be off-limits to user-mode writes: otherwise the owner
       overwrites that pointer (SetWindowLongPtr / SetWindowWord) and a later menu
       message dereferences/frees the planted value (PopupMenuWndProc MN_SETHMENU
       writes pPopupMenu->spmenu; WM_NCDESTROY frees it) -> arbitrary kernel
       write/free. The fix refuses user writes to a system class's extra bytes. */
    {
        HWND hSys = CreateWindowExW(0, L"#32768", L"sys", WS_POPUP,
                                    0, 0, 10, 10, NULL, NULL, hinst, NULL);
        /* Assert (not trace-skip) creation: if a process may not own a #32768
           window this case would otherwise pass vacuously and hide a regression
           of the very hole it exists to cover. */
        ok(hSys != NULL, "CreateWindowExW(#32768) failed, err %lu\n", GetLastError());
        if (hSys)
        {
            LONG_PTR planted = (LONG_PTR)(ULONG_PTR)0x5151515151515151ULL;
            LONG_PTR slot0;

            /* The kernel stored its real ppopupmenu at extra offset 0: it must be
               a non-zero value that is not our planted pattern, otherwise the
               "write did not take effect" checks below could pass vacuously. */
            slot0 = GetWindowLongPtrW(hSys, 0);
            ok(slot0 != 0, "#32768 extra[0] (ppopupmenu) unexpectedly zero\n");
            ok(slot0 != planted, "#32768 extra[0] already equals the planted value\n");

            /* Positive extra-byte indices are refused for every write flavour
               (SetWindowLongPtr W/A, SetWindowLong W/A, SetWindowWord), at offset
               0 (the ppopupmenu slot) and at a positive offset > 0. */
            CheckRefusedPtr(hSys, 0, planted, FALSE);
            CheckRefusedPtr(hSys, 0, planted, TRUE);
            CheckRefusedPtr(hSys, 8, planted, FALSE);
            CheckRefusedPtr(hSys, 8, planted, TRUE);
            CheckRefusedLong(hSys, 0, FALSE);
            CheckRefusedLong(hSys, 0, TRUE);
            CheckRefusedLong(hSys, 4, FALSE);
            CheckRefusedLong(hSys, 4, TRUE);
            CheckRefusedWord(hSys, 0);
            CheckRefusedWord(hSys, 8);

            /* The ppopupmenu slot survived every refused write bit-for-bit. */
            ok(GetWindowLongPtrW(hSys, 0) == slot0,
               "SECURITY: #32768 extra[0] changed (%p -> %p)\n",
               (void *)slot0, (void *)GetWindowLongPtrW(hSys, 0));

            /* NON-REGRESSION: the guard is positive-extra-byte-index ONLY. The
               negative GWL_* / GWLP_* indices on the very same genuine #32768
               window must keep working; this catches a future false positive if
               someone broadens the guard to the whole window. */
            CheckAllowedNegativeIndex(hSys, FALSE);
            CheckAllowedNegativeIndex(hSys, TRUE);

            /* ... and none of that disturbed the kernel's slot either. */
            ok(GetWindowLongPtrW(hSys, 0) == slot0,
               "SECURITY: #32768 extra[0] changed by negative-index writes (%p -> %p)\n",
               (void *)slot0, (void *)GetWindowLongPtrW(hSys, 0));

            /* With the writes refused the real ppopupmenu is intact, so teardown
               (WM_NCDESTROY frees the genuine context) is safe. */
            ok(DestroyWindow(hSys), "DestroyWindow(#32768) failed, err %lu\n", GetLastError());
        }
    }

    /* ===== Positive controls: legitimate server-side classes still work ===== */

    /* Menus: creation + item management still function (menu windows ARE the
       WC_MENU system class and pass the gate at WM_NCCREATE with instance fnid 0). */
    {
        HMENU hMenu = CreatePopupMenu();
        ok(hMenu != NULL, "CreatePopupMenu failed, err %lu\n", GetLastError());
        if (hMenu)
        {
            MENUITEMINFOW mii;
            ZeroMemory(&mii, sizeof(mii));
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_ID | MIIM_STRING;
            mii.wID = 1;
            mii.dwTypeData = (LPWSTR)L"Item0";
            ok(InsertMenuItemW(hMenu, 0, TRUE, &mii), "InsertMenuItemW failed, err %lu\n", GetLastError());
            ok(GetMenuItemCount(hMenu) == 1, "unexpected menu item count %d\n", GetMenuItemCount(hMenu));
            DestroyMenu(hMenu);
        }
    }

    /* The system popup-menu class is intact and still reserves its extra bytes. */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    if (GetClassInfoExW(NULL, L"#32768", &wc))
    {
        ok(wc.cbWndExtra >= (int)sizeof(PVOID),
           "system menu class cbWndExtra=%d too small\n", wc.cbWndExtra);
    }

    /* A message-only window (HWND_MESSAGE child of a user class) still works. */
    hMsgWnd = CreateWindowExW(0, szForgeClass, L"msg", 0,
                              0, 0, 0, 0, HWND_MESSAGE, NULL, hinst, NULL);
    if (hMsgWnd)
    {
        LRESULT lr = SendMessageW(hMsgWnd, WM_USER + 1, 0, 0);
        (void)lr; /* DefWindowProc handles it; just prove no crash. */
        ok(IsWindow(hMsgWnd), "message-only window unexpectedly gone\n");
        DestroyWindow(hMsgWnd);
    }
    else
    {
        trace("message-only window creation not available (err %lu)\n", GetLastError());
    }

    /* Drain any stray messages. */
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { /* discard */ }

    UnregisterClassW(szForgeClass, hinst);
    UnregisterClassW(szServerProc, hinst);
}
