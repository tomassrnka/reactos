/*
 * PROJECT:         ReactOS Accessibility Control Panel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            dll/cpl/access/display.c
 * PURPOSE:         Display-related accessibility settings
 * COPYRIGHT:       Copyright 2004 Johannes Anderwald (johannes.anderwald@reactos.org)
 *                  Copyright 2007 Eric Kohl
 */

#include "access.h"

#include <wingdi.h>

#define ID_BLINK_TIMER 346

static VOID
UpdateCaretBlinkTimeReg(
    _In_ UINT uCaretBlinkTime)
{
    HKEY hKey;
    WCHAR szBuffer[12];

    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Control Panel\\Desktop",
                      0, KEY_SET_VALUE,
                      &hKey) != ERROR_SUCCESS)
    {
         return;
    }

    wsprintf(szBuffer, L"%d", uCaretBlinkTime);

    RegSetValueExW(hKey, L"CursorBlinkRate",
                   0, REG_SZ,
                   (CONST BYTE*)szBuffer,
                   (wcslen(szBuffer) + 1) * sizeof(WCHAR));

    RegCloseKey(hKey);
}

#define HIGHCONTRAST_KEY L"Control Panel\\Accessibility\\HighContrast"
#define HIGHCONTRAST_SCHEME_VALUE L"High Contrast Scheme"

/* A REG_SZ or REG_EXPAND_SZ value (setup writes the scheme names as REG_EXPAND_SZ) that fits cchOut with its terminator */
static BOOL
ReadSchemeName(HKEY hKey, LPCWSTR pszValue, LPWSTR pszOut, DWORD cchOut)
{
    WCHAR szBuffer[MAX_SCHEME_NAME + 1];
    DWORD dwType, cbData = sizeof(szBuffer) - sizeof(WCHAR);

    ZeroMemory(szBuffer, sizeof(szBuffer));
    if (RegQueryValueExW(hKey, pszValue, NULL, &dwType, (LPBYTE)szBuffer, &cbData) != ERROR_SUCCESS ||
        (dwType != REG_SZ && dwType != REG_EXPAND_SZ) ||
        wcslen(szBuffer) >= cchOut)
    {
        return FALSE;
    }
    wcscpy(pszOut, szBuffer);
    return TRUE;
}

/* Win32k keeps neither the scheme name nor a usable pointer to it, so the name lives in the registry */
VOID
LoadHighContrastScheme(PGLOBAL_DATA pGlobalData)
{
    HKEY hKey;

    pGlobalData->szHighContrastScheme[0] = UNICODE_NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, HIGHCONTRAST_KEY, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
    {
        if (!ReadSchemeName(hKey, HIGHCONTRAST_SCHEME_VALUE, pGlobalData->szHighContrastScheme,
                            ARRAYSIZE(pGlobalData->szHighContrastScheme)))
        {
            pGlobalData->szHighContrastScheme[0] = UNICODE_NULL;
        }
        RegCloseKey(hKey);
    }
    pGlobalData->highContrast.lpszDefaultScheme = pGlobalData->szHighContrastScheme;
}

static VOID
SaveHighContrastScheme(PGLOBAL_DATA pGlobalData)
{
    HKEY hKey;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, HIGHCONTRAST_KEY, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, HIGHCONTRAST_SCHEME_VALUE, 0, REG_SZ,
                       (const BYTE *)pGlobalData->szHighContrastScheme,
                       (DWORD)(wcslen(pGlobalData->szHighContrastScheme) + 1) * sizeof(WCHAR));
        RegCloseKey(hKey);
    }
}

static BOOL
GetSchemeLegacyName(HKEY hScheme, LPWSTR pszName, DWORD cchName)
{
    HKEY hSize;
    BOOL bFound;

    if (ReadSchemeName(hScheme, L"LegacyName", pszName, cchName) && pszName[0] != UNICODE_NULL)
        return TRUE;
    if (RegOpenKeyExW(hScheme, L"Sizes\\0", 0, KEY_QUERY_VALUE, &hSize) != ERROR_SUCCESS)
        return FALSE;
    bFound = ReadSchemeName(hSize, L"LegacyName", pszName, cchName) && pszName[0] != UNICODE_NULL;
    RegCloseKey(hSize);
    return bFound;
}

/* The color schemes are the ones Display Properties offers, under "New Schemes" */
static VOID
FillColorSchemeComboBox(HWND hwnd, LPCWSTR pszCurrent)
{
    WCHAR szKey[256], szName[MAX_SCHEME_NAME];
    HKEY hSchemes, hScheme;
    DWORD dwIndex, cchKey;
    INT iItem;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Appearance\\New Schemes", 0,
                      KEY_ENUMERATE_SUB_KEYS, &hSchemes) != ERROR_SUCCESS)
    {
        return;
    }

    for (dwIndex = 0; ; dwIndex++)
    {
        cchKey = ARRAYSIZE(szKey);
        if (RegEnumKeyExW(hSchemes, dwIndex, szKey, &cchKey, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (RegOpenKeyExW(hSchemes, szKey, 0, KEY_QUERY_VALUE, &hScheme) != ERROR_SUCCESS)
            continue;

        if (GetSchemeLegacyName(hScheme, szName, ARRAYSIZE(szName)) &&
            SendMessageW(hwnd, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)szName) == CB_ERR)
        {
            SendMessageW(hwnd, CB_ADDSTRING, 0, (LPARAM)szName);
        }
        RegCloseKey(hScheme);
    }
    RegCloseKey(hSchemes);

    iItem = (INT)SendMessageW(hwnd, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)pszCurrent);
    if (pszCurrent[0] != UNICODE_NULL && iItem != CB_ERR)
        SendMessageW(hwnd, CB_SETCURSEL, iItem, 0);
}


INT_PTR CALLBACK
HighContrastDlgProc(HWND hwndDlg,
                    UINT uMsg,
                    WPARAM wParam,
                    LPARAM lParam)
{
    PGLOBAL_DATA pGlobalData;

    pGlobalData = (PGLOBAL_DATA)GetWindowLongPtr(hwndDlg, DWLP_USER);

    switch (uMsg)
    {
        case WM_INITDIALOG:
            pGlobalData = (PGLOBAL_DATA)lParam;
            SetWindowLongPtr(hwndDlg, DWLP_USER, (LONG_PTR)pGlobalData);

            CheckDlgButton(hwndDlg,
                           IDC_CONTRAST_ACTIVATE_CHECK,
                           pGlobalData->highContrast.dwFlags & HCF_HOTKEYACTIVE ? BST_CHECKED : BST_UNCHECKED);

            FillColorSchemeComboBox(GetDlgItem(hwndDlg, IDC_CONTRAST_COMBO),
                                    pGlobalData->szHighContrastScheme);
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDOK:
                {
                    INT nSel = (INT)SendDlgItemMessage(hwndDlg, IDC_CONTRAST_COMBO, CB_GETCURSEL, 0, 0);

                    if (nSel != CB_ERR &&
                        SendDlgItemMessage(hwndDlg, IDC_CONTRAST_COMBO, CB_GETLBTEXTLEN, nSel, 0) <
                        (LRESULT)ARRAYSIZE(pGlobalData->szHighContrastScheme))
                    {
                        SendDlgItemMessage(hwndDlg, IDC_CONTRAST_COMBO, CB_GETLBTEXT, nSel,
                                           (LPARAM)pGlobalData->szHighContrastScheme);
                    }

                    if (IsDlgButtonChecked(hwndDlg, IDC_CONTRAST_ACTIVATE_CHECK) == BST_CHECKED)
                        pGlobalData->highContrast.dwFlags |= HCF_HOTKEYACTIVE;
                    else
                        pGlobalData->highContrast.dwFlags &= ~HCF_HOTKEYACTIVE;

                    EndDialog(hwndDlg, TRUE);
                    break;
                }

                case IDCANCEL:
                    EndDialog(hwndDlg, FALSE);
                    break;

                default:
                    break;
            }
            break;
    }

    return FALSE;
}


/* Property page dialog callback */
INT_PTR CALLBACK
DisplayPageProc(HWND hwndDlg,
                UINT uMsg,
                WPARAM wParam,
                LPARAM lParam)
{
    PGLOBAL_DATA pGlobalData;
    LPPSHNOTIFY lppsn;
    INT i;

    pGlobalData = (PGLOBAL_DATA)GetWindowLongPtr(hwndDlg, DWLP_USER);

    switch (uMsg)
    {
        case WM_INITDIALOG:
            pGlobalData = (PGLOBAL_DATA)((LPPROPSHEETPAGE)lParam)->lParam;
            if (pGlobalData == NULL)
                return FALSE;

            SetWindowLongPtr(hwndDlg, DWLP_USER, (LONG_PTR)pGlobalData);

            pGlobalData->fShowCaret = TRUE;
            GetWindowRect(GetDlgItem(hwndDlg, IDC_CURSOR_WIDTH_TEXT), &pGlobalData->rcCaret);
            ScreenToClient(hwndDlg, (LPPOINT)&pGlobalData->rcCaret.left);
            ScreenToClient(hwndDlg, (LPPOINT)&pGlobalData->rcCaret.right);
            CopyRect(&pGlobalData->rcOldCaret, &pGlobalData->rcCaret);

            pGlobalData->rcCaret.right = pGlobalData->rcCaret.left + pGlobalData->uCaretWidth;

            /* Set the checkbox */
            CheckDlgButton(hwndDlg,
                           IDC_CONTRAST_BOX,
                           pGlobalData->highContrast.dwFlags & HCF_HIGHCONTRASTON ? BST_CHECKED : BST_UNCHECKED);

            SendDlgItemMessage(hwndDlg, IDC_CURSOR_BLINK_TRACK, TBM_SETRANGE, (WPARAM)TRUE, (LPARAM)MAKELONG(0, 10));
            SendDlgItemMessage(hwndDlg, IDC_CURSOR_BLINK_TRACK, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)(12 - (pGlobalData->uCaretBlinkTime / 100)));

            SendDlgItemMessage(hwndDlg, IDC_CURSOR_WIDTH_TRACK, TBM_SETRANGE, (WPARAM)TRUE, (LPARAM)MAKELONG(0, 19));
            SendDlgItemMessage(hwndDlg, IDC_CURSOR_WIDTH_TRACK, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)(pGlobalData->uCaretWidth - 1));

            /* Start the blink timer */
            pGlobalData->uCaretBlinkTime = pGlobalData->uCaretBlinkTime >= 1200 ? -1 : pGlobalData->uCaretBlinkTime;
            if ((INT)pGlobalData->uCaretBlinkTime > 0)
            {
                SetTimer(hwndDlg, ID_BLINK_TIMER, pGlobalData->uCaretBlinkTime, NULL);
            }
            else
            {
                PostMessage(hwndDlg, WM_TIMER, ID_BLINK_TIMER, 0);
            }

            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDC_CONTRAST_BOX:
                    pGlobalData->highContrast.dwFlags ^= HCF_HIGHCONTRASTON;
                    PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                    break;

                case IDC_CONTRAST_BUTTON:
                    if (DialogBoxParam(hApplet,
                                       MAKEINTRESOURCE(IDD_CONTRASTOPTIONS),
                                       hwndDlg,
                                       HighContrastDlgProc,
                                       (LPARAM)pGlobalData))
                        PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                    break;

                default:
                    break;
            }
            break;

        case WM_HSCROLL:
            switch (GetWindowLongPtr((HWND) lParam, GWL_ID))
            {
                case IDC_CURSOR_BLINK_TRACK:
                    i = SendDlgItemMessage(hwndDlg, IDC_CURSOR_BLINK_TRACK, TBM_GETPOS, 0, 0);
                    pGlobalData->uCaretBlinkTime = (12 - (UINT)i) * 100;
                    KillTimer(hwndDlg, ID_BLINK_TIMER);
                    pGlobalData->uCaretBlinkTime = pGlobalData->uCaretBlinkTime >= 1200 ? -1 : pGlobalData->uCaretBlinkTime;
                    if ((INT)pGlobalData->uCaretBlinkTime > 0)
                    {
                        SetTimer(hwndDlg, ID_BLINK_TIMER, pGlobalData->uCaretBlinkTime, NULL);
                    }
                    else if (pGlobalData->fShowCaret)
                    {
                        SendMessage(hwndDlg, WM_TIMER, ID_BLINK_TIMER, 0);
                    }
                    PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                    break;

                case IDC_CURSOR_WIDTH_TRACK:
                    i = SendDlgItemMessage(hwndDlg, IDC_CURSOR_WIDTH_TRACK, TBM_GETPOS, 0, 0);
                    pGlobalData->uCaretWidth = (UINT)i + 1;
                    pGlobalData->rcCaret.right = pGlobalData->rcCaret.left + pGlobalData->uCaretWidth;
                    if (pGlobalData->fShowCaret)
                    {
                        HDC hDC = GetDC(hwndDlg);
                        HBRUSH hBrush = GetSysColorBrush(COLOR_BTNTEXT);
                        FillRect(hDC, &pGlobalData->rcCaret, hBrush);
                        ReleaseDC(hwndDlg, hDC);
                    }
                    else
                    {
                        InvalidateRect(hwndDlg, &pGlobalData->rcOldCaret, TRUE);
                    }
                    PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                    break;
            }
            break;

        case WM_TIMER:
            if (wParam == ID_BLINK_TIMER)
            {
                if (pGlobalData->fShowCaret)
                {
                    HDC hDC = GetDC(hwndDlg);
                    HBRUSH hBrush = GetSysColorBrush(COLOR_BTNTEXT);
                    FillRect(hDC, &pGlobalData->rcCaret, hBrush);
                    ReleaseDC(hwndDlg, hDC);
                }
                else
                {
                    InvalidateRect(hwndDlg, &pGlobalData->rcOldCaret, TRUE);
                }

                pGlobalData->fShowCaret = !pGlobalData->fShowCaret;
            }
            break;

        case WM_NOTIFY:
            lppsn = (LPPSHNOTIFY)lParam;
            if (lppsn->hdr.code == PSN_APPLY)
            {
                if (SetCaretBlinkTime(pGlobalData->uCaretBlinkTime))
                {
                    UpdateCaretBlinkTimeReg(pGlobalData->uCaretBlinkTime);
                }

                SystemParametersInfo(SPI_SETCARETWIDTH,
                                     0,
                                     IntToPtr(pGlobalData->uCaretWidth),
                                     SPIF_UPDATEINIFILE | SPIF_SENDCHANGE /*0*/);
                SystemParametersInfo(SPI_SETHIGHCONTRAST,
                                     sizeof(HIGHCONTRAST),
                                     &pGlobalData->highContrast,
                                     SPIF_UPDATEINIFILE | SPIF_SENDCHANGE /*0*/);
                SaveHighContrastScheme(pGlobalData);
                return TRUE;
            }
            break;

        case WM_DESTROY:
            KillTimer(hwndDlg, ID_BLINK_TIMER);
            break;
    }

    return FALSE;
}
