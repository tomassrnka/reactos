/*
 * PROJECT:     ReactOS rosdev test tools
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Load for the mouse pointer blink test (host/ptrblink-test): repaints a window under a
 *              still pointer with the drawing calls programs use, one phase after the other
 *
 * ptrblink W H REPS PHASE:SECONDS...   e.g. ptrblink 780 560 40 idle:5 full:10 capture:10 alpha:10
 *
 * The window is topmost at (100,100), W x H client pixels, painted green once. Each phase repeats its call
 * REPS times every ~8 ms for SECONDS seconds:
 *   idle        nothing after the first paint
 *   full        BitBlt of the whole window from a memory DC
 *   capture     the same with SRCCOPY | CAPTUREBLT
 *   stretch11   StretchBlt that does not scale
 *   stretch2x   StretchBlt that doubles (not drawn around the pointer: expected to blink)
 *   alpha       AlphaBlend without scaling
 *   transparent TransparentBlt without scaling
 *   clip        the full BitBlt with the DC clipped to a corner (nothing is drawn near the pointer)
 *   alphaclip   the same with AlphaBlend; stretchclip the same with the doubling StretchBlt
 *   emptyclip   BitBlt with an empty clip region: nothing is drawn; alphaempty the same with AlphaBlend
 *   text        TextOut of a short string in the corner
 * Written without the C runtime so that it also starts from a payload on a bare install.
 */

#include <windows.h>

static int streq(const char *a, const char *b) { return lstrcmpA(a, b) == 0; }

static int toint(const char *s, int def)
{
    int n = 0;
    if (!*s) return def;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return n;
}

static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_SETCURSOR: SetCursor(LoadCursor(NULL, IDC_ARROW)); return TRUE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static void draw(const char *mode, HDC wdc, HDC hmem, HRGN corner, int cw, int ch)
{
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, 0};

    if (streq(mode, "full")) BitBlt(wdc, 0, 0, cw, ch, hmem, 0, 0, SRCCOPY);
    else if (streq(mode, "capture")) BitBlt(wdc, 0, 0, cw, ch, hmem, 0, 0, SRCCOPY | CAPTUREBLT);
    else if (streq(mode, "stretch11")) StretchBlt(wdc, 0, 0, cw, ch, hmem, 0, 0, cw, ch, SRCCOPY);
    else if (streq(mode, "stretch2x")) StretchBlt(wdc, 0, 0, cw, ch, hmem, 0, 0, cw / 2, ch / 2, SRCCOPY);
    else if (streq(mode, "alpha")) AlphaBlend(wdc, 0, 0, cw, ch, hmem, 0, 0, cw, ch, bf);
    else if (streq(mode, "transparent")) TransparentBlt(wdc, 0, 0, cw, ch, hmem, 0, 0, cw, ch, RGB(255, 0, 255));
    else if (streq(mode, "clip"))
    {
        SelectClipRgn(wdc, corner);
        BitBlt(wdc, 0, 0, cw, ch, hmem, 0, 0, SRCCOPY);
        SelectClipRgn(wdc, NULL);
    }
    else if (streq(mode, "alphaclip"))
    {
        SelectClipRgn(wdc, corner);
        AlphaBlend(wdc, 0, 0, cw, ch, hmem, 0, 0, cw, ch, bf);
        SelectClipRgn(wdc, NULL);
    }
    else if (streq(mode, "stretchclip"))
    {
        SelectClipRgn(wdc, corner);
        StretchBlt(wdc, 0, 0, cw, ch, hmem, 0, 0, cw / 2, ch / 2, SRCCOPY);
        SelectClipRgn(wdc, NULL);
    }
    else if (streq(mode, "emptyclip") || streq(mode, "alphaempty"))
    {
        HRGN hempty = CreateRectRgn(0, 0, 0, 0);

        SelectClipRgn(wdc, hempty);
        if (streq(mode, "emptyclip"))
            BitBlt(wdc, 0, 0, cw, ch, hmem, 0, 0, SRCCOPY);
        else
            AlphaBlend(wdc, 0, 0, cw, ch, hmem, 0, 0, cw, ch, bf);
        SelectClipRgn(wdc, NULL);
        DeleteObject(hempty);
    }
    else if (streq(mode, "text")) TextOutA(wdc, 2, 2, "text", 4);
}

static int run(char *argv[], int argc)
{
    int cw = argc > 1 ? toint(argv[1], 700) : 700, ch = argc > 2 ? toint(argv[2], 500) : 500, reps = argc > 3 ? toint(argv[3], 1) : 1;
    int i, rp;
    WNDCLASSA wc;
    HWND h;
    HDC wdc, sdc, hmem;
    HBITMAP hbm;
    RECT r;
    HBRUSH b;
    HRGN corner;
    MSG msg;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wp;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "ptrblink";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    h = CreateWindowExA(WS_EX_TOPMOST, "ptrblink", "ptrblink", WS_POPUP | WS_VISIBLE | WS_BORDER, 100, 100, cw, ch, NULL, NULL, wc.hInstance, NULL);
    wdc = GetDC(h);
    sdc = GetDC(NULL);
    hmem = CreateCompatibleDC(sdc);
    hbm = CreateCompatibleBitmap(sdc, cw, ch);
    SelectObject(hmem, hbm);
    r.left = 0; r.top = 0; r.right = cw; r.bottom = ch;
    b = CreateSolidBrush(RGB(0, 128, 0));
    FillRect(hmem, &r, b);
    corner = CreateRectRgn(0, 0, 40, 40);
    BitBlt(wdc, 0, 0, cw, ch, hmem, 0, 0, SRCCOPY);

    for (i = 4; i < argc; i++)
    {
        char *colon = argv[i];
        DWORD end;
        while (*colon && *colon != ':') colon++;
        if (*colon) *colon++ = 0;
        end = GetTickCount() + toint(colon, 5) * 1000;
        while ((LONG)(GetTickCount() - end) < 0)
        {
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
            for (rp = 0; rp < reps; rp++)
                draw(argv[i], wdc, hmem, corner, cw, ch);
            Sleep(8);
        }
    }
    return 0;
}

static int split(char *cmd, char *argv[], int max)
{
    int argc = 0;
    while (*cmd && argc < max)
    {
        while (*cmd == ' ') cmd++;
        if (!*cmd) break;
        argv[argc++] = cmd;
        while (*cmd && *cmd != ' ') cmd++;
        if (*cmd) *cmd++ = 0;
    }
    return argc;
}

static int entry(void)
{
    char *argv[24];
    char *cmd = GetCommandLineA();
    int argc;

    if (*cmd == '"') { cmd++; while (*cmd && *cmd != '"') cmd++; if (*cmd) cmd++; }
    else { while (*cmd && *cmd != ' ') cmd++; }
    argv[0] = "ptrblink";
    argc = 1 + split(cmd, argv + 1, 22);
    return run(argv, argc);
}

#ifdef PTRBLINK_NOCRT
#ifdef __x86_64__
__attribute__((force_align_arg_pointer))
#endif
void start(void)
{
    ExitProcess(entry());
}
#else
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow)
{
    return entry();
}
#endif
