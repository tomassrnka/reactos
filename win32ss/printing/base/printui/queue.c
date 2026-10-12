/*
 * PROJECT:     ReactOS Printer User Interface
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Printer queue window, printer properties and error reporting
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define UNICODE
#define _UNICODE
#include <stdarg.h>

#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <winnls.h>
#include <winspool.h>
#include <commctrl.h>
#include <prsht.h>
#include <shlwapi.h>
#include <strsafe.h>

#include "wine/debug.h"
#include "printui_private.h"
#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(printui);

#define QUEUE_CLASS     L"PrintUIQueue"
#define REFRESH_TIMER   1
#define REFRESH_MS      2000

enum
{
    COL_DOCUMENT,
    COL_STATUS,
    COL_OWNER,
    COL_PAGES,
    COL_SIZE,
    COL_SUBMITTED,
    COL_COUNT
};

typedef struct _QUEUE_WINDOW
{
    HWND hwnd;
    HWND hList;
    HWND hStatus;
    HANDLE hPrinter;
    context_t *cx;
    WCHAR szPrinter[MAX_PATH];
} QUEUE_WINDOW, *PQUEUE_WINDOW;

static HINSTANCE printui_hinst;

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        printui_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}

static void load_string(UINT id, LPWSTR buffer, int cch)
{
    if (!LoadStringW(printui_hinst, id, buffer, cch))
        buffer[0] = UNICODE_NULL;
}

/*
 * Looks for the /q flag the way parse_rundll reads the command line: letters after a slash are
 * commands and flags; an option letter takes the rest of its argument or the next argument as
 * its value, and some commands take the following letter as a subcommand.
 */
BOOL printui_is_quiet(int argc, LPWSTR *argv)
{
    static const WCHAR Options[] = L"abcfhjlmntrv";
    static const WCHAR TwoLetterCommands[] = L"dgiSX";
    LPCWSTR p;
    int i;

    for (i = 0; i < argc; i++)
    {
        if (argv[i][0] != L'/')
            continue;

        for (p = &argv[i][1]; *p; p++)
        {
            if (*p == L'q')
                return TRUE;

            if (wcschr(Options, *p))
            {
                if (!p[1])
                    i++;
                break;
            }

            if (wcschr(TwoLetterCommands, *p) && p[1])
                p++;
        }
    }

    return FALSE;
}

void printui_show_error(context_t *cx, DWORD error)
{
    WCHAR szCaption[64], szFormat[128], szSystem[256], szText[512];

    if (cx->flags[FLAG_Q])
        return;

    load_string(IDS_CAPTION, szCaption, _countof(szCaption));
    if (error == ERROR_INVALID_PARAMETER)
    {
        load_string(IDS_BAD_ARGUMENTS, szText, _countof(szText));
    }
    else if (error == ERROR_NOT_SUPPORTED)
    {
        load_string(IDS_NOT_SUPPORTED, szText, _countof(szText));
    }
    else
    {
        load_string(IDS_FAILED, szFormat, _countof(szFormat));
        if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0,
                            szSystem, _countof(szSystem), NULL))
        {
            StringCchPrintfW(szSystem, _countof(szSystem), L"0x%08lx", error);
        }
        StringCchPrintfW(szText, _countof(szText), szFormat, szSystem);
    }
    MessageBoxW(cx->hWnd, szText, szCaption, MB_OK | MB_ICONERROR);
}

static void printer_status_text(DWORD dwStatus, LPWSTR buffer, int cch)
{
    static const DWORD BusyStatus = PRINTER_STATUS_BUSY | PRINTER_STATUS_PROCESSING | PRINTER_STATUS_IO_ACTIVE |
                                    PRINTER_STATUS_WAITING | PRINTER_STATUS_WARMING_UP | PRINTER_STATUS_INITIALIZING;
    UINT id;

    if (!dwStatus)
        id = IDS_STATUS_READY;
    else if (dwStatus & PRINTER_STATUS_PENDING_DELETION)
        id = IDS_STATUS_DELETING;
    else if (dwStatus & PRINTER_STATUS_PAUSED)
        id = IDS_STATUS_PAUSED;
    else if (dwStatus & PRINTER_STATUS_OFFLINE)
        id = IDS_STATUS_OFFLINE;
    else if (dwStatus & PRINTER_STATUS_ERROR)
        id = IDS_STATUS_ERROR;
    else if (dwStatus & PRINTER_STATUS_PRINTING)
        id = IDS_STATUS_PRINTING;
    else if (!(dwStatus & ~BusyStatus))
        id = IDS_STATUS_BUSY;
    else
        id = IDS_STATUS_ATTENTION;
    load_string(id, buffer, cch);
}

static void job_status_text(const JOB_INFO_2W *pJob, LPWSTR buffer, int cch)
{
    UINT id = 0;

    if (pJob->pStatus && pJob->pStatus[0])
    {
        StringCchCopyW(buffer, cch, pJob->pStatus);
        return;
    }

    if (pJob->Status & (JOB_STATUS_DELETING | JOB_STATUS_DELETED))
        id = IDS_STATUS_DELETING;
    else if (pJob->Status & JOB_STATUS_PAUSED)
        id = IDS_STATUS_PAUSED;
    else if (pJob->Status & (JOB_STATUS_ERROR | JOB_STATUS_BLOCKED_DEVQ))
        id = IDS_STATUS_ERROR;
    else if (pJob->Status & JOB_STATUS_OFFLINE)
        id = IDS_STATUS_OFFLINE;
    else if (pJob->Status & JOB_STATUS_PRINTING)
        id = IDS_STATUS_PRINTING;
    else if (pJob->Status & JOB_STATUS_SPOOLING)
        id = IDS_STATUS_SPOOLING;

    if (id)
        load_string(id, buffer, cch);
    else
        buffer[0] = UNICODE_NULL;
}

static PVOID get_printer_info(HANDLE hPrinter, DWORD level)
{
    DWORD cbNeeded = 0;
    PVOID pInfo;

    GetPrinterW(hPrinter, level, NULL, 0, &cbNeeded);
    if (!cbNeeded)
        return NULL;

    pInfo = HeapAlloc(GetProcessHeap(), 0, cbNeeded);
    if (pInfo && !GetPrinterW(hPrinter, level, pInfo, cbNeeded, &cbNeeded))
    {
        HeapFree(GetProcessHeap(), 0, pInfo);
        pInfo = NULL;
    }
    return pInfo;
}

static BOOL is_default_printer(LPCWSTR pszPrinter)
{
    WCHAR szDefault[MAX_PATH];
    DWORD cch = _countof(szDefault);

    return GetDefaultPrinterW(szDefault, &cch) && !lstrcmpiW(szDefault, pszPrinter);
}

static void queue_set_title(PQUEUE_WINDOW pQueue, DWORD dwStatus)
{
    WCHAR szTitle[MAX_PATH + 64], szStatus[64];

    StringCchCopyW(szTitle, _countof(szTitle), pQueue->szPrinter);
    if (dwStatus & (PRINTER_STATUS_PAUSED | PRINTER_STATUS_OFFLINE | PRINTER_STATUS_ERROR | PRINTER_STATUS_PENDING_DELETION))
    {
        printer_status_text(dwStatus, szStatus, _countof(szStatus));
        StringCchCatW(szTitle, _countof(szTitle), L" - ");
        StringCchCatW(szTitle, _countof(szTitle), szStatus);
    }
    SetWindowTextW(pQueue->hwnd, szTitle);
}

static void queue_refresh(PQUEUE_WINDOW pQueue)
{
    PJOB_INFO_2W pJobs = NULL;
    PPRINTER_INFO_2W pInfo2;
    DWORD cbNeeded = 0, cReturned = 0, i, dwSelected = 0, dwError = ERROR_SUCCESS;
    WCHAR szText[384], szFormat[64], szSystem[256];
    LVITEMW item;
    int iSel;

    iSel = ListView_GetNextItem(pQueue->hList, -1, LVNI_SELECTED);
    if (iSel >= 0)
    {
        item.mask = LVIF_PARAM;
        item.iItem = iSel;
        item.iSubItem = 0;
        if (ListView_GetItem(pQueue->hList, &item))
            dwSelected = (DWORD)item.lParam;
    }

    /* Keep the last title when the printer cannot be queried */
    pInfo2 = get_printer_info(pQueue->hPrinter, 2);
    if (pInfo2)
    {
        queue_set_title(pQueue, pInfo2->Status);
        HeapFree(GetProcessHeap(), 0, pInfo2);
    }

    if (!EnumJobsW(pQueue->hPrinter, 0, MAXDWORD, 2, NULL, 0, &cbNeeded, &cReturned))
    {
        dwError = GetLastError();
        if (dwError == ERROR_INSUFFICIENT_BUFFER)
        {
            dwError = ERROR_SUCCESS;
            pJobs = HeapAlloc(GetProcessHeap(), 0, cbNeeded);
            if (!pJobs)
                dwError = ERROR_NOT_ENOUGH_MEMORY;
            else if (!EnumJobsW(pQueue->hPrinter, 0, MAXDWORD, 2, (LPBYTE)pJobs, cbNeeded, &cbNeeded, &cReturned))
                dwError = GetLastError();
        }
    }

    /* Keep the last list when the queue cannot be read, and say why */
    if (dwError != ERROR_SUCCESS)
    {
        load_string(IDS_QUEUE_UNAVAILABLE, szFormat, _countof(szFormat));
        if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, dwError, 0,
                            szSystem, _countof(szSystem), NULL))
        {
            StringCchPrintfW(szSystem, _countof(szSystem), L"0x%08lx", dwError);
        }
        StringCchPrintfW(szText, _countof(szText), szFormat, szSystem);
        SendMessageW(pQueue->hStatus, SB_SETTEXTW, 0, (LPARAM)szText);
        if (pJobs)
            HeapFree(GetProcessHeap(), 0, pJobs);
        return;
    }
    if (!pJobs)
        cReturned = 0;

    SendMessageW(pQueue->hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(pQueue->hList);

    for (i = 0; i < cReturned; i++)
    {
        SYSTEMTIME stLocal;
        int iItem, cch;

        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = i;
        item.iSubItem = 0;
        item.pszText = pJobs[i].pDocument ? pJobs[i].pDocument : L"";
        item.lParam = pJobs[i].JobId;
        iItem = ListView_InsertItem(pQueue->hList, &item);
        if (iItem < 0)
            continue;

        job_status_text(&pJobs[i], szText, _countof(szText));
        ListView_SetItemText(pQueue->hList, iItem, COL_STATUS, szText);

        ListView_SetItemText(pQueue->hList, iItem, COL_OWNER, pJobs[i].pUserName ? pJobs[i].pUserName : L"");

        if ((pJobs[i].Status & JOB_STATUS_PRINTING) && pJobs[i].TotalPages)
        {
            load_string(IDS_PAGES_OF, szFormat, _countof(szFormat));
            StringCchPrintfW(szText, _countof(szText), szFormat, pJobs[i].PagesPrinted, pJobs[i].TotalPages);
        }
        else if (pJobs[i].TotalPages)
        {
            StringCchPrintfW(szText, _countof(szText), L"%lu", pJobs[i].TotalPages);
        }
        else
        {
            szText[0] = UNICODE_NULL;
        }
        ListView_SetItemText(pQueue->hList, iItem, COL_PAGES, szText);

        StrFormatByteSizeW(pJobs[i].Size, szText, _countof(szText));
        ListView_SetItemText(pQueue->hList, iItem, COL_SIZE, szText);

        szText[0] = UNICODE_NULL;
        if (SystemTimeToTzSpecificLocalTime(NULL, &pJobs[i].Submitted, &stLocal))
        {
            cch = GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &stLocal, NULL, szText, _countof(szText));
            if (cch > 0 && cch < _countof(szText) - 1)
            {
                szText[cch - 1] = L' ';
                GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &stLocal, NULL, &szText[cch], _countof(szText) - cch);
            }
        }
        ListView_SetItemText(pQueue->hList, iItem, COL_SUBMITTED, szText);

        if (pJobs[i].JobId == dwSelected)
            ListView_SetItemState(pQueue->hList, iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    SendMessageW(pQueue->hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(pQueue->hList, NULL, TRUE);

    load_string(IDS_DOCUMENTS_COUNT, szFormat, _countof(szFormat));
    StringCchPrintfW(szText, _countof(szText), szFormat, cReturned);
    SendMessageW(pQueue->hStatus, SB_SETTEXTW, 0, (LPARAM)szText);

    if (pJobs)
        HeapFree(GetProcessHeap(), 0, pJobs);
}

static void queue_cancel_document(PQUEUE_WINDOW pQueue)
{
    LVITEMW item;
    int iSel;

    iSel = ListView_GetNextItem(pQueue->hList, -1, LVNI_SELECTED);
    if (iSel < 0)
        return;

    item.mask = LVIF_PARAM;
    item.iItem = iSel;
    item.iSubItem = 0;
    if (!ListView_GetItem(pQueue->hList, &item))
        return;

    if (!SetJobW(pQueue->hPrinter, (DWORD)item.lParam, 0, NULL, JOB_CONTROL_DELETE))
    {
        context_t cxError = *pQueue->cx;
        cxError.hWnd = pQueue->hwnd;
        printui_show_error(&cxError, GetLastError());
    }
    queue_refresh(pQueue);
}

static void set_default_printer(context_t *cx, HWND hwnd, LPCWSTR pszPrinter)
{
    if (SetDefaultPrinterW(pszPrinter))
    {
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"windows", SMTO_ABORTIFHUNG, 1000, NULL);
    }
    else
    {
        context_t cxError = *cx;
        cxError.hWnd = hwnd;
        printui_show_error(&cxError, GetLastError());
    }
}

/* General page of the printer properties */

typedef struct _GENERAL_PAGE
{
    LPCWSTR pszPrinter;
    HANDLE hPrinter;
} GENERAL_PAGE, *PGENERAL_PAGE;

static void general_page_init(HWND hwndDlg, PGENERAL_PAGE pPage)
{
    PPRINTER_INFO_2W pInfo2 = get_printer_info(pPage->hPrinter, 2);
    WCHAR szText[64];

    SetDlgItemTextW(hwndDlg, IDC_NAME, pPage->pszPrinter);
    if (!pInfo2)
    {
        load_string(IDS_INFO_UNAVAILABLE, szText, _countof(szText));
        SetDlgItemTextW(hwndDlg, IDC_STATUS, szText);
        return;
    }

    SetDlgItemTextW(hwndDlg, IDC_LOCATION, pInfo2->pLocation ? pInfo2->pLocation : L"");
    SetDlgItemTextW(hwndDlg, IDC_COMMENT, pInfo2->pComment ? pInfo2->pComment : L"");
    SetDlgItemTextW(hwndDlg, IDC_MODEL, pInfo2->pDriverName ? pInfo2->pDriverName : L"");
    SetDlgItemTextW(hwndDlg, IDC_PORT, pInfo2->pPortName ? pInfo2->pPortName : L"");
    printer_status_text(pInfo2->Status, szText, _countof(szText));
    SetDlgItemTextW(hwndDlg, IDC_STATUS, szText);
    SetDlgItemInt(hwndDlg, IDC_DOCUMENTS, pInfo2->cJobs, FALSE);

    HeapFree(GetProcessHeap(), 0, pInfo2);
}

static INT_PTR CALLBACK general_page_proc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_INITDIALOG)
    {
        general_page_init(hwndDlg, (PGENERAL_PAGE)((LPPROPSHEETPAGEW)lParam)->lParam);
        return TRUE;
    }
    return FALSE;
}

/* Returns a Win32 error code */
static DWORD show_properties(HWND hwndOwner, LPCWSTR pszPrinter, HANDLE hPrinter)
{
    PROPSHEETHEADERW psh = { sizeof(psh) };
    PROPSHEETPAGEW psp = { sizeof(psp) };
    HPROPSHEETPAGE hPage;
    GENERAL_PAGE page = { pszPrinter, hPrinter };
    WCHAR szFormat[64], szTitle[MAX_PATH + 64];
    DWORD dwError;

    psp.hInstance = printui_hinst;
    psp.pszTemplate = MAKEINTRESOURCEW(IDD_GENERAL);
    psp.pfnDlgProc = general_page_proc;
    psp.lParam = (LPARAM)&page;
    hPage = CreatePropertySheetPageW(&psp);
    if (!hPage)
    {
        dwError = GetLastError();
        return dwError ? dwError : ERROR_NOT_ENOUGH_MEMORY;
    }

    load_string(IDS_PROPERTIES_TITLE, szFormat, _countof(szFormat));
    StringCchPrintfW(szTitle, _countof(szTitle), szFormat, pszPrinter);

    psh.dwFlags = PSH_NOAPPLYNOW;
    psh.hwndParent = hwndOwner;
    psh.hInstance = printui_hinst;
    psh.pszCaption = szTitle;
    psh.nPages = 1;
    psh.phpage = &hPage;
    SetLastError(ERROR_SUCCESS);
    if (PropertySheetW(&psh) < 0)
    {
        dwError = GetLastError();
        return dwError ? dwError : ERROR_NOT_ENOUGH_MEMORY;
    }
    return ERROR_SUCCESS;
}

/* Queue window */

static LRESULT CALLBACK queue_wnd_proc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PQUEUE_WINDOW pQueue = (PQUEUE_WINDOW)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (uMsg)
    {
        case WM_NCCREATE:
            pQueue = ((LPCREATESTRUCTW)lParam)->lpCreateParams;
            pQueue->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pQueue);
            break;

        case WM_CREATE:
        {
            static const UINT ColumnIds[COL_COUNT] = { IDS_COL_DOCUMENT, IDS_COL_STATUS, IDS_COL_OWNER,
                                                       IDS_COL_PAGES, IDS_COL_SIZE, IDS_COL_SUBMITTED };
            static const int ColumnWidths[COL_COUNT] = { 180, 80, 80, 50, 70, 120 };
            WCHAR szColumn[64];
            LVCOLUMNW column;
            UINT i;

            pQueue->hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                            0, 0, 0, 0, hwnd, NULL, printui_hinst, NULL);
            pQueue->hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                              0, 0, 0, 0, hwnd, NULL, printui_hinst, NULL);
            if (!pQueue->hList || !pQueue->hStatus)
                return -1;

            ListView_SetExtendedListViewStyle(pQueue->hList, LVS_EX_FULLROWSELECT);
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            column.pszText = szColumn;
            for (i = 0; i < COL_COUNT; i++)
            {
                load_string(ColumnIds[i], szColumn, _countof(szColumn));
                column.cx = ColumnWidths[i];
                column.iSubItem = i;
                ListView_InsertColumn(pQueue->hList, i, &column);
            }

            queue_refresh(pQueue);
            SetTimer(hwnd, REFRESH_TIMER, REFRESH_MS, NULL);
            return 0;
        }

        case WM_SIZE:
        {
            RECT rcStatus;
            SendMessageW(pQueue->hStatus, WM_SIZE, 0, 0);
            GetWindowRect(pQueue->hStatus, &rcStatus);
            MoveWindow(pQueue->hList, 0, 0, LOWORD(lParam), HIWORD(lParam) - (rcStatus.bottom - rcStatus.top), TRUE);
            return 0;
        }

        case WM_SETFOCUS:
            SetFocus(pQueue->hList);
            return 0;

        case WM_TIMER:
            if (wParam == REFRESH_TIMER)
                queue_refresh(pQueue);
            return 0;

        case WM_INITMENUPOPUP:
        {
            HMENU hMenu = (HMENU)wParam;
            BOOL bSelected = ListView_GetNextItem(pQueue->hList, -1, LVNI_SELECTED) >= 0;

            CheckMenuItem(hMenu, IDM_SETDEFAULT, MF_BYCOMMAND |
                          (is_default_printer(pQueue->szPrinter) ? MF_CHECKED : MF_UNCHECKED));
            EnableMenuItem(hMenu, IDM_CANCELDOC, MF_BYCOMMAND | (bSelected ? MF_ENABLED : MF_GRAYED));
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDM_SETDEFAULT:
                    set_default_printer(pQueue->cx, hwnd, pQueue->szPrinter);
                    return 0;
                case IDM_PROPERTIES:
                {
                    DWORD dwError = show_properties(hwnd, pQueue->szPrinter, pQueue->hPrinter);
                    if (dwError != ERROR_SUCCESS)
                    {
                        context_t cxError = *pQueue->cx;
                        cxError.hWnd = hwnd;
                        printui_show_error(&cxError, dwError);
                    }
                    queue_refresh(pQueue);
                    return 0;
                }
                case IDM_CANCELDOC:
                    queue_cancel_document(pQueue);
                    return 0;
                case IDM_REFRESH:
                    queue_refresh(pQueue);
                    return 0;
                case IDM_CLOSE:
                    DestroyWindow(hwnd);
                    return 0;
            }
            break;

        case WM_DESTROY:
            KillTimer(hwnd, REFRESH_TIMER);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static HANDLE open_printer(context_t *cx, LPCWSTR *ppszPrinter)
{
    HANDLE hPrinter = NULL;
    LPWSTR pszPrinter = cx->options[OPT_N];
    int i;

    *ppszPrinter = NULL;
    if (!pszPrinter || !pszPrinter[0])
    {
        printui_show_error(cx, ERROR_INVALID_PARAMETER);
        return NULL;
    }

    /* Only a local printer name is supported: no server, file or other option, and no flag but /q */
    for (i = 0; i < OPT_MAX; i++)
    {
        if (i != OPT_N && cx->options[i])
        {
            printui_show_error(cx, ERROR_NOT_SUPPORTED);
            return NULL;
        }
    }
    for (i = 0; i < FLAG_MAX; i++)
    {
        if (i != FLAG_Q && cx->flags[i])
        {
            printui_show_error(cx, ERROR_NOT_SUPPORTED);
            return NULL;
        }
    }

    if (!OpenPrinterW(pszPrinter, &hPrinter, NULL))
    {
        printui_show_error(cx, GetLastError());
        return NULL;
    }

    *ppszPrinter = pszPrinter;
    return hPrinter;
}

/* /o: open the queue window of the printer given with /n */
BOOL printui_open_queue(context_t *cx)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    WNDCLASSEXW wc = { sizeof(wc) };
    QUEUE_WINDOW queue = { 0 };
    LPCWSTR pszPrinter;
    HACCEL hAccel;
    HMENU hMenu;
    HWND hwnd;
    MSG msg;

    queue.hPrinter = open_printer(cx, &pszPrinter);
    if (!queue.hPrinter)
        return FALSE;
    StringCchCopyW(queue.szPrinter, _countof(queue.szPrinter), pszPrinter);
    queue.cx = cx;

    InitCommonControlsEx(&icc);

    wc.lpfnWndProc = queue_wnd_proc;
    wc.hInstance = printui_hinst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = QUEUE_CLASS;
    wc.hIcon = LoadIconW(GetModuleHandleW(L"shell32.dll"), MAKEINTRESOURCEW(17));
    RegisterClassExW(&wc);

    hMenu = LoadMenuW(printui_hinst, MAKEINTRESOURCEW(IDM_QUEUE));
    hwnd = CreateWindowExW(0, QUEUE_CLASS, queue.szPrinter, WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 640, 300,
                           NULL, hMenu, printui_hinst, &queue);
    if (!hwnd)
    {
        DWORD dwError = GetLastError();
        if (hMenu)
            DestroyMenu(hMenu);
        ClosePrinter(queue.hPrinter);
        printui_show_error(cx, dwError ? dwError : ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    ShowWindow(hwnd, cx->nCmdShow ? (int)cx->nCmdShow : SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    hAccel = LoadAcceleratorsW(printui_hinst, MAKEINTRESOURCEW(IDA_QUEUE));
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        if (hAccel && TranslateAcceleratorW(hwnd, hAccel, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ClosePrinter(queue.hPrinter);
    return TRUE;
}

/* /p: show the properties of the printer given with /n */
BOOL printui_printer_properties(context_t *cx)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    LPCWSTR pszPrinter;
    HANDLE hPrinter;
    DWORD dwError;

    hPrinter = open_printer(cx, &pszPrinter);
    if (!hPrinter)
        return FALSE;

    InitCommonControlsEx(&icc);
    dwError = show_properties(cx->hWnd, pszPrinter, hPrinter);
    ClosePrinter(hPrinter);
    if (dwError != ERROR_SUCCESS)
    {
        printui_show_error(cx, dwError);
        return FALSE;
    }
    return TRUE;
}
