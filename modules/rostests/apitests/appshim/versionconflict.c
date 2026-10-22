/*
 * PROJECT:     ReactOS apphelp shim tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Two version layers at once: the first one answers every question about the version
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The test starts itself as a child under the layers named in __COMPAT_LAYER. A child under two version
 * layers has to tell what a child under the first of them alone tells, in every call that reports the version.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wine/test.h>

typedef VOID (WINAPI *RTLGETNTVERSIONNUMBERS)(DWORD *, DWORD *, DWORD *);

/* What a process sees as its Windows version, one text line */
static void describe_version(char *out, size_t size)
{
    OSVERSIONINFOEXW info;
    OSVERSIONINFOEXW check;
    RTLGETNTVERSIONNUMBERS pRtlGetNtVersionNumbers;
    DWORD major = 0, minor = 0, build = 0;
    ULONGLONG mask;
    BOOL v61, v62, v51;
    WCHAR product[128] = L"", current[32] = L"";
    DWORD len;
    HKEY key;

    memset(&info, 0, sizeof(info));
    info.dwOSVersionInfoSize = sizeof(info);
    GetVersionExW((OSVERSIONINFOW *)&info);

    pRtlGetNtVersionNumbers = (RTLGETNTVERSIONNUMBERS)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetNtVersionNumbers");
    if (pRtlGetNtVersionNumbers)
        pRtlGetNtVersionNumbers(&major, &minor, &build);

    memset(&check, 0, sizeof(check));
    check.dwOSVersionInfoSize = sizeof(check);
    mask = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);
    mask = VerSetConditionMask(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
    check.dwMajorVersion = 6;
    check.dwMinorVersion = 1;
    v61 = VerifyVersionInfoW(&check, VER_MAJORVERSION | VER_MINORVERSION, mask);
    check.dwMinorVersion = 2;
    v62 = VerifyVersionInfoW(&check, VER_MAJORVERSION | VER_MINORVERSION, mask);
    check.dwMajorVersion = 5;
    check.dwMinorVersion = 1;
    v51 = VerifyVersionInfoW(&check, VER_MAJORVERSION | VER_MINORVERSION, mask);

    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_QUERY_VALUE, &key))
    {
        len = sizeof(product) - sizeof(WCHAR);
        RegQueryValueExW(key, L"ProductName", NULL, NULL, (BYTE *)product, &len);
        len = sizeof(current) - sizeof(WCHAR);
        RegQueryValueExW(key, L"CurrentVersion", NULL, NULL, (BYTE *)current, &len);
        RegCloseKey(key);
    }

    _snprintf(out, size, "GetVersionEx %lu.%lu | RtlGetNtVersionNumbers %lu.%lu | >=6.1 %d >=6.2 %d >=5.1 %d | CurrentVersion %ls | ProductName %ls",
              info.dwMajorVersion, info.dwMinorVersion, major, minor, v61, v62, v51, current, product);
    out[size - 1] = 0;
}

/* Runs the child under the layers and returns its description, FALSE if it cannot be run */
static BOOL run_child(const char *layers, char *out, size_t size)
{
    char exe[MAX_PATH], tmp_dir[MAX_PATH], file[MAX_PATH], cmd[2 * MAX_PATH + 64];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD exit_code = 1;
    BOOL created;
    FILE *f;

    GetModuleFileNameA(NULL, exe, sizeof(exe));
    GetTempPathA(sizeof(tmp_dir), tmp_dir);
    GetTempFileNameA(tmp_dir, "vcf", 0, file);
    sprintf(cmd, "\"%s\" versionconflict child \"%s\"", exe, file);

    SetEnvironmentVariableA("__COMPAT_LAYER", layers);
    created = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    SetEnvironmentVariableA("__COMPAT_LAYER", NULL);
    ok(created, "CreateProcess failed with %lu\n", GetLastError());
    if (!created)
    {
        DeleteFileA(file);
        return FALSE;
    }

    ok(WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0, "The child does not finish\n");
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    out[0] = 0;
    f = fopen(file, "r");
    if (f)
    {
        if (fgets(out, size, f))
            out[strcspn(out, "\r\n")] = 0;
        fclose(f);
    }
    DeleteFileA(file);
    ok(exit_code == 0 && out[0], "The child under '%s' gave exit code %lu and '%s'\n", layers, exit_code, out);
    return exit_code == 0 && out[0];
}

static char g_native[256];

static void test_pair(const char *first, const char *second)
{
    char both[64], reverse[64], alone_first[256], alone_second[256], result[256];

    if (!run_child(first, alone_first, sizeof(alone_first)) || !run_child(second, alone_second, sizeof(alone_second)))
        return;

    /* A layer that does nothing to this process (64-bit Windows has no layer for Windows XP) cannot take the lead */
    if (!strcmp(alone_first, g_native) || !strcmp(alone_second, g_native))
    {
        skip("%s or %s does not change the answers of this process: '%s'\n", first, second, g_native);
        return;
    }

    /* The layers must differ, or the test says nothing (the layers are not applied to this process) */
    if (!strcmp(alone_first, alone_second))
    {
        skip("%s and %s give the same answers: '%s'\n", first, second, alone_first);
        return;
    }

    sprintf(both, "%s %s", first, second);
    if (run_child(both, result, sizeof(result)))
        ok(!strcmp(result, alone_first), "Layers '%s': '%s', but '%s' alone gives '%s'\n", both, result, first, alone_first);

    sprintf(reverse, "%s %s", second, first);
    if (run_child(reverse, result, sizeof(result)))
        ok(!strcmp(result, alone_second), "Layers '%s': '%s', but '%s' alone gives '%s'\n", reverse, result, second, alone_second);
}

START_TEST(versionconflict)
{
    char **argv;
    int argc = winetest_get_mainargs(&argv);

    if (argc >= 4 && !strcmp(argv[2], "child"))
    {
        char line[400];
        FILE *f;

        describe_version(line, sizeof(line));
        f = fopen(argv[3], "w");
        if (!f)
            ExitProcess(2);
        fprintf(f, "%s\n", line);
        fclose(f);
        ExitProcess(0);
    }

    /* Windows 10 and ReactOS give the answers of the first of two version layers; Windows Server 2008 R2 mixes them
       (measured with VISTASP2 and WIN7RTM: GetVersionEx follows the last layer, the registry values the first) */
    if (!is_reactos() && GetNTVersion() < 0x0A00)
    {
        skip("This Windows answers with a mixture when two version layers are named\n");
        return;
    }

    if (!run_child(NULL, g_native, sizeof(g_native)))
        return;

    test_pair("WINXPSP3", "WIN7RTM");
    test_pair("VISTASP2", "WIN7RTM");
    test_pair("WINXPSP3", "VISTASP2");
    test_pair("WINXPSP3", "WIN8RTM");
    test_pair("WIN7RTM", "WIN81RTM");
    if (is_reactos())
        test_pair("WINXPSP3", "WIN10RTM");
}
