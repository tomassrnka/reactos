/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Calls every NT 6 shell32, shlwapi and shcore export that used to be a stub, each in a child process
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "shelltest.h"
#include <shobjidl.h>
#include <shellutils.h>
#include <strsafe.h>
#include <propsys.h>

/*
 * A raising stub kills its caller with EXCEPTION_WINE_STUB; a stub that returned 0 left the outputs unset.
 * Each export is called with valid arguments in a child process, which must exit normally. When the call
 * fails, its outputs must be cleared. The run-DLL entries may wait for something on Windows, so a child that
 * is still running after the timeout counts as alive.
 */

#define CHILD_OK 0
#define CHILD_BAD_OUTPUT 1
#define CHILD_NO_EXPORT 2
#define CHILD_SETUP_FAILED 3
#define CHILD_TIMEOUT_MS 15000

#define POISON ((void *)(ULONG_PTR)1)

static int s_iResult;

#define CHECK_OUT(cond) do { if (!(cond)) { s_iResult = CHILD_BAD_OUTPUT; trace("%s: %s\n", __FUNCTION__, #cond); } } while (0)

/* An object out-parameter: set on success, NULL on failure */
static void CheckObjectImpl(HRESULT hr, IUnknown *punk, PCSTR pszCaller)
{
    if (SUCCEEDED(hr))
    {
        if (!punk || punk == POISON)
        {
            s_iResult = CHILD_BAD_OUTPUT;
            trace("%s: hr 0x%lx without an object\n", pszCaller, hr);
        }
        else
        {
            punk->Release();
        }
    }
    else if (punk)
    {
        s_iResult = CHILD_BAD_OUTPUT;
        trace("%s: hr 0x%lx, object %p\n", pszCaller, hr, punk);
    }
}
#define CheckObject(hr, punk) CheckObjectImpl((hr), (IUnknown *)(punk), __FUNCTION__)

static HMODULE s_hShell32, s_hShlwapi, s_hShcore;
static WCHAR s_szTempDir[MAX_PATH], s_szTempFile[MAX_PATH];

static FARPROC GetExport(HMODULE hMod, PCSTR pszName)
{
    return hMod ? GetProcAddress(hMod, pszName) : NULL;
}

static void CreateTempFile(void)
{
    HANDLE hFile = CreateFileW(s_szTempFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
        CloseHandle(hFile);
}

static IShellItem *CreateTempFileItem(void)
{
    IShellItem *psi = NULL;
    CreateTempFile();
    typedef HRESULT (WINAPI *FN)(PCWSTR, IBindCtx *, REFIID, void **);
    FN fn = (FN)GetExport(s_hShell32, "SHCreateItemFromParsingName");
    if (!fn || FAILED(fn(s_szTempFile, NULL, IID_PPV_ARG(IShellItem, &psi))))
        return NULL;
    return psi;
}

static PIDLIST_ABSOLUTE ParsePath(PCWSTR pszPath)
{
    PIDLIST_ABSOLUTE pidl = NULL;
    SHParseDisplayName(pszPath, NULL, &pidl, 0, NULL);
    return pidl;
}

/* Run-DLL style entries: void (HWND, HINSTANCE, command line, show) */
typedef VOID (WINAPI *FN_RunDllW)(HWND, HINSTANCE, LPWSTR, INT);
typedef VOID (WINAPI *FN_RunDllA)(HWND, HINSTANCE, LPSTR, INT);

static void Call_RunDllW(PCSTR pszName)
{
    FN_RunDllW fn = (FN_RunDllW)GetExport(s_hShell32, pszName);
    WCHAR szEmpty[1] = L"";
    fn(NULL, NULL, szEmpty, SW_HIDE);
}

static void Call_PrepareDiscForBurnRunDllW(void) { Call_RunDllW("PrepareDiscForBurnRunDllW"); }
static void Call_WaitForExplorerRestartW(void) { Call_RunDllW("WaitForExplorerRestartW"); }

static void Call_SHCreateLocalServerRunDll(void)
{
    FN_RunDllA fn = (FN_RunDllA)GetExport(s_hShell32, "SHCreateLocalServerRunDll");
    CHAR szEmpty[1] = "";
    fn(NULL, NULL, szEmpty, SW_HIDE);
}

typedef struct
{
    INT ac;
    HKEY hkClass;
    PCWSTR pszClass;
} ASSOCIATIONELEMENT_;
#define ASSOCCLASS_PROGID_STR_ 2

static void Call_AssocCreateForClasses(void)
{
    typedef HRESULT (WINAPI *FN)(const ASSOCIATIONELEMENT_ *, ULONG, REFIID, void **);
    FN fn = (FN)GetExport(s_hShell32, "AssocCreateForClasses");
    ASSOCIATIONELEMENT_ elem = { ASSOCCLASS_PROGID_STR_, NULL, L"txtfile" };
    IUnknown *punk = (IUnknown *)POISON;
    HRESULT hr = fn(&elem, 1, IID_IQueryAssociations, (void **)&punk);
    CheckObject(hr, punk);
}

/* PKEY_ItemNameDisplay */
static const PROPERTYKEY s_KeyName =
    { { 0xB725F130, 0x47EF, 0x101A, { 0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC } }, 10 };

static void Call_AssocGetDetailsOfPropKey(void)
{
    typedef HRESULT (WINAPI *FN)(IShellFolder *, PCUITEMID_CHILD, const PROPERTYKEY *, VARIANT *, BOOL *);
    FN fn = (FN)GetExport(s_hShell32, "AssocGetDetailsOfPropKey");
    CComPtr<IShellFolder> psfParent;
    PIDLIST_ABSOLUTE pidl;
    PCUITEMID_CHILD pidlLast;
    VARIANT var;
    BOOL bFound = 0x55;
    HRESULT hr;

    CreateTempFile();
    pidl = ParsePath(s_szTempFile);
    if (!pidl || FAILED(SHBindToParent(pidl, IID_PPV_ARG(IShellFolder, &psfParent), &pidlLast)))
    {
        s_iResult = CHILD_SETUP_FAILED;
        ILFree(pidl);
        return;
    }
    V_VT(&var) = VT_I4;
    V_I4(&var) = 0x5555;
    hr = fn(psfParent, pidlLast, &s_KeyName, &var, &bFound);
    /* Windows leaves the variant as it was when it fails */
    if (SUCCEEDED(hr))
    {
        CHECK_OUT(bFound == TRUE || bFound == FALSE);
        VariantClear(&var);
    }
    ILFree(pidl);
}

static void Call_InitNetworkAddressControl(void)
{
    typedef BOOL (WINAPI *FN)(void);
    FN fn = (FN)GetExport(s_hShell32, "InitNetworkAddressControl");
    fn();
}

static void Call_SHAddDefaultPropertiesByExt(void)
{
    typedef HRESULT (WINAPI *FN)(PCWSTR, IPropertyStore *);
    FN fn = (FN)GetExport(s_hShell32, "SHAddDefaultPropertiesByExt");
    typedef HRESULT (WINAPI *FNCREATE)(REFIID, void **);
    FNCREATE fnCreate = (FNCREATE)GetProcAddress(LoadLibraryW(L"propsys.dll"), "PSCreateMemoryPropertyStore");
    CComPtr<IPropertyStore> pps;
    if (!fnCreate || FAILED(fnCreate(IID_PPV_ARG(IPropertyStore, &pps))))
    {
        s_iResult = CHILD_SETUP_FAILED;
        return;
    }
    fn(L".txt", pps);
}

static void Call_SHAssocEnumHandlers(void)
{
    typedef HRESULT (WINAPI *FN)(PCWSTR, ASSOC_FILTER, IEnumAssocHandlers **);
    FN fn = (FN)GetExport(s_hShell32, "SHAssocEnumHandlers");
    IEnumAssocHandlers *penum = (IEnumAssocHandlers *)POISON;
    HRESULT hr = fn(L".txt", ASSOC_FILTER_NONE, &penum);
    CheckObject(hr, penum);
}

static void Call_SHAssocEnumHandlersForProtocolByApplication(void)
{
    typedef HRESULT (WINAPI *FN)(PCWSTR, REFIID, void **);
    FN fn = (FN)GetExport(s_hShell32, "SHAssocEnumHandlersForProtocolByApplication");
    IUnknown *punk = (IUnknown *)POISON;
    HRESULT hr = fn(L"http", IID_IEnumAssocHandlers, (void **)&punk);
    CheckObject(hr, punk);
}

static void Call_SHChangeNotifyRegisterThread(void)
{
    typedef VOID (WINAPI *FN)(INT);
    FN fn = (FN)GetExport(s_hShell32, "SHChangeNotifyRegisterThread");
    fn(1); /* SCNRT_ENABLE */
    fn(2); /* SCNRT_DISABLE */
}

static void Call_SHCreateAssociationRegistration(void)
{
    typedef HRESULT (WINAPI *FN)(REFIID, void **);
    FN fn = (FN)GetExport(s_hShell32, "SHCreateAssociationRegistration");
    IUnknown *punk = (IUnknown *)POISON;
    HRESULT hr = fn(IID_IApplicationAssociationRegistration, (void **)&punk);
    CheckObject(hr, punk);
}

static void Call_SHCreateDefaultPropertiesOp(void)
{
    typedef HRESULT (WINAPI *FN)(IShellItem *, IFileOperation **);
    FN fn = (FN)GetExport(s_hShell32, "SHCreateDefaultPropertiesOp");
    CComPtr<IShellItem> psi;
    IFileOperation *pfo = (IFileOperation *)POISON;
    HRESULT hr;

    psi.Attach(CreateTempFileItem());
    if (!psi)
    {
        s_iResult = CHILD_SETUP_FAILED;
        return;
    }
    hr = fn(psi, &pfo);
    CheckObject(hr, pfo);
}

static void Call_SHEnableServiceObject(void)
{
    typedef HRESULT (WINAPI *FN)(REFCLSID, BOOL);
    FN fn = (FN)GetExport(s_hShell32, "SHEnableServiceObject");
    fn(CLSID_NULL, FALSE);
}

static void Call_SHGetDriveMedia(void)
{
    typedef HRESULT (WINAPI *FN)(PCWSTR, DWORD *);
    FN fn = (FN)GetExport(s_hShell32, "SHGetDriveMedia");
    WCHAR szRoot[MAX_PATH];
    DWORD dwContent = 0x55555555;
    HRESULT hr;

    GetWindowsDirectoryW(szRoot, _countof(szRoot));
    PathStripToRootW(szRoot);
    hr = fn(szRoot, &dwContent);
    CHECK_OUT(SUCCEEDED(hr) ? dwContent != 0x55555555 : dwContent == 0);
}

static void Call_SHGetLocalizedName(void)
{
    typedef HRESULT (WINAPI *FN)(PCWSTR, PWSTR, UINT, int *);
    FN fn = (FN)GetExport(s_hShell32, "SHGetLocalizedName");
    WCHAR szModule[MAX_PATH];
    INT idRes = 0x5555;
    HRESULT hr;

    szModule[0] = L'X';
    hr = fn(s_szTempDir, szModule, _countof(szModule), &idRes);
    /* The folder has no localized name */
    CHECK_OUT(FAILED(hr));
    CHECK_OUT(szModule[0] == UNICODE_NULL);
}

static void Call_SHGetPropertyStoreFromIDList(void)
{
    typedef HRESULT (WINAPI *FN)(PCIDLIST_ABSOLUTE, GETPROPERTYSTOREFLAGS, REFIID, void **);
    FN fn = (FN)GetExport(s_hShell32, "SHGetPropertyStoreFromIDList");
    PIDLIST_ABSOLUTE pidl;
    IPropertyStore *pps = (IPropertyStore *)POISON;
    HRESULT hr;

    CreateTempFile();
    pidl = ParsePath(s_szTempFile);
    if (!pidl)
    {
        s_iResult = CHILD_SETUP_FAILED;
        return;
    }
    hr = fn(pidl, GPS_DEFAULT, IID_PPV_ARG(IPropertyStore, &pps));
    CheckObject(hr, pps);
    ILFree(pidl);
}

static void Call_SHGetTemporaryPropertyForItem(void)
{
    typedef HRESULT (WINAPI *FN)(IShellItem *, const PROPERTYKEY &, PROPVARIANT *);
    FN fn = (FN)GetExport(s_hShell32, "SHGetTemporaryPropertyForItem");
    CComPtr<IShellItem> psi;
    PROPVARIANT pv;

    psi.Attach(CreateTempFileItem());
    if (!psi)
    {
        s_iResult = CHILD_SETUP_FAILED;
        return;
    }
    pv.vt = VT_I4;
    pv.lVal = 0x5555;
    fn(psi, s_KeyName, &pv);
    /* No temporary property was set */
    CHECK_OUT(pv.vt == VT_EMPTY);
    PropVariantClear(&pv);
}

static void Call_SHSetTemporaryPropertyForItem(void)
{
    typedef HRESULT (WINAPI *FN)(IShellItem *, const PROPERTYKEY &, const PROPVARIANT &);
    FN fn = (FN)GetExport(s_hShell32, "SHSetTemporaryPropertyForItem");
    CComPtr<IShellItem> psi;
    PROPVARIANT pv;

    psi.Attach(CreateTempFileItem());
    if (!psi)
    {
        s_iResult = CHILD_SETUP_FAILED;
        return;
    }
    PropVariantInit(&pv);
    pv.vt = VT_UI4;
    pv.ulVal = 42;
    fn(psi, s_KeyName, pv);
}

static void Call_SHRemoveLocalizedName(void)
{
    typedef HRESULT (WINAPI *FN)(PCWSTR);
    FN fn = (FN)GetExport(s_hShell32, "SHRemoveLocalizedName");
    fn(s_szTempDir);
}

static void Call_SHSetDefaultProperties(void)
{
    typedef HRESULT (WINAPI *FN)(HWND, IShellItem *, DWORD, IFileOperationProgressSink *);
    FN fn = (FN)GetExport(s_hShell32, "SHSetDefaultProperties");
    CComPtr<IShellItem> psi;

    psi.Attach(CreateTempFileItem());
    if (!psi)
    {
        s_iResult = CHILD_SETUP_FAILED;
        return;
    }
    fn(NULL, psi, FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR, NULL);
}

static void Call_StgMakeUniqueName(void)
{
    typedef HRESULT (WINAPI *FN)(IStorage *, PCWSTR, DWORD, REFIID, void **);
    FN fn = (FN)GetExport(s_hShell32, "StgMakeUniqueName");
    CComPtr<IStorage> pstg;
    IUnknown *punk = (IUnknown *)POISON;
    HRESULT hr;

    if (FAILED(StgCreateDocfile(NULL, STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE | STGM_DELETEONRELEASE,
                                0, &pstg)))
    {
        s_iResult = CHILD_SETUP_FAILED;
        return;
    }
    hr = fn(pstg, L"Stub test.txt", STGM_READWRITE | STGM_SHARE_EXCLUSIVE | STGM_CREATE, IID_IStream, (void **)&punk);
    CheckObject(hr, punk);
}

typedef struct
{
    DWORD cbSize;
    HWND hWnd;
    UINT uID;
    GUID guidItem;
} NOTIFYICONIDENTIFIER_;

static void Call_Shell_NotifyIconGetRect(void)
{
    typedef HRESULT (WINAPI *FN)(const NOTIFYICONIDENTIFIER_ *, RECT *);
    FN fn = (FN)GetExport(s_hShell32, "Shell_NotifyIconGetRect");
    NOTIFYICONIDENTIFIER_ nii = { sizeof(nii), GetDesktopWindow(), 0x5A5A };
    RECT rc = { 1, 2, 3, 4 };
    HRESULT hr = fn(&nii, &rc);
    CHECK_OUT(FAILED(hr)); /* no such icon */
    CHECK_OUT(IsRectEmpty(&rc));
}

static void Call_SHCreateItemWithParent(void)
{
    typedef HRESULT (WINAPI *FN)(PCIDLIST_ABSOLUTE, IShellFolder *, PCUITEMID_CHILD, REFIID, void **);
    FN fn = (FN)GetExport(s_hShell32, "SHCreateItemWithParent");
    CComPtr<IShellFolder> psfParent;
    PIDLIST_ABSOLUTE pidl;
    PCUITEMID_CHILD pidlLast;
    IShellItem *psi = (IShellItem *)POISON;
    HRESULT hr;

    CreateTempFile();
    pidl = ParsePath(s_szTempFile);
    if (!pidl || FAILED(SHBindToParent(pidl, IID_PPV_ARG(IShellFolder, &psfParent), &pidlLast)))
    {
        s_iResult = CHILD_SETUP_FAILED;
        ILFree(pidl);
        return;
    }
    hr = fn(NULL, psfParent, pidlLast, IID_PPV_ARG(IShellItem, &psi));
    /* It used to return S_OK without an item */
    CHECK_OUT(hr == S_OK && psi && psi != POISON);
    if (SUCCEEDED(hr) && psi && psi != POISON)
        psi->Release();
    ILFree(pidl);
}

static void Call_SHGetStockIconInfo(void)
{
    typedef struct { DWORD cbSize; HICON hIcon; INT iSysImageIndex; INT iIcon; WCHAR szPath[MAX_PATH]; } SII;
    typedef HRESULT (WINAPI *FN)(INT, UINT, SII *);
    FN fn = (FN)GetExport(s_hShell32, "SHGetStockIconInfo");
    SII sii = { sizeof(sii) };
    HRESULT hr = fn(77 /* SIID_SHIELD */, SHGSI_ICON | SHGSI_SMALLICON, &sii);
    CHECK_OUT(hr == S_OK && sii.hIcon);
    if (sii.hIcon)
        DestroyIcon(sii.hIcon);
}

static void Call_SHGetPathFromIDListEx(void)
{
    typedef BOOL (WINAPI *FN)(PCIDLIST_ABSOLUTE, PWSTR, DWORD, GPFIDL_FLAGS);
    FN fn = (FN)GetExport(s_hShell32, "SHGetPathFromIDListEx");
    PIDLIST_ABSOLUTE pidl = ParsePath(s_szTempDir);
    WCHAR szPath[MAX_PATH];
    if (!pidl)
    {
        s_iResult = CHILD_SETUP_FAILED;
        return;
    }
    CHECK_OUT(fn(pidl, szPath, _countof(szPath), GPFIDL_DEFAULT) && szPath[0]);
    ILFree(pidl);
}

static void Call_BindToFolderIDListParent(PCSTR pszName)
{
    typedef HRESULT (WINAPI *FN)(IShellFolder *, PCUIDLIST_RELATIVE, REFIID, void **, PCUITEMID_CHILD *);
    typedef HRESULT (WINAPI *FNEX)(IShellFolder *, PCUIDLIST_RELATIVE, IBindCtx *, REFIID, void **, PCUITEMID_CHILD *);
    PIDLIST_ABSOLUTE pidl = ParsePath(s_szTempDir);
    IShellFolder *psf = NULL;
    PCUITEMID_CHILD pidlLast = NULL;
    HRESULT hr;

    if (!pidl)
    {
        s_iResult = CHILD_SETUP_FAILED;
        return;
    }
    if (!strcmp(pszName, "SHBindToFolderIDListParent"))
        hr = ((FN)GetExport(s_hShell32, pszName))(NULL, pidl, IID_PPV_ARG(IShellFolder, &psf), &pidlLast);
    else
        hr = ((FNEX)GetExport(s_hShell32, pszName))(NULL, pidl, NULL, IID_PPV_ARG(IShellFolder, &psf), &pidlLast);
    CHECK_OUT(hr == S_OK && psf && pidlLast == ILFindLastID(pidl));
    if (psf)
        psf->Release();
    ILFree(pidl);
}

static void Call_SHBindToFolderIDListParent(void) { Call_BindToFolderIDListParent("SHBindToFolderIDListParent"); }
static void Call_SHBindToFolderIDListParentEx(void) { Call_BindToFolderIDListParent("SHBindToFolderIDListParentEx"); }

static void Call_ILLoadFromStreamEx(void)
{
    typedef HRESULT (WINAPI *FN)(IStream *, PIDLIST_RELATIVE *);
    FN fn = (FN)GetExport(s_hShell32, "ILLoadFromStreamEx");
    CComPtr<IStream> pstm;
    PIDLIST_RELATIVE pidl = (PIDLIST_RELATIVE)POISON;
    HRESULT hr;

    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &pstm)))
    {
        s_iResult = CHILD_SETUP_FAILED;
        return;
    }
    hr = fn(pstm, &pidl);
    CHECK_OUT(FAILED(hr) && pidl == NULL); /* empty stream */
}

static void Call_StrChrNIW(void)
{
    typedef PWSTR (WINAPI *FN)(PCWSTR, WCHAR, UINT);
    FN fn = (FN)GetExport(s_hShlwapi, "StrChrNIW");
    static const WCHAR s_szText[] = L"abcABC";
    CHECK_OUT(fn(s_szText, L'B', 6) == s_szText + 1);
}

/* shcore (Windows 8.1 and later) */

static void Call_CreateRandomAccessStreamOnFile(void)
{
    typedef HRESULT (WINAPI *FN)(PCWSTR, DWORD, REFIID, void **);
    FN fn = (FN)GetExport(s_hShcore, "CreateRandomAccessStreamOnFile");
    IUnknown *punk = (IUnknown *)POISON;
    HRESULT hr;

    CreateTempFile();
    hr = fn(s_szTempFile, 0 /* FileAccessMode_Read */, IID_IUnknown, (void **)&punk);
    CheckObject(hr, punk);
}

static void Call_CreateStreamOverRandomAccessStream(void)
{
    typedef HRESULT (WINAPI *FNOVER)(IStream *, INT, REFIID, void **);
    typedef HRESULT (WINAPI *FN)(IUnknown *, REFIID, void **);
    FNOVER fnOver = (FNOVER)GetExport(s_hShcore, "CreateRandomAccessStreamOverStream");
    FN fn = (FN)GetExport(s_hShcore, "CreateStreamOverRandomAccessStream");
    /* IID_IRandomAccessStream */
    static const IID s_IIDRandomAccessStream =
        { 0x905a0fe1, 0xbc53, 0x11df, { 0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda } };
    CComPtr<IStream> pstm;
    CComPtr<IUnknown> pras;
    IUnknown *punk = (IUnknown *)POISON;
    HRESULT hr;

    /* Without a random-access stream to wrap (ReactOS has none), the call still has to fail cleanly */
    if (SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &pstm)) && fnOver)
        fnOver(pstm, 0 /* BSOS_DEFAULT */, s_IIDRandomAccessStream, (void **)&pras);
    if (!pras)
        trace("no random-access stream, calling with NULL\n");
    hr = fn(pras, IID_IStream, (void **)&punk);
    CheckObject(hr, punk);
}

static void Call_DllGetActivationFactory(void)
{
    typedef HRESULT (WINAPI *FN)(void *, IUnknown **);
    FN fn = (FN)GetExport(s_hShcore, "DllGetActivationFactory");
    IUnknown *punk = (IUnknown *)POISON;
    HRESULT hr = fn(NULL, &punk);
    CheckObject(hr, punk);
}

static void Call_DllGetClassObject(void)
{
    typedef HRESULT (WINAPI *FN)(REFCLSID, REFIID, void **);
    FN fn = (FN)GetExport(s_hShcore, "DllGetClassObject");
    IUnknown *punk = (IUnknown *)POISON;
    HRESULT hr = fn(CLSID_NULL, IID_IClassFactory, (void **)&punk);
    CHECK_OUT(FAILED(hr));
    CHECK_OUT(punk == NULL);
}

static void Call_GetDpiForShellUIComponent(void)
{
    typedef UINT (WINAPI *FN)(INT);
    FN fn = (FN)GetExport(s_hShcore, "GetDpiForShellUIComponent");
    CHECK_OUT(fn(0 /* SHELL_UI_COMPONENT_TASKBARS */) != 0);
}

static void Call_GetDpiForMonitor(void)
{
    typedef HRESULT (WINAPI *FN)(HMONITOR, INT, UINT *, UINT *);
    FN fn = (FN)GetExport(s_hShcore, "GetDpiForMonitor");
    POINT pt = { 0, 0 };
    UINT x = 0x5555, y = 0x5555;
    HRESULT hr = fn(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), 0 /* MDT_EFFECTIVE_DPI */, &x, &y);
    /* It used to return S_OK with the outputs unset */
    CHECK_OUT(hr == S_OK && x != 0x5555 && y != 0x5555);
}

static void Call_GetProcessDpiAwareness(void)
{
    typedef HRESULT (WINAPI *FN)(HANDLE, INT *);
    FN fn = (FN)GetExport(s_hShcore, "GetProcessDpiAwareness");
    INT iAwareness = 0x5555;
    HRESULT hr = fn(NULL, &iAwareness);
    CHECK_OUT(hr == S_OK && iAwareness >= 0 && iAwareness <= 2);
}

static void Call_SetProcessDpiAwareness(void)
{
    typedef HRESULT (WINAPI *FN)(INT);
    FN fn = (FN)GetExport(s_hShcore, "SetProcessDpiAwareness");
    HRESULT hr = fn(42);
    CHECK_OUT(hr == E_INVALIDARG);
}

static void Call_IStream_Copy(void)
{
    typedef HRESULT (WINAPI *FN)(IStream *, IStream *, ULONG);
    FN fn = (FN)GetExport(s_hShcore, "IStream_Copy");
    CComPtr<IStream> pSrc, pDst;
    LARGE_INTEGER liZero = {};
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &pSrc)) || FAILED(CreateStreamOnHGlobal(NULL, TRUE, &pDst)))
    {
        s_iResult = CHILD_SETUP_FAILED;
        return;
    }
    pSrc->Write("abcd", 4, NULL);
    pSrc->Seek(liZero, STREAM_SEEK_SET, NULL);
    CHECK_OUT(fn(pSrc, pDst, 4) == S_OK);
}

static void Call_IStream_ReadStr(void)
{
    typedef HRESULT (WINAPI *FN)(IStream *, PWSTR *);
    FN fn = (FN)GetExport(s_hShcore, "IStream_ReadStr");
    CComPtr<IStream> pstm;
    PWSTR psz = (PWSTR)POISON;
    HRESULT hr;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &pstm)))
    {
        s_iResult = CHILD_SETUP_FAILED;
        return;
    }
    hr = fn(pstm, &psz); /* empty stream */
    CHECK_OUT(FAILED(hr) && psz == NULL);
}

static void Call_IStream_WriteStr(void)
{
    typedef HRESULT (WINAPI *FN)(IStream *, PCWSTR);
    FN fn = (FN)GetExport(s_hShcore, "IStream_WriteStr");
    CComPtr<IStream> pstm;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &pstm)))
    {
        s_iResult = CHILD_SETUP_FAILED;
        return;
    }
    CHECK_OUT(fn(pstm, L"abc") == S_OK);
}

static void Call_RevokeScaleChangeNotifications(void)
{
    typedef HRESULT (WINAPI *FN)(INT, DWORD);
    FN fn = (FN)GetExport(s_hShcore, "RevokeScaleChangeNotifications");
    CHECK_OUT(fn(0 /* DEVICE_PRIMARY */, 0x5A5A) == E_INVALIDARG); /* no such registration */
}

static void Call_UnregisterScaleChangeEvent(void)
{
    typedef HRESULT (WINAPI *FN)(DWORD_PTR);
    FN fn = (FN)GetExport(s_hShcore, "UnregisterScaleChangeEvent");
    CHECK_OUT(fn(0x5A5A) == E_INVALIDARG); /* no such registration */
}

static DWORD WINAPI ThreadProc(LPVOID pv)
{
    return 0;
}

static void Call_SHCreateThreadWithHandle(void)
{
    typedef BOOL (WINAPI *FN)(LPTHREAD_START_ROUTINE, void *, DWORD, LPTHREAD_START_ROUTINE, HANDLE *);
    FN fn = (FN)GetExport(s_hShcore, "SHCreateThreadWithHandle");
    HANDLE hThread = (HANDLE)POISON;
    BOOL ret = fn(ThreadProc, NULL, 0, NULL, &hThread);
    CHECK_OUT(ret && hThread && hThread != POISON);
    if (ret && hThread && hThread != POISON)
    {
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }
}

static void Call_SHRegGetValueFromHKCUHKLM(void)
{
    typedef LSTATUS (WINAPI *FN)(PCWSTR, PCWSTR, DWORD, DWORD *, void *, DWORD *);
    FN fn = (FN)GetExport(s_hShcore, "SHRegGetValueFromHKCUHKLM");
    WCHAR szValue[MAX_PATH];
    DWORD dwType = 0, cb = sizeof(szValue);
    LSTATUS err = fn(L"Software\\Microsoft\\Windows\\CurrentVersion", L"ProgramFilesDir", SRRF_RT_ANY, &dwType,
                     szValue, &cb);
    CHECK_OUT(err == ERROR_SUCCESS && (dwType == REG_SZ || dwType == REG_EXPAND_SZ));
}

enum { M_SHELL32, M_SHLWAPI, M_SHCORE };

static const struct
{
    PCSTR pszName;
    INT iModule;
    void (*pfnCall)(void);
    BOOL bMayWait;
} s_Entries[] =
{
#define E(mod, name, wait) { #name, mod, Call_##name, wait }
    E(M_SHELL32, PrepareDiscForBurnRunDllW, TRUE),
    E(M_SHELL32, AssocCreateForClasses, FALSE),
    E(M_SHELL32, AssocGetDetailsOfPropKey, FALSE),
    E(M_SHELL32, InitNetworkAddressControl, FALSE),
    E(M_SHELL32, SHAddDefaultPropertiesByExt, FALSE),
    E(M_SHELL32, SHAssocEnumHandlers, FALSE),
    E(M_SHELL32, SHAssocEnumHandlersForProtocolByApplication, FALSE),
    E(M_SHELL32, SHBindToFolderIDListParent, FALSE),
    E(M_SHELL32, SHBindToFolderIDListParentEx, FALSE),
    E(M_SHELL32, SHChangeNotifyRegisterThread, FALSE),
    E(M_SHELL32, SHCreateAssociationRegistration, FALSE),
    E(M_SHELL32, SHCreateDefaultPropertiesOp, FALSE),
    E(M_SHELL32, SHCreateItemWithParent, FALSE),
    E(M_SHELL32, SHCreateLocalServerRunDll, TRUE),
    E(M_SHELL32, SHEnableServiceObject, FALSE),
    E(M_SHELL32, SHGetDriveMedia, FALSE),
    E(M_SHELL32, SHGetLocalizedName, FALSE),
    E(M_SHELL32, SHGetPathFromIDListEx, FALSE),
    E(M_SHELL32, SHGetPropertyStoreFromIDList, FALSE),
    E(M_SHELL32, SHGetStockIconInfo, FALSE),
    E(M_SHELL32, SHGetTemporaryPropertyForItem, FALSE),
    E(M_SHELL32, SHRemoveLocalizedName, FALSE),
    E(M_SHELL32, SHSetDefaultProperties, TRUE),
    E(M_SHELL32, SHSetTemporaryPropertyForItem, FALSE),
    E(M_SHELL32, WaitForExplorerRestartW, TRUE),
    E(M_SHELL32, ILLoadFromStreamEx, FALSE),
    E(M_SHELL32, Shell_NotifyIconGetRect, FALSE),
    E(M_SHELL32, StgMakeUniqueName, FALSE),
    E(M_SHLWAPI, StrChrNIW, FALSE),
    E(M_SHCORE, CreateRandomAccessStreamOnFile, FALSE),
    E(M_SHCORE, CreateStreamOverRandomAccessStream, FALSE),
    E(M_SHCORE, DllGetActivationFactory, FALSE),
    E(M_SHCORE, DllGetClassObject, FALSE),
    E(M_SHCORE, GetDpiForShellUIComponent, FALSE),
    E(M_SHCORE, GetDpiForMonitor, FALSE),
    E(M_SHCORE, GetProcessDpiAwareness, FALSE),
    E(M_SHCORE, SetProcessDpiAwareness, FALSE),
    E(M_SHCORE, IStream_Copy, FALSE),
    E(M_SHCORE, IStream_ReadStr, FALSE),
    E(M_SHCORE, IStream_WriteStr, FALSE),
    E(M_SHCORE, RevokeScaleChangeNotifications, FALSE),
    E(M_SHCORE, UnregisterScaleChangeEvent, FALSE),
    E(M_SHCORE, SHCreateThreadWithHandle, FALSE),
    E(M_SHCORE, SHRegGetValueFromHKCUHKLM, FALSE),
#undef E
};

static HMODULE ModuleOf(INT iModule)
{
    return (iModule == M_SHELL32) ? s_hShell32 : (iModule == M_SHLWAPI) ? s_hShlwapi : s_hShcore;
}

static void Setup(void)
{
    s_hShell32 = LoadLibraryW(L"shell32.dll");
    s_hShlwapi = LoadLibraryW(L"shlwapi.dll");
    s_hShcore = LoadLibraryW(L"shcore.dll");
    GetTempPathW(_countof(s_szTempDir), s_szTempDir);
    PathAppendW(s_szTempDir, L"FormerStubs");
    CreateDirectoryW(s_szTempDir, NULL);
    StringCchPrintfW(s_szTempFile, _countof(s_szTempFile), L"%s\\stub test.txt", s_szTempDir);
}

static int RunChild(INT iEntry)
{
    if (iEntry < 0 || iEntry >= (INT)_countof(s_Entries))
        return CHILD_SETUP_FAILED;
    if (!GetExport(ModuleOf(s_Entries[iEntry].iModule), s_Entries[iEntry].pszName))
        return CHILD_NO_EXPORT;
    CoInitialize(NULL);
    s_iResult = CHILD_OK;
    s_Entries[iEntry].pfnCall();
    return s_iResult;
}

START_TEST(FormerStubs)
{
    WCHAR szExe[MAX_PATH], szCmd[MAX_PATH + 64];
    char **argv;
    int argc = winetest_get_mainargs(&argv);
    INT i;

    Setup();

    if (argc >= 3)
    {
        /* Child: exit without the test summary, so the parent sees the call's own exit */
        ExitProcess(RunChild(atoi(argv[2])));
    }

    GetModuleFileNameW(NULL, szExe, _countof(szExe));
    for (i = 0; i < (INT)_countof(s_Entries); i++)
    {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        DWORD dwWait, dwExit = 0xDEADBEEF;

        if (!GetExport(ModuleOf(s_Entries[i].iModule), s_Entries[i].pszName))
        {
            skip("%s is not exported\n", s_Entries[i].pszName);
            continue;
        }

        StringCchPrintfW(szCmd, _countof(szCmd), L"\"%s\" FormerStubs %d", szExe, i);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        if (!CreateProcessW(NULL, szCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        {
            ok(FALSE, "%s: CreateProcess failed %lu\n", s_Entries[i].pszName, GetLastError());
            continue;
        }
        dwWait = WaitForSingleObject(pi.hProcess, CHILD_TIMEOUT_MS);
        if (dwWait == WAIT_TIMEOUT)
        {
            ok(s_Entries[i].bMayWait, "%s: still running after %u ms\n", s_Entries[i].pszName, CHILD_TIMEOUT_MS);
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        else
        {
            GetExitCodeProcess(pi.hProcess, &dwExit);
            ok(dwExit == CHILD_OK, "%s: child exit code 0x%lx\n", s_Entries[i].pszName, dwExit);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    DeleteFileW(s_szTempFile);
    RemoveDirectoryW(s_szTempDir);
}
