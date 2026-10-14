/*
 * PROJECT:         ReactOS API tests
 * LICENSE:         LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:         Messages sent with SendMessageW to a window whose procedure is ANSI
 *                  must reach the procedure, and the reply text must come back as Unicode
 */

#include "precomp.h"

static UINT g_Msg;
static UINT g_Calls;
static BOOL g_SawAnsiString;
static WNDPROC g_OldProc;

static
LRESULT
CALLBACK
PlainAnsiProc(
    _In_ HWND hWnd,
    _In_ UINT Msg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    if (Msg == g_Msg)
    {
        g_Calls++;
        switch (Msg)
        {
            case WM_ASKCBFORMATNAME:
                if (wParam < 4)
                    return 0;
                lstrcpyA((LPSTR)lParam, "fmt");
                return 3;
            case WM_SETTEXT:
                g_SawAnsiString = !lstrcmpA((LPCSTR)lParam, "settext");
                break;
        }
    }
    return DefWindowProcA(hWnd, Msg, wParam, lParam);
}

static
LRESULT
CALLBACK
SubclassAnsiProc(
    _In_ HWND hWnd,
    _In_ UINT Msg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    if (Msg == g_Msg)
        g_Calls++;
    return CallWindowProcA(g_OldProc, hWnd, Msg, wParam, lParam);
}

static
void
TestGetText(
    _In_ HWND hWnd)
{
    WCHAR Buffer[32];
    LRESULT ret;
    INT len;

    g_Msg = WM_GETTEXT;

    g_Calls = 0;
    memset(Buffer, 0x55, sizeof(Buffer));
    Buffer[RTL_NUMBER_OF(Buffer) - 1] = UNICODE_NULL;
    ret = SendMessageW(hWnd, WM_GETTEXT, RTL_NUMBER_OF(Buffer), (LPARAM)Buffer);
    ok(ret == 5, "WM_GETTEXT returned %Id\n", ret);
    ok(!wcscmp(Buffer, L"hello"), "WM_GETTEXT buffer %S\n", Buffer);
    ok(g_Calls == 1, "procedure called %u times\n", g_Calls);

    memset(Buffer, 0x55, sizeof(Buffer));
    Buffer[RTL_NUMBER_OF(Buffer) - 1] = UNICODE_NULL;
    len = GetWindowTextW(hWnd, Buffer, RTL_NUMBER_OF(Buffer));
    ok(len == 5, "GetWindowTextW returned %d\n", len);
    ok(!wcscmp(Buffer, L"hello"), "GetWindowTextW buffer %S\n", Buffer);

    ret = SendMessageW(hWnd, WM_GETTEXTLENGTH, 0, 0);
    ok(ret >= 5, "WM_GETTEXTLENGTH returned %Id\n", ret);

    ret = SendMessageW(hWnd, WM_GETTEXT, 0, (LPARAM)Buffer);
    ok(ret == 0, "WM_GETTEXT with zero size returned %Id\n", ret);
}

static
void
TestAskFormatName(
    _In_ HWND hWnd)
{
    WCHAR Buffer[32];
    LRESULT ret;

    g_Msg = WM_ASKCBFORMATNAME;
    g_Calls = 0;
    memset(Buffer, 0x55, sizeof(Buffer));
    Buffer[RTL_NUMBER_OF(Buffer) - 1] = UNICODE_NULL;
    ret = SendMessageW(hWnd, WM_ASKCBFORMATNAME, 32, (LPARAM)Buffer);
    ok(ret == 3, "WM_ASKCBFORMATNAME returned %Id\n", ret);
    ok(!wcscmp(Buffer, L"fmt"), "WM_ASKCBFORMATNAME buffer %S\n", Buffer);
    ok(g_Calls == 1, "WM_ASKCBFORMATNAME: procedure called %u times\n", g_Calls);
}

static
void
TestSetText(
    _In_ HWND hWnd)
{
    LRESULT ret;

    g_Msg = WM_SETTEXT;
    g_Calls = 0;
    g_SawAnsiString = FALSE;
    ret = SendMessageW(hWnd, WM_SETTEXT, 0, (LPARAM)L"settext");
    ok(ret == TRUE, "WM_SETTEXT returned %Id\n", ret);
    ok(g_Calls == 1, "WM_SETTEXT: procedure called %u times\n", g_Calls);
    ok(g_SawAnsiString, "WM_SETTEXT did not arrive as an ANSI string\n");
}

static
void
TestListBoxText(
    _In_ HINSTANCE hInstance)
{
    WCHAR Buffer[32];
    HWND hWnd;
    LRESULT ret;

    hWnd = CreateWindowExA(0, "LISTBOX", NULL, WS_POPUP | LBS_HASSTRINGS,
                           0, 0, 100, 100, NULL, NULL, hInstance, NULL);
    ok(hWnd != NULL, "CreateWindowExA failed %lu\n", GetLastError());
    if (!hWnd)
        return;

    ret = SendMessageA(hWnd, LB_ADDSTRING, 0, (LPARAM)"item");
    ok(ret == 0, "LB_ADDSTRING returned %Id\n", ret);

    g_OldProc = (WNDPROC)SetWindowLongPtrA(hWnd, GWLP_WNDPROC, (LONG_PTR)SubclassAnsiProc);
    g_Msg = LB_GETTEXT;
    g_Calls = 0;
    memset(Buffer, 0x55, sizeof(Buffer));
    Buffer[RTL_NUMBER_OF(Buffer) - 1] = UNICODE_NULL;
    ret = SendMessageW(hWnd, LB_GETTEXT, 0, (LPARAM)Buffer);
    ok(ret == 4, "LB_GETTEXT returned %Id\n", ret);
    ok(!wcscmp(Buffer, L"item"), "LB_GETTEXT buffer %S\n", Buffer);
    ok(g_Calls >= 1, "LB_GETTEXT: procedure called %u times\n", g_Calls);

    DestroyWindow(hWnd);
}

static
void
TestComboBoxText(
    _In_ HINSTANCE hInstance)
{
    WCHAR Buffer[32];
    HWND hWnd;
    LRESULT ret;

    hWnd = CreateWindowExA(0, "COMBOBOX", NULL, WS_POPUP | CBS_DROPDOWNLIST,
                           0, 0, 100, 100, NULL, NULL, hInstance, NULL);
    ok(hWnd != NULL, "CreateWindowExA failed %lu\n", GetLastError());
    if (!hWnd)
        return;

    ret = SendMessageA(hWnd, CB_ADDSTRING, 0, (LPARAM)"item");
    ok(ret == 0, "CB_ADDSTRING returned %Id\n", ret);

    g_OldProc = (WNDPROC)SetWindowLongPtrA(hWnd, GWLP_WNDPROC, (LONG_PTR)SubclassAnsiProc);
    g_Msg = CB_GETLBTEXT;
    g_Calls = 0;
    memset(Buffer, 0x55, sizeof(Buffer));
    Buffer[RTL_NUMBER_OF(Buffer) - 1] = UNICODE_NULL;
    ret = SendMessageW(hWnd, CB_GETLBTEXT, 0, (LPARAM)Buffer);
    ok(ret == 4, "CB_GETLBTEXT returned %Id\n", ret);
    ok(!wcscmp(Buffer, L"item"), "CB_GETLBTEXT buffer %S\n", Buffer);
    ok(g_Calls >= 1, "CB_GETLBTEXT: procedure called %u times\n", g_Calls);

    DestroyWindow(hWnd);
}

START_TEST(UnicodeToAnsiMessage)
{
    WNDCLASSA wc;
    HWND hWnd;
    HINSTANCE hInstance = GetModuleHandleW(NULL);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = PlainAnsiProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "UnicodeToAnsiMessageClass";
    if (!RegisterClassA(&wc))
    {
        skip("RegisterClassA failed %lu\n", GetLastError());
        return;
    }

    hWnd = CreateWindowExA(0, wc.lpszClassName, "hello", WS_OVERLAPPED,
                           0, 0, 100, 100, NULL, NULL, hInstance, NULL);
    ok(hWnd != NULL, "CreateWindowExA failed %lu\n", GetLastError());
    if (hWnd)
    {
        ok(!IsWindowUnicode(hWnd), "Window is Unicode\n");
        TestGetText(hWnd);
        TestAskFormatName(hWnd);
        TestSetText(hWnd);
        DestroyWindow(hWnd);
    }

    TestListBoxText(hInstance);
    TestComboBoxText(hInstance);

    UnregisterClassA(wc.lpszClassName, hInstance);
}
