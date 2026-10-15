/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the item file dialogs (IFileOpenDialog and IFileSaveDialog), driven with keyboard input
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define COBJMACROS
#include <apitest.h>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <strsafe.h>

typedef enum
{
    STEP_WAIT_FOLDER,       /* the dialog is in the folder below the test folder (empty: the test folder) */
    STEP_WAIT_EMPTY_NAME,   /* the file name box is empty */
    STEP_TYPE,              /* type the text into the file name box */
    STEP_ENTER,
    STEP_ESCAPE,
    STEP_END
} STEP_KIND;

typedef struct
{
    STEP_KIND Kind;
    PCWSTR Text;
} STEP;

typedef struct
{
    HRESULT hr;
    WCHAR Path[MAX_PATH];
    WCHAR Folder[MAX_PATH];
    HWND hwndActive;        /* active window of the thread and foreground window once Show returned */
    HWND hwndForeground;
    BOOL WasForeground;     /* the owner was the foreground window when Show was called */
} DIALOG_RESULT;

static const STEP *s_Steps;
static HWND s_hwndOwner;
static IFileDialog *s_pfd;
static WCHAR s_BaseDir[MAX_PATH];
static WCHAR s_Failure[256];
static HRESULT (WINAPI *s_pfnSHCreateItemFromParsingName)(PCWSTR, IBindCtx *, REFIID, void **);
static CRITICAL_SECTION s_CurFolderLock;
static WCHAR s_CurFolder[MAX_PATH];

static BOOL CALLBACK FindDialogProc(HWND hwnd, LPARAM lParam)
{
    WCHAR szClass[16];

    if (GetWindow(hwnd, GW_OWNER) != s_hwndOwner || !IsWindowVisible(hwnd))
        return TRUE;
    if (GetClassNameW(hwnd, szClass, _countof(szClass)) && !wcscmp(szClass, L"#32770"))
    {
        *(HWND *)lParam = hwnd;
        return FALSE;
    }
    return TRUE;
}

typedef struct
{
    PCWSTR Class;
    HWND hwnd;
} FIND_CLASS;

static BOOL CALLBACK FindClassProc(HWND hwnd, LPARAM lParam)
{
    FIND_CLASS *pFind = (FIND_CLASS *)lParam;
    WCHAR szClass[64];

    if (IsWindowVisible(hwnd) && GetClassNameW(hwnd, szClass, _countof(szClass)) &&
        !_wcsicmp(szClass, pFind->Class))
    {
        pFind->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND FindChildOfClass(HWND hwndParent, PCWSTR pszClass)
{
    FIND_CLASS Find = { pszClass, NULL };
    EnumChildWindows(hwndParent, FindClassProc, (LPARAM)&Find);
    return Find.hwnd;
}

static HWND FindFileNameEdit(HWND hwndDlg)
{
    /* The edit control of the combo box, else the first visible edit control */
    HWND hwndCombo = FindChildOfClass(hwndDlg, L"ComboBoxEx32");
    HWND hwndEdit = hwndCombo ? FindChildOfClass(hwndCombo, L"Edit") : NULL;

    return hwndEdit ? hwndEdit : FindChildOfClass(hwndDlg, L"Edit");
}

static void GetItemPath(IShellItem *psi, PWSTR pszOut, SIZE_T cchOut)
{
    PWSTR pszName = NULL;

    pszOut[0] = UNICODE_NULL;
    if (SUCCEEDED(IShellItem_GetDisplayName(psi, SIGDN_FILESYSPATH, &pszName)))
    {
        StringCchCopyW(pszOut, cchOut, pszName);
        CoTaskMemFree(pszName);
    }
}

/* Runs on the thread of the dialog: the dialog object belongs to its apartment */
static VOID CALLBACK PollFolderProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
    WCHAR szPath[MAX_PATH];
    IShellItem *psi;

    if (!s_pfd || FAILED(IFileDialog_GetFolder(s_pfd, &psi)))
        return;
    GetItemPath(psi, szPath, _countof(szPath));
    IShellItem_Release(psi);
    EnterCriticalSection(&s_CurFolderLock);
    StringCchCopyW(s_CurFolder, _countof(s_CurFolder), szPath);
    LeaveCriticalSection(&s_CurFolderLock);
}

static BOOL IsCurrentFolder(PCWSTR pszSub)
{
    WCHAR szWant[MAX_PATH], szCur[MAX_PATH];

    if (*pszSub)
        StringCchPrintfW(szWant, _countof(szWant), L"%s\\%s", s_BaseDir, pszSub);
    else
        StringCchCopyW(szWant, _countof(szWant), s_BaseDir);
    EnterCriticalSection(&s_CurFolderLock);
    StringCchCopyW(szCur, _countof(szCur), s_CurFolder);
    LeaveCriticalSection(&s_CurFolderLock);
    return !_wcsicmp(szCur, szWant);
}

static void PostKey(HWND hwnd, UINT vk, WCHAR ch)
{
    UINT scan = MapVirtualKeyW(vk, 0);

    PostMessageW(hwnd, WM_KEYDOWN, vk, 1 | (scan << 16));
    if (ch)
        PostMessageW(hwnd, WM_CHAR, ch, 1 | (scan << 16));
    PostMessageW(hwnd, WM_KEYUP, vk, 1 | (scan << 16) | 0xC0000000);
}

static BOOL RunStep(HWND hwndDlg, const STEP *pStep)
{
    DWORD dwStart = GetTickCount();
    HWND hwndEdit;
    PCWSTR p;

    switch (pStep->Kind)
    {
        case STEP_WAIT_FOLDER:
            while (GetTickCount() - dwStart < 30000)
            {
                if (IsCurrentFolder(pStep->Text))
                    return TRUE;
                Sleep(50);
            }
            StringCchPrintfW(s_Failure, _countof(s_Failure), L"the dialog is not in the folder '%s' after 30 s", pStep->Text);
            return FALSE;

        case STEP_WAIT_EMPTY_NAME:
            while (GetTickCount() - dwStart < 5000)
            {
                hwndEdit = FindFileNameEdit(hwndDlg);
                if (hwndEdit && !GetWindowTextLengthW(hwndEdit))
                    return TRUE;
                Sleep(50);
            }
            StringCchCopyW(s_Failure, _countof(s_Failure), L"the file name box keeps its text");
            return FALSE;

        case STEP_TYPE:
            hwndEdit = FindFileNameEdit(hwndDlg);
            if (!hwndEdit)
            {
                StringCchCopyW(s_Failure, _countof(s_Failure), L"no file name box");
                return FALSE;
            }
            for (p = pStep->Text; *p; p++)
                SendMessageW(hwndEdit, WM_CHAR, *p, 1);
            return TRUE;

        case STEP_ENTER:
        case STEP_ESCAPE:
            hwndEdit = FindFileNameEdit(hwndDlg);
            PostKey(hwndEdit ? hwndEdit : hwndDlg, pStep->Kind == STEP_ENTER ? VK_RETURN : VK_ESCAPE,
                    pStep->Kind == STEP_ENTER ? L'\r' : 27);
            Sleep(200);
            return TRUE;

        default:
            break;
    }
    return FALSE;
}

static DWORD WINAPI DriverThread(LPVOID pParam)
{
    DWORD dwStart = GetTickCount();
    HWND hwndDlg = NULL;
    const STEP *pStep;

    /* Wait for the dialog and its file name box */
    while (GetTickCount() - dwStart < 60000)
    {
        EnumWindows(FindDialogProc, (LPARAM)&hwndDlg);
        if (hwndDlg && FindFileNameEdit(hwndDlg))
            break;
        hwndDlg = NULL;
        Sleep(100);
    }
    if (!hwndDlg)
    {
        StringCchCopyW(s_Failure, _countof(s_Failure), L"no dialog after 60 s");
        return 1;
    }

    for (pStep = s_Steps; pStep->Kind != STEP_END; pStep++)
    {
        if (!RunStep(hwndDlg, pStep))
        {
            PostMessageW(hwndDlg, WM_COMMAND, IDCANCEL, 0);
            return 1;
        }
        if (pStep->Kind == STEP_ENTER && !IsWindow(hwndDlg))
            break;
    }
    return 0;
}

#define STEPS_END { STEP_END, NULL }

static void RunItemDialog(BOOL bSave, PCWSTR pszDefExt, PCWSTR pszFilter, const STEP *pSteps, DIALOG_RESULT *pResult)
{
    IShellItem *psiFolder = NULL, *psiResult = NULL;
    HANDLE hThread;
    UINT_PTR idTimer;
    HRESULT hr;

    ZeroMemory(pResult, sizeof(*pResult));
    s_Steps = pSteps;
    s_Failure[0] = UNICODE_NULL;
    s_CurFolder[0] = UNICODE_NULL;

    hr = CoCreateInstance(bSave ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileDialog, (void **)&s_pfd);
    ok(hr == S_OK, "CoCreateInstance failed: 0x%08lx\n", hr);
    if (FAILED(hr))
        return;

    hr = s_pfnSHCreateItemFromParsingName(s_BaseDir, NULL, &IID_IShellItem, (void **)&psiFolder);
    ok(hr == S_OK, "SHCreateItemFromParsingName(%S) failed: 0x%08lx\n", s_BaseDir, hr);
    if (SUCCEEDED(hr))
    {
        hr = IFileDialog_SetFolder(s_pfd, psiFolder);
        ok(hr == S_OK, "SetFolder failed: 0x%08lx\n", hr);
        IShellItem_Release(psiFolder);
    }
    if (pszDefExt)
        IFileDialog_SetDefaultExtension(s_pfd, pszDefExt);
    if (pszFilter)
    {
        COMDLG_FILTERSPEC Spec = { L"Filter", pszFilter };
        IFileDialog_SetFileTypes(s_pfd, 1, &Spec);
        IFileDialog_SetFileTypeIndex(s_pfd, 1);
    }

    hThread = CreateThread(NULL, 0, DriverThread, NULL, 0, NULL);
    ok(hThread != NULL, "CreateThread failed: %lu\n", GetLastError());
    idTimer = SetTimer(NULL, 0, 50, PollFolderProc);

    SetForegroundWindow(s_hwndOwner);
    pResult->WasForeground = (GetForegroundWindow() == s_hwndOwner);
    pResult->hr = IFileDialog_Show(s_pfd, s_hwndOwner);
    pResult->hwndActive = GetActiveWindow();
    pResult->hwndForeground = GetForegroundWindow();

    KillTimer(NULL, idTimer);
    if (hThread)
    {
        ok(WaitForSingleObject(hThread, 30000) == WAIT_OBJECT_0, "The driver did not finish\n");
        CloseHandle(hThread);
    }
    ok(!s_Failure[0], "The dialog could not be driven: %S\n", s_Failure);

    if (SUCCEEDED(pResult->hr) && SUCCEEDED(IFileDialog_GetResult(s_pfd, &psiResult)))
    {
        GetItemPath(psiResult, pResult->Path, _countof(pResult->Path));
        IShellItem_Release(psiResult);
    }
    if (SUCCEEDED(IFileDialog_GetFolder(s_pfd, &psiFolder)))
    {
        GetItemPath(psiFolder, pResult->Folder, _countof(pResult->Folder));
        IShellItem_Release(psiFolder);
    }
    IFileDialog_Release(s_pfd);
    s_pfd = NULL;
}

static void MakeFile(PCWSTR pszPath, PCSTR pszContent)
{
    DWORD cb;
    HANDLE hFile = CreateFileW(pszPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);

    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW(%S) failed: %lu\n", pszPath, GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        WriteFile(hFile, pszContent, (DWORD)strlen(pszContent), &cb, NULL);
        CloseHandle(hFile);
    }
}

static BOOL FileHasContent(PCWSTR pszPath, PCSTR pszContent)
{
    CHAR szBuf[64] = "";
    DWORD cb = 0;
    HANDLE hFile = CreateFileW(pszPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    ReadFile(hFile, szBuf, sizeof(szBuf) - 1, &cb, NULL);
    CloseHandle(hFile);
    return !strcmp(szBuf, pszContent);
}

/* The dialog enables its owner before it goes away, so the owner is active again; a focus of the
 * taskbar or of no window would leave the program ignoring its keyboard shortcuts until a click */
static void CheckActivation(const DIALOG_RESULT *pResult, PCSTR pszWhat)
{
    ok(pResult->hwndActive == s_hwndOwner, "%s: active window %p, expected the owner %p\n", pszWhat,
       pResult->hwndActive, s_hwndOwner);
    if (pResult->WasForeground)
        ok(pResult->hwndForeground == s_hwndOwner, "%s: foreground window %p, expected the owner %p\n", pszWhat,
           pResult->hwndForeground, s_hwndOwner);
    ok(IsWindowEnabled(s_hwndOwner), "%s: the owner is disabled\n", pszWhat);
}

static void TestNames(void)
{
    /* The names Windows 7 and 10 return (measured). The dialog changes the name only when a default
     * extension is set: it adds the extension of the file type (the first one of its filter, else the
     * default extension) unless the name has an extension that the filter lists ("*.*" lists all) or that is registered. */
    static const struct { PCWSTR Filter, DefExt, Typed, Expected; } Cases[] =
    {
        { L"*.htm;*.html", NULL, L"page.html", L"page.html" },
        { L"*.htm;*.html", NULL, L"page", L"page" },
        { L"*.htm;*.html", L"htm", L"page.html", L"page.html" },
        { L"*.htm;*.html", L"htm", L"page", L"page.htm" },
        { L"*.txt", NULL, L"photo.png", L"photo.png" },
        { L"*.txt", NULL, L"noext", L"noext" },
        { L"*.txt", L"txt", L"x", L"x.txt" },
        { NULL, L"txt", L"x.TXT", L"x.TXT" },
        { NULL, L"txt", L"x.", L"x..txt" },
        { NULL, L"wte", L"x.itemdlgreg", L"x.itemdlgreg" },
        { NULL, L"wte", L"x.itemdlgnone", L"x.itemdlgnone.wte" },
        { L"*.wt1", NULL, L"x.itemdlgnone", L"x.itemdlgnone" },
        { L"*.wt1", L"wte", L"x", L"x.wt1" },
        { L"*.wt1", L"wte", L"x.wte", L"x.wte.wt1" },
        { L"*.wt1", L"wte", L"x.itemdlgreg", L"x.itemdlgreg" },
        { L"*.wt1;*.wt2", L"wte", L"x", L"x.wt1" },
        { L"*.wt1;*.wt2", L"wte", L"x.wt2", L"x.wt2" },
        { L"*.*", L"wte", L"x", L"x.wte" },
        { L"*.*", L"wte", L"x.itemdlgnone", L"x.itemdlgnone" },
    };
    STEP Steps[] = { { STEP_WAIT_FOLDER, L"" }, { STEP_TYPE, NULL }, { STEP_ENTER, NULL }, STEPS_END };
    DIALOG_RESULT Result;
    WCHAR szWant[MAX_PATH];
    HKEY hKey;
    UINT i;

    /* A registered extension, in the user's classes so that no system key is written */
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\.itemdlgreg", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL))
    {
        skip("Cannot register an extension\n");
        return;
    }
    RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE *)L"itemdlgregfile", sizeof(L"itemdlgregfile"));
    RegCloseKey(hKey);

    for (i = 0; i < _countof(Cases); i++)
    {
        Steps[1].Text = Cases[i].Typed;
        RunItemDialog(TRUE, Cases[i].DefExt, Cases[i].Filter, Steps, &Result);
        StringCchPrintfW(szWant, _countof(szWant), L"%s\\%s", s_BaseDir, Cases[i].Expected);
        ok(Result.hr == S_OK, "Save '%S' (filter '%S', extension '%S'): Show returned 0x%08lx\n", Cases[i].Typed,
           Cases[i].Filter ? Cases[i].Filter : L"", Cases[i].DefExt ? Cases[i].DefExt : L"", Result.hr);
        ok(!_wcsicmp(Result.Path, szWant), "Save '%S' (filter '%S', extension '%S'): result '%S', expected '%S'\n",
           Cases[i].Typed, Cases[i].Filter ? Cases[i].Filter : L"", Cases[i].DefExt ? Cases[i].DefExt : L"",
           Result.Path, szWant);
    }

    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Classes\\.itemdlgreg");
}

START_TEST(ItemDialog)
{
    WCHAR szTemp[MAX_PATH], szSub[MAX_PATH], szPath[MAX_PATH], szWant[MAX_PATH];
    DIALOG_RESULT Result;
    IExplorerBrowser *peb = NULL;
    IFileDialog *pfd = NULL;
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok(SUCCEEDED(hr), "CoInitializeEx failed: 0x%08lx\n", hr);

    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog, (void **)&pfd);
    if (hr == REGDB_E_CLASSNOTREG)
    {
        skip("The item dialogs are not registered\n");
        CoUninitialize();
        return;
    }
    ok(hr == S_OK, "CoCreateInstance(FileOpenDialog) failed: 0x%08lx\n", hr);
    if (pfd)
        IFileDialog_Release(pfd);

    hr = CoCreateInstance(&CLSID_ExplorerBrowser, NULL, CLSCTX_INPROC_SERVER, &IID_IExplorerBrowser, (void **)&peb);
    ok(hr == S_OK, "CoCreateInstance(ExplorerBrowser) failed: 0x%08lx\n", hr);
    if (FAILED(hr))
    {
        skip("No ExplorerBrowser: the item dialogs cannot finish\n");
        CoUninitialize();
        return;
    }
    IExplorerBrowser_Release(peb);

    s_pfnSHCreateItemFromParsingName = (void *)GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHCreateItemFromParsingName");
    if (!s_pfnSHCreateItemFromParsingName)
    {
        skip("SHCreateItemFromParsingName is missing\n");
        CoUninitialize();
        return;
    }

    InitializeCriticalSection(&s_CurFolderLock);

    /* The dialog returns long names */
    GetTempPathW(_countof(szTemp), szTemp);
    GetLongPathNameW(szTemp, szTemp, _countof(szTemp));
    StringCchPrintfW(s_BaseDir, _countof(s_BaseDir), L"%sitemdlg-%lu", szTemp, GetTickCount());
    StringCchPrintfW(szSub, _countof(szSub), L"%s\\sub", s_BaseDir);
    ok(CreateDirectoryW(s_BaseDir, NULL), "CreateDirectoryW(%S) failed: %lu\n", s_BaseDir, GetLastError());
    ok(CreateDirectoryW(szSub, NULL), "CreateDirectoryW(%S) failed: %lu\n", szSub, GetLastError());
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\alpha.txt", s_BaseDir);
    MakeFile(szPath, "alpha");
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\sub\\gamma.txt", s_BaseDir);
    MakeFile(szPath, "gamma");

    s_hwndOwner = CreateWindowExW(0, L"STATIC", L"owner", WS_POPUP | WS_VISIBLE, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    ok(s_hwndOwner != NULL, "CreateWindowExW failed: %lu\n", GetLastError());

    /* Open: a typed folder name browses into the folder and empties the name box, a typed file name picks the file */
    {
        static const STEP Steps[] =
        {
            { STEP_WAIT_FOLDER, L"" },
            { STEP_TYPE, L"sub" },
            { STEP_ENTER, NULL },
            { STEP_WAIT_FOLDER, L"sub" },
            { STEP_WAIT_EMPTY_NAME, NULL },
            { STEP_TYPE, L"gamma.txt" },
            { STEP_ENTER, NULL },
            STEPS_END
        };
        RunItemDialog(FALSE, NULL, NULL, Steps, &Result);
        StringCchPrintfW(szWant, _countof(szWant), L"%s\\sub\\gamma.txt", s_BaseDir);
        ok(Result.hr == S_OK, "Open: Show returned 0x%08lx\n", Result.hr);
        ok(!_wcsicmp(Result.Path, szWant), "Open: result '%S', expected '%S'\n", Result.Path, szWant);
        ok(FileHasContent(Result.Path, "gamma"), "Open: the picked file has the wrong content\n");
    }

    /* Open: Escape cancels */
    {
        static const STEP Steps[] = { { STEP_WAIT_FOLDER, L"" }, { STEP_ESCAPE, NULL }, STEPS_END };
        RunItemDialog(FALSE, NULL, NULL, Steps, &Result);
        ok(Result.hr == HRESULT_FROM_WIN32(ERROR_CANCELLED), "Open cancelled: Show returned 0x%08lx\n", Result.hr);
        CheckActivation(&Result, "Open cancelled");
    }

    /* Save: the typed name is returned and the program writes the file there */
    {
        static const STEP Steps[] =
        {
            { STEP_WAIT_FOLDER, L"" },
            { STEP_TYPE, L"typed.txt" },
            { STEP_ENTER, NULL },
            STEPS_END
        };
        RunItemDialog(TRUE, NULL, NULL, Steps, &Result);
        StringCchPrintfW(szWant, _countof(szWant), L"%s\\typed.txt", s_BaseDir);
        ok(Result.hr == S_OK, "Save: Show returned 0x%08lx\n", Result.hr);
        ok(!_wcsicmp(Result.Path, szWant), "Save: result '%S', expected '%S'\n", Result.Path, szWant);
        ok(!_wcsicmp(Result.Folder, s_BaseDir), "Save: folder '%S', expected '%S'\n", Result.Folder, s_BaseDir);
        CheckActivation(&Result, "Save");
        if (Result.Path[0])
        {
            MakeFile(Result.Path, "saved");
            ok(FileHasContent(szWant, "saved"), "Save: the file was not written\n");
        }
    }

    /* Save: a folder typed into the name box browses into it, then the name saves there */
    {
        static const STEP Steps[] =
        {
            { STEP_WAIT_FOLDER, L"" },
            { STEP_TYPE, L"sub" },
            { STEP_ENTER, NULL },
            { STEP_WAIT_FOLDER, L"sub" },
            { STEP_WAIT_EMPTY_NAME, NULL },
            { STEP_TYPE, L"inside.txt" },
            { STEP_ENTER, NULL },
            STEPS_END
        };
        RunItemDialog(TRUE, NULL, NULL, Steps, &Result);
        StringCchPrintfW(szWant, _countof(szWant), L"%s\\sub\\inside.txt", s_BaseDir);
        ok(Result.hr == S_OK, "Save in a subfolder: Show returned 0x%08lx\n", Result.hr);
        ok(!_wcsicmp(Result.Path, szWant), "Save in a subfolder: result '%S', expected '%S'\n", Result.Path, szWant);
    }

    TestNames();

    DestroyWindow(s_hwndOwner);
    DeleteCriticalSection(&s_CurFolderLock);

    StringCchPrintfW(szPath, _countof(szPath), L"%s\\sub\\gamma.txt", s_BaseDir);
    DeleteFileW(szPath);
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\alpha.txt", s_BaseDir);
    DeleteFileW(szPath);
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\typed.txt", s_BaseDir);
    DeleteFileW(szPath);
    RemoveDirectoryW(szSub);
    RemoveDirectoryW(s_BaseDir);
    CoUninitialize();
}
