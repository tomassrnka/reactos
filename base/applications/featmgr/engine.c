/*
 * PROJECT:     ReactOS-Nitro Optional Features / Addon Manager (FEATMGMT-01)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Catalog engine + reversible enable/disable + registry state.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#include "featmgr.h"
#include <shlwapi.h>
#include <shlobj.h>
#include <strsafe.h>

/* Persisted feature state lives here (DISM /Get-Features-like, queryable). */
static const WCHAR STATE_KEY[] =
    L"SOFTWARE\\ReactOS\\Nitro\\OptionalFeatures";

#define LOGF(log, ...) do { if (log) { fwprintf((log), __VA_ARGS__); fflush(log); } } while (0)

/* ---- layout helpers ------------------------------------------------------ */

BOOL FmGetRootDir(LPWSTR buf, DWORD cch)
{
    WCHAR win[MAX_PATH];
    if (!GetWindowsDirectoryW(win, MAX_PATH))
        return FALSE;
    return SUCCEEDED(StringCchPrintfW(buf, cch, L"%s\\Featmgr", win));
}

BOOL FmGetCatalogDir(LPWSTR buf, DWORD cch)
{
    WCHAR root[MAX_PATH];
    if (!FmGetRootDir(root, MAX_PATH))
        return FALSE;
    return SUCCEEDED(StringCchPrintfW(buf, cch, L"%s\\Catalog", root));
}

BOOL FmGetPayloadDir(LPCWSTR id, LPWSTR buf, DWORD cch)
{
    WCHAR root[MAX_PATH];
    if (!FmGetRootDir(root, MAX_PATH))
        return FALSE;
    return SUCCEEDED(StringCchPrintfW(buf, cch, L"%s\\Payload\\%s", root, id));
}

LPCWSTR FmTierStr(FM_TIER t)
{
    return (t == FM_TIER_SOFTWARE) ? L"software" : L"os";
}

LPCWSTR FmStateStr(FM_STATE s)
{
    switch (s)
    {
        case FM_STATE_ENABLED:  return L"Enabled";
        case FM_STATE_DISABLED: return L"Disabled";
        default:                return L"NotInstalled";
    }
}

/* ---- registry state ------------------------------------------------------ */

static LONG OpenStateKey(LPCWSTR id, BOOL create, REGSAM sam, HKEY *out)
{
    WCHAR sub[512];
    StringCchPrintfW(sub, 512, L"%s\\%s", STATE_KEY, id);
    if (create)
        return RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub, 0, NULL, 0,
                               sam, NULL, out, NULL);
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, sam, out);
}

FM_STATE FmGetState(LPCWSTR id)
{
    HKEY k;
    WCHAR v[32];
    DWORD cb = sizeof(v), type = 0;
    FM_STATE s = FM_STATE_NOT_INSTALLED;

    if (OpenStateKey(id, FALSE, KEY_READ, &k) != ERROR_SUCCESS)
        return FM_STATE_NOT_INSTALLED;
    if (RegQueryValueExW(k, L"State", NULL, &type, (LPBYTE)v, &cb) == ERROR_SUCCESS
        && type == REG_SZ)
    {
        if (!_wcsicmp(v, L"Enabled"))  s = FM_STATE_ENABLED;
        else if (!_wcsicmp(v, L"Disabled")) s = FM_STATE_DISABLED;
    }
    RegCloseKey(k);
    return s;
}

static void SetStateSz(HKEY k, LPCWSTR name, LPCWSTR val)
{
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val,
                   (DWORD)((wcslen(val) + 1) * sizeof(WCHAR)));
}

/* UTC ISO-8601 timestamp */
static void UtcNow(LPWSTR buf, DWORD cch)
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    StringCchPrintfW(buf, cch, L"%04u-%02u-%02uT%02u:%02u:%02uZ",
                     st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond);
}

/* ---- MULTI_SZ helpers (installed-file + undo lists) ---------------------- */

/* Append one line to a REG_MULTI_SZ value. */
static void MultiSzAppend(HKEY k, LPCWSTR name, LPCWSTR line)
{
    DWORD cb = 0, type = 0;
    BYTE *old = NULL;
    BYTE *buf;
    DWORD oldLen = 0, addLen = (DWORD)((wcslen(line) + 1) * sizeof(WCHAR));

    if (RegQueryValueExW(k, name, NULL, &type, NULL, &cb) == ERROR_SUCCESS
        && type == REG_MULTI_SZ && cb >= sizeof(WCHAR))
    {
        old = (BYTE *)HeapAlloc(GetProcessHeap(), 0, cb);
        if (old && RegQueryValueExW(k, name, NULL, &type, old, &cb) == ERROR_SUCCESS)
            oldLen = (cb >= sizeof(WCHAR)) ? cb - sizeof(WCHAR) : 0; /* drop final NUL */
        else
            oldLen = 0;
    }

    buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, oldLen + addLen + sizeof(WCHAR));
    if (buf)
    {
        if (oldLen) memcpy(buf, old, oldLen);
        memcpy(buf + oldLen, line, addLen);
        *(WCHAR *)(buf + oldLen + addLen) = L'\0'; /* terminating empty string */
        RegSetValueExW(k, name, 0, REG_MULTI_SZ, buf, oldLen + addLen + sizeof(WCHAR));
        HeapFree(GetProcessHeap(), 0, buf);
    }
    if (old) HeapFree(GetProcessHeap(), 0, old);
}

/* Read a whole REG_MULTI_SZ; caller frees with HeapFree. NULL if absent. */
static WCHAR *MultiSzRead(HKEY k, LPCWSTR name)
{
    DWORD cb = 0, type = 0;
    WCHAR *buf;
    if (RegQueryValueExW(k, name, NULL, &type, NULL, &cb) != ERROR_SUCCESS
        || type != REG_MULTI_SZ || cb < sizeof(WCHAR))
        return NULL;
    buf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, cb + sizeof(WCHAR));
    if (!buf) return NULL;
    if (RegQueryValueExW(k, name, NULL, &type, (LPBYTE)buf, &cb) != ERROR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, buf);
        return NULL;
    }
    buf[cb / sizeof(WCHAR)] = L'\0';
    return buf;
}

/* ---- INI manifest parsing ------------------------------------------------ */

static FM_TIER ParseTier(LPCWSTR s)
{
    return (!_wcsicmp(s, L"software")) ? FM_TIER_SOFTWARE : FM_TIER_OS;
}

/* Read [Payload]/[Registry] style sections: returns a double-NUL buffer of
 * "Key=Value" lines (as from GetPrivateProfileSection). Caller frees. */
static WCHAR *ReadSection(LPCWSTR file, LPCWSTR section)
{
    DWORD cap = 2048, n;
    WCHAR *buf;
    for (;;)
    {
        buf = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(WCHAR));
        if (!buf) return NULL;
        n = GetPrivateProfileSectionW(section, buf, cap, file);
        if (n < cap - 2) return buf;
        HeapFree(GetProcessHeap(), 0, buf);
        cap *= 2;
        if (cap > 1 << 20) { return NULL; }
    }
}

/* Load the catalog fields of one manifest into *f (does not set State). */
static BOOL LoadManifest(LPCWSTR path, FM_FEATURE *f)
{
    WCHAR tmp[FM_MAX_DESC];

    ZeroMemory(f, sizeof(*f));
    StringCchCopyW(f->ManifestPath, MAX_PATH, path);

    GetPrivateProfileStringW(L"Feature", L"Id", L"", f->Id, FM_MAX_ID, path);
    if (!f->Id[0]) return FALSE;
    GetPrivateProfileStringW(L"Feature", L"Name", f->Id, f->Name, FM_MAX_NAME, path);
    GetPrivateProfileStringW(L"Feature", L"Description", L"", f->Description, FM_MAX_DESC, path);
    GetPrivateProfileStringW(L"Feature", L"Version", L"1.0.0", f->Version, FM_MAX_VER, path);
    GetPrivateProfileStringW(L"Feature", L"Dependencies", L"", f->Dependencies, FM_MAX_DEPS, path);
    GetPrivateProfileStringW(L"Feature", L"Tier", L"os", tmp, FM_MAX_DESC, path);
    f->Tier = ParseTier(tmp);
    return TRUE;
}

/* Does every declared [Payload] source file exist on disk? */
static BOOL PayloadPresent(const FM_FEATURE *f)
{
    WCHAR payloadDir[MAX_PATH], src[MAX_PATH];
    WCHAR *sect, *p, *sep;
    BOOL ok = TRUE;

    sect = ReadSection(f->ManifestPath, L"Payload");
    if (!sect) return TRUE; /* no payload section => registry-only, always available */

    if (!FmGetPayloadDir(f->Id, payloadDir, MAX_PATH)) { HeapFree(GetProcessHeap(),0,sect); return FALSE; }

    for (p = sect; *p; p += wcslen(p) + 1)
    {
        /* line is File<N>=<src>|<dest> ; take src (before '|') after '=' */
        WCHAR *eq = wcschr(p, L'=');
        if (!eq) continue;
        sep = wcschr(eq + 1, L'|');
        if (sep) *sep = L'\0';
        StringCchPrintfW(src, MAX_PATH, L"%s\\%s", payloadDir, eq + 1);
        if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) { ok = FALSE; }
        if (sep) *sep = L'|';
        if (!ok) break;
    }
    HeapFree(GetProcessHeap(), 0, sect);
    return ok;
}

/* ---- enumeration --------------------------------------------------------- */

int FmEnumFeatures(FM_FEATURE **outArr)
{
    WCHAR catDir[MAX_PATH], glob[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    FM_FEATURE *arr = NULL;
    int count = 0, cap = 0;

    *outArr = NULL;
    if (!FmGetCatalogDir(catDir, MAX_PATH)) return 0;
    StringCchPrintfW(glob, MAX_PATH, L"%s\\*.ini", catDir);

    h = FindFirstFileW(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do
    {
        FM_FEATURE f;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        StringCchPrintfW(path, MAX_PATH, L"%s\\%s", catDir, fd.cFileName);
        if (!LoadManifest(path, &f)) continue;
        f.State = FmGetState(f.Id);
        f.PayloadAvailable = PayloadPresent(&f);

        if (count == cap)
        {
            int nc = cap ? cap * 2 : 8;
            FM_FEATURE *na = (FM_FEATURE *)HeapReAlloc(GetProcessHeap(), 0, arr,
                                                       nc * sizeof(FM_FEATURE));
            if (!arr) na = (FM_FEATURE *)HeapAlloc(GetProcessHeap(), 0, nc * sizeof(FM_FEATURE));
            if (!na) break;
            arr = na; cap = nc;
        }
        arr[count++] = f;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    *outArr = arr;
    return count;
}

void FmFree(FM_FEATURE *arr)
{
    if (arr) HeapFree(GetProcessHeap(), 0, arr);
}

BOOL FmFindFeature(LPCWSTR id, FM_FEATURE *out)
{
    FM_FEATURE *arr;
    int n, i;
    BOOL found = FALSE;
    n = FmEnumFeatures(&arr);
    for (i = 0; i < n; i++)
    {
        if (!_wcsicmp(arr[i].Id, id)) { *out = arr[i]; found = TRUE; break; }
    }
    FmFree(arr);
    return found;
}

/* ---- apply / revert ------------------------------------------------------ */

static HKEY HiveFromName(LPCWSTR s)
{
    if (!_wcsicmp(s, L"HKLM")) return HKEY_LOCAL_MACHINE;
    if (!_wcsicmp(s, L"HKCU")) return HKEY_CURRENT_USER;
    if (!_wcsicmp(s, L"HKCR")) return HKEY_CLASSES_ROOT;
    return NULL;
}

/* Copy one payload file; records absolute dest into the state "Files" list.
 * A file that already exists at the destination (e.g. the in-box softpipe
 * libgallium_wgl.dll that the lavapipe pack replaces) is moved aside to
 * <dest>.fmbak first and recorded in "Backups" as "dest|bak", so disable can
 * put the original back instead of just deleting it. */
static HRESULT ApplyPayloadFile(LPCWSTR payloadDir, LPCWSTR srcRel, LPCWSTR destRaw,
                                HKEY state, FILE *log)
{
    WCHAR src[MAX_PATH], dest[MAX_PATH], destDir[MAX_PATH], bak[MAX_PATH + 8], rec[2 * MAX_PATH + 16];
    StringCchPrintfW(src, MAX_PATH, L"%s\\%s", payloadDir, srcRel);
    ExpandEnvironmentStringsW(destRaw, dest, MAX_PATH);

    StringCchCopyW(destDir, MAX_PATH, dest);
    PathRemoveFileSpecW(destDir);
    SHCreateDirectoryExW(NULL, destDir, NULL);

    if (GetFileAttributesW(dest) != INVALID_FILE_ATTRIBUTES)
    {
        StringCchPrintfW(bak, MAX_PATH + 8, L"%s.fmbak", dest);
        DeleteFileW(bak);
        LOGF(log, L"  backup: %s -> %s\n", dest, bak);
        if (!MoveFileW(dest, bak))
        {
            LOGF(log, L"  ERROR: cannot back up existing file (%lu)\n", GetLastError());
            return HRESULT_FROM_WIN32(GetLastError());
        }
        StringCchPrintfW(rec, ARRAYSIZE(rec), L"%s|%s", dest, bak);
        MultiSzAppend(state, L"Backups", rec);
    }

    LOGF(log, L"  file: %s -> %s\n", src, dest);
    MultiSzAppend(state, L"Files", dest);
    if (!CopyFileW(src, dest, FALSE))
    {
        LOGF(log, L"  ERROR: CopyFile failed (%lu)\n", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

/* Set one registry value, recording undo info. Value spec: HIVE|subkey|name|type|data
 * name "@" => default value. type REG_SZ or REG_DWORD. */
static HRESULT ApplyRegAction(LPCWSTR spec, HKEY state, FILE *log)
{
    WCHAR buf[1024];
    WCHAR *tok[5] = {0};
    int ti = 0;
    WCHAR *p, *start;
    HKEY hive, k;
    LPCWSTR valName;
    WCHAR undo[1200];
    WCHAR prev[1024];
    DWORD prevType = 0, prevCb = sizeof(prev);
    LONG r;

    /* expand %VAR% (value names/data such as the Khronos ICD manifest path) */
    if (!ExpandEnvironmentStringsW(spec, buf, 1024))
        StringCchCopyW(buf, 1024, spec);
    /* split on '|' into up to 5 fields (keep empties) */
    start = buf;
    for (p = buf; ; p++)
    {
        if (*p == L'|' || *p == L'\0')
        {
            BOOL end = (*p == L'\0');
            *p = L'\0';
            if (ti < 5) tok[ti++] = start;
            start = p + 1;
            if (end) break;
        }
    }
    if (ti < 4) { LOGF(log, L"  bad reg spec: %s\n", spec); return E_INVALIDARG; }

    hive = HiveFromName(tok[0]);
    if (!hive) { LOGF(log, L"  bad hive: %s\n", tok[0]); return E_INVALIDARG; }
    valName = (!wcscmp(tok[2], L"@")) ? L"" : tok[2];

    if (RegCreateKeyExW(hive, tok[1], 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k, NULL)
        != ERROR_SUCCESS)
    {
        LOGF(log, L"  ERROR: open/create key %s failed\n", tok[1]);
        return E_FAIL;
    }

    /* record undo: existed? save prior; else mark for delete */
    r = RegQueryValueExW(k, valName, NULL, &prevType, (LPBYTE)prev, &prevCb);
    if (r == ERROR_SUCCESS && prevType == REG_SZ)
        StringCchPrintfW(undo, 1200, L"R|%s|%s|%s|REG_SZ|%s", tok[0], tok[1], tok[2], prev);
    else if (r == ERROR_SUCCESS && prevType == REG_DWORD)
        StringCchPrintfW(undo, 1200, L"R|%s|%s|%s|REG_DWORD|%lu", tok[0], tok[1], tok[2], *(DWORD*)prev);
    else
        StringCchPrintfW(undo, 1200, L"D|%s|%s|%s", tok[0], tok[1], tok[2]);
    MultiSzAppend(state, L"RegUndo", undo);

    LOGF(log, L"  reg: %s\\%s [%s] = %s\n", tok[0], tok[1], tok[2], tok[4] ? tok[4] : L"");
    if (!_wcsicmp(tok[3], L"REG_DWORD"))
    {
        DWORD d = (DWORD)_wtoi(tok[4] ? tok[4] : L"0");
        RegSetValueExW(k, valName, 0, REG_DWORD, (const BYTE *)&d, sizeof(d));
    }
    else
    {
        LPCWSTR data = tok[4] ? tok[4] : L"";
        RegSetValueExW(k, valName, 0, REG_SZ, (const BYTE *)data,
                       (DWORD)((wcslen(data) + 1) * sizeof(WCHAR)));
    }
    RegCloseKey(k);
    return S_OK;
}

static void RevertRegUndo(LPCWSTR line, FILE *log)
{
    WCHAR buf[1200];
    WCHAR *tok[6] = {0};
    int ti = 0;
    WCHAR *p, *start;
    HKEY hive, k;
    LPCWSTR valName;

    StringCchCopyW(buf, 1200, line);
    start = buf;
    for (p = buf; ; p++)
    {
        if (*p == L'|' || *p == L'\0')
        {
            BOOL end = (*p == L'\0');
            *p = L'\0';
            if (ti < 6) tok[ti++] = start;
            start = p + 1;
            if (end) break;
        }
    }
    if (ti < 3) return;
    hive = HiveFromName(tok[1]);
    if (!hive) return;
    valName = (!wcscmp(tok[3] ? tok[3] : L"@", L"@")) ? L"" : tok[3];

    if (RegOpenKeyExW(hive, tok[2], 0, KEY_WRITE, &k) != ERROR_SUCCESS)
        return;

    if (!wcscmp(tok[0], L"D"))
    {
        LOGF(log, L"  reg-undo: delete %s\\%s [%s]\n", tok[1], tok[2], tok[3] ? tok[3] : L"@");
        RegDeleteValueW(k, valName);
    }
    else if (!wcscmp(tok[0], L"R") && ti >= 6)
    {
        LOGF(log, L"  reg-undo: restore %s\\%s [%s]=%s\n", tok[1], tok[2], tok[3], tok[5]);
        if (!_wcsicmp(tok[4], L"REG_DWORD"))
        {
            DWORD d = (DWORD)_wtoi(tok[5]);
            RegSetValueExW(k, valName, 0, REG_DWORD, (const BYTE *)&d, sizeof(d));
        }
        else
        {
            RegSetValueExW(k, valName, 0, REG_SZ, (const BYTE *)tok[5],
                           (DWORD)((wcslen(tok[5]) + 1) * sizeof(WCHAR)));
        }
    }
    RegCloseKey(k);
}

/* ---- enable / disable ---------------------------------------------------- */

static HRESULT EnableDeps(const FM_FEATURE *f, FILE *log)
{
    WCHAR deps[FM_MAX_DEPS];
    WCHAR *ctx = NULL, *d;
    HRESULT hr = S_OK;
    if (!f->Dependencies[0]) return S_OK;
    StringCchCopyW(deps, FM_MAX_DEPS, f->Dependencies);
    for (d = wcstok_s(deps, L",; ", &ctx); d; d = wcstok_s(NULL, L",; ", &ctx))
    {
        if (!d[0]) continue;
        if (FmGetState(d) == FM_STATE_ENABLED) continue;
        LOGF(log, L"dependency: enabling %s\n", d);
        hr = FmEnable(d, log);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

static void RevertState(HKEY state, FILE *log);

HRESULT FmEnable(LPCWSTR id, FILE *log)
{
    FM_FEATURE f;
    HKEY state = NULL;
    WCHAR payloadDir[MAX_PATH], now[32];
    WCHAR *sect, *p;
    HRESULT hr = S_OK;

    if (!FmFindFeature(id, &f))
    {
        LOGF(log, L"error: unknown feature '%s'\n", id);
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    if (f.State == FM_STATE_ENABLED)
    {
        LOGF(log, L"feature '%s' already enabled\n", id);
        return S_OK;
    }
    if (!f.PayloadAvailable)
    {
        LOGF(log, L"error: payload for '%s' is not staged on this system "
                  L"(catalog entry present, payload deferred)\n", id);
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    LOGF(log, L"enabling '%s' (%s, tier=%s)\n", f.Id, f.Name, FmTierStr(f.Tier));

    hr = EnableDeps(&f, log);
    if (FAILED(hr)) return hr;

    if (OpenStateKey(id, TRUE, KEY_READ | KEY_WRITE, &state) != ERROR_SUCCESS)
        return E_FAIL;

    /* fresh undo lists for this activation */
    RegDeleteValueW(state, L"Files");
    RegDeleteValueW(state, L"RegUndo");
    RegDeleteValueW(state, L"Backups");

    /* payload files */
    FmGetPayloadDir(id, payloadDir, MAX_PATH);
    sect = ReadSection(f.ManifestPath, L"Payload");
    if (sect)
    {
        for (p = sect; *p; p += wcslen(p) + 1)
        {
            WCHAR *eq = wcschr(p, L'='), *sep;
            if (!eq) continue;
            sep = wcschr(eq + 1, L'|');
            if (!sep) continue;
            *sep = L'\0';
            hr = ApplyPayloadFile(payloadDir, eq + 1, sep + 1, state, log);
            *sep = L'|';
            if (FAILED(hr)) goto done;
        }
        HeapFree(GetProcessHeap(), 0, sect);
        sect = NULL;
    }

    /* registry actions */
    sect = ReadSection(f.ManifestPath, L"Registry");
    if (sect)
    {
        for (p = sect; *p; p += wcslen(p) + 1)
        {
            WCHAR *eq = wcschr(p, L'=');
            if (!eq) continue;
            hr = ApplyRegAction(eq + 1, state, log);
            if (FAILED(hr)) goto done;
        }
    }

    UtcNow(now, 32);
    SetStateSz(state, L"State", L"Enabled");
    SetStateSz(state, L"Name", f.Name);
    SetStateSz(state, L"Version", f.Version);
    SetStateSz(state, L"Tier", FmTierStr(f.Tier));
    SetStateSz(state, L"InstallDate", now);
    LOGF(log, L"'%s' enabled.\n", id);

done:
    if (sect) HeapFree(GetProcessHeap(), 0, sect);
    if (FAILED(hr))
    {
        LOGF(log, L"enable FAILED (0x%08lx); rolling back\n", hr);
        if (state) RevertState(state, log);
    }
    if (state) RegCloseKey(state);
    return hr;
}

/* Undo everything recorded in the state key: registry, placed files, then
 * restore any backed-up originals. Used by disable and by enable rollback. */
static void RevertState(HKEY state, FILE *log)
{
    WCHAR *files, *undo, *baks, *p;

    /* revert registry first */
    undo = MultiSzRead(state, L"RegUndo");
    if (undo)
    {
        for (p = undo; *p; p += wcslen(p) + 1)
            RevertRegUndo(p, log);
        HeapFree(GetProcessHeap(), 0, undo);
    }

    /* delete installed files */
    files = MultiSzRead(state, L"Files");
    if (files)
    {
        for (p = files; *p; p += wcslen(p) + 1)
        {
            LOGF(log, L"  remove file: %s\n", p);
            if (!DeleteFileW(p) && GetLastError() != ERROR_FILE_NOT_FOUND)
                LOGF(log, L"  warn: delete failed (%lu)\n", GetLastError());
            else
            {
                /* drop a pack-private directory once it is empty (fails harmlessly if not) */
                WCHAR dir[MAX_PATH];
                StringCchCopyW(dir, MAX_PATH, p);
                PathRemoveFileSpecW(dir);
                RemoveDirectoryW(dir);
            }
        }
        HeapFree(GetProcessHeap(), 0, files);
    }

    /* put back files that enable moved aside ("dest|bak") */
    baks = MultiSzRead(state, L"Backups");
    if (baks)
    {
        for (p = baks; *p; p += wcslen(p) + 1)
        {
            WCHAR *sep = wcschr(p, L'|');
            if (!sep) continue;
            *sep = L'\0';
            LOGF(log, L"  restore file: %s <- %s\n", p, sep + 1);
            DeleteFileW(p);
            if (!MoveFileW(sep + 1, p))
                LOGF(log, L"  warn: restore failed (%lu)\n", GetLastError());
            *sep = L'|';
        }
        HeapFree(GetProcessHeap(), 0, baks);
    }

    RegDeleteValueW(state, L"Files");
    RegDeleteValueW(state, L"RegUndo");
    RegDeleteValueW(state, L"Backups");
}

HRESULT FmDisable(LPCWSTR id, FILE *log)
{
    HKEY state = NULL;

    if (OpenStateKey(id, FALSE, KEY_READ | KEY_WRITE, &state) != ERROR_SUCCESS
        || FmGetState(id) != FM_STATE_ENABLED)
    {
        LOGF(log, L"feature '%s' is not enabled; nothing to do\n", id);
        if (state) RegCloseKey(state);
        return S_OK;
    }

    LOGF(log, L"disabling '%s'\n", id);
    RevertState(state, log);
    SetStateSz(state, L"State", L"Disabled");
    LOGF(log, L"'%s' disabled.\n", id);
    RegCloseKey(state);
    return S_OK;
}
