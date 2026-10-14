/* SPDX-License-Identifier: GPL-2.0-or-later
 * Compile real user32 readers into the unit fixture. Discard unused functions
 * at link time; no guest DLL initialization or callback support is simulated. */
#include <apitest.h>
#include <user32.h>
#include "GuestState.h"
#include "../../../../win32ss/user/user32/misc/misc.c"

SHAREDINFO gSharedInfo;
PSERVERINFO gpsi;
PUSER_HANDLE_TABLE gHandleTable;
PUSER_HANDLE_ENTRY gHandleEntries;
PFNCLIENT pfnClientA, pfnClientW;

void GuestStateUser(PTEB64 Teb, PVOID Connect)
{
    PCLIENTINFO Native = (PCLIENTINFO)Teb->Win32ClientInfo;
    PCLIENTINFO Client = GetWin32ClientInfo();
    PUSERCONNECT Connection = Connect;
    PFNCLIENTWORKER Worker = {0};
    ULONGLONG Args[6] = { (ULONG_PTR)&pfnClientA, (ULONG_PTR)&pfnClientW,
                         (ULONG_PTR)&Worker, 0x31000000 };
    ULONG i, Status;
    PWND Window;
    WINDOWPLACEMENT Placement = { sizeof(Placement) };
    RECT Rect;
    PVOID SavedGuestThread = NtCurrentTeb()->Win32ThreadInfo;

    gSharedInfo = Connection->siClient;
    gpsi = gSharedInfo.psi;
    gHandleTable = gSharedInfo.aheList;
    gHandleEntries = SharedPtrToUser(gHandleTable->handles);

    ok(Client == Native, "user32 CLIENTINFO %p, native %p\n", Client, Native);
    ok(GuestStateImmClient() == Native, "imm32 reads the wrong CLIENTINFO\n");
    /* A stale, nonzero guest copy must not hide the kernel's native identity.
     * This also keeps the pre-fix control on its read path. */
    NtCurrentTeb()->Win32ThreadInfo = (PVOID)0x12340000;
    ok((ULONG_PTR)GetW32ThreadInfo() == (ULONG_PTR)Teb->Win32ThreadInfo,
       "user32 reads the wrong Win32ThreadInfo\n");
    ok(GuestStateImmThread() == (ULONG_PTR)Teb->Win32ThreadInfo,
       "imm32 reads the wrong Win32ThreadInfo\n");
    NtCurrentTeb()->Win32ThreadInfo = SavedGuestThread;
    ok(Native->pDeskInfo != NULL && Native->pClientThreadInfo != NULL,
       "kernel did not publish desktop/client-thread state\n");
    ok(Native->ppi_K64 != 0, "kernel did not publish process identity\n");
    ok(Client->dwTIFlags == Native->dwTIFlags, "dwTIFlags differs\n");
    ok(Client->dwExpWinVer == Native->dwExpWinVer, "dwExpWinVer differs\n");
    ok(Client->hKL_K64 == Native->hKL_K64, "keyboard layout differs\n");
    ok(Client->CodePage == Native->CodePage, "code page differs\n");
    ok(Client->pDeskInfo_K64 == Native->pDeskInfo_K64, "desktop pointer differs\n");
    ok(Client->ulClientDelta_K64 == Native->ulClientDelta_K64, "desktop delta differs\n");
    ok(Client->pClientThreadInfo_K64 == Native->pClientThreadInfo_K64,
       "client-thread pointer differs\n");
    ok(Client->ppi_K64 == Native->ppi_K64, "process identity differs\n");

    /* The first seven entries are the server-to-client mapping too. Native
     * SERVERINFO has different addresses and must never serve this lookup. */
    for (i = 0; i < sizeof(pfnClientA) / sizeof(ULONGLONG); ++i)
    {
        ((ULONGLONG *)&pfnClientA)[i] = 0x21000000 + i * 0x10;
        ((ULONGLONG *)&pfnClientW)[i] = 0x22000000 + i * 0x10;
    }
    Status = GuestStateCall(Native_NtUserInitializeClientPfnArrays, Args);
    ok(Status == 0, "guest procedure registration: %lx\n", Status);
    for (i = 0; i < sizeof(pfnClientA) / sizeof(ULONGLONG); ++i)
    {
        ok((ULONG_PTR)GETPFNCLIENTA(FNID_FIRST + i) == 0x21000000 + i * 0x10,
           "ANSI procedure %lu came from the native table\n", i);
        ok((ULONG_PTR)GETPFNCLIENTW(FNID_FIRST + i) == 0x22000000 + i * 0x10,
           "Unicode procedure %lu came from the native table\n", i);
        if (i < FNID_NUMSERVERPROC)
            ok(GETPFNSERVER(FNID_FIRST + i) == GETPFNCLIENTW(FNID_FIRST + i),
               "server procedure %lu does not use the guest W table\n", i);
    }

    /* Avoid following bogus pointers on the pre-fix control. Failures above
     * remain failures, and the GDI tests still run. */
    if (Client != Native || !Native->pDeskInfo) return;
    ok(GetThreadDesktopInfo() == Native->pDeskInfo, "DESKTOPINFO differs\n");
    Window = GetThreadDesktopWnd();
    ok(Window != NULL, "no desktop WND\n");
    if (!Window) return;
    ok(ValidateHwnd(Window->head.h) == Window, "handle-table and desktop WND reads differ\n");
    Rect = Window->rcWindow;
    ZeroMemory(Args, sizeof(Args));
    Args[0] = (LONGLONG)(LONG)(ULONG_PTR)Window->head.h;
    Args[1] = (ULONG_PTR)&Placement;
    Status = GuestStateCall(Native_NtUserGetWindowPlacement, Args);
    ok(Status != 0, "native GetWindowPlacement failed\n");
    ok(!memcmp(&Rect, &Placement.rcNormalPosition, sizeof(Rect)),
       "guest WND rect %ld,%ld,%ld,%ld, native %ld,%ld,%ld,%ld\n",
       Rect.left, Rect.top, Rect.right, Rect.bottom, Placement.rcNormalPosition.left,
       Placement.rcNormalPosition.top, Placement.rcNormalPosition.right, Placement.rcNormalPosition.bottom);
}
