/*
 * PROJECT:     ReactOS Print Spooler DLL API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for ScheduleJob of a job added through another printer handle
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winspool.h>

static const char TestData[] = "ReactOS ScheduleJob test\r\n";

/* 1 if the job exists, 0 if the spooler does not know it, -1 (reported) if the query failed otherwise */
static int
JobState(HANDLE hPrinter, DWORD dwJobId)
{
    BYTE Buffer[4096];
    DWORD cbNeeded = 0;

    if (GetJobW(hPrinter, dwJobId, 1, Buffer, sizeof(Buffer), &cbNeeded))
        return 1;
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        return 1;
    if (GetLastError() == ERROR_INVALID_PARAMETER)
        return 0;
    ok(FALSE, "GetJobW(%lu) failed with error %lu\n", dwJobId, GetLastError());
    return -1;
}

static BOOL
WaitForJobGone(HANDLE hPrinter, DWORD dwJobId)
{
    DWORD i;
    int State;

    for (i = 0; i < 100; i++)
    {
        State = JobState(hPrinter, dwJobId);
        if (State != 1)
            return (State == 0);
        Sleep(100);
    }

    return FALSE;
}

/* Adds a job on hPrinter and writes its spool file */
static DWORD
AddJobWithData(HANDLE hPrinter)
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

    hFile = CreateFileW(pAddJob->Path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW(%S) failed with error %lu!\n", pAddJob->Path, GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        ok(WriteFile(hFile, TestData, sizeof(TestData) - 1, &cbWritten, NULL), "WriteFile failed with error %lu!\n", GetLastError());
        CloseHandle(hFile);
    }

    return pAddJob->JobId;
}

static void
TestOtherHandle(PWSTR pwszPrinter)
{
    BOOL bResult;
    DOC_INFO_1W DocInfo;
    DWORD dwJobId, dwJobId2, dwDocJob;
    HANDLE hPrinter1, hPrinter2, hPrinter3, hJob;
    PRINTER_DEFAULTSW Defaults = { NULL, NULL, PRINTER_ALL_ACCESS };
    WCHAR wszJob[MAX_PATH + 32];

    if (!OpenPrinterW(pwszPrinter, &hPrinter1, NULL))
    {
        ok(FALSE, "OpenPrinterW failed with error %lu!\n", GetLastError());
        return;
    }
    if (!OpenPrinterW(pwszPrinter, &hPrinter2, &Defaults))
    {
        skip("OpenPrinterW with all access failed with error %lu\n", GetLastError());
        ClosePrinter(hPrinter1);
        return;
    }

    dwJobId = AddJobWithData(hPrinter1);
    if (!dwJobId)
        goto Cleanup;

    /* Only the handle that added the job can schedule it, even with all access */
    SetLastError(0xDEADBEEF);
    bResult = ScheduleJob(hPrinter2, dwJobId);
    ok(!bResult, "ScheduleJob through another handle succeeded\n");
    ok(GetLastError() == ERROR_ACCESS_DENIED, "ScheduleJob through another handle: error %lu\n", GetLastError());
    ok(JobState(hPrinter2, dwJobId) == 1, "Job %lu is gone after the refused ScheduleJob\n", dwJobId);

    /* A Printer Job handle is no printer handle for ScheduleJob */
    _snwprintf(wszJob, _countof(wszJob), L"%s, Job %lu", pwszPrinter, dwJobId);
    wszJob[_countof(wszJob) - 1] = UNICODE_NULL;
    if (OpenPrinterW(wszJob, &hJob, NULL))
    {
        SetLastError(0xDEADBEEF);
        bResult = ScheduleJob(hJob, dwJobId);
        ok(!bResult, "ScheduleJob through a job handle succeeded\n");
        ok(GetLastError() == ERROR_INVALID_HANDLE, "ScheduleJob through a job handle: error %lu\n", GetLastError());
        ClosePrinter(hJob);
    }
    else
    {
        skip("OpenPrinterW(%S) failed with error %lu\n", wszJob, GetLastError());
    }

    bResult = ScheduleJob(hPrinter1, dwJobId);
    ok(bResult, "ScheduleJob through the adding handle failed with error %lu!\n", GetLastError());
    ok(WaitForJobGone(hPrinter1, dwJobId), "Job %lu did not leave the queue\n", dwJobId);

    /* The adding handle has finished its job: it can add and schedule another one */
    dwJobId2 = AddJobWithData(hPrinter1);
    if (dwJobId2)
    {
        ok(ScheduleJob(hPrinter1, dwJobId2), "ScheduleJob of the second job failed with error %lu!\n", GetLastError());
        ok(WaitForJobGone(hPrinter1, dwJobId2), "Job %lu did not leave the queue\n", dwJobId2);
    }

    /* Closing the adding handle frees nothing: a document of a third handle survives it */
    if (OpenPrinterW(pwszPrinter, &hPrinter3, NULL))
    {
        DocInfo.pDocName = L"winspool_apitest ScheduleJob";
        DocInfo.pOutputFile = NULL;
        DocInfo.pDatatype = L"RAW";
        dwDocJob = StartDocPrinterW(hPrinter3, 1, (PBYTE)&DocInfo);
        ok(dwDocJob != 0, "StartDocPrinterW failed with error %lu!\n", GetLastError());

        bResult = ClosePrinter(hPrinter1);
        ok(bResult, "ClosePrinter of the adding handle failed with error %lu!\n", GetLastError());
        hPrinter1 = NULL;

        if (dwDocJob)
        {
            ok(JobState(hPrinter3, dwDocJob) == 1, "Job %lu of another handle is gone\n", dwDocJob);
        }

        if (dwDocJob)
        {
            ok(EndDocPrinter(hPrinter3), "EndDocPrinter failed with error %lu!\n", GetLastError());
            ok(WaitForJobGone(hPrinter3, dwDocJob), "Job %lu did not leave the queue\n", dwDocJob);
        }
        ClosePrinter(hPrinter3);
    }

Cleanup:
    if (hPrinter1)
        ClosePrinter(hPrinter1);
    ClosePrinter(hPrinter2);
}

START_TEST(ScheduleJob)
{
    BOOL bResult, bKnown, bPaused, bDirect;
    DWORD cbNeeded = 0, cchPrinter;
    HANDLE hPrinter;
    PPRINTER_INFO_2W pInfo2;
    WCHAR wszPrinter[MAX_PATH];

    cchPrinter = _countof(wszPrinter);
    if (!GetDefaultPrinterW(wszPrinter, &cchPrinter))
    {
        skip("No default printer, error %lu\n", GetLastError());
        return;
    }
    trace("Scheduling jobs on '%S'\n", wszPrinter);

    /* The queue must be usable: a paused or offline printer would keep the test's jobs */
    if (!OpenPrinterW(wszPrinter, &hPrinter, NULL))
    {
        skip("OpenPrinterW failed with error %lu\n", GetLastError());
        return;
    }
    bKnown = FALSE;
    bPaused = FALSE;
    bDirect = FALSE;
    GetPrinterW(hPrinter, 2, NULL, 0, &cbNeeded);
    pInfo2 = cbNeeded ? HeapAlloc(GetProcessHeap(), 0, cbNeeded) : NULL;
    if (pInfo2 && GetPrinterW(hPrinter, 2, (PBYTE)pInfo2, cbNeeded, &cbNeeded))
    {
        bKnown = TRUE;
        bPaused = !!(pInfo2->Status & (PRINTER_STATUS_PAUSED | PRINTER_STATUS_OFFLINE));
        /* Direct printers refuse AddJob and KEEPPRINTEDJOBS keeps finished jobs in the queue */
        bDirect = !!(pInfo2->Attributes & (PRINTER_ATTRIBUTE_DIRECT | PRINTER_ATTRIBUTE_KEEPPRINTEDJOBS));
    }
    if (pInfo2)
        HeapFree(GetProcessHeap(), 0, pInfo2);
    ClosePrinter(hPrinter);
    if (!bKnown || bPaused || bDirect)
    {
        skip("The queue of '%S' cannot be used (status known %d, paused or offline %d, direct or keeping jobs %d)\n", wszPrinter, bKnown, bPaused, bDirect);
        return;
    }

    TestOtherHandle(wszPrinter);

    /* The spooler still works */
    bResult = OpenPrinterW(wszPrinter, &hPrinter, NULL);
    ok(bResult, "OpenPrinterW failed with error %lu after the tests\n", GetLastError());
    if (bResult)
        ClosePrinter(hPrinter);
}
