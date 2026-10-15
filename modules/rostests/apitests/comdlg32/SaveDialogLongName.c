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
    TEXT_LEAF,      /* a name of exactly TotalLen characters */
    TEXT_KANA,      /* the folder and TotalLen hiragana: two bytes each on a double byte code page */
    TEXT_ABSSHORT   /* a short path on the drive of the dialog's folder */
} TEXT_KIND;

typedef enum
{
    EXPECT_ACCEPT,      /* the dialog returns the path */
    EXPECT_ACCEPT_ANY,  /* the dialog returns a path; its length depends on the code page */
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
    UINT MaxFile;       /* size of the result buffer, 0 for the size of the path (see MaxFileDelta) */
    PCWSTR DefExt;
    EXPECT Expect;
    BOOL Ansi;          /* the A function, sizes in bytes */
    INT MaxFileDelta;   /* MaxFile 0: room for the path and a terminator, plus this */
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
    WCHAR BoxText[160];   /* the text of the first message, as far as it can be read */
} RESULT;

static RESULT s_Result;
static PWSTR s_Typed;
static WCHAR s_ExpectFolder[MAX_PATH];  /* the folder the dialog starts in */
static INT s_FolderTries;
static DWORD s_LastChange;    /* tick of the last folder change the dialog reported */
static PSTR s_TypedA;       /* the same for a dialog opened by an A function */
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

static BOOL CALLBACK FindTextProc(HWND hwnd, LPARAM lParam)
{
    WCHAR szClass[16], szText[160];

    if (GetClassNameW(hwnd, szClass, _countof(szClass)) && !_wcsicmp(szClass, L"Static") &&
        GetWindowTextW(hwnd, szText, _countof(szText)) > 3)
    {
        StringCchCopyW((PWSTR)lParam, _countof(s_Result.BoxText), szText);
        return FALSE;
    }
    return TRUE;
}

/* One step of the watcher every dialog gets: closes the messages, TRUE when the dialog is quiet */
static BOOL WatchStep(HWND hwndDlg)
{
    HWND hwndBox = FindMessageBox(hwndDlg);

    if (hwndBox)
    {
        if (hwndBox != s_hwndLastBox)
        {
            s_Result.Boxes++;
            if (!s_Result.BoxText[0])
                EnumChildWindows(hwndBox, FindTextProc, (LPARAM)s_Result.BoxText);
        }
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

    if (uMsg == WM_NOTIFY && ((LPOFNOTIFYW)lParam)->hdr.code == CDN_FOLDERCHANGE)
    {
        s_LastChange = GetTickCount();
    }
    else if (uMsg == WM_NOTIFY && ((LPOFNOTIFYW)lParam)->hdr.code == CDN_INITDONE)
    {
        s_FolderTries = 0;
        s_LastChange = GetTickCount();
        SetTimer(hwnd, 2, 100, NULL);
    }
    else if (uMsg == WM_TIMER && wParam == 2)
    {
        /* The dialog goes to its folder after it is up; type when it is there, or when it is not coming */
        WCHAR szFolder[MAX_PATH] = L"", szLong[MAX_PATH];

        if (s_TypedA)
        {
            CHAR szFolderA[MAX_PATH] = "";
            SendMessageA(hwndDlg, CDM_GETFOLDERPATH, sizeof(szFolderA), (LPARAM)szFolderA);
            MultiByteToWideChar(CP_ACP, 0, szFolderA, -1, szFolder, _countof(szFolder));
        }
        else
        {
            SendMessageW(hwndDlg, CDM_GETFOLDERPATH, _countof(szFolder), (LPARAM)szFolder);
        }
        if (GetLongPathNameW(szFolder, szLong, _countof(szLong)))
            StringCchCopyW(szFolder, _countof(szFolder), szLong);
        if ((lstrcmpiW(szFolder, s_ExpectFolder) || GetTickCount() - s_LastChange < 500) && ++s_FolderTries < 60)
            return 0;
        KillTimer(hwnd, 2);
        if (s_TypedA)
        {
            SendMessageA(hwndDlg, CDM_SETCONTROLTEXT, cmb13, (LPARAM)s_TypedA);
            SendMessageA(hwndDlg, CDM_SETCONTROLTEXT, edt1, (LPARAM)s_TypedA);
        }
        else
        {
            SendMessageW(hwndDlg, CDM_SETCONTROLTEXT, cmb13, (LPARAM)s_Typed);
            SendMessageW(hwndDlg, CDM_SETCONTROLTEXT, edt1, (LPARAM)s_Typed);
        }
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

/* The folders go beyond the limit of the plain calls */
static BOOL CreateFolder(PCWSTR pszPath)
{
    WCHAR szLong[MAX_PATH + 8];
    StringCchPrintfW(szLong, _countof(szLong), L"\\\\?\\%s", pszPath);
    return CreateDirectoryW(szLong, NULL);
}

static void RemoveFolder(PCWSTR pszPath)
{
    WCHAR szLong[MAX_PATH + 8];
    StringCchPrintfW(szLong, _countof(szLong), L"\\\\?\\%s", pszPath);
    RemoveDirectoryW(szLong);
}

/* Makes the folder the dialog starts in and the ones above it; they go to the list, outermost first */
static void MakeFolder(PCWSTR pszBase, UINT cchWanted, PWSTR pszFolder, PWSTR *ppszMade, UINT *pcMade)
{
    StringCchCopyW(pszFolder, MAX_PATH, pszBase);
    ppszMade[(*pcMade)++] = _wcsdup(pszFolder);
    CreateFolder(pszFolder);
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
        if (!CreateFolder(pszFolder))
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
        RemoveFolder(ppszMade[--cMade]);
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
        case TEXT_KANA:
            pszName = Repeat(0x3042, pCase->TotalLen);
            pszTyped = HeapAlloc(GetProcessHeap(), 0, (cchFolder + pCase->TotalLen + 2) * sizeof(WCHAR));
            StringCchPrintfW(pszTyped, cchFolder + pCase->TotalLen + 2, L"%s\\%s", pszFolder, pszName);
            HeapFree(GetProcessHeap(), 0, pszName);
            return pszTyped;
        case TEXT_ABSSHORT:
            pszTyped = HeapAlloc(GetProcessHeap(), 0, 32 * sizeof(WCHAR));
            StringCchPrintfW(pszTyped, 32, L"%c:\\r3qabs.r3q", pszFolder[0]);
            return pszTyped;
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
        case EXPECT_ACCEPT_ANY:
            ok(pr->Returned, "%s: The dialog returned FALSE, error 0x%lx, %d message(s)\n", pCase->Name, pr->Error, pr->Boxes);
            ok(pr->Boxes == 0, "%s: Messages: %d\n", pCase->Name, pr->Boxes);
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

static void RunClassicCase(const CASE *pCase, UINT Index, PCWSTR pszBase)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    OPENFILENAMEA ofnA = { sizeof(ofnA) };
    WCHAR szFolder[MAX_PATH], szBaseCase[MAX_PATH];
    CHAR szFolderA[MAX_PATH * 2], szDefExtA[16], szFilterA[48];
    WCHAR szFilter[48], szExt[8];
    PWSTR Made[16], pszFile;
    PSTR pszFileA = NULL;
    UINT cMade = 0, cchFolder, nMaxFile, cchFilter;
    UINT_PTR idWatchdog;
    SIZE_T cchExpected;
    DWORD dwFlags;

    if (pCase->Kind == TEXT_KANA && GetACP() != 932)
    {
        skip("%s: needs the Japanese code page\n", pCase->Name);
        return;
    }
    ZeroMemory(&s_Result, sizeof(s_Result));
    s_hwndLastBox = NULL;
    s_Ticks = 0;

    /* a folder of its own: the shell may remember that a folder of the same name was gone a moment ago */
    StringCchPrintfW(szBaseCase, _countof(szBaseCase), L"%s-%u", pszBase, Index);
    MakeFolder(szBaseCase, pCase->FolderLen, szFolder, Made, &cMade);
    cchFolder = (UINT)wcslen(szFolder);
    if (!GetLongPathNameW(szFolder, s_ExpectFolder, _countof(s_ExpectFolder)))
        StringCchCopyW(s_ExpectFolder, _countof(s_ExpectFolder), szFolder);
    s_Typed = MakeTyped(pCase, szFolder);
    cchExpected = pCase->TotalLen + (pCase->DefExt ? 1 + wcslen(pCase->DefExt) : 0);
    nMaxFile = pCase->MaxFile;
    if (!nMaxFile)
        nMaxFile = (pCase->Kind == TEXT_KANA ? cchFolder + 1 + pCase->TotalLen : pCase->TotalLen) + 1 + pCase->MaxFileDelta;

    /* The dialog remembers the folder per extension: every run and case gets its own */
    StringCchPrintfW(szExt, _countof(szExt), L"%03x", (GetTickCount() + GetCurrentProcessId() + Index * 7) & 0xfff);
    StringCchCopyW(szFilter, _countof(szFilter), L"Probe files");
    cchFilter = (UINT)wcslen(szFilter) + 1;
    StringCchPrintfW(szFilter + cchFilter, _countof(szFilter) - cchFilter - 1, L"*.%s", szExt);
    cchFilter += (UINT)wcslen(szFilter + cchFilter) + 1;
    szFilter[cchFilter] = UNICODE_NULL;
    WideCharToMultiByte(CP_ACP, 0, szFilter, cchFilter + 1, szFilterA, sizeof(szFilterA), NULL, NULL);

    dwFlags = OFN_EXPLORER | OFN_ENABLEHOOK | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (pCase->Open)
        dwFlags |= OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT;
    else
        dwFlags |= OFN_OVERWRITEPROMPT;

    idWatchdog = SetTimer(NULL, 0, 30000, WatchdogProc);
    if (pCase->Ansi)
    {
        int cbTyped = WideCharToMultiByte(CP_ACP, 0, s_Typed, -1, NULL, 0, NULL, NULL);

        s_TypedA = HeapAlloc(GetProcessHeap(), 0, cbTyped + 1);
        WideCharToMultiByte(CP_ACP, 0, s_Typed, -1, s_TypedA, cbTyped + 1, NULL, NULL);
        pszFileA = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, nMaxFile + 8);
        WideCharToMultiByte(CP_ACP, 0, szFolder, -1, szFolderA, sizeof(szFolderA), NULL, NULL);
        WideCharToMultiByte(CP_ACP, 0, szExt, -1, szDefExtA, sizeof(szDefExtA), NULL, NULL);
        ofnA.lpstrFilter = szFilterA;
        ofnA.lpstrFile = pszFileA;
        ofnA.nMaxFile = nMaxFile;
        ofnA.lpstrInitialDir = szFolderA;
        ofnA.lpstrDefExt = pCase->DefExt ? szDefExtA : NULL;
        ofnA.Flags = dwFlags;
        ofnA.lpfnHook = HookProc;
        s_Result.Returned = pCase->Open ? GetOpenFileNameA(&ofnA) : GetSaveFileNameA(&ofnA);
        s_Result.ResultLen = strlen(pszFileA);
    }
    else
    {
        pszFile = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (nMaxFile + 4) * sizeof(WCHAR));
        ofn.lpstrFilter = szFilter;
        ofn.lpstrFile = pszFile;
        ofn.nMaxFile = nMaxFile;
        ofn.lpstrInitialDir = szFolder;
        ofn.lpstrDefExt = pCase->DefExt ? szExt : NULL;
        ofn.Flags = dwFlags;
        ofn.lpfnHook = HookProc;
        s_Result.Returned = pCase->Open ? GetOpenFileNameW(&ofn) : GetSaveFileNameW(&ofn);
        s_Result.ResultLen = wcslen(pszFile);
    }
    s_Result.Error = CommDlgExtendedError();
    KillTimer(NULL, idWatchdog);

    trace("%s: returned %d, error 0x%lx, %d message(s), result %Iu, left open %d\n", pCase->Name,
          s_Result.Returned, s_Result.Error, s_Result.Boxes, s_Result.ResultLen, s_Result.StillOpen);
    if (s_Result.Returned && !pCase->Ansi && s_Result.ResultLen > 100)
        trace("  folder %S, result %.*S\n", szFolder, 60, pszFile);
    else if (s_Result.Returned && !pCase->Ansi)
        trace("  folder %S, result %S\n", szFolder, pszFile);
    if (s_Result.BoxText[0])
        trace("  message: %S\n", s_Result.BoxText);
    CheckResult(pCase, s_Typed, cchExpected);

    if (pCase->Ansi)
    {
        HeapFree(GetProcessHeap(), 0, pszFileA);
        HeapFree(GetProcessHeap(), 0, s_TypedA);
        s_TypedA = NULL;
    }
    else
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
    { "save-258", FALSE, TEXT_ABS, 0, 258, 40000, NULL, EXPECT_ACCEPT },
    { "save-259", FALSE, TEXT_ABS, 0, 259, 40000, NULL, EXPECT_ACCEPT },
    { "save-260", FALSE, TEXT_REL, 0, 260, 40000, NULL, EXPECT_ANY },
    { "save-261", FALSE, TEXT_REL, 0, 261, 40000, NULL, EXPECT_ANY },
    { "save-330", FALSE, TEXT_REL, 0, 330, 40000, NULL, EXPECT_REFUSE },
    { "save-abs-300", FALSE, TEXT_ABS, 0, 300, 40000, NULL, EXPECT_ANY },
    { "save-deep-330", FALSE, TEXT_REL, 120, 330, 40000, NULL, EXPECT_ANY },
    { "save-missing-300", FALSE, TEXT_MISSING, 0, 300, 40000, NULL, EXPECT_REFUSE },
    { "save-5000", FALSE, TEXT_REL, 0, 5000, 40000, NULL, EXPECT_REFUSE },
    { "save-30000", FALSE, TEXT_REL, 0, 30000, 40000, NULL, EXPECT_REFUSE },
    { "save-defext-255", FALSE, TEXT_ABS, 0, 255, 40000, L"r3q", EXPECT_ACCEPT },
    { "save-defext-258", FALSE, TEXT_REL, 0, 258, 40000, L"r3q", EXPECT_ANY },
    { "save-maxfile-small", FALSE, TEXT_REL, 0, 200, 100, NULL, EXPECT_BUFSMALL },
    { "open-multi-350", TRUE, TEXT_MULTI, 0, 350, 40000, NULL, EXPECT_REFUSE },
    { "open-multi-5000", TRUE, TEXT_MULTI, 0, 5000, 40000, NULL, EXPECT_REFUSE },
    { "open-single-300", TRUE, TEXT_REL, 0, 300, 40000, NULL, EXPECT_REFUSE },
    /* The result buffer is exactly as large as the path, or has no room for a default extension */
    { "save-exact", FALSE, TEXT_ABS, 0, 100, 0, NULL, EXPECT_ACCEPT, FALSE, 0 },
    { "save-exact-minus-1", FALSE, TEXT_ABS, 0, 100, 0, NULL, EXPECT_BUFSMALL, FALSE, -1 },
    { "save-defext-fits", FALSE, TEXT_ABS, 0, 100, 0, L"r3q", EXPECT_ACCEPT, FALSE, 4 },
    { "save-defext-nofit", FALSE, TEXT_ABS, 0, 100, 0, L"r3q", EXPECT_BUFSMALL, FALSE, 3 },
    { "save-multi-small", FALSE, TEXT_MULTI, 0, 20, 40, NULL, EXPECT_BUFSMALL },
    /* The same through the A functions, where the sizes are bytes */
    { "ansi-save-exact", FALSE, TEXT_ABS, 0, 100, 0, NULL, EXPECT_ACCEPT, TRUE, 0 },
    { "ansi-save-exact-minus-1", FALSE, TEXT_ABS, 0, 100, 0, NULL, EXPECT_BUFSMALL, TRUE, -1 },
    { "ansi-save-defext-fits", FALSE, TEXT_ABS, 0, 100, 0, L"r3q", EXPECT_ACCEPT, TRUE, 4 },
    { "ansi-save-defext-nofit", FALSE, TEXT_ABS, 0, 100, 0, L"r3q", EXPECT_BUFSMALL, TRUE, 3 },
    { "ansi-save-multi-small", FALSE, TEXT_MULTI, 0, 20, 40, NULL, EXPECT_BUFSMALL, TRUE, 0 },
    /* A double byte code page needs more bytes than the path has characters */
    { "ansi-kana-fits", FALSE, TEXT_KANA, 0, 40, 4000, NULL, EXPECT_ACCEPT_ANY, TRUE, 0 },
    { "ansi-kana-bytes-nofit", FALSE, TEXT_KANA, 0, 40, 0, NULL, EXPECT_BUFSMALL, TRUE, 0 },
    { "ansi-kana-long", FALSE, TEXT_KANA, 0, 130, 4000, NULL, EXPECT_ANY, TRUE, 0 },
    /* A short path typed in a folder of 255 characters */
    { "save-abs-short-in-long-folder", FALSE, TEXT_ABSSHORT, 255, 13, 40000, NULL, EXPECT_ACCEPT },
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
    {
        /* A case name after the test name runs that case alone */
        PCSTR pszOnly = strstr(GetCommandLineA(), "SaveDialogLongName ");

        if (pszOnly && strcmp(pszOnly + 19, ClassicCases[i].Name) != 0)
            continue;
        RunClassicCase(&ClassicCases[i], i, szBase);
    }

    if (strstr(GetCommandLineA(), "SaveDialogLongName ") == NULL && SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)))
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
