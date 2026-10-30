/*
 * PROJECT:     ReactOS-Nitro Optional Features / Addon Manager (FEATMGMT-01)
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Scoop integration (SCOOP-FEATMGR-01): Scoop root/state helpers and the
 *              bucket -> software-tier catalog converter (featmgr /import-scoop).
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * featmgr does not interpret or download Scoop payloads. It reads the JSON manifests of a
 * bucket that already exists on disk (<root>\buckets\<bucket>\bucket\*.json), keeps only
 * the descriptive fields, and writes one Catalog\Software\<id>.ini per app:
 *
 *   [Feature]  Id Name Description Version Tier=software Dependencies=scoop
 *   [Install]  Provider=scoop
 *   [Scoop]    App Bucket Homepage License Verified
 *
 * Install / uninstall of such an entry shells out to "scoop install|uninstall <bucket>/<app>"
 * (engine.c), so Scoop's own hash verification, extraction and shims stay in charge.
 */

#include "featmgr.h"
#include <shlwapi.h>
#include <shlobj.h>
#include <strsafe.h>

#define LOGF(log, ...) do { if (log) { fwprintf((log), __VA_ARGS__); fflush(log); } } while (0)
/* an error goes to the log stream (CLI) and to the last-message buffer (GUI) */
#define ERRF(log, ...) do { LOGF(log, __VA_ARGS__); FmSetLastMessage(__VA_ARGS__); } while (0)

#if defined(_M_AMD64)
#define FM_SCOOP_SUPPORTED 1
#else
#define FM_SCOOP_SUPPORTED 0
#endif

static const WCHAR ENV_KEY[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";

/* ---- root / layout ------------------------------------------------------- */

BOOL FmScoopSupported(void)
{
    return FM_SCOOP_SUPPORTED;
}

/* Scoop root: FEATMGR_SCOOP_ROOT (test hook), else the machine SCOOP variable that the
 * scoop feature sets, else %SystemDrive%\scoop. The per-user environment is ignored on
 * purpose: the root must be the same for every user and for the featmgr process. */
BOOL FmScoopRoot(LPWSTR buf, DWORD cch)
{
    WCHAR v[MAX_PATH], x[MAX_PATH];
    HKEY k;
    DWORD cb, type;

    if (GetEnvironmentVariableW(L"FEATMGR_SCOOP_ROOT", v, MAX_PATH) && v[0])
        return SUCCEEDED(StringCchCopyW(buf, cch, v));

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ENV_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS)
    {
        cb = sizeof(v) - sizeof(WCHAR);
        v[0] = L'\0';
        if (RegQueryValueExW(k, L"SCOOP", NULL, &type, (LPBYTE)v, &cb) == ERROR_SUCCESS
            && (type == REG_SZ || type == REG_EXPAND_SZ) && v[0])
        {
            v[ARRAYSIZE(v) - 1] = L'\0';
            RegCloseKey(k);
            if (type == REG_EXPAND_SZ && ExpandEnvironmentStringsW(v, x, MAX_PATH))
                return SUCCEEDED(StringCchCopyW(buf, cch, x));
            return SUCCEEDED(StringCchCopyW(buf, cch, v));
        }
        RegCloseKey(k);
    }
    if (!GetEnvironmentVariableW(L"SystemDrive", v, MAX_PATH) || !v[0])
    {
        if (!GetWindowsDirectoryW(v, MAX_PATH) || !v[0] || v[1] != L':') return FALSE;
        v[2] = L'\0';
    }
    return SUCCEEDED(StringCchPrintfW(buf, cch, L"%s\\scoop", v));
}

BOOL FmPwshPath(LPWSTR buf, DWORD cch)
{
    WCHAR pf[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH) || !pf[0]) return FALSE;
    return SUCCEEDED(StringCchPrintfW(buf, cch, L"%s\\PowerShell\\7\\pwsh.exe", pf));
}

/* An app name / bucket name ends up in a file name, an id and a command line. */
static BOOL ValidName(LPCWSTR s, size_t maxLen)
{
    size_t n = wcslen(s), i;
    if (!n || n > maxLen) return FALSE;
    for (i = 0; i < n; i++)
    {
        WCHAR c = s[i];
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')
              || c == L'-' || c == L'_' || c == L'.' || c == L'+'))
            return FALSE;
    }
    return s[0] != L'.';
}

BOOL FmScoopValidApp(LPCWSTR s)    { return ValidName(s, 48); }
BOOL FmScoopValidBucket(LPCWSTR s) { return ValidName(s, 24); }

static BOOL IsOfficialBucket(LPCWSTR b)
{
    return !_wcsicmp(b, L"main") || !_wcsicmp(b, L"extras");
}

/* <root>\apps\<app>\current : the junction Scoop keeps for an installed app. */
BOOL FmScoopAppInstalled(LPCWSTR app)
{
    WCHAR root[MAX_PATH], p[MAX_PATH];
    if (!FmScoopValidApp(app) || !FmScoopRoot(root, MAX_PATH)) return FALSE;
    StringCchPrintfW(p, MAX_PATH, L"%s\\apps\\%s\\current", root, app);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* "scoop install" works once the Scoop shim exists, or when the scoop feature is in the
 * catalog (installing the app then installs Scoop first through the dependency). */
BOOL FmScoopUsable(void)
{
    WCHAR root[MAX_PATH], p[MAX_PATH];
    if (!FM_SCOOP_SUPPORTED) return FALSE;
    if (FmScoopRoot(root, MAX_PATH))
    {
        StringCchPrintfW(p, MAX_PATH, L"%s\\shims\\scoop.cmd", root);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return TRUE;
    }
    if (FmGetCatalogDir(root, MAX_PATH))
    {
        StringCchPrintfW(p, MAX_PATH, L"%s\\scoop.ini", root);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return TRUE;
    }
    return FALSE;
}

/* The command line that runs "scoop <verb> <bucket>/<app>". Production: PowerShell 7 runs Scoop's own
 * shim script directly (the shim's scoop.cmd goes through cmd.exe, whose batch exit codes are
 * not reliable on nitro, and failures must not be mistaken for success). Test hook:
 * FEATMGR_SCOOP_RUNNER = a .cmd that stands in for Scoop (used by tests\roundtrip.cmd).
 * Every variable part is validated first (no quoting tricks). */
BOOL FmScoopCommand(LPCWSTR verb, LPCWSTR bucket, LPCWSTR app, LPWSTR out, DWORD cch)
{
    WCHAR root[MAX_PATH], runner[MAX_PATH], pwsh[MAX_PATH];
    if (!FmScoopValidApp(app) || !FmScoopValidBucket(bucket) || !FmScoopRoot(root, MAX_PATH)) return FALSE;
    if (wcspbrk(root, L"\"%\r\n")) return FALSE;
    if (GetEnvironmentVariableW(L"FEATMGR_SCOOP_RUNNER", runner, MAX_PATH) && runner[0] && !wcspbrk(runner, L"\"%\r\n"))
        return SUCCEEDED(StringCchPrintfW(out, cch, L"cmd.exe /c \"\"%s\" %s %s/%s\"", runner, verb, bucket, app));
    if (!FmPwshPath(pwsh, MAX_PATH)) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch,
        L"\"%s\" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%s\\shims\\scoop.ps1\" %s %s/%s",
        pwsh, root, verb, bucket, app));
}

/* featmgr's own environment is stale right after it installed PowerShell 7 / Scoop
 * (the registry PATH changed, this process did not see it): give the child what it needs. */
void FmScoopPrepareEnv(void)
{
    WCHAR root[MAX_PATH], pw[MAX_PATH], old[32768], np[32768 + 2 * MAX_PATH];
    DWORD n;

    if (!FmScoopRoot(root, MAX_PATH)) return;
    SetEnvironmentVariableW(L"SCOOP", root);
    old[0] = L'\0';
    n = GetEnvironmentVariableW(L"PATH", old, ARRAYSIZE(old));
    if (n >= ARRAYSIZE(old)) old[0] = L'\0';
    StringCchPrintfW(np, ARRAYSIZE(np), L"%s\\shims;", root);
    if (FmPwshPath(pw, MAX_PATH))
    {
        PathRemoveFileSpecW(pw);
        StringCchCatW(np, ARRAYSIZE(np), pw);
        StringCchCatW(np, ARRAYSIZE(np), L";");
    }
    StringCchCatW(np, ARRAYSIZE(np), old);
    SetEnvironmentVariableW(L"PATH", np);
}

/* TRUE if the volume that holds path is NTFS (Scoop needs junctions). fs receives the name. */
BOOL FmVolumeIsNtfs(LPCWSTR path, LPWSTR fs, DWORD cchFs)
{
    WCHAR vol[4];
    WCHAR name[32];
    if (fs && cchFs) fs[0] = L'\0';
    if (!path[0] || path[1] != L':') return TRUE;       /* UNC / unknown: do not block */
    vol[0] = path[0]; vol[1] = L':'; vol[2] = L'\\'; vol[3] = L'\0';
    name[0] = L'\0';
    if (!GetVolumeInformationW(vol, NULL, 0, NULL, NULL, NULL, name, ARRAYSIZE(name)))
        return TRUE;                                     /* cannot tell: let the installer decide */
    if (fs && cchFs) StringCchCopyW(fs, cchFs, name);
    return !_wcsicmp(name, L"NTFS");
}

/* ---- minimal JSON reader (UTF-8) ---------------------------------------- */

typedef struct _JS { const char *p, *e; } JS;
typedef BOOL (*MEMBER_CB)(void *ctx, const char *key, JS *j, int depth);

#define JS_MAXDEPTH 32

static void JWs(JS *j)
{
    while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++;
}

static void PutUtf8(unsigned cp, char *out, size_t cap, size_t *n)
{
    char t[4];
    int l, i;
    if (cp < 0x80) { t[0] = (char)cp; l = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); l = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                             t[2] = (char)(0x80 | (cp & 0x3F)); l = 3; }
    else { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
           t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F)); l = 4; }
    if (!out) return;
    if (*n + l >= cap) return;             /* truncate, always keep room for the NUL */
    for (i = 0; i < l; i++) out[(*n)++] = t[i];
}

static int Hex4(const char *p, const char *e, unsigned *v)
{
    int i;
    *v = 0;
    if (e - p < 4) return 0;
    for (i = 0; i < 4; i++)
    {
        char c = p[i];
        *v <<= 4;
        if (c >= '0' && c <= '9') *v |= c - '0';
        else if (c >= 'a' && c <= 'f') *v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') *v |= c - 'A' + 10;
        else return 0;
    }
    return 1;
}

/* Parse a string at j->p ('"'). out == NULL skips it. */
static BOOL JStr(JS *j, char *out, size_t cap)
{
    size_t n = 0;
    if (j->p >= j->e || *j->p != '"') return FALSE;
    j->p++;
    while (j->p < j->e && *j->p != '"')
    {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '\\')
        {
            char x;
            if (j->p >= j->e) return FALSE;
            x = *j->p++;
            switch (x)
            {
                case 'n': PutUtf8('\n', out, cap, &n); break;
                case 't': PutUtf8('\t', out, cap, &n); break;
                case 'r': PutUtf8('\r', out, cap, &n); break;
                case 'b': PutUtf8('\b', out, cap, &n); break;
                case 'f': PutUtf8('\f', out, cap, &n); break;
                case 'u':
                {
                    unsigned u, lo;
                    if (!Hex4(j->p, j->e, &u)) return FALSE;
                    j->p += 4;
                    if (u >= 0xD800 && u < 0xDC00 && j->e - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u'
                        && Hex4(j->p + 2, j->e, &lo) && lo >= 0xDC00 && lo < 0xE000)
                    {
                        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                        j->p += 6;
                    }
                    PutUtf8(u, out, cap, &n);
                    break;
                }
                default: PutUtf8((unsigned char)x, out, cap, &n); break;   /* \" \\ \/ */
            }
        }
        else if (out)
        {
            if (n + 1 < cap) out[n++] = (char)c;
        }
    }
    if (j->p >= j->e) return FALSE;
    j->p++;                                   /* closing quote */
    if (out && cap) out[n < cap ? n : cap - 1] = '\0';
    return TRUE;
}

static BOOL JSkip(JS *j, int depth);

static BOOL JObject(JS *j, MEMBER_CB cb, void *ctx, int depth)
{
    char key[96];
    if (depth > JS_MAXDEPTH || j->p >= j->e || *j->p != '{') return FALSE;
    j->p++;
    JWs(j);
    if (j->p < j->e && *j->p == '}') { j->p++; return TRUE; }
    for (;;)
    {
        JWs(j);
        if (!JStr(j, key, sizeof(key))) return FALSE;
        JWs(j);
        if (j->p >= j->e || *j->p != ':') return FALSE;
        j->p++;
        JWs(j);
        if (cb ? !cb(ctx, key, j, depth + 1) : !JSkip(j, depth + 1)) return FALSE;
        JWs(j);
        if (j->p >= j->e) return FALSE;
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; return TRUE; }
        return FALSE;
    }
}

static BOOL JSkip(JS *j, int depth)
{
    if (depth > JS_MAXDEPTH) return FALSE;
    JWs(j);
    if (j->p >= j->e) return FALSE;
    switch (*j->p)
    {
        case '"': return JStr(j, NULL, 0);
        case '{': return JObject(j, NULL, NULL, depth);
        case '[':
            j->p++;
            JWs(j);
            if (j->p < j->e && *j->p == ']') { j->p++; return TRUE; }
            for (;;)
            {
                if (!JSkip(j, depth + 1)) return FALSE;
                JWs(j);
                if (j->p >= j->e) return FALSE;
                if (*j->p == ',') { j->p++; continue; }
                if (*j->p == ']') { j->p++; return TRUE; }
                return FALSE;
            }
        default:
        {
            const char *s = j->p;
            while (j->p < j->e && *j->p != ',' && *j->p != '}' && *j->p != ']'
                   && *j->p != ' ' && *j->p != '\t' && *j->p != '\r' && *j->p != '\n')
                j->p++;
            return j->p > s;                          /* number / true / false / null */
        }
    }
}

/* Is the value at j non-empty (string with text, or a non-empty array/object)? Consumes it. */
static BOOL JNonEmpty(JS *j, BOOL *nonEmpty)
{
    const char *s;
    JWs(j);
    s = j->p;
    if (!JSkip(j, 1)) return FALSE;
    JWs(j);
    if (j->p - s <= 2 && (s[0] == '"' || s[0] == '[' || s[0] == '{')) *nonEmpty = FALSE;
    else *nonEmpty = TRUE;
    return TRUE;
}

typedef struct _ARCHINFO { BOOL url, hash; } ARCHINFO;

typedef struct _MANI
{
    char version[64], desc[1024], homepage[512], license[128];
    BOOL topUrl, topHash;
    ARCHINFO a[3];                     /* 64bit, 32bit, arm64 */
} MANI;

static BOOL CbArchBlock(void *ctx, const char *key, JS *j, int depth)
{
    ARCHINFO *a = (ARCHINFO *)ctx;
    BOOL ne;
    UNREFERENCED_PARAMETER(depth);
    if (!strcmp(key, "url"))  { if (!JNonEmpty(j, &ne)) return FALSE; if (ne) a->url = TRUE; return TRUE; }
    if (!strcmp(key, "hash")) { if (!JNonEmpty(j, &ne)) return FALSE; if (ne) a->hash = TRUE; return TRUE; }
    return JSkip(j, depth);
}

static BOOL CbArch(void *ctx, const char *key, JS *j, int depth)
{
    MANI *m = (MANI *)ctx;
    int idx = !strcmp(key, "64bit") ? 0 : !strcmp(key, "32bit") ? 1 : !strcmp(key, "arm64") ? 2 : -1;
    if (idx >= 0 && j->p < j->e && *j->p == '{')
        return JObject(j, CbArchBlock, &m->a[idx], depth);
    return JSkip(j, depth);
}

static BOOL CbLicense(void *ctx, const char *key, JS *j, int depth)
{
    MANI *m = (MANI *)ctx;
    if (!strcmp(key, "identifier") && j->p < j->e && *j->p == '"')
        return JStr(j, m->license, sizeof(m->license));
    return JSkip(j, depth);
}

static BOOL CbTop(void *ctx, const char *key, JS *j, int depth)
{
    MANI *m = (MANI *)ctx;
    BOOL ne;
    BOOL isStr = (j->p < j->e && *j->p == '"');

    if (!strcmp(key, "version") && isStr)     return JStr(j, m->version, sizeof(m->version));
    if (!strcmp(key, "description") && isStr) return JStr(j, m->desc, sizeof(m->desc));
    if (!strcmp(key, "homepage") && isStr)    return JStr(j, m->homepage, sizeof(m->homepage));
    if (!strcmp(key, "license"))
    {
        if (isStr) return JStr(j, m->license, sizeof(m->license));
        if (j->p < j->e && *j->p == '{') return JObject(j, CbLicense, m, depth);
        return JSkip(j, depth);
    }
    if (!strcmp(key, "url"))  { if (!JNonEmpty(j, &ne)) return FALSE; if (ne) m->topUrl = TRUE; return TRUE; }
    if (!strcmp(key, "hash")) { if (!JNonEmpty(j, &ne)) return FALSE; if (ne) m->topHash = TRUE; return TRUE; }
    if (!strcmp(key, "architecture") && j->p < j->e && *j->p == '{')
        return JObject(j, CbArch, m, depth);
    return JSkip(j, depth);
}

static BOOL ParseManifest(const char *buf, size_t len, MANI *m)
{
    JS j;
    ZeroMemory(m, sizeof(*m));
    if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
    { buf += 3; len -= 3; }
    j.p = buf; j.e = buf + len;
    JWs(&j);
    if (!JObject(&j, CbTop, m, 0)) return FALSE;
    JWs(&j);
    return j.p == j.e;
}

/* 1 usable and hash present, 0 usable but nothing to verify the download against, -1 no
 * download for this machine (arm64-only, or no url at all). Same preference as Scoop on a
 * 64-bit machine: architecture.64bit, then the top level, then 32bit (WoW64). */
static int Classify(const MANI *m)
{
    if (m->a[0].url)  return (m->a[0].hash || m->topHash) ? 1 : 0;
    if (m->topUrl)    return m->topHash ? 1 : 0;
    if (m->a[1].url)  return (m->a[1].hash || m->topHash) ? 1 : 0;
    return -1;
}

static BOOL ReadAll(LPCWSTR path, char **out, size_t *len)
{
    HANDLE h;
    LARGE_INTEGER sz;
    DWORD got = 0;
    char *b;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (4 << 20))
    { CloseHandle(h); return FALSE; }
    b = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)sz.QuadPart + 1);
    if (!b) { CloseHandle(h); return FALSE; }
    if (!ReadFile(h, b, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart)
    { HeapFree(GetProcessHeap(), 0, b); CloseHandle(h); return FALSE; }
    CloseHandle(h);
    b[got] = '\0';
    *out = b; *len = got;
    return TRUE;
}

static void Utf8ToW(const char *s, LPWSTR out, int cch)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cch);
    if (n <= 0) { out[0] = L'\0'; return; }
    out[cch - 1] = L'\0';
    for (; *out; out++)
        if (*out == L'\r' || *out == L'\n' || *out == L'\t') *out = L' ';
}

/* ---- import -------------------------------------------------------------- */

static BOOL BucketManifestDir(LPCWSTR root, LPCWSTR bucket, LPWSTR out, DWORD cch)
{
    WCHAR b[MAX_PATH];
    StringCchPrintfW(b, MAX_PATH, L"%s\\buckets\\%s\\bucket", root, bucket);
    if (GetFileAttributesW(b) != INVALID_FILE_ATTRIBUTES)
        return SUCCEEDED(StringCchCopyW(out, cch, b));
    StringCchPrintfW(b, MAX_PATH, L"%s\\buckets\\%s", root, bucket);
    if (GetFileAttributesW(b) != INVALID_FILE_ATTRIBUTES)
        return SUCCEEDED(StringCchCopyW(out, cch, b));
    return FALSE;
}

/* Id for an app: its own name, unless that is taken by an image catalog entry or another
 * bucket's entry, then <bucket>-<app>. */
static BOOL ChooseId(LPCWSTR bucket, LPCWSTR app, LPCWSTR catDir, LPCWSTR swDir, LPWSTR id)
{
    WCHAR p[MAX_PATH], b[64];
    BOOL clash = FALSE;

    StringCchCopyW(id, FM_MAX_ID, app);
    StringCchPrintfW(p, MAX_PATH, L"%s\\%s.ini", catDir, app);
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) clash = TRUE;
    if (!clash)
    {
        StringCchPrintfW(p, MAX_PATH, L"%s\\%s.ini", swDir, app);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES)
        {
            GetPrivateProfileStringW(L"Scoop", L"Bucket", L"", b, ARRAYSIZE(b), p);
            if (_wcsicmp(b, bucket)) clash = TRUE;
        }
    }
    if (clash)
    {
        if (FAILED(StringCchPrintfW(id, FM_MAX_ID, L"%s-%s", bucket, app))) return FALSE;
    }
    return TRUE;
}

static void CleanBucket(LPCWSTR swDir, LPCWSTR bucket)
{
    WCHAR glob[MAX_PATH], f[MAX_PATH], v[64];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    StringCchPrintfW(glob, MAX_PATH, L"%s\\*.ini", swDir);
    h = FindFirstFileW(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        StringCchPrintfW(f, MAX_PATH, L"%s\\%s", swDir, fd.cFileName);
        GetPrivateProfileStringW(L"Install", L"Provider", L"", v, ARRAYSIZE(v), f);
        if (_wcsicmp(v, L"scoop")) continue;
        GetPrivateProfileStringW(L"Scoop", L"Bucket", L"", v, ARRAYSIZE(v), f);
        if (!_wcsicmp(v, bucket)) DeleteFileW(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

typedef struct _IMPSTAT { int written, skipped, nohash, noarch, invalid; } IMPSTAT;

static int ImportOneBucket(LPCWSTR root, LPCWSTR bucket, LPCWSTR dirOverride, DWORD flags, int limit,
                           IMPSTAT *st, FILE *log)
{
    WCHAR dir[MAX_PATH], catDir[MAX_PATH], swDir[MAX_PATH], glob[MAX_PATH], file[MAX_PATH], outFile[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int before = st->written;

    if (!FmScoopValidBucket(bucket))
    {
        ERRF(log, L"error: '%s' is not a valid bucket name\n", bucket);
        return -1;
    }
    if (!IsOfficialBucket(bucket) && !(flags & FM_SCOOP_TRUST))
    {
        ERRF(log, L"error: bucket '%s' is not an official Scoop bucket (main, extras). A bucket's manifests can run "
                  L"PowerShell as administrator, so importing a third-party bucket needs your explicit consent: "
                  L"add /trust if you added it to Scoop yourself and trust its maintainers.\n", bucket);
        return -1;
    }
    if (dirOverride && dirOverride[0])
        StringCchCopyW(dir, MAX_PATH, dirOverride);
    else if (!BucketManifestDir(root, bucket, dir, MAX_PATH))
    {
        WCHAR probe[MAX_PATH];
        StringCchPrintfW(probe, MAX_PATH, L"%s\\apps\\scoop", root);
        if (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES)
            ERRF(log, L"error: Scoop is not installed (no %s). Install it first: featmgr /install scoop\n", probe);
        else
            ERRF(log, L"error: bucket '%s' is not added to Scoop (%s\\buckets\\%s is missing). Add it yourself, "
                      L"then import again: scoop bucket add %s   (needs git: featmgr /install git)\n",
                 bucket, root, bucket, bucket);
        return -1;
    }
    PathRemoveBackslashW(dir);
    if (!FmGetCatalogDir(catDir, MAX_PATH) || !FmGetSoftwareCatalogDir(swDir, MAX_PATH)) return -1;
    SHCreateDirectoryExW(NULL, swDir, NULL);
    if (flags & FM_SCOOP_CLEAN) CleanBucket(swDir, bucket);

    StringCchPrintfW(glob, MAX_PATH, L"%s\\*.json", dir);
    h = FindFirstFileW(glob, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        ERRF(log, L"error: no manifests (*.json) in '%s'\n", dir);
        return -1;
    }
    do
    {
        WCHAR app[MAX_PATH], id[FM_MAX_ID], name[FM_MAX_NAME], ver[FM_MAX_VER], desc[FM_MAX_DESC + 128];
        WCHAR hp[512], lic[128];
        FM_OUTBUF o = { NULL, 0, 0, FALSE };
        MANI *m;
        char *buf = NULL;
        size_t len = 0;
        int cls;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (limit > 0 && st->written - before >= limit) break;
        StringCchCopyW(app, MAX_PATH, fd.cFileName);
        PathRemoveExtensionW(app);
        if (!FmScoopValidApp(app))
        {
            st->skipped++; st->invalid++;
            LOGF(log, L"skip: '%s' (unusable app name)\n", fd.cFileName);
            continue;
        }
        StringCchPrintfW(file, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        if (!ReadAll(file, &buf, &len))
        {
            st->skipped++; st->invalid++;
            LOGF(log, L"skip: %s (cannot read the manifest)\n", app);
            continue;
        }
        m = (MANI *)HeapAlloc(GetProcessHeap(), 0, sizeof(*m));
        if (!m) { HeapFree(GetProcessHeap(), 0, buf); break; }
        if (!ParseManifest(buf, len, m))
        {
            st->skipped++; st->invalid++;
            LOGF(log, L"skip: %s (malformed JSON)\n", app);
            HeapFree(GetProcessHeap(), 0, m); HeapFree(GetProcessHeap(), 0, buf);
            continue;
        }
        HeapFree(GetProcessHeap(), 0, buf);
        cls = Classify(m);
        if (cls < 0)
        {
            st->skipped++; st->noarch++;
            LOGF(log, L"skip: %s (no download for this architecture)\n", app);
            HeapFree(GetProcessHeap(), 0, m);
            continue;
        }
        if (cls == 0 && !(flags & FM_SCOOP_UNVERIFIED))
        {
            st->skipped++; st->nohash++;
            LOGF(log, L"skip: %s (manifest has no hash, Scoop could not verify the download; /unverified to include)\n", app);
            HeapFree(GetProcessHeap(), 0, m);
            continue;
        }
        if (!ChooseId(bucket, app, catDir, swDir, id))
        {
            st->skipped++; st->invalid++;
            LOGF(log, L"skip: %s (id too long)\n", app);
            HeapFree(GetProcessHeap(), 0, m);
            continue;
        }

        StringCchCopyW(name, FM_MAX_NAME, app);
        Utf8ToW(m->version, ver, FM_MAX_VER);
        Utf8ToW(m->homepage, hp, ARRAYSIZE(hp));
        Utf8ToW(m->license, lic, ARRAYSIZE(lic));
        desc[0] = L'\0';
        if (cls == 0) StringCchCopyW(desc, ARRAYSIZE(desc), L"[No hash in the manifest: Scoop cannot verify this download] ");
        if (m->desc[0])
        {
            WCHAR d[FM_MAX_DESC];
            Utf8ToW(m->desc, d, FM_MAX_DESC);
            StringCchCatW(desc, ARRAYSIZE(desc), d);
        }
        else
        {
            WCHAR d[96];
            StringCchPrintfW(d, ARRAYSIZE(d), L"Scoop app from the %s bucket.", bucket);
            StringCchCatW(desc, ARRAYSIZE(desc), d);
        }
        desc[FM_MAX_DESC - 1] = L'\0';
        HeapFree(GetProcessHeap(), 0, m);

        FmOutLine(&o, L"", L"; Generated by featmgr /import-scoop from a Scoop bucket manifest. Do not edit;");
        FmOutLine(&o, L"", L"; regenerate with: featmgr /import-scoop");
        FmOutLine(&o, L"", L"[Feature]");
        FmOutLine(&o, L"Id", id);
        FmOutLine(&o, L"Name", name);
        FmOutLine(&o, L"Description", desc);
        FmOutLine(&o, L"Version", ver[0] ? ver : L"-");
        FmOutLine(&o, L"Tier", L"software");
        FmOutLine(&o, L"Dependencies", L"scoop");
        FmOutLine(&o, L"", L"");
        FmOutLine(&o, L"", L"[Install]");
        FmOutLine(&o, L"Provider", L"scoop");
        FmOutLine(&o, L"", L"");
        FmOutLine(&o, L"", L"[Scoop]");
        FmOutLine(&o, L"App", app);
        FmOutLine(&o, L"Bucket", bucket);
        if (hp[0]) FmOutLine(&o, L"Homepage", hp);
        if (lic[0]) FmOutLine(&o, L"License", lic);
        FmOutLine(&o, L"Verified", cls == 1 ? L"1" : L"0");

        StringCchPrintfW(outFile, MAX_PATH, L"%s\\%s.ini", swDir, id);
        if (!o.fail && FmWriteManifest(outFile, &o))
            st->written++;
        else
        {
            st->skipped++; st->invalid++;
            LOGF(log, L"skip: %s (cannot write %s, %lu)\n", app, outFile, GetLastError());
        }
        if (o.buf) HeapFree(GetProcessHeap(), 0, o.buf);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    LOGF(log, L"bucket '%s': %d imported from %s\n", bucket, st->written - before, dir);
    return st->written - before;
}

int FmImportScoop(LPCWSTR bucket, LPCWSTR dirOverride, DWORD flags, int limit, int *skipped, FILE *log)
{
    WCHAR root[MAX_PATH];
    IMPSTAT st = { 0, 0, 0, 0, 0 };
    BOOL all = (!bucket || !bucket[0] || !wcscmp(bucket, L"*"));
    int rc = 0;

    if (skipped) *skipped = 0;
    if (!FM_SCOOP_SUPPORTED)
    {
        ERRF(log, L"error: Scoop needs 64-bit PowerShell 7, which is not available on this architecture (x86)\n");
        return -2;
    }
    if (!FmScoopRoot(root, MAX_PATH)) return -1;

    if (all)
    {
        static const LPCWSTR official[] = { L"main", L"extras" };
        int i, found = 0;
        for (i = 0; i < 2; i++)
        {
            WCHAR d[MAX_PATH];
            if (!BucketManifestDir(root, official[i], d, MAX_PATH)) continue;
            found++;
            if (ImportOneBucket(root, official[i], NULL, flags, limit, &st, log) < 0) rc = -1;
        }
        if (!found)
        {
            WCHAR probe[MAX_PATH];
            StringCchPrintfW(probe, MAX_PATH, L"%s\\apps\\scoop", root);
            if (GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES)
                ERRF(log, L"error: Scoop is not installed (no %s). Install it first: featmgr /install scoop\n", probe);
            else
                ERRF(log, L"error: no Scoop bucket found under %s\\buckets\n", root);
            return -1;
        }
    }
    else if (ImportOneBucket(root, bucket, dirOverride, flags, limit, &st, log) < 0)
        rc = -1;

    if (skipped) *skipped = st.skipped;
    LOGF(log, L"imported %d Scoop app(s) (%d skipped: %d without hash, %d without a download for this machine, "
              L"%d invalid)\n", st.written, st.skipped, st.nohash, st.noarch, st.invalid);
    return (rc < 0 && !st.written) ? -1 : st.written;
}

/* featmgr /scoop-buckets */
int FmScoopListBuckets(FILE *log)
{
    WCHAR root[MAX_PATH], glob[MAX_PATH], dir[MAX_PATH], jg[MAX_PATH];
    WIN32_FIND_DATAW fd, jf;
    HANDLE h, hj;
    int n = 0;

    if (!FmScoopRoot(root, MAX_PATH)) return -1;
    StringCchPrintfW(glob, MAX_PATH, L"%s\\buckets\\*", root);
    h = FindFirstFileW(glob, &fd);
    LOGF(log, L"Scoop root: %s\n", root);
    LOGF(log, L"%-16s %-9s %-10s\n", L"BUCKET", L"OFFICIAL", L"MANIFESTS");
    if (h == INVALID_HANDLE_VALUE)
    {
        LOGF(log, L"(no buckets: Scoop is not installed or has no bucket directory)\n");
        return 0;
    }
    do
    {
        int cnt = 0;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (BucketManifestDir(root, fd.cFileName, dir, MAX_PATH))
        {
            StringCchPrintfW(jg, MAX_PATH, L"%s\\*.json", dir);
            hj = FindFirstFileW(jg, &jf);
            if (hj != INVALID_HANDLE_VALUE)
            {
                do { if (!(jf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) cnt++; } while (FindNextFileW(hj, &jf));
                FindClose(hj);
            }
        }
        LOGF(log, L"%-16s %-9s %-10d\n", fd.cFileName, IsOfficialBucket(fd.cFileName) ? L"yes" : L"no", cnt);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
