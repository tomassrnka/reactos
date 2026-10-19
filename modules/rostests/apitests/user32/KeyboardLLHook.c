/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test the KBDLLHOOKSTRUCT a low-level keyboard hook receives for an injected key, from the
 *              thread that injects it and from another thread (the data is passed across then)
 */

#include "precomp.h"

#define EXTRA_MARK 0x4B100000
#define MAX_RECORDS 16

static BYTE g_record[MAX_RECORDS][sizeof(KBDLLHOOKSTRUCT)];
static LONG g_count;

static void Pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG msg;

    for (;;)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if ((LONG)(end - GetTickCount()) <= 0)
            break;
        MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);
    }
}

static LRESULT CALLBACK LowLevelProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && lParam && ((KBDLLHOOKSTRUCT *)lParam)->vkCode == VK_F24)
    {
        LONG i = InterlockedIncrement(&g_count) - 1;

        if (i < MAX_RECORDS)
            CopyMemory(g_record[i], (PVOID)lParam, sizeof(KBDLLHOOKSTRUCT));
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

static void SendKeys(DWORD extra)
{
    keybd_event(VK_F24, 0, 0, extra);
    keybd_event(VK_F24, 0, KEYEVENTF_KEYUP, extra);
}

static DWORD WINAPI SenderThread(LPVOID param)
{
    SendKeys(EXTRA_MARK + 2);
    return 0;
}

static void CheckRecords(LONG first, LONG count, DWORD extra, const char *what)
{
    LONG i;

    ok(g_count - first == count, "%s: %ld records, expected %ld\n", what, g_count - first, count);
    for (i = first; i < g_count && i < MAX_RECORDS; i++)
    {
        const KBDLLHOOKSTRUCT *s = (const KBDLLHOOKSTRUCT *)g_record[i];

        ok(s->vkCode == VK_F24, "%s: record %ld: vkCode %lx\n", what, i, s->vkCode);
        ok(s->dwExtraInfo == extra, "%s: record %ld: extra info %Ix\n", what, i, s->dwExtraInfo);
        ok(s->flags & LLKHF_INJECTED, "%s: record %ld: flags %lx\n", what, i, s->flags);
        ok(s->time != 0, "%s: record %ld: no time\n", what, i);
    }
}

START_TEST(KeyboardLLHook)
{
    HHOOK hook;
    HANDLE thread;
    LONG first;

    hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelProc, GetModuleHandleW(NULL), 0);
    ok(hook != NULL, "WH_KEYBOARD_LL hook failed, error %lu\n", GetLastError());
    if (!hook)
        return;
    Pump(100);

    /* the hook runs in this thread and so does the injection */
    first = g_count;
    SendKeys(EXTRA_MARK + 1);
    Pump(200);
    CheckRecords(first, 2, EXTRA_MARK + 1, "same thread");

    /* the injection comes from another thread, so the data is passed across */
    first = g_count;
    thread = CreateThread(NULL, 0, SenderThread, NULL, 0, NULL);
    while (WaitForSingleObject(thread, 10) == WAIT_TIMEOUT)
        Pump(10);
    CloseHandle(thread);
    Pump(200);
    CheckRecords(first, 2, EXTRA_MARK + 2, "other thread");

    UnhookWindowsHookEx(hook);
}
