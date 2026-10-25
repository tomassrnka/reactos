/*
 * PROJECT:     ReactOS Default Browser Chooser
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     First-login dialog that lets the user pick (and if needed
 *              install) a default web browser, then writes the full set of
 *              default-browser associations and Default-Programs capability
 *              keys.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <strsafe.h>

#include "resource.h"

#define MAX_BROWSERS 32
#define WM_APP_INSTALLDONE (WM_APP + 1)

/* Bounded wait for the network to come up before a download. */
#define NET_WAIT_TIMEOUT_MS 60000
#define NET_WAIT_POLL_MS    1000

typedef struct _BROWSER_ENTRY
{
    WCHAR szKey[128];   /* StartMenuInternet subkey name, e.g. "IEXPLORE.EXE" */
    WCHAR szName[256];  /* display name, e.g. "Internet Explorer" */
    WCHAR szExe[MAX_PATH]; /* fully expanded path to the browser executable */
} BROWSER_ENTRY, *PBROWSER_ENTRY;

static HINSTANCE g_hInstance;
static BROWSER_ENTRY g_Browsers[MAX_BROWSERS];
static int g_BrowserCount = 0;
static int g_InstallItemIndex = -1; /* listbox index of the synthetic "install Firefox" item, or -1 */
static HWND g_hDlg = NULL;

static const WCHAR szStartMenuInternet[] = L"SOFTWARE\\Clients\\StartMenuInternet";
static const WCHAR szRegisteredApps[]    = L"SOFTWARE\\RegisteredApplications";
static const WCHAR szClasses[]           = L"SOFTWARE\\Classes";
static const WCHAR szConfigKey[]         = L"SOFTWARE\\ReactOS\\BrowserChooser";
static const WCHAR szDefaultFfPackage[]  = L"firefox-esr";

/* ------------------------------------------------------------------ */
/* Logging                                                            */
/* ------------------------------------------------------------------ */

static void
LogLine(LPCWSTR fmt, ...)
{
    WCHAR szBuf[1024];
    WCHAR szPath[MAX_PATH];
    va_list args;
    HANDLE hFile;
    char szUtf8[2048];
    int cb;
    DWORD dwWritten;

    va_start(args, fmt);
    StringCchVPrintfW(szBuf, ARRAYSIZE(szBuf), fmt, args);
    va_end(args);

    /* Surface it to any attached kernel debugger / serial log. */
    OutputDebugStringW(L"BROWSERCHOOSER: ");
    OutputDebugStringW(szBuf);
    OutputDebugStringW(L"\n");

    /* Append it to a persistent on-disk log for post-mortem/harness read-back. */
    if (GetWindowsDirectoryW(szPath, ARRAYSIZE(szPath)))
    {
        StringCchCatW(szPath, ARRAYSIZE(szPath), L"\\Temp\\browserchooser.log");
        hFile = CreateFileW(szPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            cb = WideCharToMultiByte(CP_UTF8, 0, szBuf, -1, szUtf8, sizeof(szUtf8) - 2, NULL, NULL);
            if (cb > 0)
            {
                /* overwrite the terminating NUL with CRLF */
                szUtf8[cb - 1] = '\r';
                szUtf8[cb] = '\n';
                SetFilePointer(hFile, 0, NULL, FILE_END);
                WriteFile(hFile, szUtf8, cb + 1, &dwWritten, NULL);
            }
            CloseHandle(hFile);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Registry helpers                                                   */
/* ------------------------------------------------------------------ */

static LONG
SetStringValue(HKEY hRoot, LPCWSTR pszSubKey, LPCWSTR pszValue, LPCWSTR pszData)
{
    HKEY hKey;
    LONG lRet;

    lRet = RegCreateKeyExW(hRoot, pszSubKey, 0, NULL, REG_OPTION_NON_VOLATILE,
                           KEY_WRITE, NULL, &hKey, NULL);
    if (lRet != ERROR_SUCCESS)
        return lRet;

    lRet = RegSetValueExW(hKey, pszValue, 0, REG_SZ,
                          (const BYTE *)pszData,
                          (DWORD)((wcslen(pszData) + 1) * sizeof(WCHAR)));
    RegCloseKey(hKey);
    return lRet;
}

/* Strip surrounding quotes and any trailing argument list, and expand
 * environment strings, leaving just the executable path. */
static void
ExtractExePath(LPCWSTR pszCommand, LPWSTR pszOut, size_t cchOut)
{
    WCHAR szExpanded[MAX_PATH * 2];
    LPWSTR p;

    ExpandEnvironmentStringsW(pszCommand, szExpanded, ARRAYSIZE(szExpanded));

    p = szExpanded;
    while (*p == L' ')
        p++;

    if (*p == L'"')
    {
        LPWSTR end;
        p++;
        end = wcschr(p, L'"');
        if (end)
            *end = L'\0';
    }
    else
    {
        /* Bare path (StartMenuInternet convention): take the whole trimmed
         * string.  Only cut at a space if what precedes it already ends in
         * ".exe", to tolerate an accidental trailing argument. */
        LPWSTR dotexe = StrStrIW(p, L".exe");
        if (dotexe)
        {
            LPWSTR after = dotexe + 4;
            if (*after == L' ' || *after == L'\0')
                *after = L'\0';
        }
    }

    StringCchCopyW(pszOut, cchOut, p);
}

/* ------------------------------------------------------------------ */
/* Browser enumeration                                                */
/* ------------------------------------------------------------------ */

static BOOL
ReadClientCommand(HKEY hClients, LPCWSTR pszKeyName, LPWSTR pszExe, size_t cchExe)
{
    WCHAR szCmdSub[256];
    WCHAR szCommand[MAX_PATH * 2];
    DWORD cbData;
    HKEY hCmd;

    StringCchPrintfW(szCmdSub, ARRAYSIZE(szCmdSub), L"%s\\shell\\open\\command", pszKeyName);
    if (RegOpenKeyExW(hClients, szCmdSub, 0, KEY_READ, &hCmd) != ERROR_SUCCESS)
        return FALSE;

    cbData = sizeof(szCommand);
    if (RegQueryValueExW(hCmd, NULL, NULL, NULL, (LPBYTE)szCommand, &cbData) != ERROR_SUCCESS)
    {
        RegCloseKey(hCmd);
        return FALSE;
    }
    RegCloseKey(hCmd);

    ExtractExePath(szCommand, pszExe, cchExe);
    return (pszExe[0] != L'\0');
}

static void
EnumBrowsers(void)
{
    HKEY hClients;
    DWORD dwIndex = 0;
    WCHAR szSubKey[128];
    DWORD cchSubKey;

    g_BrowserCount = 0;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, szStartMenuInternet, 0, KEY_READ, &hClients) != ERROR_SUCCESS)
        return;

    for (;;)
    {
        HKEY hBrowser;
        WCHAR szName[256];
        DWORD cbName;
        PBROWSER_ENTRY pEntry;

        cchSubKey = ARRAYSIZE(szSubKey);
        if (RegEnumKeyExW(hClients, dwIndex, szSubKey, &cchSubKey, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        dwIndex++;

        if (g_BrowserCount >= MAX_BROWSERS)
            break;

        pEntry = &g_Browsers[g_BrowserCount];
        ZeroMemory(pEntry, sizeof(*pEntry));

        if (!ReadClientCommand(hClients, szSubKey, pEntry->szExe, ARRAYSIZE(pEntry->szExe)))
            continue;

        StringCchCopyW(pEntry->szKey, ARRAYSIZE(pEntry->szKey), szSubKey);

        /* Friendly name: default value of the client key, fall back to subkey name. */
        szName[0] = L'\0';
        if (RegOpenKeyExW(hClients, szSubKey, 0, KEY_READ, &hBrowser) == ERROR_SUCCESS)
        {
            cbName = sizeof(szName);
            RegQueryValueExW(hBrowser, NULL, NULL, NULL, (LPBYTE)szName, &cbName);
            RegCloseKey(hBrowser);
        }
        StringCchCopyW(pEntry->szName, ARRAYSIZE(pEntry->szName),
                       szName[0] ? szName : szSubKey);

        g_BrowserCount++;
    }

    RegCloseKey(hClients);
}

static BOOL
FindFirefoxExe(LPWSTR pszOut, size_t cchOut)
{
    static const LPCWSTR candidates[] = {
        L"%ProgramFiles%\\Mozilla Firefox\\firefox.exe",
        L"%ProgramFiles(x86)%\\Mozilla Firefox\\firefox.exe",
    };
    WCHAR szPath[MAX_PATH];
    UINT i;

    for (i = 0; i < ARRAYSIZE(candidates); i++)
    {
        ExpandEnvironmentStringsW(candidates[i], szPath, ARRAYSIZE(szPath));
        if (GetFileAttributesW(szPath) != INVALID_FILE_ATTRIBUTES)
        {
            StringCchCopyW(pszOut, cchOut, szPath);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL
IsFirefoxPresent(void)
{
    WCHAR szPath[MAX_PATH];
    int i;

    for (i = 0; i < g_BrowserCount; i++)
    {
        if (StrStrIW(g_Browsers[i].szExe, L"firefox.exe") != NULL)
            return TRUE;
    }
    return FindFirefoxExe(szPath, ARRAYSIZE(szPath));
}

/* ------------------------------------------------------------------ */
/* Association + Default-Programs capability writer                   */
/* ------------------------------------------------------------------ */

/* Build a registry-safe token from the client key name, e.g.
 * "FIREFOX.EXE" -> "FIREFOXEXE". */
static void
SanitizeToken(LPCWSTR pszKey, LPWSTR pszOut, size_t cchOut)
{
    size_t i = 0;
    LPCWSTR s = pszKey;

    while (*s && i + 1 < cchOut)
    {
        if ((*s >= L'A' && *s <= L'Z') || (*s >= L'a' && *s <= L'z') ||
            (*s >= L'0' && *s <= L'9'))
        {
            pszOut[i++] = *s;
        }
        s++;
    }
    pszOut[i] = L'\0';
}

static void
WriteProgId(LPCWSTR pszProgId, LPCWSTR pszFriendly, LPCWSTR pszExe)
{
    WCHAR szSub[320];
    WCHAR szCmd[MAX_PATH + 16];
    WCHAR szIcon[MAX_PATH + 8];

    StringCchPrintfW(szSub, ARRAYSIZE(szSub), L"%s\\%s", szClasses, pszProgId);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, NULL, pszFriendly);

    StringCchPrintfW(szSub, ARRAYSIZE(szSub), L"%s\\%s\\DefaultIcon", szClasses, pszProgId);
    StringCchPrintfW(szIcon, ARRAYSIZE(szIcon), L"%s,0", pszExe);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, NULL, szIcon);

    StringCchPrintfW(szSub, ARRAYSIZE(szSub), L"%s\\%s\\shell\\open\\command", szClasses, pszProgId);
    StringCchPrintfW(szCmd, ARRAYSIZE(szCmd), L"\"%s\" \"%%1\"", pszExe);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, NULL, szCmd);
}

static void
WriteProtocolHandler(LPCWSTR pszProto, LPCWSTR pszExe)
{
    WCHAR szSub[256];
    WCHAR szCmd[MAX_PATH + 16];

    StringCchPrintfW(szSub, ARRAYSIZE(szSub), L"%s\\%s", szClasses, pszProto);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, L"URL Protocol", L"");

    StringCchPrintfW(szSub, ARRAYSIZE(szSub), L"%s\\%s\\shell\\open\\command", szClasses, pszProto);
    StringCchPrintfW(szCmd, ARRAYSIZE(szCmd), L"\"%s\" \"%%1\"", pszExe);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, NULL, szCmd);
}

/*
 * Make <pszKey> the system default browser:
 *  - StartMenuInternet default value
 *  - http/https/ftp protocol handlers (HKLM\Software\Classes)
 *  - .htm/.html file associations through a dedicated ProgID
 *  - Default-Programs capability keys + RegisteredApplications
 */
static BOOL
SetDefaultBrowser(LPCWSTR pszKey, LPCWSTR pszName, LPCWSTR pszExe)
{
    WCHAR szToken[160];
    WCHAR szUrlProgId[224];
    WCHAR szHtmlProgId[224];
    WCHAR szCapBase[320];
    WCHAR szSub[384];
    WCHAR szData[320];

    LogLine(L"SetDefaultBrowser key=%s name=%s exe=%s", pszKey, pszName, pszExe);

    SanitizeToken(pszKey, szToken, ARRAYSIZE(szToken));
    StringCchPrintfW(szUrlProgId, ARRAYSIZE(szUrlProgId), L"Nitro.%s.URL", szToken);
    StringCchPrintfW(szHtmlProgId, ARRAYSIZE(szHtmlProgId), L"Nitro.%s.HTML", szToken);

    /* 1. StartMenuInternet default = this client */
    SetStringValue(HKEY_LOCAL_MACHINE, szStartMenuInternet, NULL, pszKey);

    /* 2. ProgIDs that actually launch the browser */
    StringCchPrintfW(szData, ARRAYSIZE(szData), L"%s URL", pszName);
    WriteProgId(szUrlProgId, szData, pszExe);
    StringCchPrintfW(szData, ARRAYSIZE(szData), L"%s HTML Document", pszName);
    WriteProgId(szHtmlProgId, szData, pszExe);

    /* 3. Protocol handlers (effective switch on NT 5.2 - no UserChoice hash) */
    WriteProtocolHandler(L"http", pszExe);
    WriteProtocolHandler(L"https", pszExe);
    WriteProtocolHandler(L"ftp", pszExe);

    /* 4. File associations */
    StringCchPrintfW(szSub, ARRAYSIZE(szSub), L"%s\\.htm", szClasses);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, NULL, szHtmlProgId);
    StringCchPrintfW(szSub, ARRAYSIZE(szSub), L"%s\\.html", szClasses);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, NULL, szHtmlProgId);

    /* 5. Default-Programs capabilities (shows in Control Panel -> Default Programs) */
    StringCchPrintfW(szCapBase, ARRAYSIZE(szCapBase), L"%s\\%s\\Capabilities",
                     szStartMenuInternet, pszKey);

    SetStringValue(HKEY_LOCAL_MACHINE, szCapBase, L"ApplicationName", pszName);
    StringCchPrintfW(szData, ARRAYSIZE(szData), L"%s,0", pszExe);
    SetStringValue(HKEY_LOCAL_MACHINE, szCapBase, L"ApplicationIcon", szData);
    StringCchPrintfW(szData, ARRAYSIZE(szData), L"%s web browser", pszName);
    SetStringValue(HKEY_LOCAL_MACHINE, szCapBase, L"ApplicationDescription", szData);

    StringCchPrintfW(szSub, ARRAYSIZE(szSub), L"%s\\URLAssociations", szCapBase);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, L"http", szUrlProgId);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, L"https", szUrlProgId);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, L"ftp", szUrlProgId);

    StringCchPrintfW(szSub, ARRAYSIZE(szSub), L"%s\\FileAssociations", szCapBase);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, L".htm", szHtmlProgId);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, L".html", szHtmlProgId);

    /* 6. RegisteredApplications entry */
    StringCchPrintfW(szData, ARRAYSIZE(szData), L"%s\\%s\\Capabilities",
                     szStartMenuInternet, pszKey);
    SetStringValue(HKEY_LOCAL_MACHINE, szRegisteredApps, pszName, szData);

    /* Notify the shell that associations changed. */
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_FLUSH, NULL, NULL);

    LogLine(L"SetDefaultBrowser done for %s", pszKey);
    return TRUE;
}

/* Register a StartMenuInternet client entry for a freshly installed Firefox,
 * in case the installer did not create one in the native registry view. */
static void
EnsureFirefoxClient(LPCWSTR pszExe)
{
    WCHAR szSub[256];
    WCHAR szIcon[MAX_PATH + 8];

    StringCchPrintfW(szSub, ARRAYSIZE(szSub), L"%s\\FIREFOX.EXE", szStartMenuInternet);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, NULL, L"Mozilla Firefox");

    StringCchPrintfW(szSub, ARRAYSIZE(szSub), L"%s\\FIREFOX.EXE\\DefaultIcon", szStartMenuInternet);
    StringCchPrintfW(szIcon, ARRAYSIZE(szIcon), L"%s,0", pszExe);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, NULL, szIcon);

    StringCchPrintfW(szSub, ARRAYSIZE(szSub), L"%s\\FIREFOX.EXE\\shell\\open\\command", szStartMenuInternet);
    SetStringValue(HKEY_LOCAL_MACHINE, szSub, NULL, pszExe);
}

/* ------------------------------------------------------------------ */
/* Network readiness                                                   */
/* ------------------------------------------------------------------ */

/* Parse a dotted-quad into four octets. */
static BOOL
ParseIPv4(const char *psz, UINT *pOct)
{
    return sscanf(psz, "%u.%u.%u.%u", &pOct[0], &pOct[1], &pOct[2], &pOct[3]) == 4
           && pOct[0] < 256 && pOct[1] < 256 && pOct[2] < 256 && pOct[3] < 256;
}

/* TRUE if at least one adapter has a usable IPv4 address (not 0.0.0.0, not
 * APIPA 169.254/16, not loopback) and a non-zero default gateway. */
static BOOL
IsNetworkReady(void)
{
    PIP_ADAPTER_INFO pInfo = NULL, pAd;
    ULONG cb = 0;
    BOOL bReady = FALSE;
    DWORD dwErr;

    dwErr = GetAdaptersInfo(NULL, &cb);
    if (dwErr != ERROR_BUFFER_OVERFLOW || !cb)
        return FALSE;

    pInfo = HeapAlloc(GetProcessHeap(), 0, cb);
    if (!pInfo)
        return FALSE;

    if (GetAdaptersInfo(pInfo, &cb) == NO_ERROR)
    {
        for (pAd = pInfo; pAd && !bReady; pAd = pAd->Next)
        {
            PIP_ADDR_STRING pIp, pGw;
            for (pIp = &pAd->IpAddressList; pIp && !bReady; pIp = pIp->Next)
            {
                UINT ip[4], gw[4];

                if (!ParseIPv4(pIp->IpAddress.String, ip))
                    continue;
                if ((ip[0] | ip[1] | ip[2] | ip[3]) == 0 || ip[0] == 127 ||
                    (ip[0] == 169 && ip[1] == 254))
                    continue;

                for (pGw = &pAd->GatewayList; pGw; pGw = pGw->Next)
                {
                    if (ParseIPv4(pGw->IpAddress.String, gw) && (gw[0] | gw[1] | gw[2] | gw[3]) != 0)
                    {
                        bReady = TRUE;
                        break;
                    }
                }
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, pInfo);
    return bReady;
}

/* A connect fired before DHCP completes fails with WSAENETUNREACH (10051) /
 * wininet 12029, so wait (bounded) for a usable address before downloading. */
static BOOL
WaitForNetwork(DWORD dwTimeoutMs)
{
    DWORD dwStart = GetTickCount();

    for (;;)
    {
        if (IsNetworkReady())
        {
            LogLine(L"WaitForNetwork: network ready after %lu ms", GetTickCount() - dwStart);
            return TRUE;
        }
        if (GetTickCount() - dwStart >= dwTimeoutMs)
            break;
        Sleep(NET_WAIT_POLL_MS);
    }

    LogLine(L"WaitForNetwork: no usable IPv4 address/gateway after %lu ms", dwTimeoutMs);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Firefox install through RAPPS                                       */
/* ------------------------------------------------------------------ */

static void
GetFirefoxPackage(LPWSTR pszOut, size_t cchOut)
{
    HKEY hKey;
    DWORD cbData;

    StringCchCopyW(pszOut, cchOut, szDefaultFfPackage);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, szConfigKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        WCHAR szVal[128];
        cbData = sizeof(szVal);
        if (RegQueryValueExW(hKey, L"FirefoxPackage", NULL, NULL, (LPBYTE)szVal, &cbData) == ERROR_SUCCESS
            && szVal[0])
        {
            StringCchCopyW(pszOut, cchOut, szVal);
        }
        RegCloseKey(hKey);
    }
}

/* Run "rapps.exe /install /S <package>" and wait for it. Returns TRUE if
 * firefox.exe is on disk afterwards. */
static BOOL
InstallFirefox(void)
{
    WCHAR szRapps[MAX_PATH];
    WCHAR szPackage[128];
    WCHAR szCmd[MAX_PATH + 128];
    WCHAR szFfExe[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD dwExit = (DWORD)-1;

    /* If Firefox is already installed, just adopt it as the default instead of
     * downloading it again (also avoids a redundant RAPPS run). */
    if (FindFirefoxExe(szFfExe, ARRAYSIZE(szFfExe)))
    {
        LogLine(L"InstallFirefox: firefox already present at %s; adopting as default", szFfExe);
        EnsureFirefoxClient(szFfExe);
        SetDefaultBrowser(L"FIREFOX.EXE", L"Mozilla Firefox", szFfExe);
        return TRUE;
    }

    if (!WaitForNetwork(NET_WAIT_TIMEOUT_MS))
    {
        LogLine(L"InstallFirefox: network not ready; not starting download");
        return FALSE;
    }

    if (!GetSystemDirectoryW(szRapps, ARRAYSIZE(szRapps)))
        return FALSE;
    StringCchCatW(szRapps, ARRAYSIZE(szRapps), L"\\rapps.exe");

    GetFirefoxPackage(szPackage, ARRAYSIZE(szPackage));
    StringCchPrintfW(szCmd, ARRAYSIZE(szCmd), L"\"%s\" /install /S %s", szRapps, szPackage);

    LogLine(L"InstallFirefox: launching %s", szCmd);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, szCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        LogLine(L"InstallFirefox: CreateProcess failed err=%lu", GetLastError());
        return FALSE;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &dwExit);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    LogLine(L"InstallFirefox: rapps exit=%lu", dwExit);

    if (FindFirefoxExe(szFfExe, ARRAYSIZE(szFfExe)))
    {
        LogLine(L"InstallFirefox: firefox present at %s", szFfExe);
        EnsureFirefoxClient(szFfExe);
        SetDefaultBrowser(L"FIREFOX.EXE", L"Mozilla Firefox", szFfExe);
        return TRUE;
    }

    LogLine(L"InstallFirefox: firefox.exe not found after install");
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Dialog                                                             */
/* ------------------------------------------------------------------ */

static DWORD WINAPI
InstallThreadProc(LPVOID lpParam)
{
    BOOL bOk = InstallFirefox();
    PostMessageW(g_hDlg, WM_APP_INSTALLDONE, (WPARAM)bOk, 0);
    return 0;
}

static void
LoadDlgString(UINT uId, LPWSTR pszBuf, int cch)
{
    pszBuf[0] = L'\0';
    LoadStringW(g_hInstance, uId, pszBuf, cch);
}

static void
PopulateList(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_BROWSERLIST);
    WCHAR szItem[300];
    int i, idx;

    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    g_InstallItemIndex = -1;

    EnumBrowsers();

    for (i = 0; i < g_BrowserCount; i++)
    {
        idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)g_Browsers[i].szName);
        SendMessageW(hList, LB_SETITEMDATA, idx, (LPARAM)i);
    }

    /* Offer the Firefox install only when no Firefox is present yet. */
    if (!IsFirefoxPresent())
    {
        LoadDlgString(IDS_INSTALLFIREFOX, szItem, ARRAYSIZE(szItem));
        idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)szItem);
        SendMessageW(hList, LB_SETITEMDATA, idx, (LPARAM)-1);
        g_InstallItemIndex = idx;
    }

    SendMessageW(hList, LB_SETCURSEL, 0, 0);
}

static void
SetBusy(HWND hDlg, BOOL bBusy, UINT uStatusId)
{
    WCHAR szStatus[256];

    EnableWindow(GetDlgItem(hDlg, IDOK), !bBusy);
    EnableWindow(GetDlgItem(hDlg, IDCANCEL), !bBusy);
    EnableWindow(GetDlgItem(hDlg, IDC_BROWSERLIST), !bBusy);

    if (uStatusId)
    {
        LoadDlgString(uStatusId, szStatus, ARRAYSIZE(szStatus));
        SetDlgItemTextW(hDlg, IDC_STATUS, szStatus);
    }
    else
    {
        SetDlgItemTextW(hDlg, IDC_STATUS, L"");
    }

    SetCursor(LoadCursorW(NULL, bBusy ? IDC_WAIT : IDC_ARROW));
}

static void
OnOk(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_BROWSERLIST);
    int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    int data;

    if (sel == LB_ERR)
        return;

    data = (int)SendMessageW(hList, LB_GETITEMDATA, sel, 0);

    if (data == -1)
    {
        /* Install-Firefox item: run RAPPS on a worker thread. */
        HANDLE hThread;
        SetBusy(hDlg, TRUE, IDS_INSTALLING);
        hThread = CreateThread(NULL, 0, InstallThreadProc, NULL, 0, NULL);
        if (hThread)
        {
            CloseHandle(hThread);
        }
        else
        {
            SetBusy(hDlg, FALSE, 0);
        }
        return;
    }

    if (data >= 0 && data < g_BrowserCount)
    {
        SetDefaultBrowser(g_Browsers[data].szKey,
                          g_Browsers[data].szName,
                          g_Browsers[data].szExe);
        EndDialog(hDlg, IDOK);
    }
}

static INT_PTR CALLBACK
DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
            g_hDlg = hDlg;
            PopulateList(hDlg);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDOK:
                    OnOk(hDlg);
                    return TRUE;

                case IDCANCEL:
                    LogLine(L"User chose Skip");
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;

                case IDC_BROWSERLIST:
                    if (HIWORD(wParam) == LBN_DBLCLK)
                    {
                        OnOk(hDlg);
                        return TRUE;
                    }
                    break;
            }
            break;

        case WM_APP_INSTALLDONE:
        {
            BOOL bOk = (BOOL)wParam;
            if (bOk)
            {
                EndDialog(hDlg, IDOK);
            }
            else
            {
                WCHAR szMsg[256], szTitle[128];
                SetBusy(hDlg, FALSE, 0);
                LoadDlgString(IDS_INSTALLFAILED, szMsg, ARRAYSIZE(szMsg));
                LoadDlgString(IDS_APPTITLE, szTitle, ARRAYSIZE(szTitle));
                MessageBoxW(hDlg, szMsg, szTitle, MB_OK | MB_ICONWARNING);
                /* Refresh the list in case something partially registered. */
                PopulateList(hDlg);
            }
            return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */

/*
 * Non-interactive automation entry points (also useful for unattended
 * provisioning scripts):
 *   bchooser.exe /set <StartMenuInternetKey>   set that client as default
 *   bchooser.exe /installfirefox               install Firefox via RAPPS + set default
 * Returns 0 on success, 1 on failure.  Without arguments the GUI chooser runs.
 */
static int
RunAutomation(int argc, LPWSTR *argv)
{
    if (!StrCmpIW(argv[0], L"/installfirefox"))
    {
        LogLine(L"Automation: /installfirefox");
        return InstallFirefox() ? 0 : 1;
    }

    if (!StrCmpIW(argv[0], L"/set"))
    {
        int i;
        if (argc < 2)
        {
            LogLine(L"Automation: /set needs a browser key");
            return 1;
        }

        EnumBrowsers();
        for (i = 0; i < g_BrowserCount; i++)
        {
            if (!StrCmpIW(g_Browsers[i].szKey, argv[1]))
            {
                return SetDefaultBrowser(g_Browsers[i].szKey,
                                         g_Browsers[i].szName,
                                         g_Browsers[i].szExe) ? 0 : 1;
            }
        }
        LogLine(L"Automation: /set - browser '%s' not found", argv[1]);
        return 1;
    }

    LogLine(L"Automation: unknown option '%s'", argv[0]);
    return 1;
}

int WINAPI
wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    INITCOMMONCONTROLSEX icc;
    int argc = 0;
    LPWSTR *argv;
    int ret = 0;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    g_hInstance = hInstance;

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    LogLine(L"Browser chooser starting");

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1)
    {
        ret = RunAutomation(argc - 1, &argv[1]);
        LocalFree(argv);
        LogLine(L"Browser chooser exiting (automation rc=%d)", ret);
        return ret;
    }
    if (argv)
        LocalFree(argv);

    DialogBoxW(hInstance, MAKEINTRESOURCEW(IDD_CHOOSER), NULL, DlgProc);

    LogLine(L"Browser chooser exiting");
    return 0;
}
