/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Contract test for the hook-chain next-hook use-after-free
 *              (ASTRA-hook-nexthook-uaf). A WH_GETMESSAGE hook chain is driven
 *              and, from inside a CallNextHookEx callback, a hook is removed
 *              (the next hook, or the running hook itself). Before the fix the
 *              kernel walked the freed hook (UAF / bugcheck, EoP-class); after
 *              the fix the removed hook is simply skipped and the dispatch
 *              completes cleanly.
 *
 *              This is a fix-correctness contract test, not a weaponized PoC:
 *              it proves the freed hook is no longer dispatched and that the
 *              walk does not fault, using only documented Win32 hook APIs.
 */

#include "precomp.h"

#define WM_SENTINEL (WM_APP + 0x51)

static HHOOK g_hhkHead;
static HHOOK g_hhkTail;

static LONG g_headCalled;
static LONG g_tailCalled;
static BOOL g_armed;

/* Scenario selector */
#define SCN_UNHOOK_NEXT    1  /* head proc unhooks the TAIL (next) before chaining  */
#define SCN_UNHOOK_SELF    2  /* tail proc unhooks ITSELF while mid-callback         */
#define SCN_SELF_CONTINUE  3  /* head unhooks ITSELF then chains; tail MUST still run */
static int g_scenario;

static BOOL IsSentinel(LPARAM lParam)
{
    const MSG *pmsg = (const MSG *)lParam;
    return (pmsg != NULL && pmsg->message == WM_SENTINEL);
}

static LRESULT CALLBACK TailProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_armed && IsSentinel(lParam))
    {
        InterlockedIncrement(&g_tailCalled);

        if (g_scenario == SCN_UNHOOK_SELF)
        {
            /* Remove the hook that is currently executing. Before the fix the
             * caller (co_UserCallNextHookEx) dereferenced this freed hook after
             * the callback (Hook->HookId on the WH_GETMESSAGE path). */
            if (g_hhkTail)
            {
                UnhookWindowsHookEx(g_hhkTail);
                g_hhkTail = NULL;
            }
        }
    }
    return CallNextHookEx(g_hhkTail, code, wParam, lParam);
}

static LRESULT CALLBACK HeadProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && g_armed && IsSentinel(lParam))
    {
        InterlockedIncrement(&g_headCalled);

        if (g_scenario == SCN_UNHOOK_NEXT)
        {
            /* Remove the NEXT hook from inside this callback, then chain. Before
             * the fix NtUserCallNextHookEx followed the cached next-hook pointer
             * into freed pool and dispatched the freed hook. After the fix the
             * next link is re-derived from the live chain, so the removed hook
             * is skipped (g_tailCalled must stay 0). */
            if (g_hhkTail)
            {
                UnhookWindowsHookEx(g_hhkTail);
                g_hhkTail = NULL;
            }
        }
        else if (g_scenario == SCN_SELF_CONTINUE)
        {
            /* Remove the hook that is currently executing (self), then chain.
             * This is a common idiom. The remaining chain MUST still run: on
             * Windows and on base the downstream hook is dispatched. (A v1 that
             * cleared sphkCurrent on removal truncated the chain here -- this
             * scenario guards against that regression; it passes on base.) */
            if (g_hhkHead)
            {
                UnhookWindowsHookEx(g_hhkHead);
                g_hhkHead = NULL;
            }
        }
    }
    return CallNextHookEx(g_hhkHead, code, wParam, lParam);
}

static void PumpOne(void)
{
    MSG msg;

    /* Retrieving the posted sentinel drives the WH_GETMESSAGE chain. */
    if (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void RunScenario(int scenario, const char *name)
{
    g_scenario = scenario;
    g_headCalled = 0;
    g_tailCalled = 0;
    g_hhkHead = NULL;
    g_hhkTail = NULL;
    g_armed = FALSE;

    /* Install tail first so that the head (installed last) sits at the chain
     * head and is dispatched first; it then chains to the tail. */
    g_hhkTail = SetWindowsHookExW(WH_GETMESSAGE, TailProc, NULL, GetCurrentThreadId());
    ok(g_hhkTail != NULL, "[%s] SetWindowsHookExW(tail) failed, le=%lu\n", name, GetLastError());

    g_hhkHead = SetWindowsHookExW(WH_GETMESSAGE, HeadProc, NULL, GetCurrentThreadId());
    ok(g_hhkHead != NULL, "[%s] SetWindowsHookExW(head) failed, le=%lu\n", name, GetLastError());

    if (!g_hhkTail || !g_hhkHead)
        goto cleanup;

    g_armed = TRUE;
    PostThreadMessageW(GetCurrentThreadId(), WM_SENTINEL, 0, 0);
    PumpOne();
    g_armed = FALSE;

    /* Reaching here at all means the kernel did not bugcheck walking a freed
     * hook -- the core of the contract. */
    ok(g_headCalled == 1, "[%s] head proc called %ld times (expected 1)\n", name, g_headCalled);

    if (scenario == SCN_UNHOOK_NEXT)
    {
        /* The tail was removed mid-callback; a correct walk must NOT dispatch it.
         * Before the fix the freed tail was walked (count 1) or the box crashed. */
        ok(g_tailCalled == 0, "[%s] tail proc dispatched after being unhooked (%ld)\n", name, g_tailCalled);
    }
    else /* SCN_UNHOOK_SELF, SCN_SELF_CONTINUE: downstream hook must still run */
    {
        ok(g_tailCalled == 1, "[%s] tail proc called %ld times (expected 1)\n", name, g_tailCalled);
    }

cleanup:
    if (g_hhkHead)
    {
        UnhookWindowsHookEx(g_hhkHead);
        g_hhkHead = NULL;
    }
    if (g_hhkTail)
    {
        UnhookWindowsHookEx(g_hhkTail);
        g_hhkTail = NULL;
    }
}

START_TEST(HookChainUnhookUaf)
{
    /* UAF vector 1: remove the next hook during a CallNextHookEx callback. */
    RunScenario(SCN_UNHOOK_NEXT, "unhook-next");

    /* UAF vector 2: a hook removes itself while it is the one being dispatched
     * (co_UserCallNextHookEx post-callback dereference). */
    RunScenario(SCN_UNHOOK_SELF, "unhook-self");

    /* Functional contract (regression guard): a hook that unhooks ITSELF and
     * then calls CallNextHookEx must still run the rest of the chain. This passes
     * on base and must keep passing on the fix. */
    RunScenario(SCN_SELF_CONTINUE, "self-unhook-continue");

    /* Run the first vector twice more to exercise pool reuse of the freed hook
     * block; on an unfixed kernel this makes the stale walk fault reliably. */
    RunScenario(SCN_UNHOOK_NEXT, "unhook-next-2");
    RunScenario(SCN_UNHOOK_NEXT, "unhook-next-3");
}
