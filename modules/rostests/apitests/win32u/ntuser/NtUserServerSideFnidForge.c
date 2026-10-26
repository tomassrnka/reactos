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
        if (hSys)
        {
            LONG_PTR planted = (LONG_PTR)(ULONG_PTR)0x5151515151515151ULL;
            LONG_PTR before, after;

            SetLastError(0xdeadf00d);
            before = SetWindowLongPtrW(hSys, 0, planted);
            /* The write must be refused ... */
            ok(GetLastError() == ERROR_ACCESS_DENIED,
               "SetWindowLongPtr on #32768 extra should fail ACCESS_DENIED, err=%lu (old=%p)\n",
               GetLastError(), (void *)before);
            /* ... and must not have taken effect (the slot must not read back the
               planted value; the real kernel ppopupmenu is still there). */
            after = GetWindowLongPtrW(hSys, 0);
            ok(after != planted,
               "SECURITY: planted pointer took effect in #32768 extra bytes (read back %p)\n",
               (void *)after);

            /* A pointer could also be forged two bytes at a time via SetWindowWord. */
            SetLastError(0xdeadf00d);
            SetWindowWord(hSys, 0, 0x5252);
            ok(GetLastError() == ERROR_ACCESS_DENIED,
               "SetWindowWord on #32768 extra should fail ACCESS_DENIED, err=%lu\n",
               GetLastError());
            ok((WORD)GetWindowWord(hSys, 0) != 0x5252,
               "SECURITY: planted word took effect in #32768 extra bytes\n");

            /* With the write refused the real ppopupmenu is intact, so teardown
               (WM_NCDESTROY frees the genuine context) is safe. */
            ok(DestroyWindow(hSys), "DestroyWindow(#32768) failed, err %lu\n", GetLastError());
        }
        else
        {
            trace("CreateWindowExW(#32768) not permitted from user mode (err %lu); "
                  "genuine-system-class extra-write hole not reachable\n", GetLastError());
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
