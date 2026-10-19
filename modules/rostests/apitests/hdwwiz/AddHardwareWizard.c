/*
 * PROJECT:     ReactOS hdwwiz.cpl API Tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Drives the Add Hardware wizard through its window messages: the page
 *              order, and a search for new hardware that ends without failing
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <commctrl.h>
#include <prsht.h>

/* dll/cpl/hdwwiz/resource.h */
#define IDS_SEARCHFAILEDTEXT  1135
#define IDC_STATUSTEXT        312
#define IDC_AUTOINSTALL       316
#define IDC_MANUALLYINST      317
#define IDC_DETECTEDHWLIST    323
#define IDC_HWTYPESLIST       325
#define IDC_CONNECTED         504

#define ID_BACK 0x3023
#define ID_NEXT 0x3024

typedef VOID (WINAPI *PADDHARDWAREWIZARD)(HWND, LPWSTR);

static DWORD dwWizardThread;

static DWORD WINAPI
WizardThread(LPVOID Param)
{
    PADDHARDWAREWIZARD pAddHardwareWizard = (PADDHARDWAREWIZARD)Param;

    pAddHardwareWizard(NULL, NULL);
    return 0;
}

/* Every message to the wizard is bounded, so a hung wizard fails the test instead of hanging it */
static LRESULT
SendBounded(HWND hwnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    DWORD_PTR Result = 0;

    if (!SendMessageTimeoutW(hwnd, Msg, wParam, lParam, SMTO_ABORTIFHUNG, 5000, &Result))
        return 0;
    return (LRESULT)Result;
}

static BOOL CALLBACK
FindSheetProc(HWND hwnd, LPARAM lParam)
{
    WCHAR szClass[16];

    if (IsWindowVisible(hwnd) &&
        GetClassNameW(hwnd, szClass, ARRAYSIZE(szClass)) && !wcscmp(szClass, L"#32770") &&
        SendBounded(hwnd, PSM_GETCURRENTPAGEHWND, 0, 0) != 0)
    {
        *(HWND *)lParam = hwnd;
        return FALSE;
    }
    return TRUE;
}

/* The sheet is the visible dialog of the wizard thread that has a current page */
static HWND
FindSheet(VOID)
{
    HWND hwnd = NULL;
    int i;

    for (i = 0; i < 100 && !hwnd; i++)
    {
        EnumThreadWindows(dwWizardThread, FindSheetProc, (LPARAM)&hwnd);
        if (!hwnd)
            Sleep(100);
    }
    return hwnd;
}

static HWND
CurPage(HWND hSheet)
{
    return (HWND)SendBounded(hSheet, PSM_GETCURRENTPAGEHWND, 0, 0);
}

/* Wait until the current page has one of the controls; NULL after the deadline */
static HWND
WaitForPage(HWND hSheet, const int *Ids, int Count, DWORD dwTimeout, DWORD *pdwWaited)
{
    DWORD dwWaited;
    HWND hPage;
    int i;

    for (dwWaited = 0; ; dwWaited += 100)
    {
        hPage = CurPage(hSheet);
        for (i = 0; hPage && i < Count; i++)
        {
            if (GetDlgItem(hPage, Ids[i]))
            {
                if (pdwWaited)
                    *pdwWaited = dwWaited;
                return hPage;
            }
        }
        if (dwWaited >= dwTimeout)
            return NULL;
        Sleep(100);
    }
}

static BOOL
ButtonEnabled(HWND hSheet, int Id)
{
    HWND hButton = GetDlgItem(hSheet, Id);
    return hButton && IsWindowVisible(hButton) && IsWindowEnabled(hButton);
}

static VOID
Press(HWND hSheet, int Button)
{
    PostMessageW(hSheet, PSM_PRESSBUTTON, Button, 0);
}

static VOID
DriveWizard(HWND hSheet, PCWSTR pszFailed)
{
    static const int MethodPage[] = { IDC_AUTOINSTALL };
    static const int AfterSearch[] = { IDC_DETECTEDHWLIST, IDC_HWTYPESLIST, IDC_CONNECTED };
    static const int TypesPage[] = { IDC_HWTYPESLIST };
    WCHAR szText[512];
    DWORD dwWaited = 0;
    HWND hPage;

    /* Welcome page */
    ok(ButtonEnabled(hSheet, ID_NEXT) && !ButtonEnabled(hSheet, ID_BACK), "welcome page buttons\n");

    /* The second page is the choice of method, the search preselected */
    Press(hSheet, PSBTN_NEXT);
    hPage = WaitForPage(hSheet, MethodPage, 1, 5000, NULL);
    ok(hPage != NULL, "the second page is not the method page\n");
    if (!hPage)
        return;
    ok(SendBounded(GetDlgItem(hPage, IDC_AUTOINSTALL), BM_GETCHECK, 0, 0) == BST_CHECKED,
       "the search is not preselected\n");

    /* Search: it must end by itself on the result page */
    Press(hSheet, PSBTN_NEXT);
    hPage = WaitForPage(hSheet, AfterSearch, ARRAYSIZE(AfterSearch), 60000, &dwWaited);
    ok(hPage && GetDlgItem(hPage, IDC_DETECTEDHWLIST), "the search did not end on the result page\n");
    if (!hPage || !GetDlgItem(hPage, IDC_DETECTEDHWLIST))
        return;
    szText[0] = UNICODE_NULL;
    GetDlgItemTextW(hPage, IDC_STATUSTEXT, szText, ARRAYSIZE(szText));
    trace("result after %lu ms: '%ls'\n", dwWaited, szText);
    ok(szText[0] != UNICODE_NULL, "the result page has no text\n");
    ok(wcscmp(szText, pszFailed) != 0, "the search failed: the list of devices could not be scanned\n");

    /* Back returns to the method page; the manual choice opens the type list */
    Press(hSheet, PSBTN_BACK);
    hPage = WaitForPage(hSheet, MethodPage, 1, 5000, NULL);
    ok(hPage != NULL, "Back from the result page did not return to the method page\n");
    if (!hPage)
        return;
    SendBounded(GetDlgItem(hPage, IDC_AUTOINSTALL), BM_SETCHECK, BST_UNCHECKED, 0);
    SendBounded(GetDlgItem(hPage, IDC_MANUALLYINST), BM_SETCHECK, BST_CHECKED, 0);
    Press(hSheet, PSBTN_NEXT);
    ok(WaitForPage(hSheet, TypesPage, 1, 5000, NULL) != NULL, "the manual choice did not open the type list\n");
}

START_TEST(AddHardwareWizard)
{
    PADDHARDWAREWIZARD pAddHardwareWizard;
    WCHAR szFailed[512];
    HMODULE hHdwwiz;
    HANDLE hThread;
    HWND hSheet;

    hHdwwiz = LoadLibraryW(L"hdwwiz.cpl");
    if (!hHdwwiz)
    {
        skip("hdwwiz.cpl not loaded, error %lu\n", GetLastError());
        return;
    }
    pAddHardwareWizard = (PADDHARDWAREWIZARD)GetProcAddress(hHdwwiz, "AddHardwareWizard");
    ok(pAddHardwareWizard != NULL, "AddHardwareWizard is not exported\n");
    szFailed[0] = UNICODE_NULL;
    ok(LoadStringW(hHdwwiz, IDS_SEARCHFAILEDTEXT, szFailed, ARRAYSIZE(szFailed)) > 0,
       "no search-failed text\n");
    if (!pAddHardwareWizard || !szFailed[0])
    {
        FreeLibrary(hHdwwiz);
        return;
    }

    hThread = CreateThread(NULL, 0, WizardThread, pAddHardwareWizard, 0, &dwWizardThread);
    ok(hThread != NULL, "CreateThread failed, error %lu\n", GetLastError());
    if (!hThread)
    {
        FreeLibrary(hHdwwiz);
        return;
    }

    hSheet = FindSheet();
    ok(hSheet != NULL, "the wizard did not open\n");
    if (hSheet)
    {
        DriveWizard(hSheet, szFailed);
        Press(hSheet, PSBTN_CANCEL);
    }

    if (WaitForSingleObject(hThread, 15000) == WAIT_OBJECT_0)
    {
        CloseHandle(hThread);
        FreeLibrary(hHdwwiz);
        return;
    }

    /* A wizard that does not close keeps hdwwiz.cpl loaded; the process exit ends it */
    ok(FALSE, "the wizard did not close\n");
    CloseHandle(hThread);
}
