/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that the WH_MOUSE data a global hook module receives for a window owned by
 *              another thread is complete: mouseData is the wheel or button value and the
 *              bytes of the structure that no field covers are zero
 */

#include "precomp.h"
#include <winreg.h>
#include "mousehook/mousehook.h"

#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif
#ifndef MOUSEEVENTF_HWHEEL
#define MOUSEEVENTF_HWHEEL 0x1000
#endif

#define EXTRA_MARK 0x7B100000

static HWND g_hwnd;
static HANDLE g_ready, g_stop;

static BOOL running_on_reactos(void)
{
    HKEY key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ReactOS\\ReactOS", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return FALSE;
    RegCloseKey(key);
    return TRUE;
}

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

static DWORD WINAPI WindowThread(LPVOID param)
{
    MSG msg;

    g_hwnd = CreateWindowW(L"MouseHookTransportWnd", L"hook", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 100, 300, 300, NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    SetEvent(g_ready);
    while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(1, &g_stop, FALSE, 20, QS_ALLINPUT);
    }
    DestroyWindow(g_hwnd);
    return 0;
}

static void CheckRecord(const MOUSEHOOK_RECORD *r, int index, DWORD expectedData, BOOL known)
{
    BYTE covered[sizeof(MOUSEHOOKSTRUCTEX)];
    const MOUSEHOOKSTRUCTEX *s = (const MOUSEHOOKSTRUCTEX *)r->data;
    size_t i;

    ok(r->size == sizeof(MOUSEHOOKSTRUCTEX), "record %d: size %lu\n", index, r->size);
    ZeroMemory(covered, sizeof(covered));
    FillMemory(covered + offsetof(MOUSEHOOKSTRUCTEX, MOUSEHOOKSTRUCT.pt), sizeof(POINT), 1);
    FillMemory(covered + offsetof(MOUSEHOOKSTRUCTEX, MOUSEHOOKSTRUCT.hwnd), sizeof(HWND), 1);
    FillMemory(covered + offsetof(MOUSEHOOKSTRUCTEX, MOUSEHOOKSTRUCT.wHitTestCode), sizeof(UINT), 1);
    FillMemory(covered + offsetof(MOUSEHOOKSTRUCTEX, MOUSEHOOKSTRUCT.dwExtraInfo), sizeof(ULONG_PTR), 1);
    FillMemory(covered + offsetof(MOUSEHOOKSTRUCTEX, mouseData), sizeof(DWORD), 1);
    /* Windows 7 leaves stack bytes in the padding after mouseData, so only ReactOS is held to zero */
    for (i = 0; i < sizeof(covered); i++)
        ok(covered[i] || r->data[i] == 0 || !running_on_reactos(), "record %d (message %lx): padding byte %u is %02x\n",
           index, r->message, (UINT)i, r->data[i]);
    if (known)
        ok(s->mouseData == expectedData, "record %d (message %lx): mouseData %08lx, expected %08lx\n",
           index, r->message, s->mouseData, expectedData);
}

START_TEST(MouseHookTransport)
{
    WNDCLASSW wc;
    HANDLE map, thread;
    MOUSEHOOK_SHARED *shared;
    HMODULE module;
    HOOKPROC proc;
    HHOOK hook;
    POINT saved, pt;
    DWORD owner;
    int i, n, wheels = 0, others = 0;
    static const LONG deltas[] = { 120, -120, 1, -1, 240, 0x7fff, -0x8000, 30 };

    GetCursorPos(&saved);

    map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(*shared), MOUSEHOOK_MAPPING_NAME);
    shared = map ? MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, sizeof(*shared)) : NULL;
    if (!shared)
    {
        skip("no shared mapping, error %lu\n", GetLastError());
        goto done;
    }
    ZeroMemory(shared, sizeof(*shared));

    module = LoadLibraryW(L"user32_mousehook.dll");
    if (!module)
    {
        skip("user32_mousehook.dll is not there, error %lu\n", GetLastError());
        goto done;
    }
    proc = (HOOKPROC)GetProcAddress(module, "MouseHookProc");
    ok(proc != NULL, "no MouseHookProc\n");

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"MouseHookTransportWnd";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);

    g_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    thread = CreateThread(NULL, 0, WindowThread, NULL, 0, &owner);
    WaitForSingleObject(g_ready, 10000);
    Pump(200);
    if (!g_hwnd)
    {
        skip("no window\n");
        goto stop;
    }

    /* the hook is installed by this thread, the window belongs to the other one */
    hook = SetWindowsHookExW(WH_MOUSE, proc, module, 0);
    ok(hook != NULL, "global WH_MOUSE hook failed, error %lu\n", GetLastError());
    if (!hook)
        goto stop;

    for (i = 0; i < 10 && GetForegroundWindow() != g_hwnd; i++)
    {
        /* a tap of Alt lets a process that is not in the foreground take it */
        if (i > 0)
        {
            keybd_event(VK_MENU, 0, 0, 0);
            keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        }
        SetForegroundWindow(g_hwnd);
        Pump(100);
    }
    if (GetForegroundWindow() != g_hwnd)
    {
        skip("the window could not take the foreground\n");
        UnhookWindowsHookEx(hook);
        goto stop;
    }

    pt.x = 250;
    pt.y = 250;
    SetCursorPos(pt.x, pt.y);
    Pump(150);
    mouse_event(MOUSEEVENTF_MOVE, 3, 2, 0, EXTRA_MARK);
    Pump(150);
    for (i = 0; i < (int)(sizeof(deltas) / sizeof(deltas[0])); i++)
    {
        mouse_event(MOUSEEVENTF_WHEEL, 0, 0, deltas[i], EXTRA_MARK + i);
        Pump(100);
        mouse_event(MOUSEEVENTF_HWHEEL, 0, 0, deltas[i], EXTRA_MARK + i);
        Pump(100);
    }
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, EXTRA_MARK);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, EXTRA_MARK);
    Pump(150);
    UnhookWindowsHookEx(hook);

    n = shared->count;
    ok(n > 0, "the hook module saw no mouse message\n");
    if (n > MOUSEHOOK_MAX_RECORDS)
        n = MOUSEHOOK_MAX_RECORDS;
    for (i = 0; i < n; i++)
    {
        const MOUSEHOOK_RECORD *r = &shared->record[i];
        const MOUSEHOOKSTRUCTEX *s = (const MOUSEHOOKSTRUCTEX *)r->data;

        if (r->message == WM_MOUSEWHEEL || r->message == WM_MOUSEHWHEEL)
        {
            /* the extra value tells which call it came from */
            LONG call = (LONG)(s->MOUSEHOOKSTRUCT.dwExtraInfo - EXTRA_MARK);

            ok(call >= 0 && call < (LONG)(sizeof(deltas) / sizeof(deltas[0])), "record %d: extra info %Ix\n",
               i, s->MOUSEHOOKSTRUCT.dwExtraInfo);
            if (call >= 0 && call < (LONG)(sizeof(deltas) / sizeof(deltas[0])))
            {
                SHORT delta = (SHORT)deltas[call];

                CheckRecord(r, i, MAKELONG(0, delta), TRUE);
            }
            wheels++;
        }
        else
        {
            CheckRecord(r, i, 0, r->message == WM_MOUSEMOVE || r->message == WM_NCMOUSEMOVE ||
                               r->message == WM_LBUTTONDOWN || r->message == WM_LBUTTONUP);
            others++;
        }
    }
    /* every call gives one wheel message */
    ok(wheels == 2 * ((int)(sizeof(deltas) / sizeof(deltas[0]))), "%d wheel messages seen\n", wheels);
    trace("%d records: %d wheel, %d other; hook ran in process %lu thread %lu, window thread %lu\n", n, wheels, others,
          n ? shared->record[0].processId : 0, n ? shared->record[0].threadId : 0, owner);

stop:
    SetEvent(g_stop);
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    CloseHandle(g_ready);
    CloseHandle(g_stop);
    UnregisterClassW(L"MouseHookTransportWnd", GetModuleHandleW(NULL));
    FreeLibrary(module);

done:
    if (shared)
        UnmapViewOfFile(shared);
    if (map)
        CloseHandle(map);
    SetCursorPos(saved.x, saved.y);
}
