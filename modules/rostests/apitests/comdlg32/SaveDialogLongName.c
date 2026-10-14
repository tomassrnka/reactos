/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for over-long names typed in the Save As and Open dialogs
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define COBJMACROS
#include <apitest.h>
#include <windows.h>
#include <commdlg.h>
#include <dlgs.h>
#include <shlobj.h>
#include <strsafe.h>

typedef enum
{
    TEXT_REL,       /* a name, typed in the dialog's folder */
    TEXT_ABS,       /* the dialog's folder, a backslash and a name */
    TEXT_MISSING,   /* a path in a folder that does not exist */
    TEXT_MULTI,     /* two quoted names, the first one long */
    TEXT_LEAF       /* a name of exactly TotalLen characters */
} TEXT_KIND;

typedef enum
{
    EXPECT_ACCEPT,      /* the dialog returns the path */
    EXPECT_ACCEPT_ITEM, /* the same, but the item dialog may add the extension of the file type */
    EXPECT_REFUSE,      /* one message, the dialog stays open and keeps the typed text */
    EXPECT_REFUSE_ITEM, /* the same, but Windows 7 cuts what is typed in the item dialog to MAX_PATH */
    EXPECT_BUFSMALL,    /* FNERR_BUFFERTOOSMALL */
    EXPECT_ANY          /* Windows versions differ: the dialog must only end without a crash */
} EXPECT;

typedef struct
{
    PCSTR Name;
    BOOL Open;
    TEXT_KIND Kind;
    UINT FolderLen;     /* length of the dialog's folder, 0 for the temporary folder as is */
    UINT TotalLen;      /* folder, backslash and name; for TEXT_LEAF and TEXT_MULTI the name */
    UINT MaxFile;       /* size of the result buffer in characters */
    PCWSTR DefExt;
    EXPECT Expect;
} CASE;

typedef struct
{
    BOOL Returned;
    DWORD Error;
    INT Boxes;
    SIZE_T SpecLen;     /* the name box after the last message */
    SIZE_T ResultLen;
    BOOL StillOpen;
    BOOL TimedOut;
} RESULT;

static RESULT s_Result;
static PWSTR s_Typed;
static HWND s_hwndLastBox;
static INT s_Ticks;

static BOOL CALLBACK FindBoxProc(HWND hwnd, LPARAM lParam)
{
    HWND *pFound = (HWND *)lParam;
    WCHAR szClass[16];

    if (hwnd == pFound[0] || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != pFound[0])
        return TRUE;
    if (!GetClassNameW(hwnd, szClass, _countof(szClass)) || wcscmp(szClass, L"#32770") != 0)
        return TRUE;
    pFound[1] = hwnd;
    return FALSE;
}

static HWND FindMessageBox(HWND hwndDlg)
{
    HWND Found[2] = { hwndDlg, NULL };
    EnumThreadWindows(GetCurrentThreadId(), FindBoxProc, (LPARAM)Found);
    return Found[1];
}

/* One step of the watcher every dialog gets: closes the messages, TRUE when the dialog is quiet */
static BOOL WatchStep(HWND hwndDlg)
{
    HWND hwndBox = FindMessageBox(hwndDlg);

    if (hwndBox)
    {
        if (hwndBox != s_hwndLastBox)
            s_Result.Boxes++;
        s_hwndLastBox = hwndBox;
        s_Ticks = 0;
        PostMessageW(hwndBox, WM_COMMAND, IDOK, 0);
        return FALSE;
    }
    /* No further message for half a second after the last one; a second without any */
    return ++s_Ticks > (s_Result.Boxes ? 5 : 10);
}

static UINT_PTR CALLBACK HookProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    HWND hwndDlg = GetParent(hwnd);

    if (uMsg == WM_NOTIFY && ((LPOFNOTIFYW)lParam)->hdr.code == CDN_INITDONE)
    {
        SetTimer(hwnd, 2, 300, NULL);
    }
    else if (uMsg == WM_TIMER && wParam == 2)
    {
        KillTimer(hwnd, 2);
        SendMessageW(hwndDlg, CDM_SETCONTROLTEXT, cmb13, (LPARAM)s_Typed);
        SendMessageW(hwndDlg, CDM_SETCONTROLTEXT, edt1, (LPARAM)s_Typed);
        SetTimer(hwnd, 1, 100, NULL);
        PostMessageW(hwndDlg, WM_COMMAND, IDOK, 0);
    }
    else if (uMsg == WM_TIMER && wParam == 1)
    {
        if (WatchStep(hwndDlg))
        {
            KillTimer(hwnd, 1);
            s_Result.StillOpen = TRUE;
            s_Result.SpecLen = SendMessageW(hwndDlg, CDM_GETSPEC, 0, 0);
            PostMessageW(hwndDlg, WM_COMMAND, IDCANCEL, 0);
        }
    }
    return 0;
}

static BOOL CALLBACK FindDialogProc(HWND hwnd, LPARAM lParam)
{
    WCHAR szClass[16];
    if (IsWindowVisible(hwnd) && GetClassNameW(hwnd, szClass, _countof(szClass)) && !wcscmp(szClass, L"#32770"))
        PostMessageW(hwnd, WM_COMMAND, IDCANCEL, 0);
    return TRUE;
}

static VOID CALLBACK WatchdogProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
    s_Result.TimedOut = TRUE;
    EnumThreadWindows(GetCurrentThreadId(), FindDialogProc, 0);
}

static PWSTR Repeat(WCHAR ch, SIZE_T cch)
{
    PWSTR psz = HeapAlloc(GetProcessHeap(), 0, (cch + 1) * sizeof(WCHAR));
    SIZE_T i;
    for (i = 0; i < cch; i++)
        psz[i] = ch;
    psz[cch] = UNICODE_NULL;
    return psz;
}

/* Makes the folder the dialog starts in and the ones above it; they go to the list, outermost first */
static void MakeFolder(PCWSTR pszBase, UINT cchWanted, PWSTR pszFolder, PWSTR *ppszMade, UINT *pcMade)
{
    StringCchCopyW(pszFolder, MAX_PATH, pszBase);
    ppszMade[(*pcMade)++] = _wcsdup(pszFolder);
    CreateDirectoryW(pszFolder, NULL);
    for (;;)
    {
        SIZE_T cch = wcslen(pszFolder);
        UINT cchLevel;

        if (cch + 2 >= cchWanted || *pcMade >= 16)
            break;
        cchLevel = min(40, cchWanted - (UINT)cch - 1);
        StringCchCatW(pszFolder, MAX_PATH, L"\\");
        StringCchCatNW(pszFolder, MAX_PATH, L"dddddddddddddddddddddddddddddddddddddddd", cchLevel);
        ppszMade[(*pcMade)++] = _wcsdup(pszFolder);
        if (!CreateDirectoryW(pszFolder, NULL))
        {
            skip("CreateDirectoryW(%S) failed: %lu\n", pszFolder, GetLastError());
            break;
        }
    }
}

static void RemoveFolders(PWSTR *ppszMade, UINT cMade)
{
    while (cMade > 0)
    {
        RemoveDirectoryW(ppszMade[--cMade]);
        free(ppszMade[cMade]);
    }
}

/* Builds what is typed for the case; the folder is the one the dialog starts in */
static PWSTR MakeTyped(const CASE *pCase, PCWSTR pszFolder)
{
    SIZE_T cchFolder = wcslen(pszFolder);
    WCHAR szTemp[MAX_PATH];
    PWSTR pszName, pszTyped;

    switch (pCase->Kind)
    {
        case TEXT_REL:
            return Repeat(L'a', pCase->TotalLen - cchFolder - 1);
        case TEXT_LEAF:
            return Repeat(L'a', pCase->TotalLen);
        case TEXT_ABS:
            pszName = Repeat(L'a', pCase->TotalLen - cchFolder - 1);
            pszTyped = HeapAlloc(GetProcessHeap(), 0, (pCase->TotalLen + 2) * sizeof(WCHAR));
            StringCchPrintfW(pszTyped, pCase->TotalLen + 2, L"%s\\%s", pszFolder, pszName);
            break;
        case TEXT_MISSING:
            GetTempPathW(_countof(szTemp), szTemp);
            StringCchCatW(szTemp, _countof(szTemp), L"r3q-missing\\");
            pszName = Repeat(L'a', pCase->TotalLen - wcslen(szTemp));
            pszTyped = HeapAlloc(GetProcessHeap(), 0, (pCase->TotalLen + 2) * sizeof(WCHAR));
            StringCchPrintfW(pszTyped, pCase->TotalLen + 2, L"%s%s", szTemp, pszName);
            break;
        default:
            pszName = Repeat(L'a', pCase->TotalLen);
            pszTyped = HeapAlloc(GetProcessHeap(), 0, (pCase->TotalLen + 16) * sizeof(WCHAR));
            StringCchPrintfW(pszTyped, pCase->TotalLen + 16, L"\"%s\" \"b\"", pszName);
            break;
    }
    HeapFree(GetProcessHeap(), 0, pszName);
    return pszTyped;
}

static void CheckResult(const CASE *pCase, PCWSTR pszTyped, SIZE_T cchExpectedResult)
{
    const RESULT *pr = &s_Result;

    ok(!pr->TimedOut, "%s: The dialog did not finish in time\n", pCase->Name);
    switch (pCase->Expect)
    {
        case EXPECT_ACCEPT:
            ok(pr->Returned, "%s: The dialog returned FALSE, error 0x%lx, %d message(s)\n", pCase->Name, pr->Error, pr->Boxes);
            ok(pr->Boxes == 0, "%s: Messages: %d\n", pCase->Name, pr->Boxes);
            ok(pr->ResultLen == cchExpectedResult, "%s: Result length %Iu, expected %Iu\n", pCase->Name, pr->ResultLen, cchExpectedResult);
            break;
        case EXPECT_ACCEPT_ITEM:
            ok(pr->Returned, "%s: The dialog returned FALSE, %d message(s)\n", pCase->Name, pr->Boxes);
            ok(pr->Boxes == 0, "%s: Messages: %d\n", pCase->Name, pr->Boxes);
            ok(pr->ResultLen >= cchExpectedResult && pr->ResultLen <= cchExpectedResult + 5, "%s: Result length %Iu, expected %Iu\n", pCase->Name, pr->ResultLen, cchExpectedResult);
            break;
        case EXPECT_REFUSE:
            ok(!pr->Returned, "%s: The dialog returned TRUE\n", pCase->Name);
            ok(pr->Boxes == 1, "%s: Messages: %d, expected 1\n", pCase->Name, pr->Boxes);
            ok(pr->StillOpen, "%s: The dialog closed\n", pCase->Name);
            ok(pr->SpecLen == wcslen(pszTyped) + 1, "%s: Name box length %Iu, expected %Iu\n", pCase->Name, pr->SpecLen, wcslen(pszTyped) + 1);
            break;
        case EXPECT_REFUSE_ITEM:
            ok(!pr->Returned, "%s: The dialog returned TRUE\n", pCase->Name);
            ok(pr->Boxes == 1, "%s: Messages: %d, expected 1\n", pCase->Name, pr->Boxes);
            ok(pr->StillOpen, "%s: The dialog closed\n", pCase->Name);
            break;
        case EXPECT_BUFSMALL:
            ok(pr->Error == FNERR_BUFFERTOOSMALL, "%s: Error 0x%lx, expected FNERR_BUFFERTOOSMALL\n", pCase->Name, pr->Error);
            break;
        default:
            break;
    }
}

static void RunClassicCase(const CASE *pCase, PCWSTR pszBase)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR szFolder[MAX_PATH];
    PWSTR Made[16], pszFile;
    UINT cMade = 0;
    UINT_PTR idWatchdog;
    SIZE_T cchExpected;

    ZeroMemory(&s_Result, sizeof(s_Result));
    s_hwndLastBox = NULL;
    s_Ticks = 0;

    MakeFolder(pszBase, pCase->FolderLen, szFolder, Made, &cMade);
    s_Typed = MakeTyped(pCase, szFolder);
    cchExpected = pCase->TotalLen + (pCase->DefExt ? 1 + wcslen(pCase->DefExt) : 0);

    pszFile = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (pCase->MaxFile + 4) * sizeof(WCHAR));
    ofn.lpstrFilter = L"Probe files\0*.r3q\0";
    ofn.lpstrFile = pszFile;
    ofn.nMaxFile = pCase->MaxFile;
    ofn.lpstrInitialDir = szFolder;
    ofn.lpstrDefExt = pCase->DefExt;
    ofn.Flags = OFN_EXPLORER | OFN_ENABLEHOOK | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (pCase->Open)
        ofn.Flags |= OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT;
    else
        ofn.Flags |= OFN_OVERWRITEPROMPT;
    ofn.lpfnHook = HookProc;

    idWatchdog = SetTimer(NULL, 0, 30000, WatchdogProc);
    s_Result.Returned = pCase->Open ? GetOpenFileNameW(&ofn) : GetSaveFileNameW(&ofn);
    s_Result.Error = CommDlgExtendedError();
    KillTimer(NULL, idWatchdog);
    s_Result.ResultLen = wcslen(pszFile);

    CheckResult(pCase, s_Typed, cchExpected);

    HeapFree(GetProcessHeap(), 0, pszFile);
    HeapFree(GetProcessHeap(), 0, s_Typed);
    s_Typed = NULL;
    RemoveFolders(Made, cMade);
}

/* A path of MAX_PATH characters or more is accepted in different ways (short names, a prefix for
 * long paths) by Windows Server 2008 R2 and Windows 10, so those cases only have to end;
 * the others behave the same on both */
static const CASE ClassicCases[] =
{
    { "save-leaf-255", FALSE, TEXT_LEAF, 0, 255, 40000, NULL, EXPECT_ANY },
    { "save-leaf-256", FALSE, TEXT_LEAF, 0, 256, 40000, NULL, EXPECT_REFUSE },
    { "save-258", FALSE, TEXT_REL, 0, 258, 40000, NULL, EXPECT_ACCEPT },
    { "save-259", FALSE, TEXT_REL, 0, 259, 40000, NULL, EXPECT_ACCEPT },
    { "save-260", FALSE, TEXT_REL, 0, 260, 40000, NULL, EXPECT_ANY },
    { "save-261", FALSE, TEXT_REL, 0, 261, 40000, NULL, EXPECT_ANY },
    { "save-330", FALSE, TEXT_REL, 0, 330, 40000, NULL, EXPECT_REFUSE },
    { "save-abs-300", FALSE, TEXT_ABS, 0, 300, 40000, NULL, EXPECT_ANY },
    { "save-deep-330", FALSE, TEXT_REL, 120, 330, 40000, NULL, EXPECT_ANY },
    { "save-missing-300", FALSE, TEXT_MISSING, 0, 300, 40000, NULL, EXPECT_REFUSE },
    { "save-5000", FALSE, TEXT_REL, 0, 5000, 40000, NULL, EXPECT_REFUSE },
    { "save-30000", FALSE, TEXT_REL, 0, 30000, 40000, NULL, EXPECT_REFUSE },
    { "save-defext-255", FALSE, TEXT_REL, 0, 255, 40000, L"r3q", EXPECT_ACCEPT },
    { "save-defext-258", FALSE, TEXT_REL, 0, 258, 40000, L"r3q", EXPECT_ANY },
    { "save-maxfile-small", FALSE, TEXT_REL, 0, 200, 100, NULL, EXPECT_BUFSMALL },
    { "open-multi-350", TRUE, TEXT_MULTI, 0, 350, 40000, NULL, EXPECT_REFUSE },
    { "open-multi-5000", TRUE, TEXT_MULTI, 0, 5000, 40000, NULL, EXPECT_REFUSE },
    { "open-single-300", TRUE, TEXT_REL, 0, 300, 40000, NULL, EXPECT_REFUSE },
};

/* The common item dialog */

static IFileDialog *s_pfd;
static HWND s_hwndItemDlg;
static BOOL s_ItemPressed;
static BOOL s_ItemChangeFilter;     /* select the second file type before pressing OK */
static WCHAR s_ItemNameAfter[8192];

static BOOL CALLBACK FindItemDialogProc(HWND hwnd, LPARAM lParam)
{
    WCHAR szClass[16];

    if (IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER) &&
        GetClassNameW(hwnd, szClass, _countof(szClass)) && !wcscmp(szClass, L"#32770"))
    {
        *(HWND *)lParam = hwnd;
        return FALSE;
    }
    return TRUE;
}

static BOOL CALLBACK FindFileTypeProc(HWND hwnd, LPARAM lParam)
{
    WCHAR szClass[16];

    if (GetClassNameW(hwnd, szClass, _countof(szClass)) && !_wcsicmp(szClass, L"ComboBox") &&
        SendMessageW(hwnd, CB_GETCOUNT, 0, 0) == 2)
    {
        *(HWND *)lParam = hwnd;
        return FALSE;
    }
    return TRUE;
}

static VOID CALLBACK ItemWatchProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
    if (!s_hwndItemDlg)
    {
        EnumThreadWindows(GetCurrentThreadId(), FindItemDialogProc, (LPARAM)&s_hwndItemDlg);
        s_Ticks = 0;
        return;
    }
    if (!s_ItemPressed)
    {
        /* The dialog browses to its folder on its own; OK before that does nothing */
        if (s_Ticks++ < 25)
            return;
        if (s_ItemChangeFilter)
        {
            HWND hwndCombo = NULL;

            EnumChildWindows(s_hwndItemDlg, FindFileTypeProc, (LPARAM)&hwndCombo);
            if (hwndCombo)
            {
                SendMessageW(hwndCombo, CB_SETCURSEL, 1, 0);
                SendMessageW(s_hwndItemDlg, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwndCombo), CBN_SELCHANGE), (LPARAM)hwndCombo);
            }
            else
            {
                skip("The file type box was not found\n");
            }
        }
        PostMessageW(s_hwndItemDlg, WM_COMMAND, IDOK, 0);
        s_ItemPressed = TRUE;
        return;
    }
    if (WatchStep(s_hwndItemDlg) && IsWindow(s_hwndItemDlg))
    {
        PWSTR pszName = NULL;

        s_Result.StillOpen = TRUE;
        if (SUCCEEDED(IFileDialog_GetFileName(s_pfd, &pszName)) && pszName)
        {
            s_Result.SpecLen = wcslen(pszName) + 1;
            StringCchCopyW(s_ItemNameAfter, _countof(s_ItemNameAfter), pszName);
            CoTaskMemFree(pszName);
        }
        PostMessageW(s_hwndItemDlg, WM_COMMAND, IDCANCEL, 0);
    }
}

static void RunItemCase(const CASE *pCase, PCWSTR pszBase, BOOL bChangeFilter)
{
    static const COMDLG_FILTERSPEC Filters[] = { { L"Probe files", L"*.r3q" }, { L"Other files", L"*.r3o" } };
    WCHAR szFolder[MAX_PATH];
    PWSTR Made[16];
    UINT cMade = 0;
    UINT_PTR idWatch, idWatchdog;
    typedef HRESULT (WINAPI *PFN_SHCREATEITEMFROMPARSINGNAME)(PCWSTR, IBindCtx *, REFIID, void **);
    PFN_SHCREATEITEMFROMPARSINGNAME pfnCreateItem;
    IShellItem *psiFolder = NULL;
    HRESULT hr;

    ZeroMemory(&s_Result, sizeof(s_Result));
    s_hwndLastBox = NULL;
    s_hwndItemDlg = NULL;
    s_ItemPressed = FALSE;
    s_ItemChangeFilter = bChangeFilter;
    s_ItemNameAfter[0] = UNICODE_NULL;
    s_Ticks = 0;

    MakeFolder(pszBase, pCase->FolderLen, szFolder, Made, &cMade);
    s_Typed = MakeTyped(pCase, szFolder);

    hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog, (void **)&s_pfd);
    if (FAILED(hr))
    {
        skip("The item dialog is not available: 0x%lx\n", hr);
        goto cleanup;
    }
    pfnCreateItem = (PFN_SHCREATEITEMFROMPARSINGNAME)GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHCreateItemFromParsingName");
    if (pfnCreateItem && SUCCEEDED(pfnCreateItem(szFolder, NULL, &IID_IShellItem, (void **)&psiFolder)))
    {
        IFileDialog_SetFolder(s_pfd, psiFolder);
        IShellItem_Release(psiFolder);
    }
    IFileDialog_SetFileTypes(s_pfd, _countof(Filters), Filters);
    IFileDialog_SetFileName(s_pfd, s_Typed);
    if (pCase->DefExt)
        IFileDialog_SetDefaultExtension(s_pfd, pCase->DefExt);

    idWatch = SetTimer(NULL, 0, 100, ItemWatchProc);
    idWatchdog = SetTimer(NULL, 0, 30000, WatchdogProc);
    hr = IFileDialog_Show(s_pfd, NULL);
    KillTimer(NULL, idWatch);
    KillTimer(NULL, idWatchdog);
    s_Result.Returned = SUCCEEDED(hr);

    if (s_Result.Returned)
    {
        IShellItem *psi = NULL;
        PWSTR pszPath = NULL;

        if (SUCCEEDED(IFileDialog_GetResult(s_pfd, &psi)))
        {
            if (SUCCEEDED(IShellItem_GetDisplayName(psi, SIGDN_FILESYSPATH, &pszPath)))
            {
                s_Result.ResultLen = wcslen(pszPath);
                CoTaskMemFree(pszPath);
            }
            IShellItem_Release(psi);
        }
    }

    CheckResult(pCase, s_Typed, pCase->TotalLen + (pCase->DefExt ? 1 + wcslen(pCase->DefExt) : 0));
    if (bChangeFilter && s_Result.StillOpen)
        ok(wcslen(s_ItemNameAfter) <= wcslen(s_Typed), "%s: The typed name grew with the file type\n", pCase->Name);

    IFileDialog_Release(s_pfd);
    s_pfd = NULL;
cleanup:
    HeapFree(GetProcessHeap(), 0, s_Typed);
    s_Typed = NULL;
    RemoveFolders(Made, cMade);
}

static const CASE ItemCases[] =
{
    { "item-250", FALSE, TEXT_REL, 0, 250, 0, NULL, EXPECT_ACCEPT_ITEM },
    { "item-leaf-256", FALSE, TEXT_LEAF, 0, 256, 0, NULL, EXPECT_REFUSE_ITEM },
    { "item-330", FALSE, TEXT_REL, 0, 330, 0, NULL, EXPECT_REFUSE_ITEM },
    { "item-missing-300", FALSE, TEXT_MISSING, 0, 300, 0, NULL, EXPECT_REFUSE_ITEM },
    { "item-5000", FALSE, TEXT_REL, 0, 5000, 0, NULL, EXPECT_REFUSE_ITEM },
    { "item-defext-258", FALSE, TEXT_REL, 0, 258, 0, L"r3q", EXPECT_ANY },
};

START_TEST(SaveDialogLongName)
{
    WCHAR szTemp[MAX_PATH], szBase[MAX_PATH];
    UINT i;

    GetTempPathW(_countof(szTemp), szTemp);
    if (!GetLongPathNameW(szTemp, szBase, _countof(szBase)))
        StringCchCopyW(szBase, _countof(szBase), szTemp);
    StringCchCopyW(szTemp, _countof(szTemp), szBase);
    StringCchPrintfW(szBase, _countof(szBase), L"%s%sr3q-ln-%lu", szTemp,
                     szTemp[wcslen(szTemp) - 1] == L'\\' ? L"" : L"\\", GetTickCount());

    for (i = 0; i < _countof(ClassicCases); i++)
        RunClassicCase(&ClassicCases[i], szBase);

    if (SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
    {
        IUnknown *punk;

        /* The item dialog does nothing without the browser that shows its folder */
        if (FAILED(CoCreateInstance(&CLSID_ExplorerBrowser, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&punk)))
        {
            skip("ExplorerBrowser is not available, the item dialog cannot work\n");
            CoUninitialize();
            return;
        }
        IUnknown_Release(punk);

        for (i = 0; i < _countof(ItemCases); i++)
            RunItemCase(&ItemCases[i], szBase, FALSE);
        /* Another file type changes the extension of the typed name, when it fits */
        RunItemCase(&ItemCases[4], szBase, TRUE);
        CoUninitialize();
    }
}
