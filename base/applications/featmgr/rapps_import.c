/*
 * PROJECT:     ReactOS-Nitro Optional Features / Addon Manager (FEATMGMT-01)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     RAPPS database -> featmgr software-tier catalog converter.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * Field mapping (RAPPS appdb\<pkg>.txt  ->  Catalog\Software\rapps-<pkg>.ini):
 *
 *   RAPPS [Section] key        featmgr manifest
 *   -------------------------  ------------------------------------------------
 *   <file name>.txt            [Feature] Id=rapps-<pkg>   [Rapps] Pkg=<pkg>
 *   Name                       [Feature] Name
 *   Description                [Feature] Description
 *   Version                    [Feature] Version
 *   (fixed)                    [Feature] Tier=software
 *   Dependencies (a|b*x64)     [Feature] Dependencies=rapps-a (only those that
 *                              exist in the imported DB); raw kept in [Rapps]
 *   URLDownload                [Source] Artifact
 *   SHA1                       [Source] ArtifactSHA1   (RAPPS integrity is SHA1)
 *   SizeBytes                  [Source] SizeBytes
 *   License / LicenseType      [Source] License  (Open Source|Freeware|Trial text
 *                              + RAPPS license string, same text RAPPS shows)
 *   Publisher, URLSite         [Source] Publisher, URLSite
 *   Category (1..16)           [Rapps] Category=<name>   (numeric kept in CategoryId)
 *   RegName, Installer,        [Rapps] RegName, Installer, SilentParameters,
 *   SilentParameters, NTVersion,   NTVersion, Scope, SaveAs, LicenseType
 *   Scope, SaveAs              (carried verbatim; rapps.exe remains the executor)
 *   (fixed)                    [Install] Provider=rapps  -> featmgr delegates the
 *                              download + SHA1 check + installer run to rapps.exe
 *
 * Section selection follows RAPPS: [Section.<arch>] overrides [Section]; on
 * non-x86 the generic URLDownload is ignored (RAPPS refuses 32-bit installers
 * there) unless FM_IMPORT_ANYARCH. Locale sections ([Section.0419]) are NOT
 * imported: manifests carry the English/neutral text. Entries without Name or
 * a usable URLDownload are skipped. Nothing is written into the image: the
 * output is a regenerable cache next to the state the user already has.
 */

#include "featmgr.h"
#include <shlwapi.h>
#include <shlobj.h>
#include <strsafe.h>

#define LOGF(log, ...) do { if (log) { fwprintf((log), __VA_ARGS__); fflush(log); } } while (0)

#if defined(_M_IX86)
#define FM_ARCH L"x86"
#elif defined(_M_AMD64)
#define FM_ARCH L"amd64"
#elif defined(_M_ARM64)
#define FM_ARCH L"arm64"
#elif defined(_M_ARM)
#define FM_ARCH L"arm"
#else
#define FM_ARCH L"unknown"
#endif

static const WCHAR NOTFOUND[] = L"\x01";

static const LPCWSTR g_Categories[] = {
    L"Other", L"Audio", L"Video", L"Graphics", L"Games", L"Internet", L"Office",
    L"Development", L"Edutainment", L"Engineering", L"Finance", L"Science",
    L"Tools", L"Drivers", L"Libraries", L"Themes", L"Other"
};

/* Read key from [Section.<arch>] then [Section]. FALSE if absent or empty. */
static BOOL GetKey(LPCWSTR file, LPCWSTR key, LPWSTR out, DWORD cch, DWORD flags)
{
    DWORD n;

    n = GetPrivateProfileStringW(L"Section." FM_ARCH, key, NOTFOUND, out, cch, file);
    if (out[0] && out[0] != 1) return TRUE;

#ifndef _M_IX86
    if (!(flags & FM_IMPORT_ANYARCH) && !_wcsicmp(key, L"URLDownload"))
    {
        out[0] = L'\0';
        return FALSE;
    }
#endif
    n = GetPrivateProfileStringW(L"Section", key, NOTFOUND, out, cch, file);
    (void)n;
    if (out[0] && out[0] != 1) return TRUE;
    out[0] = L'\0';
    return FALSE;
}

/* A package name becomes part of an id, a file name and a command line. */
static BOOL ValidPkgName(LPCWSTR s)
{
    size_t n = wcslen(s), i;
    if (!n || n > FM_MAX_ID - 8) return FALSE;      /* room for the "rapps-" prefix */
    for (i = 0; i < n; i++)
    {
        WCHAR c = s[i];
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')
              || c == L'-' || c == L'_' || c == L'.' || c == L'+'))
            return FALSE;
    }
    return s[0] != L'.';
}

/* Make a value safe for a one-line "key=value" INI entry. */
static void Flatten(LPWSTR s)
{
    for (; *s; s++)
        if (*s == L'\r' || *s == L'\n' || *s == L'\t') *s = L' ';
}

typedef struct _OUTBUF { WCHAR *buf; size_t cap, len; BOOL fail; } OUTBUF;

static void OutLine(OUTBUF *o, LPCWSTR key, LPCWSTR val)
{
    size_t need = wcslen(key) + wcslen(val) + 4;
    if (o->fail) return;
    if (o->len + need >= o->cap)
    {
        size_t nc = (o->cap + need) * 2;
        WCHAR *nb = o->buf ? (WCHAR *)HeapReAlloc(GetProcessHeap(), 0, o->buf, nc * sizeof(WCHAR))
                           : (WCHAR *)HeapAlloc(GetProcessHeap(), 0, nc * sizeof(WCHAR));
        if (!nb) { o->fail = TRUE; return; }
        o->buf = nb; o->cap = nc;
    }
    if (key[0])
        StringCchPrintfW(o->buf + o->len, o->cap - o->len, L"%s=%s\r\n", key, val);
    else
        StringCchPrintfW(o->buf + o->len, o->cap - o->len, L"%s\r\n", val);   /* raw line (section header) */
    o->len += wcslen(o->buf + o->len);
}

/* Emit "key=<value of RAPPS key>" only when RAPPS has the key. */
static void CopyKey(OUTBUF *o, LPCWSTR file, LPCWSTR rappsKey, LPCWSTR outKey, DWORD flags)
{
    WCHAR v[1024];
    if (GetKey(file, rappsKey, v, ARRAYSIZE(v), flags))
    {
        Flatten(v);
        OutLine(o, outKey, v);
    }
}

/* Same text RAPPS shows: license class word + free-text name. */
static void BuildLicense(LPCWSTR file, LPWSTR out, DWORD cch, DWORD flags)
{
    WCHAR lic[256], lt[16];
    int type = 0;
    LPCWSTR cls = NULL;

    lic[0] = lt[0] = L'\0';
    GetKey(file, L"License", lic, ARRAYSIZE(lic), flags);
    if (GetKey(file, L"LicenseType", lt, ARRAYSIZE(lt), flags)) type = _wtoi(lt);
    if (type == 0 && !_wcsicmp(lic, L"Freeware")) { type = 2; lic[0] = L'\0'; }
    if (type == 2 && !_wcsicmp(lic, L"Freeware")) lic[0] = L'\0';
    switch (type)
    {
        case 1: cls = L"Open Source"; break;
        case 2: cls = L"Freeware"; break;
        case 3: cls = L"Trial"; break;
    }
    Flatten(lic);
    if (cls && lic[0]) StringCchPrintfW(out, cch, L"%s (%s)", cls, lic);
    else if (cls)      StringCchCopyW(out, cch, cls);
    else               StringCchCopyW(out, cch, lic);
}

/* RAPPS "a|b*x64|c" -> "rapps-a,rapps-c" for entries present in srcDir. */
static void MapDependencies(LPCWSTR srcDir, LPCWSTR raw, LPWSTR out, DWORD cch)
{
    WCHAR tmp[512], path[MAX_PATH];
    WCHAR *ctx = NULL, *d;

    out[0] = L'\0';
    StringCchCopyW(tmp, ARRAYSIZE(tmp), raw);
    for (d = wcstok_s(tmp, L"|", &ctx); d; d = wcstok_s(NULL, L"|", &ctx))
    {
        WCHAR *e;
        while (*d == L' ') d++;
        e = d + wcscspn(d, L"*<=>:/\\?");        /* strip RAPPS version/arch modifiers */
        while (e > d && e[-1] == L' ') e--;
        *e = L'\0';
        if (!ValidPkgName(d)) continue;
        StringCchPrintfW(path, MAX_PATH, L"%s\\%s.txt", srcDir, d);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;
        if (out[0]) StringCchCatW(out, cch, L",");
        StringCchCatW(out, cch, L"rapps-");
        StringCchCatW(out, cch, d);
    }
}

static BOOL WriteManifest(LPCWSTR path, const OUTBUF *o)
{
    HANDLE h;
    DWORD w;
    WORD bom = 0xFEFF;     /* UTF-16LE + BOM: GetPrivateProfile* detects it, keeps non-ASCII names intact */
    BOOL ok;

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(h, &bom, sizeof(bom), &w, NULL)
      && WriteFile(h, o->buf, (DWORD)(o->len * sizeof(WCHAR)), &w, NULL);
    CloseHandle(h);
    return ok;
}

static BOOL DefaultRappsDir(LPWSTR out, DWORD cch)
{
    WCHAR base[MAX_PATH];
    if (!SHGetSpecialFolderPathW(NULL, base, CSIDL_LOCAL_APPDATA, FALSE)) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\RApps\\appdb", base));
}

int FmImportRapps(LPCWSTR srcDir, DWORD flags, int *skipped, FILE *log)
{
    WCHAR src[MAX_PATH], outDir[MAX_PATH], glob[MAX_PATH], file[MAX_PATH], outFile[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int written = 0, skip = 0;

    if (skipped) *skipped = 0;
    if (srcDir && srcDir[0])
        StringCchCopyW(src, MAX_PATH, srcDir);
    else if (!DefaultRappsDir(src, MAX_PATH))
    {
        LOGF(log, L"error: cannot locate the RAPPS database directory\n");
        return -1;
    }
    PathRemoveBackslashW(src);
    if (!FmGetSoftwareCatalogDir(outDir, MAX_PATH)) return -1;
    SHCreateDirectoryExW(NULL, outDir, NULL);

    if (flags & FM_IMPORT_CLEAN)
    {
        StringCchPrintfW(glob, MAX_PATH, L"%s\\rapps-*.ini", outDir);
        h = FindFirstFileW(glob, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do {
                StringCchPrintfW(file, MAX_PATH, L"%s\\%s", outDir, fd.cFileName);
                DeleteFileW(file);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    StringCchPrintfW(glob, MAX_PATH, L"%s\\*.txt", src);
    h = FindFirstFileW(glob, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        LOGF(log, L"error: no RAPPS entries (*.txt) in '%s'. Run rapps once to fetch its "
                  L"database, or pass the directory.\n", src);
        return -1;
    }
    do
    {
        WCHAR pkg[MAX_PATH], name[FM_MAX_NAME], url[1024], v[1024], cat[16], id[FM_MAX_ID];
        OUTBUF o = { NULL, 0, 0, FALSE };
        int catId;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        StringCchCopyW(pkg, MAX_PATH, fd.cFileName);
        PathRemoveExtensionW(pkg);
        if (!ValidPkgName(pkg)) { skip++; LOGF(log, L"skip: '%s' (unusable package name)\n", fd.cFileName); continue; }
        StringCchPrintfW(file, MAX_PATH, L"%s\\%s", src, fd.cFileName);

        if (!GetKey(file, L"Name", name, ARRAYSIZE(name), flags)
            || !GetKey(file, L"URLDownload", url, ARRAYSIZE(url), flags))
        {
            skip++;
            LOGF(log, L"skip: %s (no Name or no usable URLDownload for %s)\n", pkg, FM_ARCH);
            continue;
        }
        Flatten(name); Flatten(url);
        StringCchPrintfW(id, FM_MAX_ID, L"rapps-%s", pkg);

        OutLine(&o, L"", L"; Generated by featmgr /import-rapps from the RAPPS database. Do not edit;");
        OutLine(&o, L"", L"; regenerate with: featmgr /import-rapps");
        OutLine(&o, L"", L"[Feature]");
        OutLine(&o, L"Id", id);
        OutLine(&o, L"Name", name);
        CopyKey(&o, file, L"Description", L"Description", flags);
        if (GetKey(file, L"Version", v, ARRAYSIZE(v), flags)) { Flatten(v); OutLine(&o, L"Version", v); }
        OutLine(&o, L"Tier", L"software");
        if (GetKey(file, L"Dependencies", v, ARRAYSIZE(v), flags))
        {
            WCHAR deps[512];
            MapDependencies(src, v, deps, ARRAYSIZE(deps));
            OutLine(&o, L"Dependencies", deps);
        }
        else
            OutLine(&o, L"Dependencies", L"");

        OutLine(&o, L"", L"");
        OutLine(&o, L"", L"[Install]");
        OutLine(&o, L"Provider", L"rapps");

        OutLine(&o, L"", L"");
        OutLine(&o, L"", L"[Source]");
        OutLine(&o, L"Artifact", url);
        CopyKey(&o, file, L"SHA1", L"ArtifactSHA1", flags);
        CopyKey(&o, file, L"SizeBytes", L"SizeBytes", flags);
        BuildLicense(file, v, ARRAYSIZE(v), flags);
        if (v[0]) OutLine(&o, L"License", v);
        CopyKey(&o, file, L"Publisher", L"Publisher", flags);
        CopyKey(&o, file, L"URLSite", L"URLSite", flags);

        OutLine(&o, L"", L"");
        OutLine(&o, L"", L"[Rapps]");
        OutLine(&o, L"Pkg", pkg);
        catId = 0;
        if (GetKey(file, L"Category", cat, ARRAYSIZE(cat), flags)) catId = _wtoi(cat);
        if (catId < 0 || catId >= (int)ARRAYSIZE(g_Categories)) catId = 0;
        StringCchPrintfW(cat, ARRAYSIZE(cat), L"%d", catId);
        OutLine(&o, L"CategoryId", cat);
        OutLine(&o, L"Category", g_Categories[catId]);
        CopyKey(&o, file, L"RegName", L"RegName", flags);
        CopyKey(&o, file, L"Installer", L"Installer", flags);
        CopyKey(&o, file, L"SilentParameters", L"SilentParameters", flags);
        CopyKey(&o, file, L"NTVersion", L"NTVersion", flags);
        CopyKey(&o, file, L"Scope", L"Scope", flags);
        CopyKey(&o, file, L"SaveAs", L"SaveAs", flags);
        CopyKey(&o, file, L"LicenseType", L"LicenseType", flags);
        CopyKey(&o, file, L"Dependencies", L"Dependencies", flags);

        StringCchPrintfW(outFile, MAX_PATH, L"%s\\%s.ini", outDir, id);
        if (!o.fail && WriteManifest(outFile, &o))
            written++;
        else
        {
            skip++;
            LOGF(log, L"skip: %s (cannot write %s, %lu)\n", pkg, outFile, GetLastError());
        }
        if (o.buf) HeapFree(GetProcessHeap(), 0, o.buf);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (skipped) *skipped = skip;
    LOGF(log, L"imported %d RAPPS entr%s into %s (%d skipped)\n", written,
         written == 1 ? L"y" : L"ies", outDir, skip);
    return written;
}
