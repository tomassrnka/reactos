/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Text outside the base font must leave ink when an EDIT control paints it
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define CX 240
#define CY 40

static const WCHAR g_han[] = { 0x6C49, 0x8BED, 0x7B80, 0x4F53, 0 };
static const WCHAR g_arabic[] = { 0x0645, 0x0631, 0x062D, 0x0628, 0x0627, 0 };
static const WCHAR g_latin[] = L"Hello";

static HDC g_hdc;
static HBITMAP g_hbm;
static DWORD *g_bits;

static HFONT create_font(const WCHAR *face)
{
    LOGFONTW lf = { 0 };

    lf.lfHeight = -13;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    if (face)
    {
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
        lstrcpyW(lf.lfFaceName, face);
    }
    else
    {
        /* the request Notepad makes */
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        lstrcpyW(lf.lfFaceName, L"Lucida Console");
    }
    return CreateFontIndirectW(&lf);
}

static void clear_target(void)
{
    RECT rc = { 0, 0, CX, CY };
    FillRect(g_hdc, &rc, GetStockObject(WHITE_BRUSH));
    GdiFlush();
}

static int count_ink(void)
{
    int i, n = 0;

    GdiFlush();
    for (i = 0; i < CX * CY; i++)
        if ((g_bits[i] & 0xFFFFFF) != 0xFFFFFF)
            n++;
    return n;
}

/* pixels inside the bounding box of the highlight colour that are not the highlight colour */
static int count_selected_ink(void)
{
    COLORREF cr = GetSysColor(COLOR_HIGHLIGHT);
    DWORD hl = GetRValue(cr) << 16 | GetGValue(cr) << 8 | GetBValue(cr);
    int x, y, x0 = CX, y0 = CY, x1 = -1, y1 = -1, n = 0;

    GdiFlush();
    for (y = 0; y < CY; y++)
        for (x = 0; x < CX; x++)
            if ((g_bits[y * CX + x] & 0xFFFFFF) == hl)
            {
                x0 = min(x0, x); x1 = max(x1, x);
                y0 = min(y0, y); y1 = max(y1, y);
            }
    if (x1 < 0)
        return -1;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            if ((g_bits[y * CX + x] & 0xFFFFFF) != hl)
                n++;
    return n;
}

static BOOL font_has_glyphs(HFONT hf, const WCHAR *text)
{
    HDC hdc = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(hdc, hf);
    WORD gi[16];
    int i, n = lstrlenW(text);
    BOOL good = TRUE;

    if (GetGlyphIndicesW(hdc, text, n, gi, GGI_MARK_NONEXISTING_GLYPHS) != (DWORD)n)
        good = FALSE;
    for (i = 0; good && i < n; i++)
        if (gi[i] == 0xFFFF)
            good = FALSE;
    SelectObject(hdc, old);
    DeleteDC(hdc);
    return good;
}

static int textout_ink(HFONT hf, const WCHAR *text)
{
    HGDIOBJ old = SelectObject(g_hdc, hf);

    clear_target();
    SetBkMode(g_hdc, TRANSPARENT);
    TextOutW(g_hdc, 4, 4, text, lstrlenW(text));
    SelectObject(g_hdc, old);
    return count_ink();
}

static HWND create_edit(HWND parent, DWORD style, HFONT hf, const WCHAR *text)
{
    HWND hwnd = CreateWindowExW(0, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | style,
                                0, 0, CX, CY, parent, NULL, GetModuleHandleW(NULL), NULL);

    if (hwnd)
    {
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)hf, FALSE);
        SetWindowTextW(hwnd, text);
    }
    return hwnd;
}

static int edit_ink(HWND hwnd)
{
    clear_target();
    SendMessageW(hwnd, WM_PRINTCLIENT, (WPARAM)g_hdc, PRF_CLIENT);
    return count_ink();
}

static void test_text(const WCHAR *text, const char *name, const WCHAR *capable_face)
{
    static const struct { DWORD style; const char *name; } kinds[] =
    {
        { ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL, "multiline" },
        { ES_AUTOHSCROLL, "single line" },
    };
    HFONT hbase, hcapable;
    HWND parent;
    int ref, k;

    hbase = create_font(NULL);
    hcapable = create_font(capable_face);
    if (capable_face && !font_has_glyphs(hcapable, text))
    {
        skip("%s: no installed font has these characters\n", name);
        DeleteObject(hbase);
        DeleteObject(hcapable);
        return;
    }

    ref = textout_ink(hcapable, text);
    ok(ref > 0, "%s: reference text drew no ink\n", name);

    /* a single-line EDIT control paints into a DC only while it is visible; park the window off screen */
    parent = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", NULL, WS_POPUP | WS_VISIBLE,
                             -4000, -4000, CX, CY, NULL, NULL, GetModuleHandleW(NULL), NULL);
    ok(parent != NULL, "CreateWindowEx failed %lu\n", GetLastError());
    if (!parent)
        goto done;

    for (k = 0; k < 2; k++)
    {
        HWND hwnd = create_edit(parent, kinds[k].style | ES_NOHIDESEL, hbase, text);
        int ink, sel;

        ok(hwnd != NULL, "%s %s: CreateWindowEx failed %lu\n", name, kinds[k].name, GetLastError());
        if (!hwnd)
            continue;

        ink = edit_ink(hwnd);
        ok(ink > 0, "%s %s: the EDIT control drew no ink (TextOut reference %d)\n", name, kinds[k].name, ref);
        ok(ink * 4 >= ref, "%s %s: ink %d is far below the TextOut reference %d\n", name, kinds[k].name, ink, ref);

        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        clear_target();
        SendMessageW(hwnd, WM_PRINTCLIENT, (WPARAM)g_hdc, PRF_CLIENT);
        sel = count_selected_ink();
        ok(sel > 0, "%s %s: selected text drew no ink (%d)\n", name, kinds[k].name, sel);
        DestroyWindow(hwnd);
    }
    DestroyWindow(parent);
done:
    DeleteObject(hbase);
    DeleteObject(hcapable);
}

START_TEST(EditInk)
{
    BITMAPINFO bi = { { sizeof(bi.bmiHeader), CX, -CY, 1, 32, BI_RGB } };
    HDC hdc = GetDC(NULL);

    g_hdc = CreateCompatibleDC(hdc);
    g_hbm = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void **)&g_bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!g_hbm)
    {
        skip("CreateDIBSection failed\n");
        DeleteDC(g_hdc);
        return;
    }
    SelectObject(g_hdc, g_hbm);

    test_text(g_latin, "Latin", NULL);
    test_text(g_han, "Han", L"SimSun");
    test_text(g_arabic, "Arabic", L"Tahoma");

    DeleteDC(g_hdc);
    DeleteObject(g_hbm);
}
