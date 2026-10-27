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

BOOL FmGetSoftwareCatalogDir(LPWSTR buf, DWORD cch)
{
    WCHAR cat[MAX_PATH];
    if (!FmGetCatalogDir(cat, MAX_PATH))
        return FALSE;
    return SUCCEEDED(StringCchPrintfW(buf, cch, L"%s\\Software", cat));
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
    f->Removable = GetPrivateProfileIntW(L"Feature", L"Removable", 1, path) != 0;
    /* [Install] Provider= selects the software-tier install path. A software-tier
     * manifest without it keeps the OS-tier mechanics ([Payload]/[Registry]/[Service]
     * applied from a staged payload), so older/hand-written manifests still work. */
    if (f->Tier == FM_TIER_SOFTWARE)
        GetPrivateProfileStringW(L"Install", L"Provider", L"", f->Provider, ARRAYSIZE(f->Provider), path);
    return TRUE;
}

/* ---- software tier: command expansion + availability -------------------- */

/* Expand %PAYLOAD% (the feature's staged payload dir) and %env% variables. */
static void ExpandCmd(LPCWSTR id, LPCWSTR raw, LPWSTR out, DWORD cch)
{
    WCHAR tmp[2048], payload[MAX_PATH];
    LPCWSTR p = raw;
    DWORD n = 0;
    FmGetPayloadDir(id, payload, MAX_PATH);
    while (*p && n + 1 < ARRAYSIZE(tmp))
    {
        if (!_wcsnicmp(p, L"%PAYLOAD%", 9))
        {
            size_t l = wcslen(payload);
            if (n + l + 1 >= ARRAYSIZE(tmp)) break;
            memcpy(tmp + n, payload, l * sizeof(WCHAR));
            n += (DWORD)l;
            p += 9;
        }
        else
            tmp[n++] = *p++;
    }
    tmp[n] = L'\0';
    if (!ExpandEnvironmentStringsW(tmp, out, cch))
        StringCchCopyW(out, cch, tmp);
}

static BOOL IsAbsPath(LPCWSTR s)
{
    return (s[0] && s[1] == L':') || (s[0] == L'\\');
}

static BOOL IsScript(LPCWSTR path)
{
    LPCWSTR ext = PathFindExtensionW(path);
    return !_wcsicmp(ext, L".cmd") || !_wcsicmp(ext, L".bat");
}

/* Full path of the command-provider installer ([Install] Installer=). */
static BOOL InstallerPath(const FM_FEATURE *f, LPWSTR out, DWORD cch)
{
    WCHAR raw[MAX_PATH], exp[MAX_PATH];
    GetPrivateProfileStringW(L"Install", L"Installer", L"", raw, MAX_PATH, f->ManifestPath);
    if (!raw[0]) return FALSE;
    ExpandCmd(f->Id, raw, exp, MAX_PATH);
    if (IsAbsPath(exp))
        return SUCCEEDED(StringCchCopyW(out, cch, exp));
    FmGetPayloadDir(f->Id, out, cch);
    return SUCCEEDED(StringCchCatW(out, cch, L"\\")) && SUCCEEDED(StringCchCatW(out, cch, exp));
}

static BOOL RappsExePath(LPWSTR out, DWORD cch)
{
    WCHAR sys[MAX_PATH];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\rapps.exe", sys));
}

/* Can the install be attempted? rapps provider: rapps.exe present (the download
 * happens at install time, so there is no staged payload to check). command
 * provider: the installer file is staged. */
static BOOL SoftwareAvailable(const FM_FEATURE *f)
{
    WCHAR p[MAX_PATH];
    if (!_wcsicmp(f->Provider, L"command"))
        return InstallerPath(f, p, MAX_PATH) && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    if (_wcsicmp(f->Provider, L"rapps"))
        return FALSE;                                   /* unknown provider */
    return RappsExePath(p, MAX_PATH) && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* Does every declared [Payload] source file exist on disk? */
static BOOL PayloadPresent(const FM_FEATURE *f)
{
    WCHAR payloadDir[MAX_PATH], src[MAX_PATH];
    WCHAR *sect, *p, *sep;
    BOOL ok = TRUE;

    if (f->Tier == FM_TIER_SOFTWARE && f->Provider[0])
        return SoftwareAvailable(f);

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

/* InstallDate / PendingReboot from the persisted record. */
static void ReadStateExtras(FM_FEATURE *f)
{
    HKEY k;
    DWORD cb, type;
    WCHAR v[32];

    f->InstallDate[0] = L'\0';
    f->PendingReboot = FALSE;
    if (OpenStateKey(f->Id, FALSE, KEY_READ, &k) != ERROR_SUCCESS) return;
    cb = sizeof(f->InstallDate) - sizeof(WCHAR);
    if (RegQueryValueExW(k, L"InstallDate", NULL, &type, (LPBYTE)f->InstallDate, &cb) != ERROR_SUCCESS
        || type != REG_SZ)
        f->InstallDate[0] = L'\0';
    f->InstallDate[31] = L'\0';
    cb = sizeof(v) - sizeof(WCHAR);
    v[0] = L'\0';
    if (RegQueryValueExW(k, L"PendingReboot", NULL, &type, (LPBYTE)v, &cb) == ERROR_SUCCESS
        && type == REG_SZ)
    {
        v[31] = L'\0';
        if (!_wcsicmp(v, L"Yes")) f->PendingReboot = TRUE;
    }
    RegCloseKey(k);
}

/* ---- enumeration --------------------------------------------------------- */

/* Append every valid manifest in dir (*.ini) to the growing array. */
static void EnumDir(LPCWSTR dir, FM_FEATURE **parr, int *pcount, int *pcap)
{
    WCHAR glob[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    StringCchPrintfW(glob, MAX_PATH, L"%s\\*.ini", dir);
    h = FindFirstFileW(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        FM_FEATURE f;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        StringCchPrintfW(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        if (!LoadManifest(path, &f)) continue;
        f.State = FmGetState(f.Id);
        f.PayloadAvailable = PayloadPresent(&f);
        ReadStateExtras(&f);

        if (*pcount == *pcap)
        {
            int nc = *pcap ? *pcap * 2 : 8;
            FM_FEATURE *na = *parr
                ? (FM_FEATURE *)HeapReAlloc(GetProcessHeap(), 0, *parr, nc * sizeof(FM_FEATURE))
                : (FM_FEATURE *)HeapAlloc(GetProcessHeap(), 0, nc * sizeof(FM_FEATURE));
            if (!na) break;
            *parr = na; *pcap = nc;
        }
        (*parr)[(*pcount)++] = f;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Catalog = in-image OS manifests (Catalog\*.ini) + the generated software
 * cache (Catalog\Software\*.ini, written on demand by FmImportRapps). */
int FmEnumFeatures(FM_FEATURE **outArr)
{
    WCHAR catDir[MAX_PATH], swDir[MAX_PATH];
    FM_FEATURE *arr = NULL;
    int count = 0, cap = 0;

    *outArr = NULL;
    if (!FmGetCatalogDir(catDir, MAX_PATH)) return 0;
    EnumDir(catDir, &arr, &count, &cap);
    if (FmGetSoftwareCatalogDir(swDir, MAX_PATH))
        EnumDir(swDir, &arr, &count, &cap);

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
    WCHAR dir[MAX_PATH], path[MAX_PATH];

    /* fast path: manifest file named after the id (the software cache can hold
     * hundreds of entries, do not parse them all for a single lookup) */
    if (id[0] && !wcspbrk(id, L"\\/:*?\"<>|"))
    {
        int k;
        for (k = 0; k < 2; k++)
        {
            if (!(k ? FmGetSoftwareCatalogDir(dir, MAX_PATH) : FmGetCatalogDir(dir, MAX_PATH))) continue;
            StringCchPrintfW(path, MAX_PATH, L"%s\\%s.ini", dir, id);
            if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;
            if (LoadManifest(path, out) && !_wcsicmp(out->Id, id))
            {
                out->State = FmGetState(out->Id);
                out->PayloadAvailable = PayloadPresent(out);
                ReadStateExtras(out);
                return TRUE;
            }
        }
    }
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

/* ---- driver/service registration ----------------------------------------- */

/* [Service] spec: Name|Type|StartType|ImagePath|DisplayName|Run
 *   Type      = filesys | kernel | win32   (SERVICE_FILE_SYSTEM_DRIVER / KERNEL_DRIVER / WIN32_OWN_PROCESS)
 *   StartType = boot | system | auto | demand | disabled
 *   Run       = 1 => start the service right after creating it (failure is logged, not fatal)
 * Whether we created the service is recorded in the state value "Services"
 * (one service name per line) so disable removes exactly what enable made. */
static DWORD ParseSvcType(LPCWSTR s)
{
    if (!_wcsicmp(s, L"kernel")) return SERVICE_KERNEL_DRIVER;
    if (!_wcsicmp(s, L"win32"))  return SERVICE_WIN32_OWN_PROCESS;
    return SERVICE_FILE_SYSTEM_DRIVER;
}

static DWORD ParseSvcStart(LPCWSTR s)
{
    if (!_wcsicmp(s, L"boot"))     return SERVICE_BOOT_START;
    if (!_wcsicmp(s, L"system"))   return SERVICE_SYSTEM_START;
    if (!_wcsicmp(s, L"auto"))     return SERVICE_AUTO_START;
    if (!_wcsicmp(s, L"disabled")) return SERVICE_DISABLED;
    return SERVICE_DEMAND_START;
}

static HRESULT ApplyServiceAction(LPCWSTR spec, HKEY state, FILE *log)
{
    WCHAR buf[1024];
    WCHAR *tok[6] = {0};
    int ti = 0;
    WCHAR *p, *start;
    SC_HANDLE scm, svc;
    HRESULT hr = S_OK;

    StringCchCopyW(buf, 1024, spec);
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
    if (ti < 4) { LOGF(log, L"  bad service spec: %s\n", spec); return E_INVALIDARG; }

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) { LOGF(log, L"  ERROR: OpenSCManager (%lu)\n", GetLastError()); return HRESULT_FROM_WIN32(GetLastError()); }

    svc = CreateServiceW(scm, tok[0], (ti > 4 && tok[4][0]) ? tok[4] : tok[0],
                         SERVICE_ALL_ACCESS, ParseSvcType(tok[1]), ParseSvcStart(tok[2]),
                         SERVICE_ERROR_NORMAL, tok[3], NULL, NULL, NULL, NULL, NULL);
    if (!svc)
    {
        DWORD e = GetLastError();
        /* a pre-existing service is not ours: refuse rather than adopt + later delete it */
        LOGF(log, L"  ERROR: CreateService %s failed (%lu)%s\n", tok[0], e,
             e == ERROR_SERVICE_EXISTS ? L" - already registered by something else" : L"");
        hr = HRESULT_FROM_WIN32(e);
    }
    else
    {
        LOGF(log, L"  service: created %s (type=%s start=%s image=%s)\n", tok[0], tok[1], tok[2], tok[3]);
        MultiSzAppend(state, L"Services", tok[0]);
        if (ti > 5 && _wtoi(tok[5]))
        {
            if (StartServiceW(svc, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
                LOGF(log, L"  service: %s started\n", tok[0]);
            else
                LOGF(log, L"  warn: StartService %s failed (%lu); it stays registered\n", tok[0], GetLastError());
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return hr;
}

/* Stop (wait up to ~10 s) one service. TRUE if it is stopped / absent / never ran. */
static BOOL StopServiceByName(SC_HANDLE scm, LPCWSTR name, FILE *log)
{
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_ALL_ACCESS);
    SERVICE_STATUS ss;
    int i;
    BOOL ok = TRUE;
    if (!svc) return TRUE;
    if (QueryServiceStatus(svc, &ss) && ss.dwCurrentState != SERVICE_STOPPED)
    {
        LOGF(log, L"  service: stopping %s\n", name);
        ControlService(svc, SERVICE_CONTROL_STOP, &ss);
        for (i = 0; i < 50; i++)
        {
            if (!QueryServiceStatus(svc, &ss) || ss.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(200);
        }
        if (!QueryServiceStatus(svc, &ss) || ss.dwCurrentState != SERVICE_STOPPED)
        {
            LOGF(log, L"  ERROR: service %s did not stop (state %lu); in use?\n", name, ss.dwCurrentState);
            ok = FALSE;
        }
    }
    CloseServiceHandle(svc);
    return ok;
}

/* Unregister the services enable created. A driver that cannot be unloaded on this
 * system (WinFsp registers a UNC provider with MUP, and an FSD with a live
 * \FileSystem device never stops under ReactOS) is set to Start=disabled and marked
 * for deletion; its name goes into "PendingServices" and removal completes at reboot. */
static void RemoveRecordedServices(HKEY state, FILE *log)
{
    WCHAR *svcs = MultiSzRead(state, L"Services"), *p;
    SC_HANDLE scm, svc;
    if (!svcs) return;
    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm)
    {
        for (p = svcs; *p; p += wcslen(p) + 1)
        {
            BOOL stopped = StopServiceByName(scm, p, log);
            svc = OpenServiceW(scm, p, SERVICE_ALL_ACCESS);
            if (!svc) continue;
            if (!stopped)
            {
                LOGF(log, L"  note: %s cannot be unloaded while the system is up; disabling it, removal completes at reboot\n", p);
                ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE,
                                     NULL, NULL, NULL, NULL, NULL, NULL, NULL);
                MultiSzAppend(state, L"PendingServices", p);
            }
            LOGF(log, L"  service: deleting %s\n", p);
            if (!DeleteService(svc))
                LOGF(log, L"  warn: DeleteService %s failed (%lu)\n", p, GetLastError());
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
    }
    HeapFree(GetProcessHeap(), 0, svcs);
}

/* Is a previous disable still waiting for a reboot? Clears the markers once nothing is left. */
static BOOL PendingRebootActive(HKEY state)
{
    WCHAR *l, *p;
    BOOL active = FALSE;
    SC_HANDLE scm, svc;

    l = MultiSzRead(state, L"PendingFiles");
    if (l)
    {
        for (p = l; *p; p += wcslen(p) + 1)
            if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) active = TRUE;
        HeapFree(GetProcessHeap(), 0, l);
    }
    l = MultiSzRead(state, L"PendingServices");
    if (l)
    {
        scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        for (p = l; *p && scm; p += wcslen(p) + 1)
        {
            svc = OpenServiceW(scm, p, SERVICE_QUERY_STATUS);
            if (svc) { active = TRUE; CloseServiceHandle(svc); }
        }
        if (scm) CloseServiceHandle(scm);
        HeapFree(GetProcessHeap(), 0, l);
    }
    if (!active)
    {
        RegDeleteValueW(state, L"PendingFiles");
        RegDeleteValueW(state, L"PendingServices");
        RegDeleteValueW(state, L"PendingReboot");
    }
    return active;
}

/* ---- software tier: run installer / uninstaller ------------------------- */

#define SW_RUN_TIMEOUT_MS (60u * 60u * 1000u)   /* download + install of a big app */

/* Run a command line, wait, return its exit code in *code. A run that exceeds the
 * timeout is terminated (exact process handle) and reported as ERROR_TIMEOUT. */
static HRESULT RunAndWait(LPCWSTR cmdline, DWORD timeoutMs, DWORD *code, FILE *log)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR cl[2048];
    DWORD w;

    *code = (DWORD)-1;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    StringCchCopyW(cl, ARRAYSIZE(cl), cmdline);   /* CreateProcessW may modify its buffer */
    LOGF(log, L"  run: %s\n", cl);
    if (!CreateProcessW(NULL, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        LOGF(log, L"  ERROR: cannot start command (%lu)\n", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(pi.hThread);
    w = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (w == WAIT_TIMEOUT)
    {
        LOGF(log, L"  ERROR: command still running after %lu s; terminating pid %lu\n",
             timeoutMs / 1000, pi.dwProcessId);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess);
    LOGF(log, L"  exit code: %lu\n", *code);
    return S_OK;
}

/* Build the install + uninstall command lines for a software feature into
 * inst/uninst (cch each). The uninstall line is recorded in the state key at
 * install time so disable stays data-driven (same as the OS tier's undo lists). */
static HRESULT BuildSoftwareCommands(const FM_FEATURE *f, LPWSTR inst, LPWSTR uninst, DWORD cch, FILE *log)
{
    WCHAR pkg[128], rapps[MAX_PATH];

    inst[0] = uninst[0] = L'\0';
    if (!_wcsicmp(f->Provider, L"command"))
    {
        WCHAR path[MAX_PATH], args[512], raw[1024];
        if (!InstallerPath(f, path, MAX_PATH))
        {
            LOGF(log, L"error: '%s' has no [Install] Installer\n", f->Id);
            return E_INVALIDARG;
        }
        GetPrivateProfileStringW(L"Install", L"SilentArgs", L"", raw, ARRAYSIZE(raw), f->ManifestPath);
        ExpandCmd(f->Id, raw, args, ARRAYSIZE(args));
        if (IsScript(path))
            StringCchPrintfW(inst, cch, L"cmd.exe /c \"\"%s\" %s\"", path, args);
        else
            StringCchPrintfW(inst, cch, L"\"%s\" %s", path, args);
        GetPrivateProfileStringW(L"Install", L"UninstallCmd", L"", raw, ARRAYSIZE(raw), f->ManifestPath);
        if (raw[0])
        {
            ExpandCmd(f->Id, raw, args, ARRAYSIZE(args));
            StringCchCopyW(uninst, cch, args);
        }
        return S_OK;
    }

    if (_wcsicmp(f->Provider, L"rapps"))
    {
        LOGF(log, L"error: '%s' has unknown install provider '%s'\n", f->Id, f->Provider);
        return E_INVALIDARG;
    }
    /* rapps provider: pkg name from [Rapps] Pkg */
    GetPrivateProfileStringW(L"Rapps", L"Pkg", L"", pkg, ARRAYSIZE(pkg), f->ManifestPath);
    if (!pkg[0] || wcspbrk(pkg, L"\"\r\n"))
    {
        LOGF(log, L"error: '%s' has no valid [Rapps] Pkg\n", f->Id);
        return E_INVALIDARG;
    }
    if (!RappsExePath(rapps, MAX_PATH)) return E_FAIL;
    StringCchPrintfW(inst, cch, L"\"%s\" /install /S \"%s\"", rapps, pkg);
    StringCchPrintfW(uninst, cch, L"\"%s\" /uninstall /S \"%s\"", rapps, pkg);
    return S_OK;
}

/* Download + install. Returns S_OK only if the installer exited 0; on success the
 * uninstall command line is recorded in the state key. */
static HRESULT SoftwareInstall(const FM_FEATURE *f, HKEY state, FILE *log)
{
    WCHAR inst[2048], uninst[2048];
    DWORD code;
    HRESULT hr;

    hr = BuildSoftwareCommands(f, inst, uninst, ARRAYSIZE(inst), log);
    if (FAILED(hr)) return hr;
    LOGF(log, L"  software install via '%s' provider\n", f->Provider);
    hr = RunAndWait(inst, SW_RUN_TIMEOUT_MS, &code, log);
    if (FAILED(hr)) return hr;
    if (code != 0)
    {
        LOGF(log, L"error: installer for '%s' failed (exit code %lu)\n", f->Id, code);
        return HRESULT_FROM_WIN32(ERROR_INSTALL_FAILURE);
    }
    SetStateSz(state, L"Provider", f->Provider);
    SetStateSz(state, L"Uninstall", uninst);
    return S_OK;
}

/* Run the uninstall command recorded at install time. S_OK if there is none. */
static HRESULT SoftwareUninstall(HKEY state, FILE *log)
{
    WCHAR cmd[2048];
    DWORD cb = sizeof(cmd) - sizeof(WCHAR), type = 0, code;
    HRESULT hr;

    cmd[0] = L'\0';
    if (RegQueryValueExW(state, L"Uninstall", NULL, &type, (LPBYTE)cmd, &cb) != ERROR_SUCCESS
        || type != REG_SZ || !cmd[0])
    {
        LOGF(log, L"  note: no uninstall command recorded; nothing to run\n");
        return S_OK;
    }
    cmd[ARRAYSIZE(cmd) - 1] = L'\0';
    hr = RunAndWait(cmd, SW_RUN_TIMEOUT_MS, &code, log);
    if (FAILED(hr)) return hr;
    if (code != 0)
    {
        LOGF(log, L"error: uninstaller failed (exit code %lu)\n", code);
        return HRESULT_FROM_WIN32(ERROR_INSTALL_FAILURE);
    }
    return S_OK;
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
        if (f.Tier == FM_TIER_SOFTWARE && f.Provider[0])
            LOGF(log, L"error: installer for '%s' is not available on this system "
                      L"(provider '%s': rapps.exe or the staged installer is missing)\n", id, f.Provider);
        else
            LOGF(log, L"error: payload for '%s' is not staged on this system "
                      L"(catalog entry present, payload deferred)\n", id);
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    if (OpenStateKey(id, FALSE, KEY_READ | KEY_WRITE, &state) == ERROR_SUCCESS)
    {
        BOOL pend = PendingRebootActive(state);
        RegCloseKey(state);
        state = NULL;
        if (pend)
        {
            LOGF(log, L"error: '%s' was disabled but is still being removed; reboot, then enable again\n", id);
            return HRESULT_FROM_WIN32(ERROR_SUCCESS_REBOOT_REQUIRED);
        }
    }

    LOGF(log, L"enabling '%s' (%s, tier=%s)\n", f.Id, f.Name, FmTierStr(f.Tier));

    hr = EnableDeps(&f, log);
    if (FAILED(hr)) return hr;

    if (OpenStateKey(id, TRUE, KEY_READ | KEY_WRITE, &state) != ERROR_SUCCESS)
        return E_FAIL;

    if (f.Tier == FM_TIER_SOFTWARE && f.Provider[0])
    {
        /* install != enable: fetch + run the installer instead of placing in-image bits */
        hr = SoftwareInstall(&f, state, log);
        if (FAILED(hr))
        {
            WCHAR sub[512];
            LOGF(log, L"install FAILED (0x%08lx)\n", hr);
            RegCloseKey(state);
            if (f.State == FM_STATE_NOT_INSTALLED)  /* drop the empty record we just created */
            {
                StringCchPrintfW(sub, 512, L"%s\\%s", STATE_KEY, id);
                RegDeleteKeyW(HKEY_LOCAL_MACHINE, sub);
            }
            return hr;
        }
        UtcNow(now, 32);
        SetStateSz(state, L"State", L"Enabled");
        SetStateSz(state, L"Name", f.Name);
        SetStateSz(state, L"Version", f.Version);
        SetStateSz(state, L"Tier", FmTierStr(f.Tier));
        SetStateSz(state, L"InstallDate", now);
        LOGF(log, L"'%s' installed.\n", id);
        RegCloseKey(state);
        return S_OK;
    }

    /* fresh undo lists for this activation */
    RegDeleteValueW(state, L"Files");
    RegDeleteValueW(state, L"RegUndo");
    RegDeleteValueW(state, L"Backups");
    RegDeleteValueW(state, L"Services");

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
        HeapFree(GetProcessHeap(), 0, sect);
        sect = NULL;
    }

    /* driver/service registration (after files + registry, so the image exists) */
    sect = ReadSection(f.ManifestPath, L"Service");
    if (sect)
    {
        for (p = sect; *p; p += wcslen(p) + 1)
        {
            WCHAR *eq = wcschr(p, L'=');
            if (!eq) continue;
            hr = ApplyServiceAction(eq + 1, state, log);
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

    /* unregister services first: the driver image must not be in use when deleted */
    RemoveRecordedServices(state, log);

    /* revert registry */
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
            {
                DWORD e = GetLastError();
                if (MoveFileExW(p, NULL, MOVEFILE_DELAY_UNTIL_REBOOT))
                {
                    LOGF(log, L"  note: %s is in use (%lu); will be deleted at reboot\n", p, e);
                    MultiSzAppend(state, L"PendingFiles", p);
                }
                else
                    LOGF(log, L"  warn: delete failed (%lu), reboot-delete failed (%lu)\n", e, GetLastError());
            }
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
    RegDeleteValueW(state, L"Services");
}

static HRESULT DisableOne(LPCWSTR id, DWORD flags, FILE *log)
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
    {
        /* software tier: disable == run the app's uninstaller */
        FM_FEATURE sf;
        if (FmFindFeature(id, &sf) && sf.Tier == FM_TIER_SOFTWARE && sf.Provider[0])
        {
            HRESULT shr = SoftwareUninstall(state, log);
            if (FAILED(shr))
            {
                if (!(flags & FM_DISABLE_FORCE))
                {
                    LOGF(log, L"'%s' left enabled (use /force to forget it anyway)\n", id);
                    RegCloseKey(state);
                    return shr;
                }
                LOGF(log, L"warn: uninstaller failed but /force given; marking '%s' disabled\n", id);
            }
        }
    }
    RegDeleteValueW(state, L"PendingFiles");
    RegDeleteValueW(state, L"PendingServices");
    RegDeleteValueW(state, L"PendingReboot");
    RevertState(state, log);
    {
        DWORD cb = 0;
        if (RegQueryValueExW(state, L"PendingFiles", NULL, NULL, NULL, &cb) == ERROR_SUCCESS ||
            RegQueryValueExW(state, L"PendingServices", NULL, NULL, NULL, &cb) == ERROR_SUCCESS)
        {
            SetStateSz(state, L"PendingReboot", L"Yes");
            LOGF(log, L"note: some components are in use; removal completes at the next reboot\n");
        }
    }
    SetStateSz(state, L"State", L"Disabled");
    LOGF(log, L"'%s' disabled.\n", id);
    RegCloseKey(state);
    return S_OK;
}

/* ---- dependents / disable policy / uninstall ----------------------------- */

/* Base components (catalog Removable=0) are part of the image: refuse to
 * disable or uninstall them, regardless of dependents or current state. */
static HRESULT CheckRemovable(LPCWSTR id, FILE *log)
{
    FM_FEATURE feat;
    if (FmFindFeature(id, &feat) && !feat.Removable)
    {
        LOGF(log, L"error: '%s' is a base component of the system and cannot be disabled or removed\n", id);
        return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    }
    return S_OK;
}

static BOOL DepsContain(LPCWSTR deps, LPCWSTR id)
{
    WCHAR tmp[FM_MAX_DEPS];
    WCHAR *ctx = NULL, *d;
    StringCchCopyW(tmp, FM_MAX_DEPS, deps);
    for (d = wcstok_s(tmp, L",; ", &ctx); d; d = wcstok_s(NULL, L",; ", &ctx))
        if (!_wcsicmp(d, id)) return TRUE;
    return FALSE;
}

int FmGetDependents(LPCWSTR id, LPWSTR buf, DWORD cch)
{
    FM_FEATURE *arr;
    int n, i, cnt = 0;
    if (cch) buf[0] = L'\0';
    n = FmEnumFeatures(&arr);
    for (i = 0; i < n; i++)
    {
        if (arr[i].State != FM_STATE_ENABLED || !_wcsicmp(arr[i].Id, id)) continue;
        if (!DepsContain(arr[i].Dependencies, id)) continue;
        if (cnt++ && cch) StringCchCatW(buf, cch, L",");
        if (cch) StringCchCatW(buf, cch, arr[i].Id);
    }
    FmFree(arr);
    return cnt;
}

HRESULT FmDisableEx(LPCWSTR id, DWORD flags, FILE *log)
{
    WCHAR deps[FM_MAX_DEPS * 2];
    WCHAR *ctx = NULL, *d;
    HRESULT hr;

    hr = CheckRemovable(id, log);      /* first: base component guard (GFX-FU) */
    if (FAILED(hr)) return hr;

    if (FmGetDependents(id, deps, ARRAYSIZE(deps)))
    {
        if (!(flags & FM_DISABLE_CASCADE))
        {
            LOGF(log, L"error: cannot disable '%s': enabled feature(s) depend on it: %s "
                      L"(disable those first, or use /cascade)\n", id, deps);
            return HRESULT_FROM_WIN32(ERROR_DEPENDENT_SERVICES_RUNNING);
        }
        /* dependents first; each recursive call handles its own dependents */
        for (d = wcstok_s(deps, L",", &ctx); d; d = wcstok_s(NULL, L",", &ctx))
        {
            LOGF(log, L"cascade: disabling dependent '%s'\n", d);
            hr = FmDisableEx(d, flags, log);
            if (FAILED(hr)) return hr;
        }
    }
    return DisableOne(id, flags, log);
}

HRESULT FmDisable(LPCWSTR id, FILE *log)
{
    return FmDisableEx(id, 0, log);
}

HRESULT FmUninstall(LPCWSTR id, FILE *log)
{
    return FmUninstallEx(id, 0, log);
}

HRESULT FmUninstallEx(LPCWSTR id, DWORD flags, FILE *log)
{
    HKEY state;
    HRESULT hr;
    WCHAR sub[512];
    BOOL pending = FALSE;

    hr = CheckRemovable(id, log);
    if (FAILED(hr)) return hr;

    if (FmGetState(id) == FM_STATE_NOT_INSTALLED)
    {
        LOGF(log, L"feature '%s' is not installed; nothing to do\n", id);
        return S_OK;
    }
    hr = FmDisableEx(id, flags, log);   /* no-op if already Disabled */
    if (FAILED(hr)) return hr;

    if (OpenStateKey(id, FALSE, KEY_READ | KEY_WRITE, &state) == ERROR_SUCCESS)
    {
        pending = PendingRebootActive(state);
        RegCloseKey(state);
    }
    if (pending)
    {
        LOGF(log, L"note: '%s' still has components pending removal; record kept until reboot\n", id);
        return HRESULT_FROM_WIN32(ERROR_SUCCESS_REBOOT_REQUIRED);
    }
    StringCchPrintfW(sub, 512, L"%s\\%s", STATE_KEY, id);
    if (RegDeleteKeyW(HKEY_LOCAL_MACHINE, sub) != ERROR_SUCCESS)
    {
        LOGF(log, L"warn: could not delete state record for '%s'\n", id);
        return E_FAIL;
    }
    LOGF(log, L"'%s' uninstalled (state record removed).\n", id);
    return S_OK;
}
