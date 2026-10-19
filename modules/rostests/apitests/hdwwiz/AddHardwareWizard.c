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
#define IDC_ISCONNECTED       504

#define ID_BACK 0x3023
#define ID_NEXT 0x3024

typedef VOID (WINAPI *PADDHARDWAREWIZARD)(HWND, LPWSTR);

static HMODULE hHdwwiz;
static DWORD dwWizardThread;

static DWORD WINAPI
WizardThread(LPVOID Param)
{
    PADDHARDWAREWIZARD pAddHardwareWizard = (PADDHARDWAREWIZARD)Param;

    pAddHardwareWizard(NULL, NULL);
    return 0;
}

static BOOL CALLBACK
FindSheetProc(HWND hwnd, LPARAM lParam)
{
    if (IsWindowVisible(hwnd) && SendMessageW(hwnd, PSM_GETCURRENTPAGEHWND, 0, 0) != 0)
    {
        *(HWND *)lParam = hwnd;
        return FALSE;
    }
    return TRUE;
}

/* The sheet is the visible window of the wizard thread that has a current page */
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
    return (HWND)SendMessageW(hSheet, PSM_GETCURRENTPAGEHWND, 0, 0);
}

static BOOL
ButtonEnabled(HWND hSheet, int Id)
{
    HWND hButton = GetDlgItem(hSheet, Id);
    return hButton && IsWindowVisible(hButton) && IsWindowEnabled(hButton);
}

static VOID
Press(HWND hSheet, int Button, DWORD dwWait)
{
    PostMessageW(hSheet, PSM_PRESSBUTTON, Button, 0);
    Sleep(dwWait);
}

START_TEST(AddHardwareWizard)
{
    PADDHARDWAREWIZARD pAddHardwareWizard;
    WCHAR szFailed[512], szText[512];
    HANDLE hThread;
    HWND hSheet, hPage;
    DWORD dwWaited;
    int i;

    hHdwwiz = LoadLibraryW(L"hdwwiz.cpl");
    if (!hHdwwiz)
    {
        skip("hdwwiz.cpl not loaded, error %lu\n", GetLastError());
        return;
    }
    pAddHardwareWizard = (PADDHARDWAREWIZARD)GetProcAddress(hHdwwiz, "AddHardwareWizard");
    ok(pAddHardwareWizard != NULL, "AddHardwareWizard is not exported\n");
    if (!pAddHardwareWizard)
        return;
    ok(LoadStringW(hHdwwiz, IDS_SEARCHFAILEDTEXT, szFailed, ARRAYSIZE(szFailed)) > 0,
       "no search-failed text\n");

    hThread = CreateThread(NULL, 0, WizardThread, pAddHardwareWizard, 0, &dwWizardThread);
    ok(hThread != NULL, "CreateThread failed, error %lu\n", GetLastError());
    if (!hThread)
        return;

    hSheet = FindSheet();
    ok(hSheet != NULL, "the wizard did not open\n");
    if (!hSheet)
        goto cleanup;
    Sleep(700);

    /* Welcome page */
    ok(ButtonEnabled(hSheet, ID_NEXT) && !ButtonEnabled(hSheet, ID_BACK), "welcome page buttons\n");

    /* The second page is the choice of method, the search preselected */
    Press(hSheet, PSBTN_NEXT, 800);
    hPage = CurPage(hSheet);
    ok(GetDlgItem(hPage, IDC_AUTOINSTALL) != NULL, "the second page is not the method page\n");
    ok(SendDlgItemMessageW(hPage, IDC_AUTOINSTALL, BM_GETCHECK, 0, 0) == BST_CHECKED,
       "the search is not preselected\n");
    ok(GetDlgItem(hPage, IDC_ISCONNECTED) == NULL, "the XP connected page came second\n");

    /* Search: it must end by itself on the result page */
    Press(hSheet, PSBTN_NEXT, 0);
    for (dwWaited = 0; dwWaited < 60000; dwWaited += 100)
    {
        Sleep(100);
        hPage = CurPage(hSheet);
        if (GetDlgItem(hPage, IDC_DETECTEDHWLIST) || GetDlgItem(hPage, IDC_HWTYPESLIST) ||
            GetDlgItem(hPage, IDC_ISCONNECTED))
        {
            break;
        }
    }
    hPage = CurPage(hSheet);
    ok(GetDlgItem(hPage, IDC_DETECTEDHWLIST) != NULL, "the search did not end on the result page (%lu ms)\n", dwWaited);
    szText[0] = UNICODE_NULL;
    GetDlgItemTextW(hPage, IDC_STATUSTEXT, szText, ARRAYSIZE(szText));
    trace("result after %lu ms: '%ls'\n", dwWaited, szText);
    ok(szText[0] != UNICODE_NULL, "the result page has no text\n");
    ok(wcscmp(szText, szFailed) != 0, "the search failed: the list of devices could not be scanned\n");

    /* Back returns to the method page; the manual choice opens the type list */
    Press(hSheet, PSBTN_BACK, 800);
    hPage = CurPage(hSheet);
    ok(GetDlgItem(hPage, IDC_AUTOINSTALL) != NULL, "Back from the result page did not return to the method page\n");
    SendDlgItemMessageW(hPage, IDC_AUTOINSTALL, BM_SETCHECK, BST_UNCHECKED, 0);
    SendDlgItemMessageW(hPage, IDC_MANUALLYINST, BM_SETCHECK, BST_CHECKED, 0);
    Press(hSheet, PSBTN_NEXT, 1000);
    ok(GetDlgItem(CurPage(hSheet), IDC_HWTYPESLIST) != NULL, "the manual choice did not open the type list\n");

    Press(hSheet, PSBTN_CANCEL, 0);

cleanup:
    if (WaitForSingleObject(hThread, 10000) != WAIT_OBJECT_0)
    {
        ok(FALSE, "the wizard did not close\n");
        for (i = 0; i < 3 && (hSheet = FindSheet()) != NULL; i++)
            Press(hSheet, PSBTN_CANCEL, 1000);
        if (WaitForSingleObject(hThread, 5000) != WAIT_OBJECT_0)
            TerminateThread(hThread, 1);
    }
    CloseHandle(hThread);
}
