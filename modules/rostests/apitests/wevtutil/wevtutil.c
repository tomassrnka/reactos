/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for wevtutil.exe: install and uninstall of a manifest, enum-logs, enum-publishers,
 *              get-log and query-events, and what wevtapi shows of the installed publisher and channels
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected output, the exit codes and the registry that wevtutil im writes are those that Windows 10 22H2
 * has (the test passes there). The test manifest is installed, looked at and removed again; nothing stays.
 */

#define WIN32_NO_STATUS
#include <apitest.h>
#include <windows.h>
#include <winevt.h>
#include <strsafe.h>

#define PUBLISHER  L"NitroWevtutilApitest"
#define GUID_LOWER L"{7e3a1f52-9b64-4c0d-8a21-6d5e4f3b2a10}"
#define GUID_UPPER L"{7E3A1F52-9B64-4C0D-8A21-6D5E4F3B2A10}"
#define CH_ADMIN   PUBLISHER L"/Admin"
#define CH_OPER    PUBLISHER L"/Operational"
#define CH_DEBUG   PUBLISHER L"/Debug"
#define WINEVT     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WINEVT"

static const char s_Manifest[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
    "<instrumentationManifest xmlns=\"http://schemas.microsoft.com/win/2004/08/events\" "
    "xmlns:win=\"http://manifests.microsoft.com/win/2004/08/windows/events\" "
    "xmlns:xs=\"http://www.w3.org/2001/XMLSchema\">\r\n"
    " <instrumentation><events>\r\n"
    "  <provider name=\"NitroWevtutilApitest\" guid=\"{7E3A1F52-9B64-4C0D-8A21-6D5E4F3B2A10}\" symbol=\"NitroApi\" "
    "resourceFileName=\"%windir%\\system32\\wevtutil.exe\" messageFileName=\"%windir%\\system32\\wevtutil.exe\">\r\n"
    "   <channels>\r\n"
    "    <importChannel name=\"System\" chid=\"C1\"/>\r\n"
    "    <channel name=\"NitroWevtutilApitest/Admin\" chid=\"CA\" symbol=\"NitroApi_Admin\" type=\"Admin\" enabled=\"true\"/>\r\n"
    "    <channel name=\"NitroWevtutilApitest/Operational\" chid=\"CO\" value=\"20\" type=\"Operational\" enabled=\"true\"/>\r\n"
    "    <channel name=\"NitroWevtutilApitest/Debug\" chid=\"CD\" type=\"Debug\" enabled=\"false\" isolation=\"Custom\" "
    "access=\"O:BAG:BAD:(A;;0x2;;;BU)(A;;0xf0007;;;SY)(A;;0x7;;;BA)\"/>\r\n"
    "   </channels>\r\n"
    "   <events><event value=\"1\" level=\"win:Informational\" channel=\"CO\" symbol=\"E1\"/></events>\r\n"
    "  </provider>\r\n"
    " </events></instrumentation>\r\n"
    "</instrumentationManifest>\r\n";

static WCHAR s_SysDir[MAX_PATH];
static WCHAR s_Tool[MAX_PATH];
static WCHAR s_ManifestPath[MAX_PATH];

/* Runs wevtutil.exe with the arguments, gives its output (standard output and error, one text) and its exit code */
static DWORD Run(PCWSTR pszArgs, WCHAR **ppszOut)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    HANDLE hRead, hWrite;
    WCHAR szCmd[4096];
    DWORD dwExit = (DWORD)-1, cap = 0, len = 0;
    char *pOut = NULL;
    WCHAR *pw = NULL;
    int n;

    *ppszOut = NULL;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
    {
        ok(0, "CreatePipe failed, error %lu\n", GetLastError());
        return dwExit;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    StringCchPrintfW(szCmd, _countof(szCmd), L"\"%s\" %s", s_Tool, pszArgs);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = hWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (!CreateProcessW(NULL, szCmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        ok(0, "CreateProcess(%ls) failed, error %lu\n", szCmd, GetLastError());
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return dwExit;
    }
    CloseHandle(hWrite);
    for (;;)
    {
        DWORD dwRead;

        if (len + 4096 > cap)
        {
            char *p;

            if (cap >= 16 * 1024 * 1024) break;
            cap = cap ? cap * 2 : 65536;
            p = realloc(pOut, cap);
            if (!p) break;
            pOut = p;
        }
        if (!ReadFile(hRead, pOut + len, cap - len, &dwRead, NULL) || !dwRead) break;
        len += dwRead;
    }
    CloseHandle(hRead);
    WaitForSingleObject(pi.hProcess, 120000);
    GetExitCodeProcess(pi.hProcess, &dwExit);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    n = pOut ? MultiByteToWideChar(CP_OEMCP, 0, pOut, len, NULL, 0) : 0;
    pw = malloc((n + 1) * sizeof(WCHAR));
    if (pw)
    {
        if (n) MultiByteToWideChar(CP_OEMCP, 0, pOut, len, pw, n);
        pw[n] = 0;
    }
    free(pOut);
    *ppszOut = pw;
    return dwExit;
}

/* The line with the text in it, for a list: the text between line ends equal to the name */
static BOOL HasLine(PCWSTR pszOut, PCWSTR pszLine)
{
    SIZE_T cch = wcslen(pszLine);
    PCWSTR p = pszOut;

    while (p && *p)
    {
        PCWSTR pEnd = wcspbrk(p, L"\r\n");
        SIZE_T len = pEnd ? (SIZE_T)(pEnd - p) : wcslen(p);

        if (len == cch && !wcsncmp(p, pszLine, cch)) return TRUE;
        p += len;
        while (*p == '\r' || *p == '\n') p++;
    }
    return FALSE;
}

static BOOL Contains(PCWSTR pszOut, PCWSTR pszText)
{
    return pszOut && wcsstr(pszOut, pszText) != NULL;
}

static BOOL StartsWith(PCWSTR pszOut, PCWSTR pszText)
{
    return pszOut && !wcsncmp(pszOut, pszText, wcslen(pszText));
}

/* Whether wevtapi lists the name, from EvtNextChannelPath or EvtNextPublisherId. The service may need a moment. */
static BOOL ApiLists(BOOL bChannel, PCWSTR pszName)
{
    int iTry;

    for (iTry = 0; iTry < 20; iTry++)
    {
        EVT_HANDLE hEnum = bChannel ? EvtOpenChannelEnum(NULL, 0) : EvtOpenPublisherEnum(NULL, 0);
        BOOL bFound = FALSE;

        if (!hEnum) return FALSE;
        for (;;)
        {
            WCHAR sz[512];
            DWORD dwUsed;
            BOOL bOk = bChannel ? EvtNextChannelPath(hEnum, _countof(sz), sz, &dwUsed) : EvtNextPublisherId(hEnum, _countof(sz), sz, &dwUsed);

            if (!bOk) break;
            if (!_wcsicmp(sz, pszName)) bFound = TRUE;
        }
        EvtClose(hEnum);
        if (bFound) return TRUE;
        Sleep(250);
    }
    return FALSE;
}

static BOOL KeyExists(PCWSTR pszSub)
{
    HKEY hKey;
    WCHAR szPath[512];

    StringCchPrintfW(szPath, _countof(szPath), L"%s\\%s", WINEVT, pszSub);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPath, 0, KEY_READ, &hKey)) return FALSE;
    RegCloseKey(hKey);
    return TRUE;
}

/* Checks a string value (NULL name is the default value) */
static void CheckString(PCWSTR pszSub, PCWSTR pszName, DWORD dwType, PCWSTR pszExpected)
{
    HKEY hKey;
    WCHAR szPath[512], szValue[1024];
    DWORD dwActual = 0, cb = sizeof(szValue) - sizeof(WCHAR);
    LSTATUS status;

    StringCchPrintfW(szPath, _countof(szPath), L"%s\\%s", WINEVT, pszSub);
    status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPath, 0, KEY_READ, &hKey);
    ok(status == ERROR_SUCCESS, "RegOpenKeyEx(%ls) returned %ld\n", pszSub, status);
    if (status) return;
    memset(szValue, 0, sizeof(szValue));
    status = RegQueryValueExW(hKey, pszName, NULL, &dwActual, (BYTE *)szValue, &cb);
    ok(status == ERROR_SUCCESS, "%ls: value %ls returned %ld\n", pszSub, pszName ? pszName : L"(default)", status);
    if (!status)
    {
        ok(dwActual == dwType, "%ls: value %ls has type %lu, expected %lu\n", pszSub, pszName ? pszName : L"(default)", dwActual, dwType);
        ok(!_wcsicmp(szValue, pszExpected), "%ls: value %ls is '%ls', expected '%ls'\n", pszSub, pszName ? pszName : L"(default)", szValue, pszExpected);
    }
    RegCloseKey(hKey);
}

static void CheckDword(PCWSTR pszSub, PCWSTR pszName, DWORD dwExpected)
{
    HKEY hKey;
    WCHAR szPath[512];
    DWORD dwActual = 0xdeadbeef, dwType = 0, cb = sizeof(DWORD);
    LSTATUS status;

    StringCchPrintfW(szPath, _countof(szPath), L"%s\\%s", WINEVT, pszSub);
    status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPath, 0, KEY_READ, &hKey);
    ok(status == ERROR_SUCCESS, "RegOpenKeyEx(%ls) returned %ld\n", pszSub, status);
    if (status) return;
    status = RegQueryValueExW(hKey, pszName, NULL, &dwType, (BYTE *)&dwActual, &cb);
    ok(status == ERROR_SUCCESS, "%ls: value %ls returned %ld\n", pszSub, pszName, status);
    if (!status)
    {
        ok(dwType == REG_DWORD, "%ls: value %ls has type %lu\n", pszSub, pszName, dwType);
        ok(dwActual == dwExpected, "%ls: value %ls is %#lx, expected %#lx\n", pszSub, pszName, dwActual, dwExpected);
    }
    RegCloseKey(hKey);
}

static void DeleteLogFiles(void)
{
    static const PCWSTR s_Names[] = { L"Admin", L"Operational" };
    WCHAR szPath[MAX_PATH];
    int i;

    for (i = 0; i < 2; i++)
    {
        int iTry;

        StringCchPrintfW(szPath, _countof(szPath), L"%s\\winevt\\Logs\\" PUBLISHER L"%%4%s.evtx", s_SysDir, s_Names[i]);
        /* the service of Windows has the file open for a moment after the manifest is uninstalled */
        for (iTry = 0; iTry < 40; iTry++)
        {
            if (DeleteFileW(szPath) || GetLastError() == ERROR_FILE_NOT_FOUND) break;
            Sleep(250);
        }
    }
}

static void test_help_and_errors(void)
{
    WCHAR *pszOut;
    DWORD dwExit;

    dwExit = Run(L"/?", &pszOut);
    ok(dwExit == 0, "wevtutil /? exit code %lu\n", dwExit);
    ok(StartsWith(pszOut, L"Windows Events Command Line Utility."), "unexpected help: %.60ls\n", pszOut ? pszOut : L"(null)");
    ok(Contains(pszOut, L"el | enum-logs"), "help does not list enum-logs\n");
    free(pszOut);

    dwExit = Run(L"el /?", &pszOut);
    ok(dwExit == 0, "wevtutil el /? exit code %lu\n", dwExit);
    ok(StartsWith(pszOut, L"List the names of all logs."), "unexpected el help: %.60ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"bogus", &pszOut);
    ok(dwExit == ERROR_INVALID_PARAMETER, "wevtutil bogus exit code %lu\n", dwExit);
    ok(Contains(pszOut, L"Command bogus is not supported."), "unexpected text: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"gl", &pszOut);
    ok(dwExit == ERROR_INVALID_PARAMETER, "wevtutil gl exit code %lu\n", dwExit);
    ok(Contains(pszOut, L"Required argument(s) is/are not specified."), "unexpected text: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"el foo", &pszOut);
    ok(dwExit == ERROR_INVALID_PARAMETER, "wevtutil el foo exit code %lu\n", dwExit);
    ok(Contains(pszOut, L"Too many arguments are specified."), "unexpected text: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"gl System /zz:1", &pszOut);
    ok(dwExit == ERROR_INVALID_PARAMETER, "wevtutil gl /zz exit code %lu\n", dwExit);
    ok(Contains(pszOut, L"Invalid option zz. Option is not supported."), "unexpected text: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"gl System /f:bogus", &pszOut);
    ok(dwExit == ERROR_INVALID_PARAMETER, "wevtutil gl /f:bogus exit code %lu\n", dwExit);
    ok(Contains(pszOut, L"Invalid value for option f."), "unexpected text: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"im C:\\nonexistent-nitro.man", &pszOut);
    ok(dwExit == ERROR_FILE_NOT_FOUND, "wevtutil im nonexistent exit code %lu\n", dwExit);
    ok(Contains(pszOut, L"Failed to load XML document C:\\nonexistent-nitro.man."), "unexpected text: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"gl nosuchlog-nitro", &pszOut);
    ok(dwExit == ERROR_EVT_CHANNEL_NOT_FOUND, "wevtutil gl nosuchlog exit code %lu\n", dwExit);
    ok(Contains(pszOut, L"Failed to read configuration for log nosuchlog-nitro."), "unexpected text: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"qe nosuchlog-nitro /c:1", &pszOut);
    ok(dwExit == ERROR_EVT_CHANNEL_NOT_FOUND, "wevtutil qe nosuchlog exit code %lu\n", dwExit);
    ok(Contains(pszOut, L"Failed to open event query."), "unexpected text: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"qe System /c:0", &pszOut);
    ok(dwExit == ERROR_INVALID_PARAMETER, "wevtutil qe /c:0 exit code %lu\n", dwExit);
    ok(Contains(pszOut, L"Invalid value for property count."), "unexpected text: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);
}

static void test_query(void)
{
    WCHAR *pszOut;
    DWORD dwExit;

    dwExit = Run(L"qe System /c:1 /rd:true /f:xml", &pszOut);
    ok(dwExit == 0, "qe xml exit code %lu\n", dwExit);
    ok(StartsWith(pszOut, L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='"),
       "unexpected xml: %.100ls\n", pszOut ? pszOut : L"(null)");
    ok(Contains(pszOut, L"<Channel>System</Channel>"), "no channel in: %ls\n", pszOut ? pszOut : L"(null)");
    ok(Contains(pszOut, L"</Event>"), "event not closed: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"qe System /c:1 /rd:true", &pszOut);
    ok(dwExit == 0, "qe default exit code %lu\n", dwExit);
    ok(StartsWith(pszOut, L"<Event xmlns="), "default format is not xml: %.60ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"qe System /c:2 /rd:true /f:text", &pszOut);
    ok(dwExit == 0, "qe text exit code %lu\n", dwExit);
    ok(StartsWith(pszOut, L"Event[0]:\r\n  Log Name: System\r\n  Source: "), "unexpected text: %.100ls\n", pszOut ? pszOut : L"(null)");
    ok(Contains(pszOut, L"\r\nEvent[1]:\r\n  Log Name: System\r\n"), "no second event: %ls\n", pszOut ? pszOut : L"(null)");
    ok(Contains(pszOut, L"\r\n  Date: 20"), "no date\n");
    ok(Contains(pszOut, L"\r\n  Event ID: "), "no event id\n");
    ok(Contains(pszOut, L"\r\n  Keyword: "), "no keyword\n");
    ok(Contains(pszOut, L"\r\n  Computer: "), "no computer\n");
    ok(Contains(pszOut, L"\r\n  Description: \r\n"), "no description\n");
    ok(!Contains(pszOut, L"Event[2]:"), "more than two events\n");
    free(pszOut);

    dwExit = Run(L"qe System /c:2 /rd:true /e:root", &pszOut);
    ok(dwExit == 0, "qe /e exit code %lu\n", dwExit);
    ok(StartsWith(pszOut, L"<root>\r\n<Event "), "unexpected text: %.60ls\n", pszOut ? pszOut : L"(null)");
    ok(Contains(pszOut, L"</Event>\r\n</root>\r\n"), "root not closed: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"qe System /c:1 /f:bogus", &pszOut);
    ok(dwExit == ERROR_INVALID_PARAMETER, "qe /f:bogus exit code %lu\n", dwExit);
    free(pszOut);

    /* the log of a channel that a manifest installs has no events, and the query works */
    dwExit = Run(L"qe " CH_ADMIN L" /c:5", &pszOut);
    ok(dwExit == 0, "qe of the new channel exit code %lu\n", dwExit);
    ok(pszOut && !pszOut[0], "the new channel has events: %.100ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);
}

START_TEST(wevtutil)
{
    WCHAR *pszOut, szArgs[1024];
    DWORD dwExit, cb;
    HANDLE hFile;

    GetSystemDirectoryW(s_SysDir, _countof(s_SysDir));
    StringCchPrintfW(s_Tool, _countof(s_Tool), L"%s\\wevtutil.exe", s_SysDir);
    if (GetFileAttributesW(s_Tool) == INVALID_FILE_ATTRIBUTES)
    {
        skip("wevtutil.exe is not there\n");
        return;
    }
    GetTempPathW(_countof(s_ManifestPath), s_ManifestPath);
    StringCchCatW(s_ManifestPath, _countof(s_ManifestPath), L"nitro-wevtutil-apitest.man");

    test_help_and_errors();

    hFile = CreateFileW(s_ManifestPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "cannot write the manifest, error %lu\n", GetLastError());
    if (hFile == INVALID_HANDLE_VALUE) return;
    WriteFile(hFile, s_Manifest, sizeof(s_Manifest) - 1, &cb, NULL);
    CloseHandle(hFile);

    /* start from nothing, as a test that ran before may have left the publisher */
    StringCchPrintfW(szArgs, _countof(szArgs), L"um \"%s\"", s_ManifestPath);
    Run(szArgs, &pszOut);
    free(pszOut);
    ok(!KeyExists(L"Publishers\\" GUID_LOWER), "the publisher is installed before the test\n");

    /* install */
    StringCchPrintfW(szArgs, _countof(szArgs), L"im \"%s\" /rf:%s\\kernel32.dll /mf:%s\\kernel32.dll /pf:%s\\user32.dll",
                     s_ManifestPath, s_SysDir, s_SysDir, s_SysDir);
    dwExit = Run(szArgs, &pszOut);
    ok(dwExit == 0, "im exit code %lu, output: %ls\n", dwExit, pszOut ? pszOut : L"(null)");
    ok(!Contains(pszOut, GUID_LOWER L" is installed on"), "the first install says that the publisher is installed\n");
    free(pszOut);

    /* the registry as Windows has it after the install */
    CheckString(L"Publishers\\" GUID_LOWER, NULL, REG_SZ, PUBLISHER);
    {
        WCHAR szExpected[MAX_PATH];

        StringCchPrintfW(szExpected, _countof(szExpected), L"%s\\kernel32.dll", s_SysDir);
        CheckString(L"Publishers\\" GUID_LOWER, L"ResourceFileName", REG_EXPAND_SZ, szExpected);
        CheckString(L"Publishers\\" GUID_LOWER, L"MessageFileName", REG_EXPAND_SZ, szExpected);
        StringCchPrintfW(szExpected, _countof(szExpected), L"%s\\user32.dll", s_SysDir);
        CheckString(L"Publishers\\" GUID_LOWER, L"ParameterFileName", REG_EXPAND_SZ, szExpected);
    }
    CheckDword(L"Publishers\\" GUID_LOWER, L"Enabled", 1);
    CheckDword(L"Publishers\\" GUID_LOWER L"\\ChannelReferences", L"Count", 4);
    CheckString(L"Publishers\\" GUID_LOWER L"\\ChannelReferences\\0", NULL, REG_SZ, L"System");
    CheckDword(L"Publishers\\" GUID_LOWER L"\\ChannelReferences\\0", L"Id", 8);
    CheckDword(L"Publishers\\" GUID_LOWER L"\\ChannelReferences\\0", L"Flags", 1);
    CheckString(L"Publishers\\" GUID_LOWER L"\\ChannelReferences\\1", NULL, REG_SZ, CH_ADMIN);
    CheckDword(L"Publishers\\" GUID_LOWER L"\\ChannelReferences\\1", L"Id", 0x10);
    CheckDword(L"Publishers\\" GUID_LOWER L"\\ChannelReferences\\1", L"Flags", 0);
    CheckString(L"Publishers\\" GUID_LOWER L"\\ChannelReferences\\2", NULL, REG_SZ, CH_OPER);
    CheckDword(L"Publishers\\" GUID_LOWER L"\\ChannelReferences\\2", L"Id", 20);
    CheckString(L"Publishers\\" GUID_LOWER L"\\ChannelReferences\\3", NULL, REG_SZ, CH_DEBUG);
    CheckDword(L"Publishers\\" GUID_LOWER L"\\ChannelReferences\\3", L"Id", 0x11);

    CheckString(L"Channels\\" CH_ADMIN, L"OwningPublisher", REG_SZ, GUID_LOWER);
    CheckDword(L"Channels\\" CH_ADMIN, L"Enabled", 1);
    CheckDword(L"Channels\\" CH_ADMIN, L"Isolation", 0);
    CheckDword(L"Channels\\" CH_ADMIN, L"Type", 0);
    CheckString(L"Channels\\" CH_ADMIN, L"ChannelAccess", REG_SZ,
                L"O:BAG:SYD:(A;;0x2;;;S-1-15-2-1)(A;;0x2;;;S-1-15-3-1024-3153509613-960666767-3724611135-2725662640-12138253-543910227-1950414635-4190290187)"
                L"(A;;0xf0007;;;SY)(A;;0x7;;;BA)(A;;0x7;;;SO)(A;;0x3;;;IU)(A;;0x3;;;SU)(A;;0x3;;;S-1-5-3)(A;;0x3;;;S-1-5-33)(A;;0x1;;;S-1-5-32-573)");
    CheckDword(L"Channels\\" CH_OPER, L"Type", 1);
    CheckDword(L"Channels\\" CH_OPER, L"Enabled", 1);
    CheckString(L"Channels\\" CH_DEBUG, L"OwningPublisher", REG_SZ, GUID_LOWER);
    CheckDword(L"Channels\\" CH_DEBUG, L"Enabled", 0);
    CheckDword(L"Channels\\" CH_DEBUG, L"Isolation", 2);
    CheckDword(L"Channels\\" CH_DEBUG, L"Type", 3);
    CheckString(L"Channels\\" CH_DEBUG, L"ChannelAccess", REG_SZ, L"O:BAG:BAD:(A;;0x2;;;BU)(A;;0xf0007;;;SY)(A;;0x7;;;BA)");
    ok(!KeyExists(L"Channels\\System\\" PUBLISHER), "the imported channel is registered\n");

    /* wevtapi sees the publisher and the channels */
    ok(ApiLists(FALSE, PUBLISHER), "EvtNextPublisherId does not list the publisher\n");
    ok(ApiLists(TRUE, CH_ADMIN), "EvtNextChannelPath does not list %ls\n", CH_ADMIN);
    ok(ApiLists(TRUE, CH_OPER), "EvtNextChannelPath does not list %ls\n", CH_OPER);
    ok(ApiLists(TRUE, CH_DEBUG), "EvtNextChannelPath does not list %ls\n", CH_DEBUG);
    ok(ApiLists(TRUE, L"System"), "EvtNextChannelPath does not list System\n");

    /* enum-logs and enum-publishers */
    dwExit = Run(L"el", &pszOut);
    ok(dwExit == 0, "el exit code %lu\n", dwExit);
    ok(HasLine(pszOut, CH_ADMIN), "el does not list %ls\n", CH_ADMIN);
    ok(HasLine(pszOut, CH_OPER), "el does not list %ls\n", CH_OPER);
    ok(HasLine(pszOut, CH_DEBUG), "el does not list %ls\n", CH_DEBUG);
    ok(HasLine(pszOut, L"System"), "el does not list System\n");
    ok(HasLine(pszOut, L"Application"), "el does not list Application\n");
    free(pszOut);

    dwExit = Run(L"enum-logs", &pszOut);
    ok(dwExit == 0 && HasLine(pszOut, CH_ADMIN), "enum-logs exit code %lu\n", dwExit);
    free(pszOut);

    dwExit = Run(L"ep", &pszOut);
    ok(dwExit == 0, "ep exit code %lu\n", dwExit);
    ok(HasLine(pszOut, PUBLISHER), "ep does not list %ls\n", PUBLISHER);
    ok(HasLine(pszOut, L"Service Control Manager"), "ep does not list Service Control Manager\n");
    free(pszOut);

    /* get-log */
    dwExit = Run(L"gl " CH_OPER, &pszOut);
    ok(dwExit == 0, "gl exit code %lu\n", dwExit);
    ok(StartsWith(pszOut, L"name: " CH_OPER L"\r\nenabled: true\r\ntype: Operational\r\nowningPublisher: " PUBLISHER L"\r\nisolation: Application\r\nchannelAccess: O:BAG:SYD:"),
       "unexpected gl text: %.300ls\n", pszOut ? pszOut : L"(null)");
    ok(Contains(pszOut, L"\r\nlogging:\r\n  logFileName: %SystemRoot%\\System32\\Winevt\\Logs\\" PUBLISHER L"%4Operational.evtx\r\n  retention: false\r\n  autoBackup: false\r\n  maxSize: 1052672\r\npublishing:\r\n  fileMax: 1\r\n"),
       "unexpected gl logging: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"gl " CH_DEBUG L" /f:text", &pszOut);
    ok(dwExit == 0, "gl debug exit code %lu\n", dwExit);
    ok(Contains(pszOut, L"enabled: false\r\ntype: Debug\r\nowningPublisher: " PUBLISHER L"\r\nisolation: Custom\r\nchannelAccess: O:BAG:BAD:(A;;0x2;;;BU)(A;;0xf0007;;;SY)(A;;0x7;;;BA)\r\n"),
       "unexpected gl debug text: %ls\n", pszOut ? pszOut : L"(null)");
    ok(Contains(pszOut, L"logFileName: %SystemRoot%\\System32\\Winevt\\Logs\\" PUBLISHER L"%4Debug.etl\r\n  retention: true\r\n"), "unexpected gl debug file: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    dwExit = Run(L"gl System /f:xml", &pszOut);
    ok(dwExit == 0, "gl xml exit code %lu\n", dwExit);
    ok(StartsWith(pszOut, L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<channel name=\"System\" enabled=\"true\" type=\"Admin\" owningPublisher=\"\" isolation=\"System\" channelAccess=\"O:BAG:SYD:"),
       "unexpected gl xml: %.300ls\n", pszOut ? pszOut : L"(null)");
    ok(Contains(pszOut, L" xmlns=\"http://schemas.microsoft.com/win/2004/08/events\">\r\n  <logging>\r\n    <logFileName>"), "unexpected gl xml structure: %ls\n", pszOut ? pszOut : L"(null)");
    ok(Contains(pszOut, L"</logFileName>\r\n    <retention>false</retention>\r\n    <autoBackup>false</autoBackup>\r\n    <maxSize>"), "unexpected gl xml logging: %ls\n", pszOut ? pszOut : L"(null)");
    ok(Contains(pszOut, L"  </logging>\r\n  <publishing>\r\n    <fileMax>1</fileMax>\r\n  </publishing>\r\n</channel>\r\n"), "unexpected gl xml end: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);

    test_query();

    /* a second install says that the publisher is installed, writes the publisher again from the manifest and leaves the channels as they are */
    {
        HKEY hKey;
        DWORD dwOff = 0;

        if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, WINEVT L"\\Channels\\" CH_ADMIN, 0, KEY_WRITE, &hKey))
        {
            RegSetValueExW(hKey, L"Enabled", 0, REG_DWORD, (BYTE *)&dwOff, sizeof(dwOff));
            RegCloseKey(hKey);
        }
    }
    StringCchPrintfW(szArgs, _countof(szArgs), L"im \"%s\"", s_ManifestPath);
    dwExit = Run(szArgs, &pszOut);
    ok(dwExit == 0, "second im exit code %lu\n", dwExit);
    ok(Contains(pszOut, L"**** Warning: Publisher " GUID_LOWER L" is installed on\r\nthe system. Only new values would be added."),
       "no warning about the installed publisher: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);
    CheckString(L"Publishers\\" GUID_LOWER, L"ResourceFileName", REG_EXPAND_SZ, L"%windir%\\system32\\wevtutil.exe");
    CheckString(L"Publishers\\" GUID_LOWER, L"MessageFileName", REG_EXPAND_SZ, L"%windir%\\system32\\wevtutil.exe");
    {
        HKEY hKey;

        if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, WINEVT L"\\Publishers\\" GUID_LOWER, 0, KEY_READ, &hKey))
        {
            ok(RegQueryValueExW(hKey, L"ParameterFileName", NULL, NULL, NULL, NULL) == ERROR_FILE_NOT_FOUND, "ParameterFileName stays after the second im\n");
            RegCloseKey(hKey);
        }
        else
            ok(0, "the publisher is gone\n");
    }
    CheckDword(L"Channels\\" CH_ADMIN, L"Enabled", 0);

    /* uninstall */
    StringCchPrintfW(szArgs, _countof(szArgs), L"um \"%s\"", s_ManifestPath);
    dwExit = Run(szArgs, &pszOut);
    ok(dwExit == 0, "um exit code %lu, output: %ls\n", dwExit, pszOut ? pszOut : L"(null)");
    ok(!pszOut || !pszOut[0], "um wrote: %ls\n", pszOut ? pszOut : L"(null)");
    free(pszOut);
    ok(!KeyExists(L"Publishers\\" GUID_LOWER), "the publisher key is there after um\n");
    ok(!KeyExists(L"Channels\\" CH_ADMIN), "the Admin channel key is there after um\n");
    ok(!KeyExists(L"Channels\\" CH_OPER), "the Operational channel key is there after um\n");
    ok(!KeyExists(L"Channels\\" CH_DEBUG), "the Debug channel key is there after um\n");
    ok(KeyExists(L"Publishers"), "the Publishers key is gone\n");

    dwExit = Run(L"el", &pszOut);
    ok(dwExit == 0 && !HasLine(pszOut, CH_ADMIN), "el lists the channel after um (exit %lu)\n", dwExit);
    free(pszOut);
    dwExit = Run(L"ep", &pszOut);
    ok(dwExit == 0 && !HasLine(pszOut, PUBLISHER), "ep lists the publisher after um (exit %lu)\n", dwExit);
    free(pszOut);
    dwExit = Run(L"gl " CH_ADMIN, &pszOut);
    ok(dwExit == ERROR_EVT_CHANNEL_NOT_FOUND, "gl of the removed channel exit code %lu\n", dwExit);
    free(pszOut);

    /* removing again is not an error */
    dwExit = Run(szArgs, &pszOut);
    ok(dwExit == 0, "second um exit code %lu\n", dwExit);
    free(pszOut);

    DeleteLogFiles();
    DeleteFileW(s_ManifestPath);
}
