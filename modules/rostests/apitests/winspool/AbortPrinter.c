/*
 * PROJECT:     ReactOS Print Spooler DLL API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for AbortPrinter
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winspool.h>
#include <strsafe.h>

static const char TestData[] = "ReactOS AbortPrinter test\r\n";

/* TRUE if the job exists, FALSE if the spooler does not know it; any other failure is reported */
static BOOL
JobExists(HANDLE hPrinter, DWORD dwJobId)
{
    BYTE Buffer[4096];
    DWORD cbNeeded = 0;

    if (GetJobW(hPrinter, dwJobId, 1, Buffer, sizeof(Buffer), &cbNeeded))
        return TRUE;
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        return TRUE;
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "GetJobW(%lu) failed with error %lu\n", dwJobId, GetLastError());
    return FALSE;
}

/* The spooler may finish deleting an aborted job asynchronously */
static BOOL
WaitForJobGone(HANDLE hPrinter, DWORD dwJobId)
{
    DWORD i;

    for (i = 0; i < 50; i++)
    {
        if (!JobExists(hPrinter, dwJobId))
            return TRUE;
        Sleep(100);
    }

    return FALSE;
}

static void
TestNoDocument(PWSTR pwszPrinter)
{
    BOOL bResult;
    HANDLE hPrinter;

    if (!OpenPrinterW(pwszPrinter, &hPrinter, NULL))
    {
        ok(FALSE, "OpenPrinterW failed with error %lu!\n", GetLastError());
        return;
    }

    SetLastError(0xDEADBEEF);
    bResult = AbortPrinter(hPrinter);
    ok(!bResult, "AbortPrinter without a document succeeded\n");
    ok(GetLastError() == ERROR_SPL_NO_STARTDOC, "AbortPrinter without a document: error %lu\n", GetLastError());

    /* The handle still works */
    SetLastError(0xDEADBEEF);
    bResult = ClosePrinter(hPrinter);
    ok(bResult, "ClosePrinter failed with error %lu!\n", GetLastError());
}

static void
TestStartedDocument(PWSTR pwszPrinter, PCWSTR pwszOutputFile)
{
    BOOL bResult;
    DOC_INFO_1W DocInfo;
    DWORD cbWritten, dwJobId;
    HANDLE hPrinter;

    if (!OpenPrinterW(pwszPrinter, &hPrinter, NULL))
    {
        ok(FALSE, "OpenPrinterW failed with error %lu!\n", GetLastError());
        return;
    }

    DocInfo.pDocName = L"winspool_apitest AbortPrinter";
    DocInfo.pOutputFile = (PWSTR)pwszOutputFile;
    DocInfo.pDatatype = L"RAW";

    dwJobId = StartDocPrinterW(hPrinter, 1, (PBYTE)&DocInfo);
    ok(dwJobId != 0, "StartDocPrinterW failed with error %lu!\n", GetLastError());
    if (!dwJobId)
    {
        ClosePrinter(hPrinter);
        return;
    }

    bResult = StartPagePrinter(hPrinter);
    ok(bResult, "StartPagePrinter failed with error %lu!\n", GetLastError());
    bResult = WritePrinter(hPrinter, (PVOID)TestData, sizeof(TestData) - 1, &cbWritten);
    ok(bResult, "WritePrinter failed with error %lu!\n", GetLastError());
    ok(JobExists(hPrinter, dwJobId), "Job %lu does not exist before AbortPrinter\n", dwJobId);

    SetLastError(0xDEADBEEF);
    bResult = AbortPrinter(hPrinter);
    ok(bResult, "AbortPrinter failed with error %lu!\n", GetLastError());

    ok(WaitForJobGone(hPrinter, dwJobId), "Job %lu still exists after AbortPrinter\n", dwJobId);

    /* The document is gone */
    SetLastError(0xDEADBEEF);
    bResult = WritePrinter(hPrinter, (PVOID)TestData, sizeof(TestData) - 1, &cbWritten);
    ok(!bResult, "WritePrinter after AbortPrinter succeeded\n");
    ok(GetLastError() == ERROR_SPL_NO_STARTDOC, "WritePrinter after AbortPrinter: error %lu\n", GetLastError());

    SetLastError(0xDEADBEEF);
    bResult = EndDocPrinter(hPrinter);
    ok(!bResult, "EndDocPrinter after AbortPrinter succeeded\n");
    ok(GetLastError() == ERROR_SPL_NO_STARTDOC, "EndDocPrinter after AbortPrinter: error %lu\n", GetLastError());

    /* A second abort has nothing to abort */
    SetLastError(0xDEADBEEF);
    bResult = AbortPrinter(hPrinter);
    ok(!bResult, "Second AbortPrinter succeeded\n");
    ok(GetLastError() == ERROR_SPL_NO_STARTDOC, "Second AbortPrinter: error %lu\n", GetLastError());

    /* The handle can start a new document */
    dwJobId = StartDocPrinterW(hPrinter, 1, (PBYTE)&DocInfo);
    ok(dwJobId != 0, "StartDocPrinterW after AbortPrinter failed with error %lu!\n", GetLastError());
    if (dwJobId)
    {
        bResult = AbortPrinter(hPrinter);
        ok(bResult, "AbortPrinter of the new document failed with error %lu!\n", GetLastError());
        ok(WaitForJobGone(hPrinter, dwJobId), "Job %lu still exists after AbortPrinter\n", dwJobId);
    }

    bResult = ClosePrinter(hPrinter);
    ok(bResult, "ClosePrinter failed with error %lu!\n", GetLastError());

    if (pwszOutputFile)
    {
        Sleep(1000);
        ok(GetFileAttributesW(pwszOutputFile) == INVALID_FILE_ATTRIBUTES, "Output file %S was written\n", pwszOutputFile);
        DeleteFileW(pwszOutputFile);
    }
}

/* Adds a job on hPrinter and writes its spool file */
static DWORD
AddJobWithData(HANDLE hPrinter, PWSTR pwszPath, DWORD cchPath)
{
    BYTE Buffer[MAX_PATH * sizeof(WCHAR) + sizeof(ADDJOB_INFO_1W)];
    DWORD cbNeeded, cbWritten;
    HANDLE hFile;
    PADDJOB_INFO_1W pAddJob = (PADDJOB_INFO_1W)Buffer;

    if (!AddJobW(hPrinter, 1, Buffer, sizeof(Buffer), &cbNeeded))
    {
        ok(FALSE, "AddJobW failed with error %lu!\n", GetLastError());
        return 0;
    }

    StringCchCopyW(pwszPath, cchPath, pAddJob->Path);
    hFile = CreateFileW(pAddJob->Path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW(%S) failed with error %lu!\n", pAddJob->Path, GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        WriteFile(hFile, TestData, sizeof(TestData) - 1, &cbWritten, NULL);
        CloseHandle(hFile);
    }

    return pAddJob->JobId;
}

static void
TestAddedJob(PWSTR pwszPrinter)
{
    BOOL bResult;
    DWORD dwJobId;
    HANDLE hPrinter;
    WCHAR wszPath[MAX_PATH];

    if (!OpenPrinterW(pwszPrinter, &hPrinter, NULL))
    {
        ok(FALSE, "OpenPrinterW failed with error %lu!\n", GetLastError());
        return;
    }

    dwJobId = AddJobWithData(hPrinter, wszPath, _countof(wszPath));
    if (dwJobId)
    {
        /* A job added through AddJob is no started document */
        SetLastError(0xDEADBEEF);
        bResult = AbortPrinter(hPrinter);
        ok(!bResult, "AbortPrinter after AddJob succeeded\n");
        ok(GetLastError() == ERROR_SPL_NO_STARTDOC, "AbortPrinter after AddJob: error %lu\n", GetLastError());
        ok(JobExists(hPrinter, dwJobId), "Job %lu is gone after AbortPrinter\n", dwJobId);

        /* SetJob deletes it when ScheduleJob ends it, and nothing is printed */
        SetLastError(0xDEADBEEF);
        bResult = SetJobW(hPrinter, dwJobId, 0, NULL, JOB_CONTROL_DELETE);
        ok(bResult, "SetJobW(JOB_CONTROL_DELETE) failed with error %lu!\n", GetLastError());
        ok(JobExists(hPrinter, dwJobId), "Job %lu is gone before ScheduleJob\n", dwJobId);

        SetLastError(0xDEADBEEF);
        bResult = ScheduleJob(hPrinter, dwJobId);
        ok(bResult, "ScheduleJob of the deleted job failed with error %lu!\n", GetLastError());
        ok(WaitForJobGone(hPrinter, dwJobId), "Job %lu still exists after ScheduleJob\n", dwJobId);
        ok(GetFileAttributesW(wszPath) == INVALID_FILE_ATTRIBUTES, "Spool file %S still exists\n", wszPath);
    }

    /* A deleted job that is never scheduled goes away with its handle */
    dwJobId = AddJobWithData(hPrinter, wszPath, _countof(wszPath));
    if (dwJobId)
    {
        bResult = SetJobW(hPrinter, dwJobId, 0, NULL, JOB_CONTROL_DELETE);
        ok(bResult, "SetJobW(JOB_CONTROL_DELETE) failed with error %lu!\n", GetLastError());
    }

    bResult = ClosePrinter(hPrinter);
    ok(bResult, "ClosePrinter failed with error %lu!\n", GetLastError());

    if (dwJobId && OpenPrinterW(pwszPrinter, &hPrinter, NULL))
    {
        ok(WaitForJobGone(hPrinter, dwJobId), "Job %lu still exists after ClosePrinter\n", dwJobId);
        ClosePrinter(hPrinter);
    }

    /* The spooler need not delete the spool file of a job that was never scheduled */
    if (dwJobId)
        DeleteFileW(wszPath);
}

static void
TestDeletedByOtherHandle(PWSTR pwszPrinter)
{
    BOOL bResult;
    DOC_INFO_1W DocInfo;
    DWORD cbWritten, dwJobId;
    HANDLE hPrinter, hOther;

    if (!OpenPrinterW(pwszPrinter, &hPrinter, NULL) || !OpenPrinterW(pwszPrinter, &hOther, NULL))
    {
        ok(FALSE, "OpenPrinterW failed with error %lu!\n", GetLastError());
        return;
    }

    DocInfo.pDocName = L"winspool_apitest AbortPrinter deleted";
    DocInfo.pOutputFile = NULL;
    DocInfo.pDatatype = L"RAW";
    dwJobId = StartDocPrinterW(hPrinter, 1, (PBYTE)&DocInfo);
    ok(dwJobId != 0, "StartDocPrinterW failed with error %lu!\n", GetLastError());
    if (dwJobId)
    {
        WritePrinter(hPrinter, (PVOID)TestData, sizeof(TestData) - 1, &cbWritten);

        /* Another handle deletes the document while it is spooled; ending it discards it */
        SetLastError(0xDEADBEEF);
        bResult = SetJobW(hOther, dwJobId, 0, NULL, JOB_CONTROL_DELETE);
        ok(bResult, "SetJobW(JOB_CONTROL_DELETE) failed with error %lu!\n", GetLastError());

        SetLastError(0xDEADBEEF);
        bResult = EndDocPrinter(hPrinter);
        ok(bResult, "EndDocPrinter of the deleted document failed with error %lu!\n", GetLastError());
        ok(WaitForJobGone(hOther, dwJobId), "Job %lu still exists after EndDocPrinter\n", dwJobId);
    }

    ClosePrinter(hOther);
    ClosePrinter(hPrinter);
}

START_TEST(AbortPrinter)
{
    BOOL bResult;
    DWORD cchPrinter;
    HANDLE hPrinter;
    WCHAR wszPrinter[MAX_PATH];
    WCHAR wszPath[MAX_PATH];

    SetLastError(0xDEADBEEF);
    ok(!AbortPrinter(NULL), "AbortPrinter(NULL) succeeded\n");
    ok(GetLastError() == ERROR_INVALID_HANDLE, "AbortPrinter(NULL): error %lu\n", GetLastError());

    cchPrinter = _countof(wszPrinter);
    if (!GetDefaultPrinterW(wszPrinter, &cchPrinter))
    {
        skip("No default printer, error %lu\n", GetLastError());
        return;
    }
    trace("Aborting documents of '%S'\n", wszPrinter);

    TestNoDocument(wszPrinter);
    TestStartedDocument(wszPrinter, NULL);

    GetTempPathW(_countof(wszPath), wszPath);
    StringCchPrintfW(wszPath + wcslen(wszPath), _countof(wszPath) - wcslen(wszPath),
                     L"winspool_abort_%lu.prn", GetCurrentProcessId());
    DeleteFileW(wszPath);
    TestStartedDocument(wszPrinter, wszPath);

    TestAddedJob(wszPrinter);
    TestDeletedByOtherHandle(wszPrinter);

    /* The spooler still works */
    bResult = OpenPrinterW(wszPrinter, &hPrinter, NULL);
    ok(bResult, "OpenPrinterW failed with error %lu after the tests\n", GetLastError());
    if (bResult)
        ClosePrinter(hPrinter);
}
