/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test the log of a package refused before installation starts
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#include <winnls.h>
#include <wtypes.h>
#include <msi.h>
#include <msidefs.h>
#include <msiquery.h>
#include <strsafe.h>

static BOOL RunQuery(MSIHANDLE hDatabase, LPCWSTR pszQuery)
{
    MSIHANDLE hView;
    UINT r;

    r = MsiDatabaseOpenViewW(hDatabase, pszQuery, &hView);
    if (r == ERROR_SUCCESS)
    {
        r = MsiViewExecute(hView, 0);
        MsiCloseHandle(hView);
    }
    ok(r == ERROR_SUCCESS, "%S failed: %u\n", pszQuery, r);
    return r == ERROR_SUCCESS;
}

static BOOL CreatePackage(LPCWSTR pszPath, LPCWSTR pszTemplate)
{
    MSIHANDLE hDatabase, hSummary;
    UINT r;

    DeleteFileW(pszPath);
    r = MsiOpenDatabaseW(pszPath, (LPCWSTR)MSIDBOPEN_CREATE, &hDatabase);
    ok(r == ERROR_SUCCESS, "MsiOpenDatabaseW failed: %u\n", r);
    if (r != ERROR_SUCCESS)
        return FALSE;

    if (!RunQuery(hDatabase, L"CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` CHAR(0) NOT NULL PRIMARY KEY `Property`)") ||
        !RunQuery(hDatabase, L"INSERT INTO `Property` (`Property`, `Value`) VALUES ('ProductCode', '{8C6E2D2A-9A62-4B2B-9C0B-2E5F6A1D3B47}')") ||
        !RunQuery(hDatabase, L"INSERT INTO `Property` (`Property`, `Value`) VALUES ('ProductName', 'Refused package test')") ||
        !RunQuery(hDatabase, L"INSERT INTO `Property` (`Property`, `Value`) VALUES ('ProductVersion', '1.0.0')") ||
        !RunQuery(hDatabase, L"INSERT INTO `Property` (`Property`, `Value`) VALUES ('Manufacturer', 'ReactOS')") ||
        !RunQuery(hDatabase, L"INSERT INTO `Property` (`Property`, `Value`) VALUES ('ProductLanguage', '1033')"))
    {
        MsiCloseHandle(hDatabase);
        return FALSE;
    }

    r = MsiGetSummaryInformationW(hDatabase, NULL, 5, &hSummary);
    ok(r == ERROR_SUCCESS, "MsiGetSummaryInformationW failed: %u\n", r);
    if (r == ERROR_SUCCESS)
    {
        MsiSummaryInfoSetPropertyW(hSummary, PID_TEMPLATE, VT_LPSTR, 0, NULL, pszTemplate);
        MsiSummaryInfoSetPropertyW(hSummary, PID_REVNUMBER, VT_LPSTR, 0, NULL, L"{3D7C2B1A-4E5F-4A6B-8C9D-0E1F2A3B4C5D}");
        MsiSummaryInfoSetPropertyW(hSummary, PID_PAGECOUNT, VT_I4, 200, NULL, NULL);
        MsiSummaryInfoSetPropertyW(hSummary, PID_WORDCOUNT, VT_I4, 0, NULL, NULL);
        r = MsiSummaryInfoPersist(hSummary);
        ok(r == ERROR_SUCCESS, "MsiSummaryInfoPersist failed: %u\n", r);
        MsiCloseHandle(hSummary);
    }

    if (r == ERROR_SUCCESS)
    {
        r = MsiDatabaseCommit(hDatabase);
        ok(r == ERROR_SUCCESS, "MsiDatabaseCommit failed: %u\n", r);
    }
    MsiCloseHandle(hDatabase);
    return r == ERROR_SUCCESS;
}

/* Returns the log as text; Windows writes it as UTF-16 with a BOM or in the ANSI code page */
static PWSTR ReadLog(LPCWSTR pszPath)
{
    HANDLE hFile;
    DWORD dwSize, dwRead;
    PBYTE pData;
    PWSTR pszText = NULL;
    INT cch;

    hFile = CreateFileW(pszPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return NULL;
    dwSize = GetFileSize(hFile, NULL);
    pData = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwSize + sizeof(WCHAR));
    if (pData && ReadFile(hFile, pData, dwSize, &dwRead, NULL) && dwRead == dwSize)
    {
        if (dwSize >= 2 && pData[0] == 0xFF && pData[1] == 0xFE)
        {
            pszText = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwSize + sizeof(WCHAR));
            if (pszText)
                CopyMemory(pszText, pData + 2, dwSize - 2);
        }
        else
        {
            cch = MultiByteToWideChar(CP_ACP, 0, (LPCSTR)pData, dwSize, NULL, 0);
            pszText = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (cch + 1) * sizeof(WCHAR));
            if (pszText)
                MultiByteToWideChar(CP_ACP, 0, (LPCSTR)pData, dwSize, pszText, cch);
        }
    }
    HeapFree(GetProcessHeap(), 0, pData);
    CloseHandle(hFile);
    return pszText;
}

static void TestRefusedPackage(LPCWSTR pszTemplate, DWORD dwLogMode, BOOL bExpectMessage)
{
    WCHAR szDir[MAX_PATH], szPackage[MAX_PATH], szLog[MAX_PATH];
    PWSTR pszMessage = NULL, pszLog;
    DWORD cch;
    UINT r;

    GetTempPathW(ARRAYSIZE(szDir), szDir);
    StringCchPrintfW(szPackage, ARRAYSIZE(szPackage), L"%srefused_package_test.msi", szDir);
    StringCchPrintfW(szLog, ARRAYSIZE(szLog), L"%srefused_package_test.log", szDir);

    if (!CreatePackage(szPackage, pszTemplate))
        return;

    r = MsiEnableLogW(dwLogMode, szLog, 0);
    ok(r == ERROR_SUCCESS, "MsiEnableLogW failed: %u\n", r);
    r = MsiInstallProductW(szPackage, NULL);
    ok(r == ERROR_INSTALL_PLATFORM_UNSUPPORTED, "%S: MsiInstallProductW returned %u\n", pszTemplate, r);
    MsiEnableLogW(0, NULL, 0);

    cch = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                         NULL, ERROR_INSTALL_PLATFORM_UNSUPPORTED, 0, (PWSTR)&pszMessage, 0, NULL);
    while (cch && (pszMessage[cch - 1] == L'\r' || pszMessage[cch - 1] == L'\n'))
        pszMessage[--cch] = UNICODE_NULL;

    pszLog = ReadLog(szLog);
    ok(pszLog != NULL, "%S: no log\n", pszTemplate);
    if (pszLog && pszMessage)
    {
        if (bExpectMessage)
        {
            ok(wcsstr(pszLog, pszMessage) != NULL, "%S, mode 0x%lx: log lacks \"%S\": \"%S\"\n",
               pszTemplate, dwLogMode, pszMessage, pszLog);
            ok(wcsstr(pszLog, szPackage) != NULL, "%S, mode 0x%lx: log lacks the package path: \"%S\"\n",
               pszTemplate, dwLogMode, pszLog);
        }
        else
        {
            ok(wcsstr(pszLog, pszMessage) == NULL, "%S, mode 0x%lx: unexpected message in the log: \"%S\"\n",
               pszTemplate, dwLogMode, pszLog);
        }
    }

    HeapFree(GetProcessHeap(), 0, pszLog);
    LocalFree(pszMessage);
    DeleteFileW(szLog);
    DeleteFileW(szPackage);
}

START_TEST(RefusedPackageLog)
{
    MsiSetInternalUI(INSTALLUILEVEL_NONE, NULL);

    /* A platform name msi does not know, and one this machine does not run */
    TestRefusedPackage(L"Bogus;1033", INSTALLLOGMODE_INFO, TRUE);
#if defined(_M_ARM64)
    TestRefusedPackage(L"Arm;1033", INSTALLLOGMODE_INFO, TRUE);
#else
    TestRefusedPackage(L"Arm64;1033", INSTALLLOGMODE_INFO, TRUE);
#endif
    TestRefusedPackage(L"Bogus;1033", INSTALLLOGMODE_INFO | INSTALLLOGMODE_WARNING | INSTALLLOGMODE_ERROR |
                       INSTALLLOGMODE_VERBOSE, TRUE);

    /* The message is an information message: an errors-only log does not get it */
    TestRefusedPackage(L"Bogus;1033", INSTALLLOGMODE_ERROR, FALSE);
}
