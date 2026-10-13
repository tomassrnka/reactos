/*
 * PROJECT:     ReactOS Services
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        base/applications/mscutils/servman/propsheet_recovery.c
 * PURPOSE:     Recovery property page
 * COPYRIGHT:   Eric Kohl
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

typedef struct _RECOVERYDATA
{
    ENUM_SERVICE_STATUS_PROCESS *pService;
    LPSERVICE_FAILURE_ACTIONS pServiceFailure;
    BOOL bChanged;
    BOOL bInitializing;
    DWORD dwLoadError;
    /* What the user edited; everything else is saved back unchanged */
    BOOL bActionsChanged;
    BOOL bRestartTimeChanged;
    BOOL bResetTimeChanged;
    BOOL bCommandChanged;
    /* An unquoted stored program as shown; written back unquoted while the box still shows it */
    PWSTR pszUnquotedProgram;
} RECOVERYDATA, *PRECOVERYDATA;

static
VOID
InitRecoveryPage(
    HWND hwndDlg)
{
    LPWSTR lpAction;
    INT id;

    for (id = IDS_NO_ACTION; id <= IDS_RESTART_COMPUTER; id++)
    {
        if (AllocAndLoadString(&lpAction,
                               hInstance,
                               id))
        {
            SendDlgItemMessageW(hwndDlg,
                                IDC_FIRST_FAILURE,
                                CB_ADDSTRING,
                                0,
                                (LPARAM)lpAction);

            SendDlgItemMessageW(hwndDlg,
                                IDC_SECOND_FAILURE,
                                CB_ADDSTRING,
                                0,
                                (LPARAM)lpAction);

            SendDlgItemMessageW(hwndDlg,
                                IDC_SUBSEQUENT_FAILURES,
                                CB_ADDSTRING,
                                0,
                                (LPARAM)lpAction);

            LocalFree(lpAction);
        }
    }

    SendDlgItemMessageW(hwndDlg,
                        IDC_FIRST_FAILURE,
                        CB_SETCURSEL,
                        0,
                        0);

    SendDlgItemMessageW(hwndDlg,
                        IDC_SECOND_FAILURE,
                        CB_SETCURSEL,
                        0,
                        0);

    SendDlgItemMessageW(hwndDlg,
                        IDC_SUBSEQUENT_FAILURES,
                        CB_SETCURSEL,
                        0,
                        0);

    SendDlgItemMessageW(hwndDlg,
                        IDC_RESET_TIME,
                        WM_SETTEXT,
                        0,
                        (LPARAM)L"0");

    SendDlgItemMessageW(hwndDlg,
                        IDC_RESTART_TIME,
                        WM_SETTEXT,
                        0,
                        (LPARAM)L"1");

    for (id = IDC_RESTART_TEXT1; id <= IDC_RESTART_OPTIONS; id++)
        EnableWindow(GetDlgItem(hwndDlg, id), FALSE);
}


static
BOOL
GetServiceFailure(
    PRECOVERYDATA pRecoveryData)
{
    LPSERVICE_FAILURE_ACTIONS pServiceFailure = NULL;
    SC_HANDLE hManager = NULL;
    SC_HANDLE hService = NULL;
    BOOL bResult = TRUE;
    DWORD cbBytesNeeded = 0;

    hManager = OpenSCManager(NULL,
                             NULL,
                             SC_MANAGER_CONNECT);
    if (hManager == NULL)
    {
        bResult = FALSE;
        goto done;
    }

    hService = OpenService(hManager, pRecoveryData->pService->lpServiceName, SERVICE_QUERY_CONFIG);
    if (hService == NULL)
    {
        bResult = FALSE;
        goto done;
    }

    if (!QueryServiceConfig2(hService,
                             SERVICE_CONFIG_FAILURE_ACTIONS,
                             NULL,
                             0,
                             &cbBytesNeeded))
    {
        if (cbBytesNeeded == 0)
        {
            bResult = FALSE;
            goto done;
        }
    }

    pServiceFailure = HeapAlloc(GetProcessHeap(), 0, cbBytesNeeded);
    if (pServiceFailure == NULL)
    {
        SetLastError(ERROR_OUTOFMEMORY);
        bResult = FALSE;
        goto done;
    }

    if (!QueryServiceConfig2(hService,
                             SERVICE_CONFIG_FAILURE_ACTIONS,
                             (LPBYTE)pServiceFailure,
                             cbBytesNeeded,
                             &cbBytesNeeded))
    {
        bResult = FALSE;
        goto done;
    }

    pRecoveryData->pServiceFailure = pServiceFailure;

done:
    if (bResult == FALSE && pServiceFailure != NULL)
        HeapFree(GetProcessHeap(), 0, pServiceFailure);

    if (hService)
        CloseServiceHandle(hService);

    if (hManager)
        CloseServiceHandle(hManager);

    return bResult;
}

/* The directories CreateProcess searches for pszName, in its order: the directory of the
 * executable, the current directory unless NeedCurrentDirectoryForExePath excludes it, the
 * system directory, the 16-bit system directory, the Windows directory and PATH */
static
PWSTR
GetExeSearchPath(
    PCWSTR pszName)
{
    WCHAR szApp[MAX_PATH], szSystem[MAX_PATH], szWindows[MAX_PATH];
    PWSTR pszPath, pszEnd;
    DWORD cchApp, cchSystem, cchWindows, cchEnv;
    SIZE_T cchPath;

    cchApp = GetModuleFileNameW(NULL, szApp, _countof(szApp));
    if (cchApp == 0 || cchApp >= _countof(szApp))
        return NULL;
    pszEnd = wcsrchr(szApp, L'\\');
    if (pszEnd == NULL)
        return NULL;
    pszEnd[(pszEnd == szApp + 2 && szApp[1] == L':') ? 1 : 0] = UNICODE_NULL;

    cchSystem = GetSystemDirectoryW(szSystem, _countof(szSystem));
    cchWindows = GetWindowsDirectoryW(szWindows, _countof(szWindows));
    if (cchSystem == 0 || cchSystem >= _countof(szSystem) ||
        cchWindows == 0 || cchWindows >= _countof(szWindows))
        return NULL;

    cchEnv = GetEnvironmentVariableW(L"PATH", NULL, 0);
    cchPath = wcslen(szApp) + 3 + cchSystem + 2 * cchWindows + 12 + cchEnv + 1;
    pszPath = HeapAlloc(GetProcessHeap(), 0, cchPath * sizeof(WCHAR));
    if (pszPath == NULL)
        return NULL;

    StringCchPrintfW(pszPath, cchPath, L"%s;%s%s;%s\\system;%s;",
                     szApp, NeedCurrentDirectoryForExePathW(pszName) ? L".;" : L"",
                     szSystem, szWindows, szWindows);
    if (cchEnv > 0)
    {
        SIZE_T cchUsed = wcslen(pszPath);

        if (GetEnvironmentVariableW(L"PATH", pszPath + cchUsed, (DWORD)(cchPath - cchUsed)) >= cchPath - cchUsed)
            pszPath[cchUsed] = UNICODE_NULL;
    }

    return pszPath;
}

/* TRUE when CreateProcess would run this program: found with .exe, not a directory */
static
BOOL
IsRunnableProgram(
    PCWSTR pszName)
{
    WCHAR szPath[MAX_PATH];
    PWSTR pszSearchPath;
    DWORD cchPath, dwAttributes;

    if (*pszName == UNICODE_NULL)
        return FALSE;

    pszSearchPath = GetExeSearchPath(pszName);
    if (pszSearchPath == NULL)
        return FALSE;

    cchPath = SearchPathW(pszSearchPath, pszName, L".exe", _countof(szPath), szPath, NULL);
    HeapFree(GetProcessHeap(), 0, pszSearchPath);
    if (cchPath == 0 || cchPath >= _countof(szPath))
        return FALSE;

    dwAttributes = GetFileAttributesW(szPath);
    return (dwAttributes != INVALID_FILE_ATTRIBUTES && !(dwAttributes & FILE_ATTRIBUTE_DIRECTORY));
}

/* The edit control reports a failed allocation only with EN_ERRSPACE, and its
 * WM_SETTEXT still returns TRUE: compare the length it holds instead */
static
BOOL
SetEditText(
    HWND hwndDlg,
    INT id,
    PCWSTR pszText)
{
    SendDlgItemMessageW(hwndDlg, id, WM_SETTEXT, 0, (LPARAM)pszText);
    return (SIZE_T)GetWindowTextLengthW(GetDlgItem(hwndDlg, id)) == wcslen(pszText);
}


/* FALSE when the command could not be shown for lack of memory */
static
BOOL
ShowFailureActions(
    HWND hwndDlg,
    PRECOVERYDATA pRecoveryData)
{
    LPSERVICE_FAILURE_ACTIONS pFailure = pRecoveryData->pServiceFailure;
    WCHAR szBuffer[16];
    PWSTR pszCommand, startPtr, endPtr, failPtr;
    INT index, id;
    DWORD i;

    /* The last action repeats for every later failure */
    for (i = 0; i < 3 && pFailure->cActions > 0; i++)
    {
        LPSC_ACTION pAction = &pFailure->lpsaActions[min(i, pFailure->cActions - 1)];

        index = -1;

        switch (pAction->Type)
        {
            case SC_ACTION_NONE:
                index = 0;
                break;

            case SC_ACTION_RESTART:
                index = 1;

                StringCchPrintfW(szBuffer, _countof(szBuffer), L"%lu", pAction->Delay / 60000);
                SendDlgItemMessageW(hwndDlg,
                                    IDC_RESTART_TIME,
                                    WM_SETTEXT,
                                    0,
                                    (LPARAM)szBuffer);

                for (id = IDC_RESTART_TEXT1; id <= IDC_RESTART_TEXT2; id++)
                     EnableWindow(GetDlgItem(hwndDlg, id), TRUE);
                break;

            case SC_ACTION_REBOOT:
                index = 3;

                EnableWindow(GetDlgItem(hwndDlg, IDC_RESTART_OPTIONS), TRUE);
                break;

            case SC_ACTION_RUN_COMMAND:
                index = 2;

                for (id = IDC_RUN_GROUPBOX; id <= IDC_ADD_FAILCOUNT; id++)
                    EnableWindow(GetDlgItem(hwndDlg, id), TRUE);
                break;
        }

        if (index != -1)
        {
            SendDlgItemMessageW(hwndDlg,
                                IDC_FIRST_FAILURE + i,
                                CB_SETCURSEL,
                                index,
                                0);
        }
    }

    StringCchPrintfW(szBuffer, _countof(szBuffer), L"%lu", pFailure->dwResetPeriod / 86400);
    SendDlgItemMessageW(hwndDlg,
                        IDC_RESET_TIME,
                        WM_SETTEXT,
                        0,
                        (LPARAM)szBuffer);

    if (pFailure->lpCommand == NULL || *pFailure->lpCommand == UNICODE_NULL)
        return TRUE;

    /* Split "program" parameters /fail=%1% (or program parameters) in a copy */
    pszCommand = HeapAlloc(GetProcessHeap(), 0, (wcslen(pFailure->lpCommand) + 1) * sizeof(WCHAR));
    if (pszCommand == NULL)
        return FALSE;
    wcscpy(pszCommand, pFailure->lpCommand);

    startPtr = pszCommand;
    if (*startPtr == L'\"')
    {
        startPtr++;
        endPtr = wcschr(startPtr, L'\"');
        if (endPtr != NULL)
        {
            *endPtr++ = UNICODE_NULL;
            /* Only the separator goes: the parameters keep their own blanks */
            if (*endPtr == L' ' || *endPtr == L'\t')
                endPtr++;
        }
    }
    else
    {
        /* Unquoted: the program is the shortest blank-delimited prefix that
         * CreateProcess finds; when none is found, it ends at the first blank */
        endPtr = NULL;
        for (failPtr = startPtr; ; failPtr++)
        {
            WCHAR chSaved = *failPtr;
            BOOL bFound;

            if (chSaved != UNICODE_NULL && chSaved != L' ' && chSaved != L'\t')
                continue;

            *failPtr = UNICODE_NULL;
            bFound = IsRunnableProgram(startPtr);
            *failPtr = chSaved;

            if (!bFound && chSaved == UNICODE_NULL)
                failPtr = wcspbrk(startPtr, L" \t");

            if (bFound || chSaved == UNICODE_NULL)
            {
                if (failPtr != NULL && *failPtr != UNICODE_NULL)
                {
                    *failPtr = UNICODE_NULL;
                    endPtr = failPtr + 1;
                }
                break;
            }
        }
    }

    if (!SetEditText(hwndDlg, IDC_PROGRAM, startPtr))
    {
        HeapFree(GetProcessHeap(), 0, pszCommand);
        return FALSE;
    }

    /* The split above is only for display: the service controller resolves an unquoted
     * program in its own environment, so it is not quoted on save unless it was edited */
    if (startPtr == pszCommand && *startPtr != UNICODE_NULL)
    {
        pRecoveryData->pszUnquotedProgram = HeapAlloc(GetProcessHeap(), 0, (wcslen(startPtr) + 1) * sizeof(WCHAR));
        if (pRecoveryData->pszUnquotedProgram == NULL)
        {
            HeapFree(GetProcessHeap(), 0, pszCommand);
            return FALSE;
        }
        wcscpy(pRecoveryData->pszUnquotedProgram, startPtr);
    }

    if (endPtr != NULL)
    {
        SIZE_T cchEnd;

        /* Only a trailing, separate /fail=%1% is the check box; other parameters stay as they are */
        cchEnd = wcslen(endPtr);
        while (cchEnd > 0 && iswspace(endPtr[cchEnd - 1]))
            cchEnd--;
        if (cchEnd >= 9 &&
            wcsncmp(&endPtr[cchEnd - 9], L"/fail=%1%", 9) == 0 &&
            (cchEnd == 9 || iswspace(endPtr[cchEnd - 10])))
        {
            SendDlgItemMessageW(hwndDlg,
                                IDC_ADD_FAILCOUNT,
                                BM_SETCHECK,
                                BST_CHECKED,
                                0);

            /* Only the blank BuildFailureCommand puts before the marker goes */
            failPtr = &endPtr[cchEnd - 9];
            if (failPtr > endPtr)
                failPtr--;
            *failPtr = UNICODE_NULL;
        }

        if (!SetEditText(hwndDlg, IDC_PARAMETERS, endPtr))
        {
            HeapFree(GetProcessHeap(), 0, pszCommand);
            return FALSE;
        }
    }

    HeapFree(GetProcessHeap(), 0, pszCommand);
    return TRUE;
}


static
VOID
UpdateFailureActions(
    HWND hwndDlg,
    PRECOVERYDATA pRecoveryData)
{
    INT id, index;
    BOOL bRestartService = FALSE;
    BOOL bRunProgram = FALSE;
    BOOL bRebootComputer = FALSE;

    for (id = IDC_FIRST_FAILURE; id <= IDC_SUBSEQUENT_FAILURES; id++)
    {
        index = SendDlgItemMessageW(hwndDlg,
                                    id,
                                    CB_GETCURSEL,
                                    0,
                                    0);
        switch (index)
        {
            case 1: /* Restart Service */
                bRestartService = TRUE;
                break;

            case 2: /* Run Program */
                bRunProgram = TRUE;
                break;

            case 3: /* Reboot Computer */
                bRebootComputer = TRUE;
                break;
        }
    }

    for (id = IDC_RESTART_TEXT1; id <= IDC_RESTART_TEXT2; id++)
         EnableWindow(GetDlgItem(hwndDlg, id), bRestartService);

    for (id = IDC_RUN_GROUPBOX; id <= IDC_ADD_FAILCOUNT; id++)
         EnableWindow(GetDlgItem(hwndDlg, id), bRunProgram);

    EnableWindow(GetDlgItem(hwndDlg, IDC_RESTART_OPTIONS), bRebootComputer);
}


static
VOID
BrowseFile(
    HWND hwndDlg)
{
    WCHAR szFile[MAX_PATH] = {'\0'};
    PWCHAR pszFilter = L"Executable Files (*.exe;*.com;*.cmd;*.bat)\0*.exe;*.com;*.cmd;*.bat\0";
    OPENFILENAME ofn;

    ZeroMemory(&ofn, sizeof(ofn));

    ofn.lStructSize = sizeof(ofn);
    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ENABLESIZING;
    ofn.hwndOwner = hwndDlg;
    ofn.lpstrFilter = pszFilter;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;

    if (GetOpenFileName(&ofn))
    {
        SendDlgItemMessageW(hwndDlg,
                            IDC_PROGRAM,
                            WM_SETTEXT,
                            0,
                            (LPARAM)szFile);
    }
}


static
BOOL
GetDlgItemDword(
    HWND hwndDlg,
    INT nIDDlgItem,
    DWORD dwMax,
    PDWORD pdwValue)
{
    WCHAR szText[16];
    ULONGLONG ullValue = 0;
    PWSTR p;

    /* The whole text must be a number of at most dwMax */
    if (GetDlgItemTextW(hwndDlg, nIDDlgItem, szText, _countof(szText)) == 0 ||
        GetWindowTextLengthW(GetDlgItem(hwndDlg, nIDDlgItem)) >= _countof(szText))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    for (p = szText; *p != UNICODE_NULL; p++)
    {
        if (*p < L'0' || *p > L'9')
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        ullValue = ullValue * 10 + (*p - L'0');
        if (ullValue > dwMax)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }

    *pdwValue = (DWORD)ullValue;
    return TRUE;
}


static
LPWSTR
BuildFailureCommand(
    HWND hwndDlg,
    PRECOVERYDATA pRecoveryData)
{
    INT cchProgram, cchParameters;
    SIZE_T cchCommand;
    LPWSTR lpCommand, lpProgram, lpParameters;

    cchProgram = GetWindowTextLengthW(GetDlgItem(hwndDlg, IDC_PROGRAM));
    cchParameters = GetWindowTextLengthW(GetDlgItem(hwndDlg, IDC_PARAMETERS));
    cchCommand = cchProgram + cchParameters + 16;

    lpCommand = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                          (cchCommand + cchProgram + 1 + cchParameters + 1) * sizeof(WCHAR));
    if (lpCommand == NULL)
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    lpProgram = lpCommand + cchCommand;
    lpParameters = lpProgram + cchProgram + 1;
    GetDlgItemTextW(hwndDlg, IDC_PROGRAM, lpProgram, cchProgram + 1);
    GetDlgItemTextW(hwndDlg, IDC_PARAMETERS, lpParameters, cchParameters + 1);

    /* An empty command deletes it */
    if (*lpProgram != UNICODE_NULL)
    {
        if (pRecoveryData->pszUnquotedProgram != NULL &&
            wcscmp(lpProgram, pRecoveryData->pszUnquotedProgram) == 0)
            StringCchCopyW(lpCommand, cchCommand, lpProgram);
        else
            StringCchPrintfW(lpCommand, cchCommand, L"\"%s\"", lpProgram);
        if (*lpParameters != UNICODE_NULL)
        {
            StringCchCatW(lpCommand, cchCommand, L" ");
            StringCchCatW(lpCommand, cchCommand, lpParameters);
        }
        if (IsDlgButtonChecked(hwndDlg, IDC_ADD_FAILCOUNT) == BST_CHECKED)
            StringCchCatW(lpCommand, cchCommand, L" /fail=%1%");
    }

    return lpCommand;
}


/* The box index ShowFailureActions selects for an action type ("Take No Action" for others) */
static
INT
GetActionIndex(
    SC_ACTION_TYPE Type)
{
    switch (Type)
    {
        case SC_ACTION_RESTART:
            return 1;
        case SC_ACTION_RUN_COMMAND:
            return 2;
        case SC_ACTION_REBOOT:
            return 3;
        default:
            return 0;
    }
}

/* TRUE when the three boxes still show the stored actions: a selection notification
 * also comes when the user picks the item that was already selected */
static
BOOL
AreActionsAsStored(
    HWND hwndDlg,
    LPSERVICE_FAILURE_ACTIONS pOld)
{
    DWORD i;

    for (i = 0; i < 3; i++)
    {
        INT index = 0;

        if (pOld->cActions > 0)
            index = GetActionIndex(pOld->lpsaActions[min(i, pOld->cActions - 1)].Type);

        if (SendDlgItemMessageW(hwndDlg, IDC_FIRST_FAILURE + i, CB_GETCURSEL, 0, 0) != index)
            return FALSE;
    }

    return TRUE;
}

/* Property sheets run on their own threads and share the process token: one
 * enable, service call and restore of the shutdown privilege at a time */
static
HANDLE
GetPrivilegeLock(VOID)
{
    static HANDLE hLock = NULL;
    HANDLE hNew;

    if (hLock != NULL)
        return hLock;

    hNew = CreateMutexW(NULL, FALSE, NULL);
    if (hNew == NULL)
        return NULL;

    if (InterlockedCompareExchangePointer(&hLock, hNew, NULL) != NULL)
        CloseHandle(hNew);

    return hLock;
}


static
BOOL
SetFailureActions(
    HWND hwndDlg,
    PRECOVERYDATA pRecoveryData)
{
    LPSERVICE_FAILURE_ACTIONS pOld = pRecoveryData->pServiceFailure;
    SERVICE_FAILURE_ACTIONSW FailureActions;
    SC_ACTION Actions[3];
    LPSC_ACTION pKeptActions = NULL;
    LPWSTR lpCommand = NULL;
    SC_HANDLE hManager = NULL;
    SC_HANDLE hService = NULL;
    HANDLE hToken = NULL;
    HANDLE hLock = NULL;
    TOKEN_PRIVILEGES Privileges, OldPrivileges;
    DWORD dwSize, dwError;
    BOOL bPrivilegeAdjusted = FALSE;
    BOOL bActionsChanged;
    BOOL bRebootComputer = FALSE;
    BOOL bRestartService = FALSE;
    BOOL bRestartMinutesRead = FALSE;
    BOOL bResult = FALSE;
    DWORD dwRestartMinutes = 1, dwResetDays = 0;
    DWORD i;
    INT index;

    ZeroMemory(&FailureActions, sizeof(FailureActions));
    ZeroMemory(Actions, sizeof(Actions));

    /* Never save against a configuration that could not be read */
    if (pOld == NULL)
    {
        SetLastError(pRecoveryData->dwLoadError ? pRecoveryData->dwLoadError : ERROR_INVALID_DATA);
        goto done;
    }

    bActionsChanged = pRecoveryData->bActionsChanged && !AreActionsAsStored(hwndDlg, pOld);

    if (pRecoveryData->bResetTimeChanged &&
        !GetDlgItemDword(hwndDlg, IDC_RESET_TIME, MAXDWORD / 86400, &dwResetDays))
    {
        goto done;
    }

    /* No array keeps the stored actions and reset period (also a reboot action, which would need
     * the shutdown privilege when passed); one is passed only for what was edited */
    if (!bActionsChanged && pOld->cActions > 0)
    {
        /* An edited minutes box changes only the restart delays */
        if (pRecoveryData->bRestartTimeChanged)
        {
            for (i = 0; i < pOld->cActions; i++)
            {
                if (pOld->lpsaActions[i].Type != SC_ACTION_RESTART)
                    continue;

                if (pKeptActions == NULL)
                {
                    if (!GetDlgItemDword(hwndDlg, IDC_RESTART_TIME, MAXDWORD / 60000, &dwRestartMinutes))
                        goto done;

                    pKeptActions = HeapAlloc(GetProcessHeap(), 0, pOld->cActions * sizeof(SC_ACTION));
                    if (pKeptActions == NULL)
                    {
                        SetLastError(ERROR_OUTOFMEMORY);
                        goto done;
                    }
                    CopyMemory(pKeptActions, pOld->lpsaActions, pOld->cActions * sizeof(SC_ACTION));
                }
                pKeptActions[i].Delay = dwRestartMinutes * 60000;
            }
        }

        /* Every action is kept, including those the page cannot show */
        if (pKeptActions != NULL || pRecoveryData->bResetTimeChanged)
        {
            FailureActions.cActions = pOld->cActions;
            FailureActions.lpsaActions = pKeptActions ? pKeptActions : pOld->lpsaActions;
        }
    }
    else if (!bActionsChanged)
    {
        /* An empty array would delete the reset period; three "no action" keep it */
        if (pRecoveryData->bResetTimeChanged)
        {
            FailureActions.cActions = 3;
            FailureActions.lpsaActions = Actions;
        }
    }
    else
    {
        /* One action each for the first, second and subsequent failures */
        for (i = 0; i < 3; i++)
        {
            LPSC_ACTION pOldAction = NULL;

            if (pOld->cActions > 0)
                pOldAction = &pOld->lpsaActions[min(i, pOld->cActions - 1)];

            index = SendDlgItemMessageW(hwndDlg,
                                        IDC_FIRST_FAILURE + i,
                                        CB_GETCURSEL,
                                        0,
                                        0);
            switch (index)
            {
                case 1: /* Restart Service */
                    Actions[i].Type = SC_ACTION_RESTART;
                    break;

                case 2: /* Run Program */
                    Actions[i].Type = SC_ACTION_RUN_COMMAND;
                    break;

                case 3: /* Reboot Computer */
                    Actions[i].Type = SC_ACTION_REBOOT;
                    break;

                default: /* Take No Action */
                    Actions[i].Type = SC_ACTION_NONE;
                    break;
            }

            /* An unchanged action keeps its delay */
            if (pOldAction != NULL && pOldAction->Type == Actions[i].Type)
                Actions[i].Delay = pOldAction->Delay;
            else if (Actions[i].Type == SC_ACTION_REBOOT)
                Actions[i].Delay = 60000; /* FIXME: The restart options are not implemented */

            /* The minutes box counts only for a restart action that takes it */
            if (Actions[i].Type == SC_ACTION_RESTART &&
                (pRecoveryData->bRestartTimeChanged || pOldAction == NULL || pOldAction->Type != SC_ACTION_RESTART))
            {
                if (!bRestartMinutesRead)
                {
                    if (!GetDlgItemDword(hwndDlg, IDC_RESTART_TIME, MAXDWORD / 60000, &dwRestartMinutes))
                        goto done;
                    bRestartMinutesRead = TRUE;
                }
                Actions[i].Delay = dwRestartMinutes * 60000;
            }
        }

        FailureActions.cActions = 3;
        FailureActions.lpsaActions = Actions;
    }

    if (!pRecoveryData->bResetTimeChanged)
        FailureActions.dwResetPeriod = pOld->dwResetPeriod;
    else
        FailureActions.dwResetPeriod = dwResetDays * 86400;

    /* Any reboot action written, also a kept one, needs the shutdown privilege */
    for (i = 0; i < FailureActions.cActions; i++)
    {
        if (FailureActions.lpsaActions[i].Type == SC_ACTION_REBOOT)
            bRebootComputer = TRUE;
        else if (FailureActions.lpsaActions[i].Type == SC_ACTION_RESTART)
            bRestartService = TRUE;
    }

    /* Nothing that was edited differs from what is stored */
    if (FailureActions.lpsaActions == NULL && !pRecoveryData->bCommandChanged)
    {
        bResult = TRUE;
        goto done;
    }

    /* The command is written only when it was edited */
    if (pRecoveryData->bCommandChanged)
    {
        lpCommand = BuildFailureCommand(hwndDlg, pRecoveryData);
        if (lpCommand == NULL)
            goto done;
        FailureActions.lpCommand = lpCommand;
    }

    /* A reboot action needs the shutdown privilege of the caller, for this call only */
    if (bRebootComputer)
    {
        hLock = GetPrivilegeLock();
        if (hLock == NULL)
            goto done;
        WaitForSingleObject(hLock, INFINITE);

        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        {
            Privileges.PrivilegeCount = 1;
            Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &Privileges.Privileges[0].Luid) &&
                AdjustTokenPrivileges(hToken, FALSE, &Privileges, sizeof(OldPrivileges), &OldPrivileges, &dwSize) &&
                GetLastError() == ERROR_SUCCESS)
            {
                bPrivilegeAdjusted = TRUE;
            }
        }
    }

    hManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (hManager == NULL)
        goto done;

    /* Windows needs SERVICE_START for a written restart action, the ReactOS service
     * controller for every failure actions change: without that right, only a change
     * that writes no restart action is tried with SERVICE_CHANGE_CONFIG alone */
    hService = OpenServiceW(hManager,
                            pRecoveryData->pService->lpServiceName,
                            SERVICE_CHANGE_CONFIG | SERVICE_START);
    if (hService == NULL && GetLastError() == ERROR_ACCESS_DENIED && !bRestartService)
    {
        hService = OpenServiceW(hManager,
                                pRecoveryData->pService->lpServiceName,
                                SERVICE_CHANGE_CONFIG);
    }
    if (hService == NULL)
        goto done;

    bResult = ChangeServiceConfig2W(hService,
                                    SERVICE_CONFIG_FAILURE_ACTIONS,
                                    &FailureActions);

done:
    dwError = GetLastError();

    if (hService)
        CloseServiceHandle(hService);

    if (hManager)
        CloseServiceHandle(hManager);

    if (bPrivilegeAdjusted)
        AdjustTokenPrivileges(hToken, FALSE, &OldPrivileges, 0, NULL, NULL);

    if (hToken)
        CloseHandle(hToken);

    if (hLock)
        ReleaseMutex(hLock);

    if (lpCommand)
        HeapFree(GetProcessHeap(), 0, lpCommand);

    if (pKeptActions)
        HeapFree(GetProcessHeap(), 0, pKeptActions);

    /* The message box comes after the privilege and the lock are given back */
    if (!bResult)
    {
        SetLastError(dwError);
        GetError();
    }

    return bResult;
}


INT_PTR
CALLBACK
RecoveryPageProc(
    HWND hwndDlg,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    PRECOVERYDATA pRecoveryData;

    /* Get the window context */
    pRecoveryData = (PRECOVERYDATA)GetWindowLongPtr(hwndDlg,
                                                    GWLP_USERDATA);
    if (pRecoveryData == NULL && uMsg != WM_INITDIALOG)
        return FALSE;

    switch (uMsg)
    {
        case WM_INITDIALOG:
            pRecoveryData = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RECOVERYDATA));
            if (pRecoveryData != NULL)
            {
                SetWindowLongPtr(hwndDlg,
                                 GWLP_USERDATA,
                                 (LONG_PTR)pRecoveryData);

                pRecoveryData->pService = ((PSERVICEPROPSHEET)(((LPPROPSHEETPAGE)lParam)->lParam))->pService;

                /* Filling the controls is not a change by the user */
                pRecoveryData->bInitializing = TRUE;

                InitRecoveryPage(hwndDlg);

                if (GetServiceFailure(pRecoveryData))
                {
                    /* A command that cannot be shown is not saved over either */
                    if (!ShowFailureActions(hwndDlg, pRecoveryData))
                    {
                        HeapFree(GetProcessHeap(), 0, pRecoveryData->pServiceFailure);
                        pRecoveryData->pServiceFailure = NULL;
                        pRecoveryData->dwLoadError = ERROR_OUTOFMEMORY;
                        SetLastError(ERROR_OUTOFMEMORY);
                        GetError();
                    }
                }
                else
                {
                    pRecoveryData->dwLoadError = GetLastError();
                    GetError();
                }

                pRecoveryData->bInitializing = FALSE;
            }
            break;

        case WM_DESTROY:
            if (pRecoveryData != NULL)
            {
                if (pRecoveryData->pServiceFailure != NULL)
                    HeapFree(GetProcessHeap(), 0, pRecoveryData->pServiceFailure);

                if (pRecoveryData->pszUnquotedProgram != NULL)
                    HeapFree(GetProcessHeap(), 0, pRecoveryData->pszUnquotedProgram);

                HeapFree(GetProcessHeap(), 0, pRecoveryData);
            }
            break;

        case WM_COMMAND:
            if (pRecoveryData->bInitializing)
                break;

            switch(LOWORD(wParam))
            {
                case IDC_FIRST_FAILURE:
                case IDC_SECOND_FAILURE:
                case IDC_SUBSEQUENT_FAILURES:
                    if (HIWORD(wParam) == CBN_SELCHANGE)
                    {
                        UpdateFailureActions(hwndDlg, pRecoveryData);
                        pRecoveryData->bActionsChanged = TRUE;
                        pRecoveryData->bChanged = TRUE;
                        PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                    }
                    break;

                case IDC_RESET_TIME:
                case IDC_RESTART_TIME:
                    if (HIWORD(wParam) == EN_CHANGE)
                    {
                        if (LOWORD(wParam) == IDC_RESET_TIME)
                            pRecoveryData->bResetTimeChanged = TRUE;
                        else
                            pRecoveryData->bRestartTimeChanged = TRUE;
                        pRecoveryData->bChanged = TRUE;
                        PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                    }
                    break;

                case IDC_PROGRAM:
                case IDC_PARAMETERS:
                    if (HIWORD(wParam) == EN_CHANGE)
                    {
                        /* An edited or browsed program is the user's: it is quoted on save */
                        if (LOWORD(wParam) == IDC_PROGRAM && pRecoveryData->pszUnquotedProgram != NULL)
                        {
                            HeapFree(GetProcessHeap(), 0, pRecoveryData->pszUnquotedProgram);
                            pRecoveryData->pszUnquotedProgram = NULL;
                        }
                        pRecoveryData->bCommandChanged = TRUE;
                        pRecoveryData->bChanged = TRUE;
                        PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                    }
                    break;

                case IDC_ADD_FAILCOUNT:
                    if (HIWORD(wParam) == BN_CLICKED)
                    {
                        pRecoveryData->bCommandChanged = TRUE;
                        pRecoveryData->bChanged = TRUE;
                        PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
                    }
                    break;

                case IDC_BROWSE_PROGRAM:
                    BrowseFile(hwndDlg);
                    break;

                case IDC_RESTART_OPTIONS:
                    break;
            }
            break;

        case WM_NOTIFY:
            switch (((LPNMHDR)lParam)->code)
            {
                case PSN_APPLY:
                    if (pRecoveryData->bChanged)
                    {
                        if (!SetFailureActions(hwndDlg, pRecoveryData))
                        {
                            SetWindowLongPtr(hwndDlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                            return TRUE;
                        }
                        pRecoveryData->bChanged = FALSE;
                        pRecoveryData->bActionsChanged = FALSE;
                        pRecoveryData->bRestartTimeChanged = FALSE;
                        pRecoveryData->bResetTimeChanged = FALSE;
                        pRecoveryData->bCommandChanged = FALSE;

                        /* The next Apply starts from what was saved now, or is refused */
                        {
                            LPSERVICE_FAILURE_ACTIONS pSaved = pRecoveryData->pServiceFailure;

                            pRecoveryData->pServiceFailure = NULL;
                            if (!GetServiceFailure(pRecoveryData))
                                pRecoveryData->dwLoadError = GetLastError();
                            HeapFree(GetProcessHeap(), 0, pSaved);
                        }
                    }
                    break;
            }
            break;
    }

    return FALSE;
}
