/*
 * Internet Explorer wrapper
 *
 * Copyright 2006 Mike McCormack
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <windows.h>
#include <advpub.h>
#include <ole2.h>
#include <rpcproxy.h>

#include "wine/unicode.h"
#include "wine/debug.h"

extern DWORD WINAPI IEWinMain(const WCHAR*, int);

static BOOL check_native_ie(void)
{
    DWORD handle, size;
    LPWSTR file_desc;
    UINT bytes;
    void* buf;
    BOOL ret = TRUE;
    LPWORD translation;

    static const WCHAR browseui_dllW[] = {'b','r','o','w','s','e','u','i','.','d','l','l',0};
    static const WCHAR wineW[] = {'W','i','n','e',0};
    static const WCHAR translationW[] =
        {'\\','V','a','r','F','i','l','e','I','n','f','o',
         '\\','T','r','a','n','s','l','a','t','i','o','n',0};
    static const WCHAR file_desc_fmtW[] =
        {'\\','S','t','r','i','n','g','F','i','l','e','I','n','f','o',
         '\\','%','0','4','x','%','0','4','x',
         '\\','F','i','l','e','D','e','s','c','r','i','p','t','i','o','n',0};
    WCHAR file_desc_strW[48];

    size = GetFileVersionInfoSizeW(browseui_dllW, &handle);
    if(!size)
        return TRUE;

    buf = HeapAlloc(GetProcessHeap(), 0, size);
    GetFileVersionInfoW(browseui_dllW, 0, size,buf);
    if (VerQueryValueW(buf, translationW, (void **)&translation, &bytes))
    {
        wsprintfW(file_desc_strW, file_desc_fmtW, translation[0], translation[1]);
        ret = !VerQueryValueW(buf, file_desc_strW, (void**)&file_desc, &bytes) || !strstrW(file_desc, wineW);
    }

    HeapFree(GetProcessHeap(), 0, buf);
    return ret;
}

static DWORD register_iexplore(BOOL doregister)
{
    HRESULT hres;

    if (check_native_ie()) {
        WINE_MESSAGE("Native IE detected, not doing registration\n");
        return 0;
    }

    hres = RegInstallA(NULL, doregister ? "RegisterIE" : "UnregisterIE", NULL);
    return FAILED(hres);
}

#ifdef __REACTOS__
/* Without Wine Gecko every page stays blank and nothing says why */
static BOOL is_gecko_installed(void)
{
    WCHAR path[MAX_PATH];
    DWORD size = sizeof(path) - sizeof(WCHAR), type, attr;
    HKEY hkey;
    LONG res;

    /* The version must match GECKO_VERSION in mshtml */
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Wine\\MSHTML\\2.40", 0, KEY_QUERY_VALUE, &hkey) != ERROR_SUCCESS)
        return FALSE;
    res = RegQueryValueExW(hkey, L"GeckoPath", NULL, &type, (BYTE *)path, &size);
    RegCloseKey(hkey);
    if (res != ERROR_SUCCESS || type != REG_SZ)
        return FALSE;
    path[size / sizeof(WCHAR)] = 0;

    attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* The leading switches, parsed as IEWinMain does */
static BOOL is_com_activation(const WCHAR *cmdline)
{
    while (*cmdline)
    {
        int length = 0;

        while (*cmdline == ' ' || *cmdline == '\t') cmdline++;
        if (*cmdline != '-') break;
        while (cmdline[length] && cmdline[length] != ' ' && cmdline[length] != '\t') length++;

        if ((length == 10 && !strncmpiW(cmdline, L"-embedding", 10)) ||
            (length == 13 && !strncmpiW(cmdline, L"-startmanager", 13)))
            return TRUE;
        cmdline += length;
    }
    return FALSE;
}

static void install_gecko(void)
{
    static const WCHAR rundllW[] = L"\\rundll32.exe";
    WCHAR app[MAX_PATH], cmdline[MAX_PATH + 64];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    UINT len;

    len = GetSystemDirectoryW(app, MAX_PATH - sizeof(rundllW) / sizeof(WCHAR));
    if (!len || len >= MAX_PATH - sizeof(rundllW) / sizeof(WCHAR))
        return;
    lstrcpyW(app + len, rundllW);
    wsprintfW(cmdline, L"\"%s\" shell32.dll,Control_RunDLL appwiz.cpl,,install_gecko", app);

    /* appwiz explains what is missing and offers the download; control.exe would not
     * wait for it, rundll32 returns when the dialog closes */
    if (CreateProcessW(app, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
    }
}
#endif

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE prev, WCHAR *cmdline, int show)
{
    static const WCHAR regserverW[] = {'r','e','g','s','e','r','v','e','r',0};
    static const WCHAR unregserverW[] = {'u','n','r','e','g','s','e','r','v','e','r',0};

    if(*cmdline == '-' || *cmdline == '/') {
        if(!strcmpiW(cmdline+1, regserverW))
            return register_iexplore(TRUE);
        if(!strcmpiW(cmdline+1, unregserverW))
            return register_iexplore(FALSE);
    }

#ifdef __REACTOS__
    /* Not for COM activation, which has no user waiting for a window */
    if (!is_com_activation(cmdline) && !is_gecko_installed())
        install_gecko();
#endif

    return IEWinMain(cmdline, show);
}
