/*
 * PROJECT:     ReactOS-Nitro Optional Features / Addon Manager (FEATMGMT-01)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     featmgr.exe - CLI for automation (/list /query /enable /disable).
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#include "featmgr.h"
#include <stdio.h>

/* Write a wide string to stdout as UTF-8 so file-based capture is readable. */
static void Out(LPCWSTR fmt, ...)
{
    WCHAR wbuf[2048];
    char abuf[4096];
    int n;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(wbuf, 2047, fmt, ap);
    va_end(ap);
    wbuf[2047] = L'\0';
    n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, abuf, sizeof(abuf), NULL, NULL);
    if (n > 0) fwrite(abuf, 1, n - 1, stdout);
    fflush(stdout);
}

static void Usage(void)
{
    Out(L"featmgr - ReactOS-Nitro Optional Features manager (FEATMGMT-01)\n\n");
    Out(L"Usage:\n");
    Out(L"  featmgr /list                list all catalog features + state\n");
    Out(L"  featmgr /query   <id>        show one feature in detail\n");
    Out(L"  featmgr /enable  <id>        enable (install) an optional feature\n");
    Out(L"  featmgr /disable <id>        disable (revert) an optional feature\n");
    Out(L"  featmgr /install <id>        alias for /enable\n");
    Out(L"  featmgr /uninstall <id>      alias for /disable\n");
    Out(L"  featmgr /help                this text\n");
}

static int DoList(void)
{
    FM_FEATURE *arr;
    int n, i;
    n = FmEnumFeatures(&arr);
    Out(L"%-18s %-9s %-13s %-8s %s\n", L"ID", L"TIER", L"STATE", L"VERSION", L"NAME");
    Out(L"%-18s %-9s %-13s %-8s %s\n", L"------------------", L"---------",
        L"-------------", L"--------", L"----");
    for (i = 0; i < n; i++)
    {
        WCHAR state[32];
        _snwprintf(state, 31, L"%s%s", FmStateStr(arr[i].State),
                   arr[i].PayloadAvailable ? L"" : L"*");
        Out(L"%-18s %-9s %-13s %-8s %s\n",
            arr[i].Id, FmTierStr(arr[i].Tier), state, arr[i].Version, arr[i].Name);
    }
    Out(L"\n%d feature(s). '*' = payload not staged (catalog entry only).\n", n);
    FmFree(arr);
    return 0;
}

static int DoQuery(LPCWSTR id)
{
    FM_FEATURE f;
    if (!FmFindFeature(id, &f))
    {
        Out(L"error: unknown feature '%s'\n", id);
        return 1;
    }
    Out(L"Id:           %s\n", f.Id);
    Out(L"Name:         %s\n", f.Name);
    Out(L"Tier:         %s\n", FmTierStr(f.Tier));
    Out(L"Version:      %s\n", f.Version);
    Out(L"State:        %s\n", FmStateStr(f.State));
    Out(L"Payload:      %s\n", f.PayloadAvailable ? L"staged" : L"NOT staged (deferred)");
    Out(L"Dependencies: %s\n", f.Dependencies[0] ? f.Dependencies : L"(none)");
    Out(L"Manifest:     %s\n", f.ManifestPath);
    Out(L"Description:  %s\n", f.Description);
    return 0;
}

int wmain(int argc, WCHAR **argv)
{
    LPCWSTR cmd;
    HRESULT hr;

    if (argc < 2) { Usage(); return 0; }
    cmd = argv[1];

    if (!_wcsicmp(cmd, L"/list") || !_wcsicmp(cmd, L"list"))
        return DoList();

    if (!_wcsicmp(cmd, L"/help") || !_wcsicmp(cmd, L"/?") || !_wcsicmp(cmd, L"help"))
    { Usage(); return 0; }

    if (argc < 3) { Out(L"error: missing feature id\n\n"); Usage(); return 2; }

    if (!_wcsicmp(cmd, L"/query") || !_wcsicmp(cmd, L"query"))
        return DoQuery(argv[2]);

    if (!_wcsicmp(cmd, L"/enable") || !_wcsicmp(cmd, L"/install")
        || !_wcsicmp(cmd, L"enable") || !_wcsicmp(cmd, L"install"))
    {
        hr = FmEnable(argv[2], stdout);
        return SUCCEEDED(hr) ? 0 : 3;
    }

    if (!_wcsicmp(cmd, L"/disable") || !_wcsicmp(cmd, L"/uninstall")
        || !_wcsicmp(cmd, L"disable") || !_wcsicmp(cmd, L"uninstall"))
    {
        hr = FmDisable(argv[2], stdout);
        return SUCCEEDED(hr) ? 0 : 3;
    }

    Out(L"error: unknown command '%s'\n\n", cmd);
    Usage();
    return 2;
}
