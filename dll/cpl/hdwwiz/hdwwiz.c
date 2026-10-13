/*
 * ReactOS New devices installation
 * Copyright (C) 2005, 2008 ReactOS Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */
/*
 * PROJECT:         ReactOS Add hardware control panel
 * FILE:            dll/cpl/hdwwiz/hdwwiz.c
 * PURPOSE:         ReactOS Add hardware control panel
 * PROGRAMMER:      Hervé Poussineau (hpoussin@reactos.org)
 *                  Dmitry Chapyshev (dmitry@reactos.org)
 */

#include "resource.h"
#include "hdwwiz.h"

/* GLOBALS ******************************************************************/

HINSTANCE hApplet = NULL;
HFONT hTitleFont;
SP_CLASSIMAGELIST_DATA ImageListData;
HANDLE hProcessHeap;
HDEVINFO hDevInfoTypes;

#define WM_SEARCH_DONE (WM_APP + 1)
#define MAX_FOUND_DEVICES 32
#define RESULT_PAGE_INDEX 3

/* Outcome of one hardware search */
typedef struct _SEARCH_RESULT
{
    LONG Generation;
    BOOL bSearched;
    DWORD dwFound;
    WCHAR szName[MAX_FOUND_DEVICES][MAX_STR_SIZE];
} SEARCH_RESULT, *PSEARCH_RESULT;

typedef struct _SEARCH_THREAD_DATA
{
    HWND hwndPage;
    HMODULE hModule;
    LONG Generation;
} SEARCH_THREAD_DATA, *PSEARCH_THREAD_DATA;

static PSEARCH_RESULT pSearchResult;
static BOOL bSearchRunning;
static HANDLE hSearchThread;
static UINT HwTypesBackPage = IDD_SELECTWAYPAGE;

/* Guards SearchGeneration and PendingResult, shared with the search thread */
static CRITICAL_SECTION SearchLock;
static LONG SearchGeneration;
static PSEARCH_RESULT PendingResult;

/* One wizard per process: the search state above is global */
static LONG WizardActive;
static HWND hwndWizard;

typedef BOOL (WINAPI *PINSTALL_NEW_DEVICE)(HWND, LPGUID, PDWORD);


/* STATIC FUNCTIONS *********************************************************/

static HFONT
CreateTitleFont(VOID)
{
    NONCLIENTMETRICS ncm;
    LOGFONT LogFont;
    HDC hdc;
    INT FontSize;
    HFONT hFont;

    ncm.cbSize = sizeof(NONCLIENTMETRICS);
    SystemParametersInfo(SPI_GETNONCLIENTMETRICS, 0, &ncm, 0);

    LogFont = ncm.lfMessageFont;
    LogFont.lfWeight = FW_BOLD;
    wcscpy(LogFont.lfFaceName, L"MS Shell Dlg");

    hdc = GetDC(NULL);
    FontSize = 12;
    LogFont.lfHeight = 0 - GetDeviceCaps (hdc, LOGPIXELSY) * FontSize / 72;
    hFont = CreateFontIndirect(&LogFont);
    ReleaseDC(NULL, hdc);

    return hFont;
}

static INT_PTR CALLBACK
StartPageDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            /* Set title font */
            SendDlgItemMessage(hwndDlg, IDC_FINISHTITLE, WM_SETFONT, (WPARAM)hTitleFont, (LPARAM)TRUE);
        }
        break;

        case WM_NOTIFY:
        {
            LPNMHDR lpnm = (LPNMHDR)lParam;

            switch (lpnm->code)
            {
                case PSN_SETACTIVE:
                {
                    PropSheet_SetWizButtons(GetParent(hwndDlg), PSWIZB_NEXT);
                }
                break;
            }
        }
        break;
    }

    return FALSE;
}

static BOOL
GetDeviceInstanceId(HDEVINFO hDevInfo, PSP_DEVINFO_DATA pDevInfoData, LPWSTR pszId, DWORD cchId)
{
    return SetupDiGetDeviceInstanceIdW(hDevInfo, pDevInfoData, pszId, cchId, NULL);
}

/* The device list also holds devices that are not present: ask the device tree */
static BOOL
IsDevicePresent(PSP_DEVINFO_DATA pDevInfoData)
{
    ULONG ulStatus, ulProblem;

    return CM_Get_DevNode_Status(&ulStatus, &ulProblem, pDevInfoData->DevInst, 0) == CR_SUCCESS;
}

/* Instance IDs of the devices present before the search, as one multi-string */
static LPWSTR
GetPresentDeviceIds(VOID)
{
    HDEVINFO hDevInfo;
    SP_DEVINFO_DATA DevInfoData;
    WCHAR szId[MAX_DEVICE_ID_LEN];
    LPWSTR pszIds, pszNew;
    SIZE_T cchUsed = 0, cchMax = 4096, cchId;
    DWORD Index;

    hDevInfo = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE)
        return NULL;

    pszIds = (LPWSTR)HeapAlloc(hProcessHeap, 0, cchMax * sizeof(WCHAR));
    DevInfoData.cbSize = sizeof(DevInfoData);
    for (Index = 0; pszIds && SetupDiEnumDeviceInfo(hDevInfo, Index, &DevInfoData); Index++)
    {
        if (!IsDevicePresent(&DevInfoData) ||
            !GetDeviceInstanceId(hDevInfo, &DevInfoData, szId, ARRAYSIZE(szId)))
        {
            continue;
        }

        cchId = wcslen(szId) + 1;
        if (cchUsed + cchId + 1 > cchMax)
        {
            cchMax = 2 * cchMax + cchId;
            pszNew = (LPWSTR)HeapReAlloc(hProcessHeap, 0, pszIds, cchMax * sizeof(WCHAR));
            if (!pszNew)
            {
                HeapFree(hProcessHeap, 0, pszIds);
                pszIds = NULL;
                break;
            }
            pszIds = pszNew;
        }
        CopyMemory(pszIds + cchUsed, szId, cchId * sizeof(WCHAR));
        cchUsed += cchId;
    }
    if (pszIds)
        pszIds[cchUsed] = UNICODE_NULL;

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return pszIds;
}

static BOOL
IsIdInList(LPCWSTR pszIds, LPCWSTR pszId)
{
    for (; *pszIds; pszIds += wcslen(pszIds) + 1)
    {
        if (!lstrcmpiW(pszIds, pszId))
            return TRUE;
    }

    return FALSE;
}

static DWORD
CountDevices(HDEVINFO hDevInfo)
{
    SP_DEVINFO_DATA DevInfoData;
    DWORD Index, dwCount = 0;

    DevInfoData.cbSize = sizeof(DevInfoData);
    for (Index = 0; SetupDiEnumDeviceInfo(hDevInfo, Index, &DevInfoData); Index++)
    {
        if (IsDevicePresent(&DevInfoData))
            dwCount++;
    }

    return dwCount;
}

/* Re-enumerating a node does not re-enumerate its started children: ask every started node */
static VOID
ReenumerateChildren(DEVINST devParent, ULONG ulDepth)
{
    DEVINST devChild;
    ULONG ulStatus, ulProblem;
    CONFIGRET cr;

    if (ulDepth > 64)
        return;

    for (cr = CM_Get_Child(&devChild, devParent, 0);
         cr == CR_SUCCESS;
         cr = CM_Get_Sibling(&devChild, devChild, 0))
    {
        if (CM_Get_DevNode_Status(&ulStatus, &ulProblem, devChild, 0) == CR_SUCCESS &&
            (ulStatus & DN_STARTED))
        {
            CM_Reenumerate_DevNode(devChild, 0);
            ReenumerateChildren(devChild, ulDepth + 1);
        }
    }
}

static HDEVINFO
GetPresentDevices(VOID)
{
    return SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
}

static DWORD WINAPI
SearchThread(LPVOID lpParameter)
{
    PSEARCH_THREAD_DATA pData = (PSEARCH_THREAD_DATA)lpParameter;
    HWND hwndPage = pData->hwndPage;
    HMODULE hModule = pData->hModule;
    LONG Generation = pData->Generation;
    PSEARCH_RESULT pResult;
    LPWSTR pszBefore;
    HDEVINFO hAfter = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA DevInfoData;
    WCHAR szId[MAX_DEVICE_ID_LEN];
    DEVINST devRoot;
    DWORD Index, dwCount, dwLastCount = (DWORD)-1, dwTries;

    HeapFree(hProcessHeap, 0, pData);

    pResult = (PSEARCH_RESULT)HeapAlloc(hProcessHeap, HEAP_ZERO_MEMORY, sizeof(SEARCH_RESULT));
    if (!pResult)
        goto done;

    pszBefore = GetPresentDeviceIds();
    if (pszBefore &&
        CM_Locate_DevNodeW(&devRoot, NULL, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
        CM_Reenumerate_DevNode(devRoot, CM_REENUMERATE_SYNCHRONOUS) == CR_SUCCESS)
    {
        ReenumerateChildren(devRoot, 0);

        /* Devices found by the rescan appear asynchronously: wait until the count settles */
        for (dwTries = 0; dwTries < 8; dwTries++)
        {
            Sleep(250);
            if (hAfter != INVALID_HANDLE_VALUE)
                SetupDiDestroyDeviceInfoList(hAfter);
            hAfter = GetPresentDevices();
            if (hAfter == INVALID_HANDLE_VALUE)
                break;
            dwCount = CountDevices(hAfter);
            if (dwCount == dwLastCount && dwTries >= 3)
                break;
            dwLastCount = dwCount;
        }

        if (hAfter != INVALID_HANDLE_VALUE)
        {
            pResult->bSearched = TRUE;

            DevInfoData.cbSize = sizeof(DevInfoData);
            for (Index = 0; SetupDiEnumDeviceInfo(hAfter, Index, &DevInfoData); Index++)
            {
                LPWSTR pszName;

                if (!IsDevicePresent(&DevInfoData) ||
                    !GetDeviceInstanceId(hAfter, &DevInfoData, szId, ARRAYSIZE(szId)) ||
                    IsIdInList(pszBefore, szId))
                {
                    continue;
                }

                if (pResult->dwFound >= MAX_FOUND_DEVICES)
                {
                    pResult->dwFound++;
                    continue;
                }

                pszName = pResult->szName[pResult->dwFound++];
                if (!SetupDiGetDeviceRegistryPropertyW(hAfter, &DevInfoData, SPDRP_FRIENDLYNAME, NULL,
                                                       (PBYTE)pszName, MAX_STR_SIZE * sizeof(WCHAR), NULL) &&
                    !SetupDiGetDeviceRegistryPropertyW(hAfter, &DevInfoData, SPDRP_DEVICEDESC, NULL,
                                                       (PBYTE)pszName, MAX_STR_SIZE * sizeof(WCHAR), NULL))
                {
                    lstrcpynW(pszName, szId, MAX_STR_SIZE);
                }
                pszName[MAX_STR_SIZE - 1] = UNICODE_NULL;
            }
            SetupDiDestroyDeviceInfoList(hAfter);
        }
    }

    if (pszBefore)
        HeapFree(hProcessHeap, 0, pszBefore);

done:
    /* The result stays with the applet, so a wizard closed meanwhile cannot leak it */
    EnterCriticalSection(&SearchLock);
    if (Generation == SearchGeneration)
    {
        if (pResult)
            pResult->Generation = Generation;
        if (PendingResult)
            HeapFree(hProcessHeap, 0, PendingResult);
        PendingResult = pResult;
        pResult = NULL;
        PostMessageW(hwndPage, WM_SEARCH_DONE, (WPARAM)Generation, 0);
    }
    LeaveCriticalSection(&SearchLock);

    if (pResult)
        HeapFree(hProcessHeap, 0, pResult);

    FreeLibraryAndExitThread(hModule, 0);
    return 0;
}

static BOOL
StartSearch(HWND hwndPage)
{
    PSEARCH_THREAD_DATA pData;
    HANDLE hThread;

    pData = (PSEARCH_THREAD_DATA)HeapAlloc(hProcessHeap, 0, sizeof(SEARCH_THREAD_DATA));
    if (!pData)
        return FALSE;

    pData->hwndPage = hwndPage;
    EnterCriticalSection(&SearchLock);
    pData->Generation = SearchGeneration;
    LeaveCriticalSection(&SearchLock);
    /* The thread keeps the applet loaded until it ends */
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)SearchThread, &pData->hModule))
    {
        HeapFree(hProcessHeap, 0, pData);
        return FALSE;
    }

    /* An earlier search of this wizard has posted its result and is ending */
    if (hSearchThread)
    {
        WaitForSingleObject(hSearchThread, INFINITE);
        CloseHandle(hSearchThread);
        hSearchThread = NULL;
    }

    hThread = CreateThread(NULL, 0, SearchThread, pData, 0, NULL);
    if (!hThread)
    {
        FreeLibrary(pData->hModule);
        HeapFree(hProcessHeap, 0, pData);
        return FALSE;
    }

    hSearchThread = hThread;
    return TRUE;
}

static VOID
SetResultTitle(HWND hwndSheet)
{
    WCHAR szTitle[MAX_STR_SIZE];
    UINT uId;

    if (!pSearchResult || !pSearchResult->bSearched)
        uId = IDS_SEARCHFAILEDTITLE;
    else if (pSearchResult->dwFound == 0)
        uId = IDS_NOTFOUNDTITLE;
    else
        uId = IDS_FOUNDTITLE;

    if (LoadStringW(hApplet, uId, szTitle, ARRAYSIZE(szTitle)))
        SendMessageW(hwndSheet, PSM_SETHEADERTITLEW, RESULT_PAGE_INDEX, (LPARAM)szTitle);
}

static INT_PTR CALLBACK
SearchPageDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            WCHAR szText[MAX_STR_SIZE];

            if (LoadStringW(hApplet, IDS_SEARCHTEXT, szText, ARRAYSIZE(szText)))
                SetDlgItemTextW(hwndDlg, IDC_STATUSTEXT, szText);
        }
        break;

        case WM_SEARCH_DONE:
        {
            HWND hwndSheet = GetParent(hwndDlg);
            PSEARCH_RESULT pResult;
            BOOL bCurrent;

            EnterCriticalSection(&SearchLock);
            bCurrent = ((LONG)wParam == SearchGeneration);
            pResult = NULL;
            if (bCurrent)
            {
                pResult = PendingResult;
                PendingResult = NULL;
            }
            LeaveCriticalSection(&SearchLock);

            /* A notification from an earlier wizard is ignored */
            if (!bCurrent)
                return TRUE;

            if (pSearchResult)
                HeapFree(hProcessHeap, 0, pSearchResult);
            pSearchResult = pResult;
            bSearchRunning = FALSE;

            SetResultTitle(hwndSheet);
            if (PropSheet_GetCurrentPageHwnd(hwndSheet) == hwndDlg)
                PropSheet_SetCurSelByID(hwndSheet, IDD_DETECTEDHWPAGE);
            return TRUE;
        }

        case WM_NOTIFY:
        {
            LPNMHDR lpnm = (LPNMHDR)lParam;

            switch (lpnm->code)
            {
                case PSN_SETACTIVE:
                {
                    /* No navigation while the search runs; Cancel stays available */
                    PropSheet_SetWizButtons(GetParent(hwndDlg), 0);
                    if (!bSearchRunning)
                    {
                        bSearchRunning = StartSearch(hwndDlg);
                        if (!bSearchRunning)
                        {
                            LONG Generation;

                            EnterCriticalSection(&SearchLock);
                            Generation = SearchGeneration;
                            LeaveCriticalSection(&SearchLock);
                            PostMessageW(hwndDlg, WM_SEARCH_DONE, (WPARAM)Generation, 0);
                        }
                    }
                }
                break;
            }
        }
        break;
    }

    return FALSE;
}

static INT_PTR CALLBACK
ResultPageDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            HWND hList = GetDlgItem(hwndDlg, IDC_DETECTEDHWLIST);
            LV_COLUMN Column;
            RECT Rect;

            GetClientRect(hList, &Rect);
            ZeroMemory(&Column, sizeof(Column));
            Column.mask = LVCF_FMT | LVCF_WIDTH;
            Column.fmt = LVCFMT_LEFT;
            Column.cx = Rect.right - GetSystemMetrics(SM_CXVSCROLL);
            (VOID) ListView_InsertColumn(hList, 0, &Column);
        }
        break;

        case WM_NOTIFY:
        {
            LPNMHDR lpnm = (LPNMHDR)lParam;

            switch (lpnm->code)
            {
                case PSN_SETACTIVE:
                {
                    HWND hList = GetDlgItem(hwndDlg, IDC_DETECTEDHWLIST);
                    WCHAR szText[MAX_STR_SIZE];
                    BOOL bFound = (pSearchResult && pSearchResult->bSearched && pSearchResult->dwFound);
                    UINT uText;
                    DWORD Index;
                    LV_ITEM Item;

                    if (!pSearchResult || !pSearchResult->bSearched)
                        uText = IDS_SEARCHFAILEDTEXT;
                    else if (!bFound)
                        uText = IDS_NOTFOUNDTEXT;
                    else
                        uText = IDS_FOUNDTEXT;
                    szText[0] = UNICODE_NULL;
                    LoadStringW(hApplet, uText, szText, ARRAYSIZE(szText));
                    SetDlgItemTextW(hwndDlg, IDC_STATUSTEXT, szText);

                    (VOID) ListView_DeleteAllItems(hList);
                    if (bFound)
                    {
                        ZeroMemory(&Item, sizeof(Item));
                        Item.mask = LVIF_TEXT;
                        for (Index = 0; Index < min(pSearchResult->dwFound, MAX_FOUND_DEVICES); Index++)
                        {
                            Item.iItem = Index;
                            Item.pszText = pSearchResult->szName[Index];
                            (VOID) ListView_InsertItem(hList, &Item);
                        }
                    }
                    ShowWindow(GetDlgItem(hwndDlg, IDC_DETECTEDHWTEXT), bFound ? SW_SHOW : SW_HIDE);
                    ShowWindow(hList, bFound ? SW_SHOW : SW_HIDE);

                    PropSheet_SetWizButtons(GetParent(hwndDlg), PSWIZB_BACK | PSWIZB_NEXT);
                }
                break;

                case PSN_WIZBACK:
                {
                    SetWindowLongPtr(hwndDlg, DWLP_MSGRESULT, IDD_SELECTWAYPAGE);
                    return TRUE;
                }

                case PSN_WIZNEXT:
                {
                    HwTypesBackPage = IDD_DETECTEDHWPAGE;
                    SetWindowLongPtr(hwndDlg, DWLP_MSGRESULT, IDD_HWTYPESPAGE);
                    return TRUE;
                }
            }
        }
        break;
    }

    return FALSE;
}

static INT_PTR CALLBACK
SelectWayPageDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_NOTIFY:
        {
            LPNMHDR lpnm = (LPNMHDR)lParam;

            switch (lpnm->code)
            {
                case PSN_SETACTIVE:
                {
                    PropSheet_SetWizButtons(GetParent(hwndDlg), PSWIZB_NEXT | PSWIZB_BACK);
                    if (SendDlgItemMessage(hwndDlg, IDC_AUTOINSTALL, BM_GETCHECK, 0, 0) == BST_CHECKED)
                        SendDlgItemMessage(hwndDlg, IDC_MANUALLYINST, BM_SETCHECK, 0, 0);
                    else
                    {
                        SendDlgItemMessage(hwndDlg, IDC_AUTOINSTALL, BM_SETCHECK, 1, 1);
                        SendDlgItemMessage(hwndDlg, IDC_MANUALLYINST, BM_SETCHECK, 0, 0);
                    }
                }
                break;

                case PSN_WIZNEXT:
                {
                    if (SendDlgItemMessage(hwndDlg, IDC_AUTOINSTALL, BM_GETCHECK, 0, 0) == BST_CHECKED)
                    {
                        SetWindowLongPtr(hwndDlg, DWLP_MSGRESULT, IDD_SEARCHPAGE);
                    }
                    else
                    {
                        HwTypesBackPage = IDD_SELECTWAYPAGE;
                        SetWindowLongPtr(hwndDlg, DWLP_MSGRESULT, IDD_HWTYPESPAGE);
                    }

                    return TRUE;
                }
            }
        }
        break;
    }

    return FALSE;
}

static INT
EnumDeviceClasses(INT ClassIndex,
                  LPWSTR DevClassName,
                  LPWSTR DevClassDesc,
                  BOOL *DevPresent,
                  INT *ClassImage)
{
    GUID ClassGuid;
    HKEY KeyClass;
    WCHAR ClassName[MAX_STR_SIZE];
    DWORD RequiredSize = MAX_STR_SIZE;
    UINT Ret;

    *DevPresent = FALSE;
    *DevClassName = L'\0';

    Ret = CM_Enumerate_Classes(ClassIndex,
                               &ClassGuid,
                               0);
    if (Ret != CR_SUCCESS)
    {
        /* All classes enumerated */
        if(Ret == CR_NO_SUCH_VALUE)
        {
            hDevInfoTypes = NULL;
            return -1;
        }

        if (Ret == CR_INVALID_DATA)
        {
            ; /* FIXME: What should we do here? */
        }

        /* Handle other errors... */
    }

    if (SetupDiClassNameFromGuid(&ClassGuid,
                                 ClassName,
                                 RequiredSize,
                                 &RequiredSize))
    {
        lstrcpy(DevClassName, ClassName);
    }

    if (!SetupDiGetClassImageIndex(&ImageListData,
                                   &ClassGuid,
                                   ClassImage))
    {
        /* FIXME: Can we do this?
         * Set the blank icon: IDI_SETUPAPI_BLANK = 41
         * It'll be image 24 in the imagelist */
        *ClassImage = 24;
    }

    /* Get device info for all devices of a particular class */
    hDevInfoTypes = SetupDiGetClassDevs(&ClassGuid,
                                   NULL,
                                   NULL,
                                   DIGCF_PRESENT);
    if (hDevInfoTypes == INVALID_HANDLE_VALUE)
    {
        hDevInfoTypes = NULL;
        return 0;
    }

    KeyClass = SetupDiOpenClassRegKeyEx(&ClassGuid,
                                        MAXIMUM_ALLOWED,
                                        DIOCR_INSTALLER,
                                        NULL,
                                        0);
    if (KeyClass != INVALID_HANDLE_VALUE)
    {

        LONG dwSize = MAX_STR_SIZE;

        if (RegQueryValue(KeyClass,
                          NULL,
                          DevClassDesc,
                          &dwSize) != ERROR_SUCCESS)
        {
            *DevClassDesc = L'\0';
        }
    }
    else
    {
        return -3;
    }

    *DevPresent = TRUE;

    RegCloseKey(KeyClass);

    return 0;
}

static VOID
InitHardWareTypesPage(HWND hwndDlg)
{
    HWND hList = GetDlgItem(hwndDlg, IDC_HWTYPESLIST);
    WCHAR DevName[MAX_STR_SIZE];
    WCHAR DevDesc[MAX_STR_SIZE];
    BOOL DevExist = FALSE;
    INT ClassRet, DevImage, Index = 0;
    LV_COLUMN Column;
    LV_ITEM Item;
    RECT Rect;

    if (!hList) return;

    ZeroMemory(&Column, sizeof(LV_COLUMN));

    GetClientRect(hList, &Rect);

    Column.mask         = LVCF_FMT | LVCF_TEXT | LVCF_WIDTH;
    Column.fmt          = LVCFMT_LEFT;
    Column.iSubItem     = 0;
    Column.pszText      = NULL;
    Column.cx           = Rect.right - GetSystemMetrics(SM_CXVSCROLL);
    (VOID) ListView_InsertColumn(hList, 0, &Column);

    ZeroMemory(&Item, sizeof(LV_ITEM));

    if (!ImageListData.ImageList)
    {
        ImageListData.cbSize = sizeof(ImageListData);
        if (!SetupDiGetClassImageList(&ImageListData))
            ImageListData.ImageList = NULL;
    }

    do
    {
        ClassRet = EnumDeviceClasses(Index,
                                     DevName,
                                     DevDesc,
                                     &DevExist,
                                     &DevImage);

        if ((ClassRet != -1) && (DevExist))
        {
            Item.mask   = LVIF_TEXT | LVIF_PARAM | LVIF_STATE | LVIF_IMAGE;
            Item.iItem  = Index;
            Item.iImage = DevImage;

            if (DevDesc[0] != L'\0')
                Item.pszText = (LPWSTR) DevDesc;
            else
                Item.pszText = (LPWSTR) DevName;

            (VOID) ListView_InsertItem(hList, &Item);

            /* Kill InfoList initialized in EnumDeviceClasses */
            if (hDevInfoTypes)
            {
                SetupDiDestroyDeviceInfoList(hDevInfoTypes);
                hDevInfoTypes = NULL;
            }
        }
        Index++;
    }
    while (ClassRet != -1);

    (VOID) ListView_SetImageList(hList, ImageListData.ImageList, LVSIL_SMALL);
}

static INT_PTR CALLBACK
HdTypesPageDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            WCHAR szText[MAX_STR_SIZE];

            /* Installing a driver chosen from this list is not implemented */
            if (LoadStringW(hApplet, IDS_HWTYPESNOTAVAIL, szText, ARRAYSIZE(szText)))
                SetDlgItemTextW(hwndDlg, IDC_STATUSTEXT, szText);
            InitHardWareTypesPage(hwndDlg);
        }
        break;

        case WM_NOTIFY:
        {
            LPNMHDR lpnm = (LPNMHDR)lParam;

            switch (lpnm->code)
            {
                case PSN_SETACTIVE:
                {
                    PropSheet_SetWizButtons(GetParent(hwndDlg), PSWIZB_BACK);
                }
                break;

                case PSN_WIZBACK:
                {
                    SetWindowLongPtr(hwndDlg, DWLP_MSGRESULT, HwTypesBackPage);
                    return TRUE;
                }
            }
        }
        break;
    }

    return FALSE;
}

static int CALLBACK
PropSheetProc(HWND hwndDlg, UINT uMsg, LPARAM lParam)
{
    // NOTE: This callback is needed to set large icon correctly.
    HICON hIcon;
    switch (uMsg)
    {
        case PSCB_INITIALIZED:
        {
            hwndWizard = hwndDlg;
            hIcon = LoadIconW(hApplet, MAKEINTRESOURCEW(IDI_CPLICON));
            SendMessageW(hwndDlg, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            break;
        }
    }
    return 0;
}

static VOID
HardwareWizardInit(HWND hwnd)
{
    HPROPSHEETPAGE ahpsp[5];
    PROPSHEETPAGE psp = {0};
    PROPSHEETHEADER psh;
    UINT nPages = 0;

    if (InterlockedCompareExchange(&WizardActive, 1, 0) != 0)
    {
        /* Already open in this process: bring it to the front */
        if (hwndWizard)
            SetForegroundWindow(hwndWizard);
        return;
    }

    /* Create the Start page, until setup is working */
    psp.dwSize = sizeof(PROPSHEETPAGE);
    psp.dwFlags = PSP_DEFAULT | PSP_HIDEHEADER;
    psp.hInstance = hApplet;
    psp.lParam = 0;
    psp.pfnDlgProc = StartPageDlgProc;
    psp.pszTemplate = MAKEINTRESOURCE(IDD_STARTPAGE);
    ahpsp[nPages++] = CreatePropertySheetPage(&psp);

    /* Create select search way page */
    psp.dwSize = sizeof(PROPSHEETPAGE);
    psp.dwFlags = PSP_DEFAULT | PSP_USEHEADERTITLE | PSP_USEHEADERSUBTITLE;
    psp.pszHeaderTitle = MAKEINTRESOURCE(IDS_SELECTWAYTITLE);
    psp.pszHeaderSubTitle = NULL;
    psp.hInstance = hApplet;
    psp.lParam = 0;
    psp.pfnDlgProc = SelectWayPageDlgProc;
    psp.pszTemplate = MAKEINTRESOURCE(IDD_SELECTWAYPAGE);
    ahpsp[nPages++] = CreatePropertySheetPage(&psp);

    /* Create search page */
    psp.dwSize = sizeof(PROPSHEETPAGE);
    psp.dwFlags = PSP_DEFAULT | PSP_USEHEADERTITLE | PSP_USEHEADERSUBTITLE;
    psp.pszHeaderTitle = MAKEINTRESOURCE(IDS_SEARCHTITLE);
    psp.pszHeaderSubTitle = NULL;
    psp.hInstance = hApplet;
    psp.lParam = 0;
    psp.pfnDlgProc = SearchPageDlgProc;
    psp.pszTemplate = MAKEINTRESOURCE(IDD_SEARCHPAGE);
    ahpsp[nPages++] = CreatePropertySheetPage(&psp);

    /* Create search result page (its title is set when the search ends) */
    psp.dwSize = sizeof(PROPSHEETPAGE);
    psp.dwFlags = PSP_DEFAULT | PSP_USEHEADERTITLE | PSP_USEHEADERSUBTITLE;
    psp.pszHeaderTitle = MAKEINTRESOURCE(IDS_NOTFOUNDTITLE);
    psp.pszHeaderSubTitle = NULL;
    psp.hInstance = hApplet;
    psp.lParam = 0;
    psp.pfnDlgProc = ResultPageDlgProc;
    psp.pszTemplate = MAKEINTRESOURCE(IDD_DETECTEDHWPAGE);
    ASSERT(nPages == RESULT_PAGE_INDEX);
    ahpsp[nPages++] = CreatePropertySheetPage(&psp);

    /* Create hardware types page */
    psp.dwSize = sizeof(PROPSHEETPAGE);
    psp.dwFlags = PSP_DEFAULT | PSP_USEHEADERTITLE | PSP_USEHEADERSUBTITLE;
    psp.pszHeaderTitle = MAKEINTRESOURCE(IDS_HDTYPESTITLE);
    psp.pszHeaderSubTitle = NULL;
    psp.hInstance = hApplet;
    psp.lParam = 0;
    psp.pfnDlgProc = HdTypesPageDlgProc;
    psp.pszTemplate = MAKEINTRESOURCE(IDD_HWTYPESPAGE);
    ahpsp[nPages++] = CreatePropertySheetPage(&psp);

    /* Create the property sheet */
    psh.dwSize = sizeof(PROPSHEETHEADER);
    psh.dwFlags = PSH_WIZARD97 | PSH_WATERMARK | PSH_HEADER | PSH_USEICONID | PSH_USECALLBACK;
    psh.hInstance = hApplet;
    psh.pszIcon = MAKEINTRESOURCEW(IDI_CPLICON);
    psh.hwndParent = hwnd;
    psh.nPages = nPages;
    psh.nStartPage = 0;
    psh.phpage = ahpsp;
    psh.pszbmWatermark = MAKEINTRESOURCE(IDB_WATERMARK);
    psh.pszbmHeader = MAKEINTRESOURCE(IDB_HEADER);
    psh.pfnCallback = PropSheetProc;

    /* Create title font */
    hTitleFont = CreateTitleFont();

    pSearchResult = NULL;
    bSearchRunning = FALSE;
    HwTypesBackPage = IDD_SELECTWAYPAGE;
    EnterCriticalSection(&SearchLock);
    SearchGeneration++;
    LeaveCriticalSection(&SearchLock);

    /* Display the wizard */
    PropertySheet(&psh);

    DeleteObject(hTitleFont);

    if (pSearchResult)
    {
        HeapFree(hProcessHeap, 0, pSearchResult);
        pSearchResult = NULL;
    }
    if (ImageListData.ImageList)
    {
        SetupDiDestroyClassImageList(&ImageListData);
        ImageListData.ImageList = NULL;
    }

    /* A search still running belongs to this wizard: drop its result and let it
       finish before another wizard can use SetupAPI from this process */
    EnterCriticalSection(&SearchLock);
    SearchGeneration++;
    LeaveCriticalSection(&SearchLock);
    if (hSearchThread)
    {
        WaitForSingleObject(hSearchThread, INFINITE);
        CloseHandle(hSearchThread);
        hSearchThread = NULL;
    }
    EnterCriticalSection(&SearchLock);
    if (PendingResult)
    {
        HeapFree(hProcessHeap, 0, PendingResult);
        PendingResult = NULL;
    }
    LeaveCriticalSection(&SearchLock);

    hwndWizard = NULL;
    InterlockedExchange(&WizardActive, 0);
}

/* FUNCTIONS ****************************************************************/

BOOL WINAPI
InstallNewDevice(HWND hwndParent, LPGUID ClassGuid, PDWORD pReboot)
{
    return FALSE;
}

VOID WINAPI
AddHardwareWizard(HWND hwnd, LPWSTR lpName)
{
    if (lpName != NULL)
    {
        DPRINT1("No support of remote installation yet!\n");
        return;
    }

    HardwareWizardInit(hwnd);
}

/* Control Panel Callback */
LONG CALLBACK
CPlApplet(HWND hwndCpl, UINT uMsg, LPARAM lParam1, LPARAM lParam2)
{
    switch (uMsg)
    {
        case CPL_INIT:
            return TRUE;

        case CPL_GETCOUNT:
            return 1;

        case CPL_INQUIRE:
            {
                CPLINFO *CPlInfo = (CPLINFO*)lParam2;
                CPlInfo->lData = 0;
                CPlInfo->idIcon = IDI_CPLICON;
                CPlInfo->idName = IDS_CPLNAME;
                CPlInfo->idInfo = IDS_CPLDESCRIPTION;
            }
            break;

        case CPL_DBLCLK:
            AddHardwareWizard(hwndCpl, NULL);
            break;
    }

    return FALSE;
}

BOOL WINAPI
DllMain(HINSTANCE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
            hApplet = hinstDLL;
            hProcessHeap = GetProcessHeap();
            InitializeCriticalSection(&SearchLock);
            DisableThreadLibraryCalls(hinstDLL);
            break;

        case DLL_PROCESS_DETACH:
            if (!lpvReserved)
                DeleteCriticalSection(&SearchLock);
            break;
    }

    return TRUE;
}
