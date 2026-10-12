/*
 * PROJECT:     ReactOS Local Spooler
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Implementation of the Thread that actually performs the printing process
 * COPYRIGHT:   Copyright 2015-2017 Colin Finck (colin@reactos.org)
 */

#include "precomp.h"

DWORD WINAPI
PrintingThreadProc(PLOCAL_JOB pJob)
{
    const DWORD cchMaxJobIdDigits = 5;              // Job ID is limited to 5 decimal digits, see IS_VALID_JOB_ID
    const WCHAR wszJobAppendix[] = L", Job ";
    const DWORD cchJobAppendix = _countof(wszJobAppendix) - 1;
    const WCHAR wszPortAppendix[] = L", Port";

    BOOL bPortLocked = FALSE;
    DWORD cchPortName;
    DWORD cchPrinterName;
    DWORD dwErrorCode;
    DWORD dwJobID = pJob->dwJobID;
    HANDLE hPrintProcessor = NULL;
    PLOCAL_JOB pFailedJob;
    PLOCAL_PORT pPort = pJob->pPrinter->pPort;
    PLOCAL_PRINT_PROCESSOR pPrintProcessor = pJob->pPrintProcessor;
    PRINTPROCESSOROPENDATA OpenData;
    PWSTR pwszPrinterAndJob = NULL;
    PWSTR pwszPrinterPort = NULL;
    PWSTR pwszSPLFile = NULL;

    TRACE("PrintingThreadProc(%p)\n", pJob);

    // A job with an output file is printed to the FILE: port, which writes it into that file.
    if (pJob->pwszOutputFile)
    {
        pPort = FindPort(L"FILE:");
        if (!pPort)
        {
            dwErrorCode = ERROR_UNKNOWN_PORT;
            ERR("No FILE: port for the output file of job %lu!\n", dwJobID);
            goto Cleanup;
        }
    }

    // Prepare the pPrinterName parameter.
    // This is the string for LocalOpenPrinter to open a port (e.g. "LPT1:, Port").
    cchPortName = wcslen(pPort->pwszName);
    pwszPrinterPort = DllAllocSplMem(cchPortName * sizeof(WCHAR) + sizeof(wszPortAppendix));
    if (!pwszPrinterPort)
    {
        dwErrorCode = ERROR_NOT_ENOUGH_MEMORY;
        ERR("DllAllocSplMem failed!\n");
        goto Cleanup;
    }

    CopyMemory(pwszPrinterPort, pPort->pwszName, cchPortName * sizeof(WCHAR));
    CopyMemory(&pwszPrinterPort[cchPortName], wszPortAppendix, sizeof(wszPortAppendix));

    // Prepare the pPrintProcessorOpenData parameter.
    OpenData.JobId = pJob->dwJobID;
    OpenData.pDatatype = pJob->pwszDatatype;
    OpenData.pDevMode = pJob->pDevMode;
    OpenData.pDocumentName = pJob->pwszDocumentName;
    OpenData.pOutputFile = pJob->pwszOutputFile;
    OpenData.pParameters = pJob->pwszPrintProcessorParameters;
    OpenData.pPrinterName = pJob->pPrinter->pwszPrinterName;

    // Associate our job to the port. The next port handle created through LocalOpenPrinter will pick this up.
    // LocalStartDocPrinter needs this information to call StartDocPort of the Print Monitor, but as the parameters
    // for LocalOpenPrinter and LocalStartDocPrinter are fixed, we can only pass over the information this way.
    // The port prints one job at a time, so that no other job takes over this association.
    // Only this thread, which runs the Print Processor, may claim the job.
    EnterCriticalSection(&pPort->PrintingSection);
    bPortLocked = TRUE;
    pPort->dwPrintingThreadId = GetCurrentThreadId();
    pPort->pNextJobToProcess = pJob;

    // Open a handle to the Print Processor.
    hPrintProcessor = pPrintProcessor->pfnOpenPrintProcessor(pwszPrinterPort, &OpenData);
    if (!hPrintProcessor)
    {
        dwErrorCode = GetLastError();
        ERR("OpenPrintProcessor failed with error %lu!\n", dwErrorCode);
        goto Cleanup;
    }

    // Let other functions use the Print Processor as well while it's opened.
    pJob->hPrintProcessor = hPrintProcessor;

    // Prepare the pDocumentName parameter.
    cchPrinterName = wcslen(OpenData.pPrinterName);
    pwszPrinterAndJob = DllAllocSplMem((cchPrinterName + cchJobAppendix + cchMaxJobIdDigits + 1) * sizeof(WCHAR));
    if (!pwszPrinterAndJob)
    {
        dwErrorCode = ERROR_NOT_ENOUGH_MEMORY;
        ERR("DllAllocSplMem failed!\n");
        goto Cleanup;
    }

    CopyMemory(pwszPrinterAndJob, OpenData.pPrinterName, cchPrinterName * sizeof(WCHAR));
    CopyMemory(&pwszPrinterAndJob[cchPrinterName], wszJobAppendix, cchJobAppendix * sizeof(WCHAR));
    _ultow(OpenData.JobId, &pwszPrinterAndJob[cchPrinterName + cchJobAppendix], 10);

    // Printing starts here.
    pJob->dwStatus |= JOB_STATUS_PRINTING;

    // Print the document.
    // Note that pJob is freed after this function, so we may not access it anymore.
    if (!pPrintProcessor->pfnPrintDocumentOnPrintProcessor(hPrintProcessor, pwszPrinterAndJob))
    {
        dwErrorCode = GetLastError();
        ERR("PrintDocumentOnPrintProcessor failed with error %lu!\n", dwErrorCode);
        goto Cleanup;
    }

    // Close the Print Processor.
    pPrintProcessor->pfnClosePrintProcessor(hPrintProcessor);
    hPrintProcessor = NULL;

    // Delete the spool file.
    pwszSPLFile = DllAllocSplMem(GetJobFilePath(L"SPL", 0, NULL));
    if (!pwszSPLFile)
    {
        dwErrorCode = ERROR_NOT_ENOUGH_MEMORY;
        ERR("DllAllocSplMem failed!\n");
        goto Cleanup;
    }

    GetJobFilePath(L"SPL", OpenData.JobId, pwszSPLFile);
    DeleteFileW(pwszSPLFile);

    // We were successful!
    dwErrorCode = ERROR_SUCCESS;

Cleanup:
    if (hPrintProcessor)
        pPrintProcessor->pfnClosePrintProcessor(hPrintProcessor);

    if (bPortLocked)
    {
        pPort->pNextJobToProcess = NULL;
        pPort->dwPrintingThreadId = 0;
        LeaveCriticalSection(&pPort->PrintingSection);
    }

    // A job that could not be printed is discarded with its spool file, so that it does not stay in the queue forever.
    // The job is still in the job list unless the Print Monitor reported it as sent to the printer.
    if (dwErrorCode != ERROR_SUCCESS)
    {
        WCHAR wszFullPath[MAX_PATH];

        ERR("Discarding job %lu, which could not be printed!\n", dwJobID);
        GetJobFilePath(L"SPL", dwJobID, wszFullPath);
        DeleteFileW(wszFullPath);

        pFailedJob = LookupElementSkiplist(&GlobalJobList, &dwJobID, NULL);
        if (pFailedJob)
        {
            pFailedJob->hPrintProcessor = NULL;
            FreeJob(pFailedJob);
        }
    }

    if (pwszPrinterPort)
        DllFreeSplMem(pwszPrinterPort);

    if (pwszPrinterAndJob)
        DllFreeSplMem(pwszPrinterAndJob);

    if (pwszSPLFile)
        DllFreeSplMem(pwszSPLFile);

    return dwErrorCode;
}
