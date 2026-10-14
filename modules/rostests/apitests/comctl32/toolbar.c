/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Test for toolbar window class v6
 * PROGRAMMERS:     Giannis Adamopoulos
 */

#include "wine/test.h"
#include <stdio.h>
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <undocuser.h>
#include <msgtrace.h>
#include <user32testhelpers.h>

HANDLE _CreateV5ActCtx()
{
    ACTCTXW ActCtx = {sizeof(ACTCTX)};
    WCHAR buffer[MAX_PATH] , *separator;

    ok (GetModuleFileNameW(NULL, buffer, MAX_PATH), "GetModuleFileName failed\n");
    separator = wcsrchr(buffer, L'\\');
    if (separator)
        wcscpy(separator + 1, L"comctl32v5.manifest");

    ActCtx.lpSource = buffer;

    return CreateActCtxW(&ActCtx);;
}


void TestVersionMessage()
{
    HWND hwnd;
    int version;

    hwnd = CreateWindowExW(0, TOOLBARCLASSNAMEW, L"Test", 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    ok(hwnd != NULL, "CreateWindowEx failed\n");

    version = SendMessageW(hwnd, CCM_GETVERSION, 0, 0);
    ok(version == 6, "Got %d, expected 6\n", version);

    version = SendMessageW(hwnd, CCM_SETVERSION, 5, 0);
    ok(version == 6, "Got %d, expected 6\n", version);

    version = SendMessageW(hwnd, CCM_GETVERSION, 0, 0);
    ok(version == 6, "Got %d, expected 6\n", version);

    version = SendMessageW(hwnd, CCM_SETVERSION, 7, 0);
    ok(version == 6, "Got %d, expected 6\n", version);

    version = SendMessageW(hwnd, CCM_GETVERSION, 0, 0);
    ok(version == 6, "Got %d, expected 6\n", version);

    version = SendMessageW(hwnd, CCM_SETVERSION, 4, 0);
    ok(version == 6, "Got %d, expected 6\n", version);

    version = SendMessageW(hwnd, CCM_GETVERSION, 0, 0);
    ok(version == 6, "Got %d, expected 6\n", version);

    DestroyWindow(hwnd);
}

void TestSetButtonSize()
{
    HWND hwnd;
    LRESULT bsize;

    hwnd = CreateWindowExW(0, TOOLBARCLASSNAMEW, L"Test", 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    ok(hwnd != NULL, "CreateWindowEx failed\n");

    bsize = SendMessageW(hwnd, TB_GETBUTTONSIZE, 0, 0);
    ok(bsize == 0x160017, "Expected 0x160017 got %lx\n", bsize);

    SendMessageW(hwnd, TB_SETBUTTONSIZE, 0, MAKELONG(0, 0));
    bsize = SendMessageW(hwnd, TB_GETBUTTONSIZE, 0, 0);
    ok(bsize == 0x160018, "Expected 0x160018 got %lx\n", bsize);

    SendMessageW(hwnd, TB_SETBUTTONSIZE, 0, 0x10001);
    bsize = SendMessageW(hwnd, TB_GETBUTTONSIZE, 0, 0);
    ok(bsize == 0x160017, "Expected 0x160017 got %lx\n", bsize);

    SendMessageW(hwnd, TB_SETBUTTONSIZE, 0, 0x100001);
    bsize = SendMessageW(hwnd, TB_GETBUTTONSIZE, 0, 0);
    ok(bsize == 0x160017, "Expected 0x160017 got %lx\n", bsize);

    SendMessageW(hwnd, TB_SETBUTTONSIZE, 0, 0x160017);
    bsize = SendMessageW(hwnd, TB_GETBUTTONSIZE, 0, 0);
    ok(bsize == 0x160017, "Expected 0x160017 got %lx\n", bsize);

    SendMessageW(hwnd, TB_SETBUTTONSIZE, 0, 0x170017);
    bsize = SendMessageW(hwnd, TB_GETBUTTONSIZE, 0, 0);
    ok(bsize == 0x170017, "Expected 0x170017 got %lx\n", bsize);

    DestroyWindow(hwnd);
}

void TestPadding()
{
    HWND hwnd;
    LRESULT bsize;

    hwnd = CreateWindowExW(0, TOOLBARCLASSNAMEW, L"Test", 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    ok(hwnd != NULL, "CreateWindowEx failed\n");

    bsize = SendMessageW(hwnd, TB_GETPADDING, 0, 0);
    ok(bsize == 0x60007, "Expected 0x60007 got %lx\n", bsize);

    SendMessageW(hwnd, TB_SETPADDING, 0, 0x10001);
    SendMessageW(hwnd, TB_SETBITMAPSIZE, 0, 0x10001);
    SendMessageW(hwnd, TB_SETBUTTONSIZE, 0, 0x10001);

    bsize = SendMessageW(hwnd, TB_GETPADDING, 0, 0);
    ok(bsize == 0x10001, "Expected 0x10001 got %lx\n", bsize);

    bsize = SendMessageW(hwnd, TB_GETBUTTONSIZE, 0, 0);
    ok(bsize == 0x20002 || bsize == 0xe0002, "Expected 0x20002 got %lx\n", bsize);

#if 0 /* Luna specific */
    SetWindowTheme(hwnd, L"TaskBand", NULL);

    bsize = SendMessageW(hwnd, TB_GETBUTTONSIZE, 0, 0);
    ok(bsize == 0x20002, "Expected 0x20002 got %lx\n", bsize);

    bsize = SendMessageW(hwnd, TB_GETPADDING, 0, 0);
    ok(bsize == 0x10001, "Expected 0x10001 got %lx\n", bsize);

    SendMessageW(hwnd, TB_SETBUTTONSIZE, 0, 0x10001);

    bsize = SendMessageW(hwnd, TB_GETBUTTONSIZE, 0, 0);
    ok(bsize == 0x160006, "Expected 0x160006 got %lx\n", bsize);

    bsize = SendMessageW(hwnd, TB_GETPADDING, 0, 0);
    ok(bsize == 0x10001, "Expected 0x10001 got %lx\n", bsize);

    SendMessageW(hwnd, TB_SETPADDING, 0, 0x10001);
    SendMessageW(hwnd, TB_SETBITMAPSIZE, 0, 0x10001);
    SendMessageW(hwnd, TB_SETBUTTONSIZE, 0, 0x10001);

    bsize = SendMessageW(hwnd, TB_GETPADDING, 0, 0);
    ok(bsize == 0x10001, "Expected 0x10001 got %lx\n", bsize);

    bsize = SendMessageW(hwnd, TB_GETBUTTONSIZE, 0, 0);
    ok(bsize == 0x160006, "Expected 0x160006 got %lx\n", bsize);

    SendMessageW(hwnd, TB_SETBITMAPSIZE, 0, 0x10001);
    SendMessageW(hwnd, TB_SETPADDING, 0, 0x20002);
    SendMessageW(hwnd, TB_SETBUTTONSIZE, 0, 0x10001);

    bsize = SendMessageW(hwnd, TB_GETBUTTONSIZE, 0, 0);
    ok(bsize == 0x170007, "Expected 0x170007 got %lx\n", bsize);

    SendMessageW(hwnd, TB_SETBITMAPSIZE, 0, 0x20002);
    SendMessageW(hwnd, TB_SETPADDING, 0, 0x20002);
    SendMessageW(hwnd, TB_SETBUTTONSIZE, 0, 0x10001);

    bsize = SendMessageW(hwnd, TB_GETBUTTONSIZE, 0, 0);
    ok(bsize == 0x170008, "Expected 0x170008 got %lx\n", bsize);

    SendMessageW(hwnd, TB_SETBITMAPSIZE, 0, 0x100010);
    SendMessageW(hwnd, TB_SETPADDING, 0, 0x20002);
    SendMessageW(hwnd, TB_SETBUTTONSIZE, 0, 0x10001);

    bsize = SendMessageW(hwnd, TB_GETBUTTONSIZE, 0, 0);

    /* With a big enough image size the button size is bitmap size + pading + theme content margins */
    ok(bsize == 0x1a0016, "Expected 0x1a0016 got %lx\n", bsize);
#endif

    DestroyWindow(hwnd);
}

void TestButtonSpacing()
{
    HWND hwnd;
    TBMETRICS metrics;
    LRESULT lres;

    hwnd = CreateWindowExW(0, TOOLBARCLASSNAMEW, L"Test", 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    ok(hwnd != NULL, "CreateWindowEx failed\n");

    memset(&metrics, 0, sizeof(metrics));
    lres = SendMessageW(hwnd, TB_GETMETRICS, 0, (LPARAM)&metrics);
    ok (lres == 0, "Got %d result\n", (int)lres);
    ok (metrics.dwMask == 0, "Got %lu\n", metrics.dwMask);
    ok (metrics.cxPad == 0, "Got %d\n", metrics.cxPad);

    metrics.cbSize = sizeof(metrics);
    metrics.dwMask = TBMF_PAD|TBMF_BARPAD|TBMF_BUTTONSPACING;
    lres = SendMessageW(hwnd, TB_GETMETRICS, 0, (LPARAM)&metrics);
    ok (lres == 0, "Got %lu result\n", lres);
    ok (metrics.dwMask == (TBMF_PAD|TBMF_BARPAD|TBMF_BUTTONSPACING), "Got %lu\n", metrics.dwMask);
    ok (metrics.cxPad == 7, "Got %d\n", metrics.cxPad);
    ok (metrics.cyPad == 6, "Got %d\n", metrics.cyPad);
    ok (metrics.cxButtonSpacing == 0, "Got %d\n", metrics.cxButtonSpacing);
    ok (metrics.cyButtonSpacing == 0, "Got %d\n", metrics.cyButtonSpacing);
}

void TestV5VersionMessage()
{
    HWND hwnd;
    int version;

    hwnd = CreateWindowExW(0, TOOLBARCLASSNAMEW, L"Test", 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    ok(hwnd != NULL, "CreateWindowEx failed\n");

    version = SendMessageW(hwnd, CCM_GETVERSION, 0, 0);
    ok(version == 0, "Got %d, expected 0\n", version);

    version = SendMessageW(hwnd, CCM_SETVERSION, 6, 0);
    ok(version == -1, "Got %d, expected -1\n", version);

    version = SendMessageW(hwnd, CCM_SETVERSION, 7, 0);
    ok(version == -1, "Got %d, expected -1\n", version);

    version = SendMessageW(hwnd, CCM_SETVERSION, 5, 0);
    ok(version == 0, "Got %d, expected -1\n", version);

    version = SendMessageW(hwnd, CCM_GETVERSION, 0, 0);
    ok(version == 5, "Got %d, expected 5\n", version);

    version = SendMessageW(hwnd, CCM_SETVERSION, 4, 0);
    ok(version == 5, "Got %d, expected -1\n", version);

    version = SendMessageW(hwnd, CCM_GETVERSION, 0, 0);
    ok(version == 4, "Got %d, expected 5\n", version);

    version = SendMessageW(hwnd, CCM_SETVERSION, 3, 0);
    ok(version == 4, "Got %d, expected -1\n", version);

    version = SendMessageW(hwnd, CCM_GETVERSION, 0, 0);
    ok(version == 3, "Got %d, expected 5\n", version);

    DestroyWindow(hwnd);
}

/* A control hosted inside a toolbar notifies the toolbar, which passes the notification on to its parent */

#define NTF_RESULT 0x12340000

static BOOL g_ntf_ansi_parent;
static int g_ntf_calls;
static const void *g_ntf_ptr;
static NMHDR g_ntf_hdr;
static WPARAM g_ntf_wp;
static INT g_ntf_why;
static CHAR g_ntf_text[64];
static UINT g_ntf_codes[8];
static LRESULT g_ntf_result;

static LRESULT CALLBACK NotifyParentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NOTIFYFORMAT && lp == NF_QUERY)
        return g_ntf_ansi_parent ? NFR_ANSI : NFR_UNICODE;

    if (msg == WM_NOTIFY)
    {
        NMHDR *hdr = (NMHDR *)lp;

        if (g_ntf_calls < (int)ARRAY_SIZE(g_ntf_codes))
            g_ntf_codes[g_ntf_calls] = hdr->code;
        g_ntf_calls++;
        g_ntf_hdr = *hdr;
        g_ntf_ptr = hdr;
        g_ntf_wp = wp;
        if (hdr->code == CBEN_ENDEDITW)
        {
            NMCBEENDEDITW *ed = (NMCBEENDEDITW *)hdr;
            g_ntf_why = ed->iWhy;
            WideCharToMultiByte(CP_ACP, 0, ed->szText, -1, g_ntf_text, sizeof(g_ntf_text), NULL, NULL);
        }
        else if (hdr->code == CBEN_ENDEDITA)
        {
            NMCBEENDEDITA *ed = (NMCBEENDEDITA *)hdr;
            g_ntf_why = ed->iWhy;
            lstrcpynA(g_ntf_text, ed->szText, sizeof(g_ntf_text));
        }
        return g_ntf_result;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void NotifyReset(void)
{
    g_ntf_calls = 0;
    g_ntf_ptr = NULL;
    memset(&g_ntf_hdr, 0, sizeof(g_ntf_hdr));
    g_ntf_wp = 0;
    g_ntf_why = -1;
    g_ntf_text[0] = 0;
    memset(g_ntf_codes, 0, sizeof(g_ntf_codes));
}

static HWND CreateNotifyParent(BOOL ansi)
{
    static BOOL registered;
    HINSTANCE hinst = GetModuleHandleW(NULL);

    if (!registered)
    {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = NotifyParentProc;
        wc.hInstance = hinst;
        wc.lpszClassName = L"ToolbarNotifyParent";
        RegisterClassW(&wc);
        registered = TRUE;
    }
    g_ntf_ansi_parent = ansi;
    g_ntf_result = NTF_RESULT;
    return CreateWindowExW(0, L"ToolbarNotifyParent", L"parent", WS_OVERLAPPEDWINDOW, 0, 0, 300, 100,
                           NULL, NULL, hinst, NULL);
}

/* The notification is sent with an idFrom that differs from the control id seen by the toolbar */
static LRESULT SendChildNotify(HWND toolbar, HWND from, UINT code)
{
    NMCBEENDEDITW nm;

    memset(&nm, 0, sizeof(nm));
    nm.hdr.hwndFrom = from;
    nm.hdr.idFrom = 77;
    nm.hdr.code = code;
    nm.iWhy = CBENF_RETURN;
    lstrcpyW(nm.szText, L"abc");
    NotifyReset();
    return SendMessageW(toolbar, WM_NOTIFY, 88, (LPARAM)&nm);
}

static void ExpectForwarded(HWND toolbar, HWND from, UINT code, const char *name)
{
    NMCBEENDEDITW nm;
    LRESULT ret;

    memset(&nm, 0, sizeof(nm));
    nm.hdr.hwndFrom = from;
    nm.hdr.idFrom = 77;
    nm.hdr.code = code;
    nm.iWhy = CBENF_RETURN;
    lstrcpyW(nm.szText, L"abc");
    NotifyReset();
    ret = SendMessageW(toolbar, WM_NOTIFY, 88, (LPARAM)&nm);
    ok(ret == NTF_RESULT, "%s: parent result not returned, got %#lx\n", name, (long)ret);
    ok(g_ntf_calls == 1, "%s: parent got %d notifications\n", name, g_ntf_calls);
    ok(g_ntf_ptr == &nm, "%s: structure was copied\n", name);
    ok(g_ntf_hdr.code == code, "%s: code %d, expected %d\n", name, (int)g_ntf_hdr.code, (int)code);
    ok(g_ntf_hdr.hwndFrom == from, "%s: hwndFrom %p, expected %p\n", name, g_ntf_hdr.hwndFrom, from);
    ok(g_ntf_hdr.idFrom == 77, "%s: idFrom %lu\n", name, (ULONG)g_ntf_hdr.idFrom);
    ok(g_ntf_wp == 77, "%s: wParam %lu, expected the idFrom of the header\n", name, (ULONG)g_ntf_wp);
}

static void TestChildNotifyUnicode(BOOL v6)
{
    static const struct { UINT code; const char *name; } codes[] =
    {
        { CBEN_ENDEDITW, "CBEN_ENDEDITW" }, { CBEN_ENDEDITA, "CBEN_ENDEDITA" },
        { CBEN_BEGINEDIT, "CBEN_BEGINEDIT" }, { CBEN_GETDISPINFOW, "CBEN_GETDISPINFOW" },
        { CBEN_DRAGBEGINW, "CBEN_DRAGBEGINW" }, { NM_CLICK, "NM_CLICK" }, { NM_RETURN, "NM_RETURN" },
        { NM_SETFOCUS, "NM_SETFOCUS" }, { NM_KILLFOCUS, "NM_KILLFOCUS" }, { EN_CHANGE, "EN_CHANGE" },
        { TBN_DROPDOWN, "TBN_DROPDOWN" }, { TBN_GETINFOTIPW, "TBN_GETINFOTIPW" },
        { DTN_DATETIMECHANGE, "DTN_DATETIMECHANGE" }, { TCN_SELCHANGE, "TCN_SELCHANGE" },
        { 5000, "5000" }, { (UINT)-2000, "-2000" },
    };
    HINSTANCE hinst = GetModuleHandleW(NULL);
    HWND parent, parent2, toolbar, child, tooltip;
    LRESULT ret;
    UINT i;

    parent = CreateNotifyParent(FALSE);
    toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE | TBSTYLE_TOOLTIPS, 0, 0, 200, 30,
                              parent, (HMENU)5, hinst, NULL);
    ok(toolbar != NULL, "CreateWindowEx failed\n");
    child = CreateWindowExW(0, L"STATIC", L"child", WS_CHILD | WS_VISIBLE, 10, 2, 50, 20, toolbar, (HMENU)9, hinst, NULL);
    ok(child != NULL, "CreateWindowEx failed\n");
    ok(SendMessageW(toolbar, TB_GETUNICODEFORMAT, 0, 0) == 1, "toolbar should be Unicode\n");

    for (i = 0; i < ARRAY_SIZE(codes); i++)
        ExpectForwarded(toolbar, child, codes[i].code, codes[i].name);

    /* the notification of the toolbar's own control is forwarded as well */
    ExpectForwarded(toolbar, toolbar, CBEN_ENDEDITW, "from the toolbar itself");

    tooltip = (HWND)SendMessageW(toolbar, TB_GETTOOLTIPS, 0, 0);
    ok(tooltip != NULL, "toolbar has no tooltip control\n");
    if (tooltip)
    {
        ExpectForwarded(toolbar, tooltip, TTN_POP, "tooltip TTN_POP");
        ExpectForwarded(toolbar, tooltip, NM_CUSTOMDRAW, "tooltip NM_CUSTOMDRAW");
        /* TTN_SHOW stays with a version 6 toolbar */
        if (v6)
        {
            ret = SendChildNotify(toolbar, tooltip, TTN_SHOW);
            ok(ret == 0, "TTN_SHOW returned %#lx\n", (long)ret);
            ok(g_ntf_calls == 0, "TTN_SHOW reached the parent\n");
        }
        else
            ExpectForwarded(toolbar, tooltip, TTN_SHOW, "tooltip TTN_SHOW");
    }

    /* the new parent gets them after TB_SETPARENT */
    parent2 = CreateNotifyParent(FALSE);
    ret = SendMessageW(toolbar, TB_SETPARENT, (WPARAM)parent2, 0);
    ok(ret == (LRESULT)parent, "TB_SETPARENT returned %p, expected %p\n", (HWND)ret, parent);
    ExpectForwarded(toolbar, child, NM_CLICK, "after TB_SETPARENT");

    DestroyWindow(parent2);
    DestroyWindow(parent);
}

static void TestChildNotifyAnsiParent(void)
{
    HINSTANCE hinst = GetModuleHandleW(NULL);
    HWND parent, toolbar, child;
    NMCBEENDEDITW nm;
    LRESULT ret;

    parent = CreateNotifyParent(TRUE);
    toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 200, 30, parent, (HMENU)5, hinst, NULL);
    ok(toolbar != NULL, "CreateWindowEx failed\n");
    child = CreateWindowExW(0, L"STATIC", L"child", WS_CHILD | WS_VISIBLE, 10, 2, 50, 20, toolbar, (HMENU)9, hinst, NULL);
    ok(SendMessageW(toolbar, TB_GETUNICODEFORMAT, 0, 0) == 0, "toolbar should be ANSI\n");

    /* the Unicode form of the end of editing is converted for an ANSI parent */
    memset(&nm, 0, sizeof(nm));
    nm.hdr.hwndFrom = child;
    nm.hdr.idFrom = 77;
    nm.hdr.code = CBEN_ENDEDITW;
    nm.iWhy = CBENF_RETURN;
    nm.iNewSelection = 3;
    lstrcpyW(nm.szText, L"abc");
    NotifyReset();
    ret = SendMessageW(toolbar, WM_NOTIFY, 88, (LPARAM)&nm);
    ok(ret == NTF_RESULT, "result %#lx\n", (long)ret);
    ok(g_ntf_calls == 1, "parent got %d notifications\n", g_ntf_calls);
    ok(g_ntf_hdr.code == CBEN_ENDEDITA, "code %d, expected %d\n", (int)g_ntf_hdr.code, CBEN_ENDEDITA);
    ok(g_ntf_ptr != &nm, "structure was not converted\n");
    ok(g_ntf_hdr.hwndFrom == child && g_ntf_hdr.idFrom == 77 && g_ntf_wp == 77, "header %p %lu wParam %lu\n",
       g_ntf_hdr.hwndFrom, (ULONG)g_ntf_hdr.idFrom, (ULONG)g_ntf_wp);
    ok(g_ntf_why == CBENF_RETURN, "iWhy %d\n", g_ntf_why);
    ok(!strcmp(g_ntf_text, "abc"), "text '%s'\n", g_ntf_text);

    /* the ANSI form and the notifications that need no conversion are forwarded as they are */
    ExpectForwarded(toolbar, child, CBEN_ENDEDITA, "CBEN_ENDEDITA");
    ExpectForwarded(toolbar, child, NM_CLICK, "NM_CLICK");
    ExpectForwarded(toolbar, child, 5000, "5000");

    DestroyWindow(parent);
}

/* The 7-Zip File Manager address bar: a ComboBoxEx hosted in a toolbar, Enter in its edit control */
static void TestComboBoxExInToolbar(BOOL ansi)
{
    HINSTANCE hinst = GetModuleHandleW(NULL);
    HWND parent, toolbar, combo, edit;

    parent = CreateNotifyParent(ansi);
    toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 250, 30, parent, (HMENU)5, hinst, NULL);
    combo = CreateWindowExW(0, WC_COMBOBOXEXW, NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWN, 0, 0, 200, 200,
                            toolbar, (HMENU)7, hinst, NULL);
    ok(combo != NULL, "CreateWindowEx failed\n");
    edit = (HWND)SendMessageW(combo, CBEM_GETEDITCONTROL, 0, 0);
    ok(edit != NULL, "ComboBoxEx has no edit control\n");
    if (!edit)
        goto done;

    SetWindowTextW(edit, L"typed");
    NotifyReset();
    SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
    ok(g_ntf_calls >= 1, "Enter did not reach the toolbar's parent\n");
    if (g_ntf_calls >= 1)
    {
        UINT expected = ansi ? CBEN_ENDEDITA : CBEN_ENDEDITW;
        ok(g_ntf_codes[0] == expected, "first notification %d, expected %d\n", (int)g_ntf_codes[0], (int)expected);
    }

    NotifyReset();
    g_ntf_result = 0;
    SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 0);
    ok(g_ntf_calls >= 1, "Escape did not reach the toolbar's parent\n");
    ok(g_ntf_why == CBENF_ESCAPE, "Escape: iWhy %d, expected %d\n", g_ntf_why, CBENF_ESCAPE);

done:
    DestroyWindow(parent);
}

static void TestToolbarChildNotify(BOOL v6)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_USEREX_CLASSES };

    InitCommonControlsEx(&icc);
    TestChildNotifyUnicode(v6);
    TestChildNotifyAnsiParent();
    TestComboBoxExInToolbar(FALSE);
    TestComboBoxExInToolbar(TRUE);
}


START_TEST(toolbarv6)
{
    HANDLE hV5ActCtx;

    LoadLibraryW(L"comctl32.dll");

    TestVersionMessage();
    TestSetButtonSize();
    TestPadding();
    TestButtonSpacing();
    TestToolbarChildNotify(TRUE);

    hV5ActCtx = _CreateV5ActCtx();
    ok (hV5ActCtx != INVALID_HANDLE_VALUE, "CreateActCtxW failed\n");
    if (hV5ActCtx)
    {
        ULONG_PTR cookie;
        BOOL bActivated = ActivateActCtx(hV5ActCtx, &cookie);
        if (bActivated)
        {
            TestV5VersionMessage();
            TestToolbarChildNotify(FALSE);
            DeactivateActCtx(0, cookie);
        }
    }
}
