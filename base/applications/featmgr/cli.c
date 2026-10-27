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
    Out(L"  featmgr /list [/state:<Enabled|Disabled|NotInstalled>] [/tier:<os|software>]\n");
    Out(L"                               list catalog features + state (optionally filtered)\n");
    Out(L"  featmgr /state   <id>        print just the state word (scripting)\n");
    Out(L"  featmgr /query   <id>        show one feature in detail\n");
    Out(L"  featmgr /enable  <id>        enable (install) an optional feature\n");
    Out(L"  featmgr /disable <id> [/cascade] [/force]  disable (revert); refused while enabled\n");
    Out(L"                               features depend on it unless /cascade. Software tier:\n");
    Out(L"                               runs the app's uninstaller; /force = proceed if it fails\n");
    Out(L"  featmgr /install <id>        alias for /enable (software tier: download + run installer)\n");
    Out(L"  featmgr /uninstall <id> [/force]  disable, then forget the record (-> NotInstalled)\n");
    Out(L"  featmgr /import-rapps [<appdb-dir>] [/clean] [/anyarch]\n");
    Out(L"                               convert the RAPPS database (default %%LOCALAPPDATA%%\\RApps\\appdb)\n");
    Out(L"                               into Tier=software catalog entries (Catalog\\Software)\n");
    Out(L"  featmgr /help                this text\n");
}

static int DoList(int argc, WCHAR **argv)
{
    FM_FEATURE *arr;
    int n, i, shown = 0, a;
    int fState = -1, fTier = -1;

    for (a = 2; a < argc; a++)
    {
        if (!_wcsnicmp(argv[a], L"/state:", 7))
        {
            LPCWSTR v = argv[a] + 7;
            if (!_wcsicmp(v, L"Enabled")) fState = FM_STATE_ENABLED;
            else if (!_wcsicmp(v, L"Disabled")) fState = FM_STATE_DISABLED;
            else if (!_wcsicmp(v, L"NotInstalled")) fState = FM_STATE_NOT_INSTALLED;
            else { Out(L"error: bad state filter '%s'\n", v); return 2; }
        }
        else if (!_wcsnicmp(argv[a], L"/tier:", 6))
        {
            LPCWSTR v = argv[a] + 6;
            if (!_wcsicmp(v, L"os")) fTier = FM_TIER_OS;
            else if (!_wcsicmp(v, L"software")) fTier = FM_TIER_SOFTWARE;
            else { Out(L"error: bad tier filter '%s'\n", v); return 2; }
        }
        else { Out(L"error: unknown /list option '%s'\n", argv[a]); return 2; }
    }
    n = FmEnumFeatures(&arr);
    Out(L"%-18s %-9s %-13s %-8s %s\n", L"ID", L"TIER", L"STATE", L"VERSION", L"NAME");
    Out(L"%-18s %-9s %-13s %-8s %s\n", L"------------------", L"---------",
        L"-------------", L"--------", L"----");
    for (i = 0; i < n; i++)
    {
        WCHAR state[32];
        if (fState >= 0 && (int)arr[i].State != fState) continue;
        if (fTier >= 0 && (int)arr[i].Tier != fTier) continue;
        shown++;
        _snwprintf(state, 31, L"%s%s", FmStateStr(arr[i].State),
                   arr[i].PayloadAvailable ? L"" : L"*");
        Out(L"%-18s %-9s %-13s %-8s %s\n",
            arr[i].Id, FmTierStr(arr[i].Tier), state, arr[i].Version, arr[i].Name);
    }
    Out(L"\n%d feature(s). '*' = payload not staged (catalog entry only).\n", shown);
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
    if (f.InstallDate[0]) Out(L"InstallDate:  %s\n", f.InstallDate);
    if (f.PendingReboot) Out(L"PendingReboot: Yes (removal completes at next reboot)\n");
    {
        WCHAR dep[FM_MAX_DEPS * 2];
        FmGetDependents(f.Id, dep, ARRAYSIZE(dep));
        Out(L"EnabledDependents: %s\n", dep[0] ? dep : L"(none)");
    }
    Out(L"Payload:      %s\n", f.PayloadAvailable ? L"staged" : L"NOT staged (deferred)");
    Out(L"Dependencies: %s\n", f.Dependencies[0] ? f.Dependencies : L"(none)");
    Out(L"Manifest:     %s\n", f.ManifestPath);
    Out(L"Description:  %s\n", f.Description);
    {
        WCHAR v[512];
        static const WCHAR *k[] = { L"Artifact", L"ArtifactSHA256", L"ArtifactSHA1", L"SizeBytes",
                                    L"License", L"Publisher", L"URLSite" };
        int i;
        if (f.Provider[0]) Out(L"Provider:     %s\n", f.Provider);
        GetPrivateProfileStringW(L"Rapps", L"Pkg", L"", v, 512, f.ManifestPath);
        if (v[0]) Out(L"Rapps.Pkg:    %s\n", v);
        GetPrivateProfileStringW(L"Rapps", L"Category", L"", v, 512, f.ManifestPath);
        if (v[0]) Out(L"Category:     %s\n", v);
        for (i = 0; i < 7; i++)
        {
            GetPrivateProfileStringW(L"Source", k[i], L"", v, 512, f.ManifestPath);
            if (v[0]) Out(L"Source.%s: %s\n", k[i], v);
        }
    }
    return 0;
}

static int DoImportRapps(int argc, WCHAR **argv)
{
    LPCWSTR dir = NULL;
    DWORD fl = 0;
    int a, n, skipped = 0;

    for (a = 2; a < argc; a++)
    {
        if (!_wcsicmp(argv[a], L"/clean")) fl |= FM_IMPORT_CLEAN;
        else if (!_wcsicmp(argv[a], L"/anyarch")) fl |= FM_IMPORT_ANYARCH;
        else if (argv[a][0] != L'/' && !dir) dir = argv[a];
        else { Out(L"error: unknown /import-rapps option '%s'\n", argv[a]); return 2; }
    }
    n = FmImportRapps(dir, fl, &skipped, stdout);
    return n < 0 ? 3 : 0;
}

/* flags after the feature id: /cascade /force */
static DWORD ParseDisableFlags(int argc, WCHAR **argv, int from)
{
    DWORD fl = 0;
    int a;
    for (a = from; a < argc; a++)
    {
        if (!_wcsicmp(argv[a], L"/cascade")) fl |= FM_DISABLE_CASCADE;
        else if (!_wcsicmp(argv[a], L"/force")) fl |= FM_DISABLE_FORCE;
    }
    return fl;
}

int wmain(int argc, WCHAR **argv)
{
    LPCWSTR cmd;
    HRESULT hr;

    if (argc < 2) { Usage(); return 0; }
    cmd = argv[1];

    if (!_wcsicmp(cmd, L"/list") || !_wcsicmp(cmd, L"list"))
        return DoList(argc, argv);

    if (!_wcsicmp(cmd, L"/help") || !_wcsicmp(cmd, L"/?") || !_wcsicmp(cmd, L"help"))
    { Usage(); return 0; }

    if (!_wcsicmp(cmd, L"/import-rapps") || !_wcsicmp(cmd, L"import-rapps"))
        return DoImportRapps(argc, argv);

    if (argc < 3) { Out(L"error: missing feature id\n\n"); Usage(); return 2; }

    if (!_wcsicmp(cmd, L"/query") || !_wcsicmp(cmd, L"query"))
        return DoQuery(argv[2]);

    if (!_wcsicmp(cmd, L"/state") || !_wcsicmp(cmd, L"state"))
    {
        FM_FEATURE f;
        if (!FmFindFeature(argv[2], &f)) { Out(L"Unknown\n"); return 1; }
        Out(L"%s\n", FmStateStr(f.State));
        return 0;
    }

    if (!_wcsicmp(cmd, L"/enable") || !_wcsicmp(cmd, L"/install")
        || !_wcsicmp(cmd, L"enable") || !_wcsicmp(cmd, L"install"))
    {
        hr = FmEnable(argv[2], stdout);
        return SUCCEEDED(hr) ? 0 : 3;
    }

    if (!_wcsicmp(cmd, L"/disable") || !_wcsicmp(cmd, L"disable"))
    {
        hr = FmDisableEx(argv[2], ParseDisableFlags(argc, argv, 3), stdout);
        return SUCCEEDED(hr) ? 0 : 3;
    }

    if (!_wcsicmp(cmd, L"/uninstall") || !_wcsicmp(cmd, L"uninstall"))
    {
        hr = FmUninstallEx(argv[2], ParseDisableFlags(argc, argv, 3), stdout);
        return SUCCEEDED(hr) ? 0 : 3;
    }

    Out(L"error: unknown command '%s'\n\n", cmd);
    Usage();
    return 2;
}
