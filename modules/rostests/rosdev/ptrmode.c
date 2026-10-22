/*
 * PROJECT:     ReactOS rosdev
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Guest side of host/ptrmode-test: the mouse pointer after a display mode switch, without a move
 *
 * The main thread owns a green topmost window at (100,100) with the pointer parked over it. After 8 seconds a second
 * thread, whose queue does not own the window under the pointer (so UserSetCursor sets no shape again), switches the
 * display mode: by default to another size at the same depth, with 'depth' to another depth, with 'back' then back to the
 * first mode. The window does not paint
 * again near the pointer; the main thread then paints a blue bar at its top as the sign for the host, which looks
 * whether the pointer is on the screen although nothing moved it. A red window means no such mode exists.
 */

#include <windows.h>
#include <shlwapi.h>

#define WND_X 100
#define WND_Y 100
#define WND_CX 600
#define WND_CY 450
#define PTR_X 400
#define PTR_Y 350

static HWND g_hwnd;
static BOOL g_bDepth, g_bBack;
static DEVMODEW g_dmFirst;

static void Fill(HWND hwnd, COLORREF cr, int top, int bottom)
{
    HDC hdc = GetDC(hwnd);
    HBRUSH hbr = CreateSolidBrush(cr);
    RECT rc = { 0, top, WND_CX, bottom };

    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);
    ReleaseDC(hwnd, hdc);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_USER + 1:
            Fill(hwnd, RGB(0, 0, 200), 0, 40);
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            /* Nothing is drawn again after the switch: only the mode switch code may bring the pointer back */
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static BOOL SwitchMode(void)
{
    DEVMODEW cur, dm;
    DWORD i = 0;
    WCHAR szMsg[128];

    ZeroMemory(&cur, sizeof(cur));
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &cur))
        return FALSE;
    g_dmFirst = cur;
    g_dmFirst.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;

    for (i = 0; ; i++)
    {
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(NULL, i, &dm))
            break;
        if (dm.dmBitsPerPel < 16 || dm.dmPelsWidth < 800 || dm.dmPelsHeight < 600)
            continue;
        if (g_bDepth ? dm.dmBitsPerPel != cur.dmBitsPerPel :
                       (dm.dmBitsPerPel == cur.dmBitsPerPel &&
                        (dm.dmPelsWidth != cur.dmPelsWidth || dm.dmPelsHeight != cur.dmPelsHeight)))
        {
            wsprintfW(szMsg, L"ptrmode: %lux%lux%lu -> %lux%lux%lu\n", cur.dmPelsWidth, cur.dmPelsHeight,
                      cur.dmBitsPerPel, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel);
            OutputDebugStringW(szMsg);
            dm.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
            return ChangeDisplaySettingsW(&dm, 0) == DISP_CHANGE_SUCCESSFUL;
        }
    }
    wsprintfW(szMsg, L"ptrmode: no such mode (%lu modes, current %lu bits)\n", i, cur.dmBitsPerPel);
    OutputDebugStringW(szMsg);
    return FALSE;
}

static DWORD WINAPI Worker(LPVOID pv)
{
    UNREFERENCED_PARAMETER(pv);

    Sleep(8000);
    if (!SwitchMode() || (g_bBack && (Sleep(3000), ChangeDisplaySettingsW(&g_dmFirst, 0) != DISP_CHANGE_SUCCESSFUL)))
    {
        Sleep(1000);
        Fill(g_hwnd, RGB(200, 0, 0), 0, WND_CY);
        return 0;
    }
    Sleep(3000);
    PostMessageW(g_hwnd, WM_USER + 1, 0, 0);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR pszCmdLine, int nShowCmd)
{
    WNDCLASSW wc;
    MSG msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nShowCmd);

    g_bDepth = (StrStrIW(pszCmdLine, L"depth") != NULL);
    g_bBack = (StrStrIW(pszCmdLine, L"back") != NULL);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ptrmode";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST, L"ptrmode", L"ptrmode", WS_POPUP | WS_VISIBLE,
                             WND_X, WND_Y, WND_CX, WND_CY, NULL, NULL, hInstance, NULL);
    Fill(g_hwnd, RGB(0, 128, 0), 0, WND_CY);
    SetCursorPos(PTR_X, PTR_Y);
    CreateThread(NULL, 0, Worker, NULL, 0, NULL);

    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
