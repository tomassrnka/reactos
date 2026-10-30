/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for netapi32!DavGetHTTPFromUNCPath and DavGetUNCFromHTTPPath
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2 (x64 and WOW64). LibreOffice (mergedlo.dll) imports DavGetHTTPFromUNCPath.
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <winerror.h>

typedef DWORD (WINAPI *PFN_Convert)(PCWSTR, PWSTR, PDWORD);

typedef struct
{
    PCWSTR Input;
    DWORD Error;
    PCWSTR Output;
} CASE;

static const CASE g_UncToHttp[] =
{
    { L"\\\\server\\share\\dir\\f.txt", ERROR_SUCCESS, L"http://server/share/dir/f.txt" },
    { L"//server/share", ERROR_SUCCESS, L"http://server/share" },
    { L"\\\\s", ERROR_SUCCESS, L"http://s" },
    { L"\\\\server", ERROR_SUCCESS, L"http://server" },
    { L"\\\\server\\", ERROR_SUCCESS, L"http://server" },
    { L"\\\\server@SSL\\share", ERROR_SUCCESS, L"https://server/share" },
    { L"\\\\server@ssl\\a", ERROR_SUCCESS, L"https://server/a" },
    { L"\\\\server@SSL", ERROR_SUCCESS, L"https://server" },
    { L"\\\\server@8080\\a\\b\\", ERROR_SUCCESS, L"http://server:8080/a/b" },
    { L"\\\\server@SSL@8443\\x", ERROR_SUCCESS, L"https://server:8443/x" },
    { L"\\\\server@80\\x", ERROR_SUCCESS, L"http://server/x" },
    { L"\\\\server@443\\a", ERROR_SUCCESS, L"http://server:443/a" },
    { L"\\\\server@SSL@443\\a", ERROR_SUCCESS, L"https://server/a" },
    { L"\\\\server@SSL@80\\a", ERROR_SUCCESS, L"https://server:80/a" },
    { L"\\\\server@0\\a", ERROR_SUCCESS, L"http://server:0/a" },
    { L"\\\\server@SSL@0\\a", ERROR_SUCCESS, L"https://server:0/a" },
    { L"\\\\server\\DavWWWRoot\\x", ERROR_SUCCESS, L"http://server/x" },
    { L"\\\\ser\\ver\\a/b\\c", ERROR_SUCCESS, L"http://ser/ver/a/b/c" },
    { L"", ERROR_BAD_NET_NAME, NULL },
    { L"\\", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\", ERROR_BAD_NET_NAME, NULL },
    { L"/server/share", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\server@xyz\\a", ERROR_INVALID_PARAMETER, NULL },
    { L"\\\\server@SSL@\\a", ERROR_INVALID_PARAMETER, NULL },
    { L"\\\\server@99999999999\\a", ERROR_INVALID_PARAMETER, NULL },
    { L"\\\\server@8080@SSL\\a", ERROR_INVALID_PARAMETER, NULL },
    { L"\\\\server@8080@9\\a", ERROR_INVALID_PARAMETER, NULL },
    { L"\\\\server@-1\\a", ERROR_INVALID_PARAMETER, NULL },
    { L"\\\\server@1x\\a", ERROR_INVALID_PARAMETER, NULL },
    { L"\\\\.\\x", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\.", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\s\\c:", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\s\\a:b", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\s\\a<b", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\s\\a|b", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\s\\..\\b", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\s\\\\a", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\s\\a\\\\b", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\s\\a//b", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\se:rver\\a", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\ser*ver\\a", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\[::1]\\a", ERROR_BAD_NET_NAME, NULL },
    { L"\\\\s\\a*b", ERROR_SUCCESS, L"http://s/a*b" },
    { L"\\\\s\\a?b", ERROR_SUCCESS, L"http://s/a%3Fb" },
    { L"\\\\s\\a b", ERROR_SUCCESS, L"http://s/a%20b" },
    { L"\\\\s\\a%20b", ERROR_SUCCESS, L"http://s/a%2520b" },
    { L"\\\\s\\a#b", ERROR_SUCCESS, L"http://s/a%23b" },
    { L"\\\\s\\a@b", ERROR_SUCCESS, L"http://s/a%40b" },
    { L"\\\\s\\.\\b", ERROR_SUCCESS, L"http://s/b" },
    { L"\\\\s\\a/b\\c", ERROR_SUCCESS, L"http://s/a/b/c" },
    { L"\\\\s\\DAVWWWROOT\\x", ERROR_SUCCESS, L"http://s/x" },
    { L"\\\\s\\DavWWWRootx\\y", ERROR_SUCCESS, L"http://s/DavWWWRootx/y" },
    { L"\\\\s\\DavWWWRoot", ERROR_SUCCESS, L"http://s" },
    { L"\\\\s\\x\\DavWWWRoot\\y", ERROR_SUCCESS, L"http://s/x/DavWWWRoot/y" },
    { L"\\\\s@SSL\\DavWWWRoot\\y", ERROR_SUCCESS, L"https://s/y" },
    { L"\\\\1.2.3.4\\a", ERROR_SUCCESS, L"http://1.2.3.4/a" },
    { L"\\\\s.s\\a", ERROR_SUCCESS, L"http://s.s/a" },
    { L"\\\\se rver\\a", ERROR_SUCCESS, L"http://se rver/a" },
};

static const CASE g_HttpToUnc[] =
{
    { L"http://server/share/dir", ERROR_SUCCESS, L"\\\\server\\DavWWWRoot\\share\\dir" },
    { L"HTTP://Server/A", ERROR_SUCCESS, L"\\\\Server\\DavWWWRoot\\A" },
    { L"https://server:8443/x/", ERROR_SUCCESS, L"\\\\server@SSL@8443\\DavWWWRoot\\x" },
    { L"https://s:443/x", ERROR_SUCCESS, L"\\\\s@SSL\\DavWWWRoot\\x" },
    { L"https://s:444/x", ERROR_SUCCESS, L"\\\\s@SSL@444\\DavWWWRoot\\x" },
    { L"HTTPS://s", ERROR_SUCCESS, L"\\\\s@SSL\\DavWWWRoot" },
    { L"http://s:80", ERROR_SUCCESS, L"\\\\s\\DavWWWRoot" },
    { L"http://s:80/", ERROR_SUCCESS, L"\\\\s\\DavWWWRoot" },
    { L"http://server:80/", ERROR_SUCCESS, L"\\\\server\\DavWWWRoot" },
    { L"http://s/", ERROR_SUCCESS, L"\\\\s\\DavWWWRoot" },
    { L"http://s/a/", ERROR_SUCCESS, L"\\\\s\\DavWWWRoot\\a" },
    { L"http://u@s/x", ERROR_SUCCESS, L"\\\\s\\DavWWWRoot\\x" },
    { L"", ERROR_INVALID_PARAMETER, NULL },
    { L"http", ERROR_INVALID_PARAMETER, NULL },
    { L"httpx://s", ERROR_INVALID_PARAMETER, NULL },
    { L"ftp://x/", ERROR_INVALID_PARAMETER, NULL },
    { L"http:", ERROR_BAD_NET_NAME, NULL },
    { L"http:/", ERROR_BAD_NET_NAME, NULL },
    { L"http://s/a//b", ERROR_BAD_NET_NAME, NULL },
    { L"http://server//x", ERROR_BAD_NET_NAME, NULL },
    { L"http://s?q=1", ERROR_BAD_NET_NAME, NULL },
    { L"http://", ERROR_BAD_NETPATH, NULL },
    { L"http:///x", ERROR_BAD_NETPATH, NULL },
    { L"http://s:", ERROR_BAD_NETPATH, NULL },
    { L"http://s:abc/x", ERROR_BAD_NETPATH, NULL },
    { L"http://s:99999999/x", ERROR_BAD_NETPATH, NULL },
    { L"http://s/a:b", ERROR_BAD_NET_NAME, NULL },
    { L"http://s/a|b", ERROR_BAD_NET_NAME, NULL },
    { L"http://./x", ERROR_BAD_NET_NAME, NULL },
    { L"http://[::1]/x", ERROR_BAD_NET_NAME, NULL },
    { L"http://[::1]:8080/x", ERROR_BAD_NET_NAME, NULL },
    { L"http://se:rver/x", ERROR_BAD_NETPATH, NULL },
    { L"http://s:65536/x", ERROR_BAD_NETPATH, NULL },
    { L"http://s/a*b", ERROR_SUCCESS, L"\\\\s\\DavWWWRoot\\a*b" },
    { L"http://s/a b", ERROR_SUCCESS, L"\\\\s\\DavWWWRoot\\a b" },
    { L"http://s/a%20b", ERROR_SUCCESS, L"\\\\s\\DavWWWRoot\\a b" },
    { L"http://s/a#b", ERROR_SUCCESS, L"\\\\s\\DavWWWRoot\\a" },
    { L"http://s/a?b", ERROR_SUCCESS, L"\\\\s\\DavWWWRoot\\a" },
    { L"http://s.s/x", ERROR_SUCCESS, L"\\\\s.s\\DavWWWRoot\\x" },
    { L"http://se rver/x", ERROR_SUCCESS, L"\\\\se rver\\DavWWWRoot\\x" },
    { L"http://1.2.3.4/x", ERROR_SUCCESS, L"\\\\1.2.3.4\\DavWWWRoot\\x" },
    { L"http://s/DavWWWRoot/x", ERROR_SUCCESS, L"\\\\s\\DavWWWRoot\\DavWWWRoot\\x" },
    { L"http://s:8080/a/", ERROR_SUCCESS, L"\\\\s@8080\\DavWWWRoot\\a" },
    { L"http://s:0/x", ERROR_SUCCESS, L"\\\\s@0\\DavWWWRoot\\x" },
    { L"http://s:65535/x", ERROR_SUCCESS, L"\\\\s@65535\\DavWWWRoot\\x" },
    { L"http://s:080/x", ERROR_SUCCESS, L"\\\\s\\DavWWWRoot\\x" },
};

static void RunCases(PFN_Convert pConvert, PCSTR Name, const CASE *Cases, UINT Count)
{
    WCHAR Buffer[300];
    DWORD Length, Error;
    UINT i;

    for (i = 0; i < Count; i++)
    {
        Length = _countof(Buffer);
        Buffer[0] = L'x';
        Error = pConvert(Cases[i].Input, Buffer, &Length);
        ok(Error == Cases[i].Error, "%s(%ls): error %lu, expected %lu\n", Name, Cases[i].Input, Error, Cases[i].Error);
        if (Error == ERROR_SUCCESS && Cases[i].Error == ERROR_SUCCESS)
        {
            ok(!wcscmp(Buffer, Cases[i].Output), "%s(%ls): got '%ls', expected '%ls'\n",
               Name, Cases[i].Input, Buffer, Cases[i].Output);
            ok(Length == wcslen(Cases[i].Output) + 1, "%s(%ls): length %lu\n", Name, Cases[i].Input, Length);
        }
    }
}

START_TEST(DavPaths)
{
    HMODULE Netapi32 = LoadLibraryW(L"netapi32.dll");
    PFN_Convert pHttpFromUnc = (PFN_Convert)GetProcAddress(Netapi32, "DavGetHTTPFromUNCPath");
    PFN_Convert pUncFromHttp = (PFN_Convert)GetProcAddress(Netapi32, "DavGetUNCFromHTTPPath");
    WCHAR Buffer[300];
    DWORD Length, Error;

    ok(pHttpFromUnc != NULL, "DavGetHTTPFromUNCPath is not exported by netapi32.dll\n");
    ok(pUncFromHttp != NULL, "DavGetUNCFromHTTPPath is not exported by netapi32.dll\n");
    if (!pHttpFromUnc || !pUncFromHttp)
    {
        skip("The WebDAV path functions are not exported\n");
        return;
    }

    RunCases(pHttpFromUnc, "DavGetHTTPFromUNCPath", g_UncToHttp, _countof(g_UncToHttp));
    RunCases(pUncFromHttp, "DavGetUNCFromHTTPPath", g_HttpToUnc, _countof(g_HttpToUnc));

    /* Buffer sizes: the needed length counts the terminator and comes back with ERROR_INSUFFICIENT_BUFFER */
    Length = 0;
    Error = pHttpFromUnc(L"\\\\server\\a", Buffer, &Length);
    ok(Error == ERROR_INSUFFICIENT_BUFFER, "Error is %lu\n", Error);
    ok(Length == 16, "Length is %lu\n", Length);
    Length = 15;
    Error = pHttpFromUnc(L"\\\\server\\a", Buffer, &Length);
    ok(Error == ERROR_INSUFFICIENT_BUFFER, "Error is %lu\n", Error);
    ok(Length == 16, "Length is %lu\n", Length);
    Length = 16;
    Error = pHttpFromUnc(L"\\\\server\\a", Buffer, &Length);
    ok(Error == ERROR_SUCCESS, "Error is %lu\n", Error);
    ok(Length == 16, "Length is %lu\n", Length);
    Length = 300;
    Error = pHttpFromUnc(L"\\\\server\\a", NULL, &Length);
    ok(Error == ERROR_INVALID_PARAMETER, "Error is %lu\n", Error);

    Length = 0;
    Error = pUncFromHttp(L"http://server/share/dir", Buffer, &Length);
    ok(Error == ERROR_INSUFFICIENT_BUFFER, "Error is %lu\n", Error);
    ok(Length == 30, "Length is %lu\n", Length);
}
