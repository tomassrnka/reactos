/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the caption of a wizard that is made of property sheet pages
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "wine/test.h"
#include <windows.h>
#include <commctrl.h>
#include <prsht.h>

#define PAGE_COUNT 3
#define WM_SAMPLE (WM_APP + 1)

typedef struct
{
    PCWSTR PageTitle[PAGE_COUNT];   /* NULL: the page has no title of its own */
    PCWSTR SetTitle;                /* sent with PSM_SETTITLEW when the wizard starts, or NULL */
    PCWSTR Expected[PAGE_COUNT];    /* caption seen on each page */
} TITLE_CASE;

static const TITLE_CASE TitleCases[] =
{
    /* No page has a title: the one set with PSM_SETTITLEW stays */
    { { NULL, NULL, NULL }, L"SetTitleCap", { L"SetTitleCap", L"SetTitleCap", L"SetTitleCap" } },
    /* Only the first page has a title: it stays on the pages that have none */
    { { L"Page1Title", NULL, NULL }, NULL, { L"Page1Title", L"Page1Title", L"Page1Title" } },
    /* An empty title does not replace the caption either */
    { { L"P1", L"", L"P3" }, NULL, { L"P1", L"P1", L"P3" } },
    { { L"P1", L"P2", L"P3" }, NULL, { L"P1", L"P2", L"P3" } },
};

static const TITLE_CASE *CurrentCase;
static WCHAR SeenCaptions[PAGE_COUNT][128];
static INT SeenCount;

static
INT_PTR
CALLBACK
PageProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    HWND hwndSheet = GetParent(hwnd);

    if (uMsg == WM_NOTIFY && ((NMHDR *)lParam)->code == PSN_SETACTIVE)
    {
        /* Look at the caption once the sheet has finished switching to this page */
        PostMessageW(hwnd, WM_SAMPLE, 0, 0);
        SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, 0);
        return TRUE;
    }
    if (uMsg == WM_SAMPLE)
    {
        if (SeenCount < PAGE_COUNT)
        {
            GetWindowTextW(hwndSheet, SeenCaptions[SeenCount], _countof(SeenCaptions[0]));
            SeenCount++;
        }
        if (SeenCount < PAGE_COUNT)
            PostMessageW(hwndSheet, PSM_SETCURSEL, SeenCount, 0);
        else
            PostMessageW(hwndSheet, PSM_PRESSBUTTON, PSBTN_CANCEL, 0);
        return TRUE;
    }
    return FALSE;
}

static
INT
CALLBACK
SheetProc(
    _In_ HWND hwnd,
    _In_ UINT uMsg,
    _In_ LPARAM lParam)
{
    if (uMsg == PSCB_INITIALIZED && CurrentCase->SetTitle)
        SendMessageW(hwnd, PSM_SETTITLEW, 0, (LPARAM)CurrentCase->SetTitle);
    return 0;
}

#include <pshpack2.h>
typedef struct
{
    DLGTEMPLATE Template;
    WORD Menu;
    WORD Class;
    WORD Title;
} PAGE_TEMPLATE;
#include <poppack.h>

START_TEST(propsheet_title)
{
    UINT CaseIndex;
    PAGE_TEMPLATE PageTemplate;
    INITCOMMONCONTROLSEX Controls = { sizeof(Controls), ICC_WIN95_CLASSES };

    InitCommonControlsEx(&Controls);
    ZeroMemory(&PageTemplate, sizeof(PageTemplate));
    PageTemplate.Template.style = WS_CHILD | DS_CONTROL;
    PageTemplate.Template.cx = 100;
    PageTemplate.Template.cy = 60;

    for (CaseIndex = 0; CaseIndex < _countof(TitleCases); CaseIndex++)
    {
        PROPSHEETPAGEW Pages[PAGE_COUNT];
        PROPSHEETHEADERW Header;
        INT PageIndex;

        CurrentCase = &TitleCases[CaseIndex];
        SeenCount = 0;
        ZeroMemory(Pages, sizeof(Pages));
        for (PageIndex = 0; PageIndex < PAGE_COUNT; PageIndex++)
        {
            Pages[PageIndex].dwSize = sizeof(Pages[PageIndex]);
            Pages[PageIndex].dwFlags = PSP_DLGINDIRECT;
            Pages[PageIndex].pResource = &PageTemplate.Template;
            Pages[PageIndex].pfnDlgProc = PageProc;
            if (CurrentCase->PageTitle[PageIndex])
            {
                Pages[PageIndex].dwFlags |= PSP_USETITLE;
                Pages[PageIndex].pszTitle = CurrentCase->PageTitle[PageIndex];
            }
        }
        ZeroMemory(&Header, sizeof(Header));
        Header.dwSize = sizeof(Header);
        Header.dwFlags = PSH_PROPSHEETPAGE | PSH_USECALLBACK | PSH_NOAPPLYNOW | PSH_WIZARD;
        Header.pszCaption = L"HeaderCaption";
        Header.nPages = PAGE_COUNT;
        Header.ppsp = Pages;
        Header.pfnCallback = SheetProc;
        Header.hInstance = GetModuleHandleW(NULL);
        PropertySheetW(&Header);

        ok(SeenCount == PAGE_COUNT, "case %u: saw %d pages\n", CaseIndex, SeenCount);
        for (PageIndex = 0; PageIndex < SeenCount; PageIndex++)
        {
            ok(!wcscmp(SeenCaptions[PageIndex], CurrentCase->Expected[PageIndex]),
               "case %u, page %d: expected %s, got %s\n", CaseIndex, PageIndex,
               wine_dbgstr_w(CurrentCase->Expected[PageIndex]),
               wine_dbgstr_w(SeenCaptions[PageIndex]));
        }
    }
}
