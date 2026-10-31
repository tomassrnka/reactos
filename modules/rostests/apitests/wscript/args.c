/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Tests for cscript.exe command line splitting and saved options (//S)
 */

#include <apitest.h>
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>

#define WSH_SETTINGS_KEY "Software\\Microsoft\\Windows Script Host\\Settings"

static BOOL makefile(LPWSTR path, SIZE_T cchpath, LPCSTR ext, LPCSTR contents)
{
    UINT cch = GetTempPathW(cchpath, path);
    UINT16 i = 0;

    if (!cch || cch > cchpath)
        return FALSE;
    while (++i)
    {
        HANDLE hFile;
        DWORD written;

        if (_snwprintf(path + cch, cchpath - cch, L"~wa%u.%hs", i, ext) >= cchpath - cch)
            return FALSE;
        hFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, 0, NULL);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            BOOL succ = WriteFile(hFile, contents, strlen(contents), &written, NULL);
            CloseHandle(hFile);
            return succ;
        }
    }
    return FALSE;
}

/* Runs cscript.exe with the exact command line "cscript.exe <tail>"; stdout goes to outfile when given. */
static DWORD run_cscript(LPCWSTR tail, LPCWSTR outfile)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR cmd[2048];
    DWORD code = ERROR_INTERNAL_ERROR;
    HANDLE hOut = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };

    if (_snwprintf(cmd, _countof(cmd), L"cscript.exe %s", tail) >= (int)_countof(cmd))
        return ERROR_BUFFER_OVERFLOW;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (outfile)
    {
        hOut = CreateFileW(outfile, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, NULL);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = hOut;
        si.hStdError = hOut;
    }
    if (CreateProcessW(NULL, cmd, NULL, NULL, outfile != NULL, 0, NULL, NULL, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 60000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    else
    {
        code = GetLastError();
    }
    if (hOut != INVALID_HANDLE_VALUE)
        CloseHandle(hOut);
    return code;
}

static void append_js_string(char *dst, SIZE_T cch, const char *s)
{
    SIZE_T n = strlen(dst);

    dst[n++] = '\'';
    for (; *s && n + 3 < cch; ++s)
    {
        if (*s == '\\' || *s == '\'')
            dst[n++] = '\\';
        dst[n++] = *s;
    }
    dst[n++] = '\'';
    dst[n] = 0;
}

/* Runs a script that quits with 0 only when WScript.Arguments equals expected[] */
static DWORD check_args(LPCWSTR tail, const char *const *expected, int count)
{
    static const char tmpl[] =
        "var a = WScript.Arguments;\r\n"
        "if (a.Count() != e.length) WScript.Quit(100 + a.Count());\r\n"
        "for (var i = 0; i < e.length; i++) if (a(i) != e[i]) WScript.Quit(i + 1);\r\n"
        "WScript.Quit(0);\r\n";
    char script[2048] = "var e = [";
    WCHAR file[MAX_PATH], cmd[1024];
    DWORD code;
    int i;

    for (i = 0; i < count; i++)
    {
        if (i)
            strcat(script, ",");
        append_js_string(script, sizeof(script), expected[i]);
    }
    strcat(script, "];\r\n");
    strcat(script, tmpl);
    if (!makefile(file, _countof(file), "js", script))
        return ERROR_FILE_NOT_FOUND;
    _snwprintf(cmd, _countof(cmd), L"//nologo \"%s\" %s", file, tail);
    code = run_cscript(cmd, NULL);
    DeleteFileW(file);
    return code;
}

#define CHECK(tail, ...) do { \
        static const char *const exp_[] = { __VA_ARGS__ }; \
        DWORD code_ = check_args(tail, exp_, _countof(exp_)); \
        ok(code_ == 0, "%ls: script exited with %lu\n", tail, code_); \
    } while (0)

static void test_argument_splitting(void)
{
    /* What PowerShell builds for 'a"b c"d' with PSNativeCommandArgumentPassing=Windows */
    CHECK(L"\"a\"b c\"d\" \"a\"b c\"d\" \"ab cd\" \"a'b c'd\"", "ab", "cd", "ab", "cd", "ab cd", "a'b c'd");
    CHECK(L"a\"b c\"d e", "ab cd", "e");
    CHECK(L"\"a b\" c", "a b", "c");
    /* Backslashes are never an escape character: a quote always toggles quoting */
    CHECK(L"\"a\\\"b c\" d", "a\\b", "c d");
    CHECK(L"\"a\\\\\" b", "a\\\\", "b");
    CHECK(L"\"\" x", "", "x");
    CHECK(L"\"a\"\"b c\" d", "ab c", "d");
    CHECK(L"  a \t b  ", "a", "b");
}

static BOOL getdw(const char *name, DWORD *out)
{
    DWORD size = sizeof(*out);

    return !SHGetValueA(HKEY_CURRENT_USER, WSH_SETTINGS_KEY, name, NULL, out, &size);
}

static void restoredw(const char *name, BOOL had, DWORD v)
{
    if (had)
        SHSetValueA(HKEY_CURRENT_USER, WSH_SETTINGS_KEY, name, REG_DWORD, &v, sizeof(v));
    else
        SHDeleteValueA(HKEY_CURRENT_USER, WSH_SETTINGS_KEY, name);
}

static BOOL logo_shown(LPCWSTR script)
{
    WCHAR out[MAX_PATH], cmd[MAX_PATH * 2];
    char buf[256] = "";
    DWORD got = 0;
    HANDLE hFile;
    BOOL shown;

    if (!makefile(out, _countof(out), "out", ""))
        return FALSE;
    _snwprintf(cmd, _countof(cmd), L"\"%s\"", script);
    ok(run_cscript(cmd, out) == 0, "script failed\n");
    hFile = CreateFileW(out, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        ReadFile(hFile, buf, sizeof(buf) - 1, &got, NULL);
        CloseHandle(hFile);
    }
    buf[got] = 0;
    DeleteFileW(out);
    shown = strstr(buf, "Script Host") != NULL;
    ok(strstr(buf, "marker") != NULL, "No script output: '%s'\n", buf);
    return shown;
}

static void test_save_options(void)
{
    DWORD oldlogo = 0, oldtimeout = 0, dw;
    BOOL hadlogo = getdw("DisplayLogo", &oldlogo);
    BOOL hadtimeout = getdw("Timeout", &oldtimeout);
    WCHAR script[MAX_PATH];

    if (!makefile(script, _countof(script), "js", "WScript.Echo('marker');\r\n"))
    {
        skip("Unable to create script\n");
        return;
    }

    ok(run_cscript(L"//nologo //s", NULL) == 0, "//S failed\n");
    ok(getdw("DisplayLogo", &dw) && dw == 0, "DisplayLogo not saved as 0\n");
    ok(getdw("Timeout", &dw) && dw == 0, "Timeout not saved\n");
    ok(!logo_shown(script), "Logo shown after //nologo //s\n");

    ok(run_cscript(L"//logo //s", NULL) == 0, "//S failed\n");
    ok(getdw("DisplayLogo", &dw) && dw == 1, "DisplayLogo not saved as 1\n");
    ok(logo_shown(script), "Logo not shown after //logo //s\n");

    DeleteFileW(script);
    restoredw("DisplayLogo", hadlogo, oldlogo);
    restoredw("Timeout", hadtimeout, oldtimeout);
}

START_TEST(args)
{
    test_argument_splitting();
    test_save_options();
}
