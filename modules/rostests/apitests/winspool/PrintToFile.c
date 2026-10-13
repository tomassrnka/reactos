/*
 * PROJECT:     ReactOS Print Spooler DLL API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for printing a document into a file given in DOC_INFO_1W.pOutputFile
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winspool.h>
#include <strsafe.h>

static const char TestData[] = "ReactOS print to file test\r\nSecond line\r\n";

static BOOL
WaitForOutputFile(PCWSTR pwszPath, PSTR pBuffer, DWORD cbBuffer, PDWORD pcbRead)
{
    DWORD i;
    HANDLE hFile;

    *pcbRead = 0;

    /* The spooler prints the job asynchronously after EndDocPrinter. */
    for (i = 0; i < 300; i++)
    {
        hFile = CreateFileW(pwszPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            BOOL bRead = ReadFile(hFile, pBuffer, cbBuffer, pcbRead, NULL);
            CloseHandle(hFile);
            if (bRead && *pcbRead >= sizeof(TestData) - 1)
                return TRUE;
        }
        Sleep(100);
    }

    return FALSE;
}

static void
TestPrintToFile(PCWSTR pwszPrinter, PWSTR pwszDatatype, PCWSTR pwszSuffix)
{
    BOOL bResult;
    char Buffer[256];
    DOC_INFO_1W DocInfo;
    DWORD cbRead, cbWritten, dwJobId;
    HANDLE hPrinter;
    WCHAR wszPath[MAX_PATH];

    GetTempPathW(_countof(wszPath), wszPath);
    StringCchPrintfW(wszPath + wcslen(wszPath), _countof(wszPath) - wcslen(wszPath),
                     L"winspool_ptf_%lu_%s.prn", GetCurrentProcessId(), pwszSuffix);
    DeleteFileW(wszPath);

    if (!OpenPrinterW((PWSTR)pwszPrinter, &hPrinter, NULL))
    {
        ok(FALSE, "OpenPrinterW failed with error %lu!\n", GetLastError());
        return;
    }

    DocInfo.pDocName = L"winspool_apitest PrintToFile";
    DocInfo.pOutputFile = wszPath;
    DocInfo.pDatatype = pwszDatatype;

    SetLastError(0xDEADBEEF);
    dwJobId = StartDocPrinterW(hPrinter, 1, (PBYTE)&DocInfo);
    ok(dwJobId != 0, "StartDocPrinterW failed with error %lu!\n", GetLastError());
    if (!dwJobId)
    {
        ClosePrinter(hPrinter);
        return;
    }

    bResult = StartPagePrinter(hPrinter);
    ok(bResult, "StartPagePrinter failed with error %lu!\n", GetLastError());

    cbWritten = 0;
    bResult = WritePrinter(hPrinter, (PVOID)TestData, sizeof(TestData) - 1, &cbWritten);
    ok(bResult, "WritePrinter failed with error %lu!\n", GetLastError());
    ok(cbWritten == sizeof(TestData) - 1, "WritePrinter wrote %lu bytes\n", cbWritten);

    bResult = EndPagePrinter(hPrinter);
    ok(bResult, "EndPagePrinter failed with error %lu!\n", GetLastError());

    bResult = EndDocPrinter(hPrinter);
    ok(bResult, "EndDocPrinter failed with error %lu!\n", GetLastError());

    ClosePrinter(hPrinter);

    ZeroMemory(Buffer, sizeof(Buffer));
    bResult = WaitForOutputFile(wszPath, Buffer, sizeof(Buffer) - 1, &cbRead);
    ok(bResult, "Output file %S was not written (%lu bytes read)\n", wszPath, cbRead);
    ok(cbRead == sizeof(TestData) - 1, "Output file has %lu bytes, expected %lu\n", cbRead, (DWORD)(sizeof(TestData) - 1));
    ok(cbRead == sizeof(TestData) - 1 && !memcmp(Buffer, TestData, sizeof(TestData) - 1),
       "Output file content is '%s'\n", Buffer);

    DeleteFileW(wszPath);
}

static void
TestUnwritableOutputFile(PCWSTR pwszPrinter)
{
    BOOL bResult;
    DOC_INFO_1W DocInfo;
    DWORD cbWritten, dwJobId, i;
    HANDLE hPrinter;
    WCHAR wszPath[MAX_PATH];

    /* A directory that does not exist: the job cannot be printed */
    GetTempPathW(_countof(wszPath), wszPath);
    StringCchPrintfW(wszPath + wcslen(wszPath), _countof(wszPath) - wcslen(wszPath),
                     L"winspool_ptf_missing_%lu\\out.prn", GetCurrentProcessId());

    if (!OpenPrinterW((PWSTR)pwszPrinter, &hPrinter, NULL))
    {
        ok(FALSE, "OpenPrinterW failed with error %lu!\n", GetLastError());
        return;
    }

    DocInfo.pDocName = L"winspool_apitest PrintToFile missing directory";
    DocInfo.pOutputFile = wszPath;
    DocInfo.pDatatype = NULL;

    dwJobId = StartDocPrinterW(hPrinter, 1, (PBYTE)&DocInfo);
    if (dwJobId)
    {
        StartPagePrinter(hPrinter);
        WritePrinter(hPrinter, (PVOID)TestData, sizeof(TestData) - 1, &cbWritten);
        EndPagePrinter(hPrinter);
        EndDocPrinter(hPrinter);
    }
    ClosePrinter(hPrinter);

    /* Give the spooler time to try the job */
    Sleep(3000);
    ok(GetFileAttributesW(wszPath) == INVALID_FILE_ATTRIBUTES, "Output file %S exists\n", wszPath);

    /* The spooler still accepts requests */
    for (i = 0; i < 2; i++)
    {
        bResult = OpenPrinterW((PWSTR)pwszPrinter, &hPrinter, NULL);
        ok(bResult, "OpenPrinterW failed with error %lu after the failed job\n", GetLastError());
        if (bResult)
            ClosePrinter(hPrinter);
    }
}

START_TEST(PrintToFile)
{
    DWORD cchPrinter;
    WCHAR wszPrinter[MAX_PATH];

    cchPrinter = _countof(wszPrinter);
    if (!GetDefaultPrinterW(wszPrinter, &cchPrinter))
    {
        skip("No default printer, error %lu\n", GetLastError());
        return;
    }

    trace("Printing to files through '%S'\n", wszPrinter);
    TestPrintToFile(wszPrinter, NULL, L"default");
    TestPrintToFile(wszPrinter, L"RAW", L"raw");
    TestUnwritableOutputFile(wszPrinter);

    /* Printing still works after the failed job */
    TestPrintToFile(wszPrinter, NULL, L"after");
}
