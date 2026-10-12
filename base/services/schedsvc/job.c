/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS Services
 * FILE:             base/services/schedsvc/job.c
 * PURPOSE:          Scheduling service
 * PROGRAMMER:       Eric Kohl <eric.kohl@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include "precomp.h"
#include <winbase_undoc.h>

WINE_DEFAULT_DEBUG_CHANNEL(schedsvc);


/* GLOBALS ******************************************************************/

typedef struct _SCHEDULE
{
    DWORD JobTime;
    DWORD DaysOfMonth;
    UCHAR DaysOfWeek;
    UCHAR Flags;
    WORD Reserved;
} SCHEDULE, PSCHEDULE;

DWORD dwNextJobId = 1;
DWORD dwJobCount = 0;
LIST_ENTRY JobListHead;
RTL_RESOURCE JobListLock;

LIST_ENTRY StartListHead;
RTL_RESOURCE StartListLock;
FILETIME NextJobStartTime;
BOOL bValidNextJobStartTime = FALSE;



/* FUNCTIONS *****************************************************************/

VOID
GetNextJobTimeout(HANDLE hTimer)
{
    PLIST_ENTRY CurrentEntry;
    FILETIME DueTime;
    PJOB CurrentJob;

    bValidNextJobStartTime = FALSE;
    CurrentEntry = JobListHead.Flink;
    while (CurrentEntry != &JobListHead)
    {
        CurrentJob = CONTAINING_RECORD(CurrentEntry, JOB, JobEntry);

        if (bValidNextJobStartTime == FALSE)
        {
            CopyMemory(&NextJobStartTime, &CurrentJob->StartTime, sizeof(FILETIME));
            bValidNextJobStartTime = TRUE;
        }
        else
        {
            if (CompareFileTime(&NextJobStartTime, &CurrentJob->StartTime) > 0)
                CopyMemory(&NextJobStartTime, &CurrentJob->StartTime, sizeof(FILETIME));
        }

        CurrentEntry = CurrentEntry->Flink;
    }

    if (bValidNextJobStartTime == FALSE)
    {
        TRACE("No valid job!\n");
        CancelWaitableTimer(hTimer);
        return;
    }

    LocalFileTimeToFileTime(&NextJobStartTime, &DueTime);

    SetWaitableTimer(hTimer,
                     (PLARGE_INTEGER)&DueTime,
                     0,
                     NULL,
                     NULL,
                     TRUE);
}

/* Called after a job was started: a periodic job gets its next start time, a one-time job
 * loses the day it ran for and is removed when no day is left */
static
VOID
ReScheduleJob(
    PJOB pJob)
{
    SYSTEMTIME StartTime;
    BOOL bRemove = FALSE;

    if ((pJob->Flags & JOB_RUN_PERIODICALLY) == 0)
    {
        if (pJob->DaysOfWeek == 0 && pJob->DaysOfMonth == 0)
        {
            bRemove = TRUE;
        }
        else
        {
            FileTimeToSystemTime(&pJob->StartTime, &StartTime);
            /* Bit 0 of DaysOfWeek is Monday, wDayOfWeek 0 is Sunday */
            pJob->DaysOfWeek &= ~(1 << ((StartTime.wDayOfWeek + 6) % 7));
            pJob->DaysOfMonth &= ~(1UL << (StartTime.wDay - 1));
            bRemove = (pJob->DaysOfWeek == 0 && pJob->DaysOfMonth == 0);
            if (!bRemove)
                SaveJob(pJob);
        }
    }

    if (bRemove)
    {
        DeleteJob(pJob);
        RemoveEntryList(&pJob->JobEntry);
        dwJobCount--;
        HeapFree(GetProcessHeap(), 0, pJob);
        return;
    }

    CalculateNextStartTime(pJob);
}


VOID
RunCurrentJobs(VOID)
{
    PROCESS_INFORMATION ProcessInformation;
    STARTUPINFOW StartupInfo;
    PLIST_ENTRY CurrentEntry, NextEntry;
    PJOB CurrentJob;
    BOOL bRet;

    CurrentEntry = JobListHead.Flink;
    while (CurrentEntry != &JobListHead)
    {
        CurrentJob = CONTAINING_RECORD(CurrentEntry, JOB, JobEntry);
        NextEntry = CurrentEntry->Flink;

        if (CompareFileTime(&NextJobStartTime, &CurrentJob->StartTime) == 0)
        {
            TRACE("Run job %ld: %S\n", CurrentJob->JobId, CurrentJob->Command);

            ZeroMemory(&StartupInfo, sizeof(StartupInfo));
            StartupInfo.cb = sizeof(StartupInfo);
            StartupInfo.lpTitle = CurrentJob->Command;
            StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
            StartupInfo.wShowWindow = SW_SHOWDEFAULT;

            if ((CurrentJob->Flags & JOB_NONINTERACTIVE) == 0)
            {
                StartupInfo.dwFlags |= STARTF_INHERITDESKTOP;
                StartupInfo.lpDesktop = L"WinSta0\\Default";
            }

            bRet = CreateProcessW(NULL,
                                  CurrentJob->Command,
                                  NULL,
                                  NULL,
                                  FALSE,
                                  CREATE_NEW_CONSOLE,
                                  NULL,
                                  NULL,
                                  &StartupInfo,
                                  &ProcessInformation);
            if (bRet == FALSE)
            {
                ERR("CreateProcessW() failed (Error %lu)\n", GetLastError());
            }
            else
            {
                CloseHandle(ProcessInformation.hThread);
                CloseHandle(ProcessInformation.hProcess);
            }

            /* Without this the job would be due again at once */
            ReScheduleJob(CurrentJob);
        }

        CurrentEntry = NextEntry;
    }
}


static
VOID
GetJobName(
    HKEY hJobsKey,
    PWSTR pszJobName)
{
    WCHAR szNameBuffer[JOB_NAME_LENGTH];
    FILETIME SystemTime;
    ULONG ulSeed, ulValue;
    HKEY hKey;
    LONG lError;

    GetSystemTimeAsFileTime(&SystemTime);
    ulSeed = SystemTime.dwLowDateTime;
    for (;;)
    {
        ulValue = RtlRandomEx(&ulSeed);
        _swprintf(szNameBuffer, L"%08lx", ulValue);

        hKey = NULL;
        lError = RegOpenKeyEx(hJobsKey,
                              szNameBuffer,
                              0,
                              KEY_READ,
                              &hKey);
        if (lError != ERROR_SUCCESS)
        {
            wcscpy(pszJobName, szNameBuffer);
            return;
        }

        RegCloseKey(hKey);
    }
}


LONG
SaveJob(
    _In_ PJOB pJob)
{
    SCHEDULE Schedule;
    HKEY hJobsKey = NULL, hJobKey = NULL;
    LONG lError;

    TRACE("SaveJob()\n");

    lError = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                             L"System\\CurrentControlSet\\Services\\Schedule\\Jobs",
                             0,
                             NULL,
                             REG_OPTION_NON_VOLATILE,
                             KEY_WRITE,
                             NULL,
                             &hJobsKey,
                             NULL);
    if (lError != ERROR_SUCCESS)
        goto done;

    if (pJob->Name[0] == UNICODE_NULL)
        GetJobName(hJobsKey, pJob->Name);

    lError = RegCreateKeyExW(hJobsKey,
                             pJob->Name,
                             0,
                             NULL,
                             REG_OPTION_NON_VOLATILE,
                             KEY_WRITE,
                             NULL,
                             &hJobKey,
                             NULL);
    if (lError != ERROR_SUCCESS)
        goto done;

    Schedule.JobTime = pJob->JobTime;
    Schedule.DaysOfMonth = pJob->DaysOfMonth;
    Schedule.DaysOfWeek = pJob->DaysOfWeek;
    Schedule.Flags = pJob->Flags;

    lError = RegSetValueEx(hJobKey,
                           L"Schedule",
                           0,
                           REG_BINARY,
                           (PBYTE)&Schedule,
                           sizeof(Schedule));
    if (lError != ERROR_SUCCESS)
        goto done;

    lError = RegSetValueEx(hJobKey,
                           L"Command",
                           0,
                           REG_SZ,
                           (PBYTE)pJob->Command,
                           (wcslen(pJob->Command) + 1) * sizeof(WCHAR));
    if (lError != ERROR_SUCCESS)
        goto done;

done:
    if (hJobKey != NULL)
        RegCloseKey(hJobKey);

    if (hJobsKey != NULL)
        RegCloseKey(hJobsKey);

    return lError;
}


LONG
DeleteJob(
    _In_ PJOB pJob)
{
    HKEY hJobsKey = NULL;
    LONG lError;

    TRACE("DeleteJob()\n");

    lError = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                             L"System\\CurrentControlSet\\Services\\Schedule\\Jobs",
                             0,
                             NULL,
                             REG_OPTION_NON_VOLATILE,
                             KEY_WRITE,
                             NULL,
                             &hJobsKey,
                             NULL);
    if (lError != ERROR_SUCCESS)
        goto done;

    lError = RegDeleteKey(hJobsKey,
                          pJob->Name);
    if (lError != ERROR_SUCCESS)
        goto done;

done:
    if (hJobsKey != NULL)
        RegCloseKey(hJobsKey);

    return lError;
}


LONG
LoadJobs(VOID)
{
    SCHEDULE Schedule;
    WCHAR szNameBuffer[JOB_NAME_LENGTH];
    DWORD dwNameLength, dwIndex, dwSize;
    HKEY hJobsKey = NULL, hJobKey = NULL;
    PJOB pJob = NULL;
    LONG lError;

    TRACE("LoadJobs()\n");

    lError = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                             L"System\\CurrentControlSet\\Services\\Schedule\\Jobs",
                             0,
                             NULL,
                             REG_OPTION_NON_VOLATILE,
                             KEY_READ,
                             NULL,
                             &hJobsKey,
                             NULL);
    if (lError != ERROR_SUCCESS)
        goto done;

    for (dwIndex = 0; dwIndex < 1000; dwIndex++)
    {
        dwNameLength = JOB_NAME_LENGTH;
        lError = RegEnumKeyEx(hJobsKey,
                              dwIndex,
                              szNameBuffer,
                              &dwNameLength,
                              NULL,
                              NULL,
                              NULL,
                              NULL);
        if (lError != ERROR_SUCCESS)
        {
            lError = ERROR_SUCCESS;
            break;
        }

        TRACE("KeyName: %S\n", szNameBuffer);

        lError = RegOpenKeyEx(hJobsKey,
                              szNameBuffer,
                              0,
                              KEY_READ,
                              &hJobKey);
        if (lError != ERROR_SUCCESS)
            break;

        dwSize = sizeof(SCHEDULE);
        lError = RegQueryValueEx(hJobKey,
                                 L"Schedule",
                                 NULL,
                                 NULL,
                                 (PBYTE)&Schedule,
                                 &dwSize);
        if (lError == ERROR_SUCCESS)
        {
            dwSize = 0;
            RegQueryValueEx(hJobKey,
                            L"Command",
                            NULL,
                            NULL,
                            NULL,
                            &dwSize);
            if (dwSize != 0)
            {
                /* Allocate a new job object */
                pJob = HeapAlloc(GetProcessHeap(),
                                 HEAP_ZERO_MEMORY,
                                 sizeof(JOB) + dwSize - sizeof(WCHAR));
                if (pJob == NULL)
                {
                    lError = ERROR_OUTOFMEMORY;
                    break;
                }

                lError = RegQueryValueEx(hJobKey,
                                         L"Command",
                                         NULL,
                                         NULL,
                                         (PBYTE)pJob->Command,
                                         &dwSize);
                if (lError != ERROR_SUCCESS)
                    break;

                wcscpy(pJob->Name, szNameBuffer);
                pJob->JobTime = Schedule.JobTime;
                pJob->DaysOfMonth = Schedule.DaysOfMonth;
                pJob->DaysOfWeek = Schedule.DaysOfWeek;
                pJob->Flags = Schedule.Flags;

                /* Acquire the job list lock exclusively */
                RtlAcquireResourceExclusive(&JobListLock, TRUE);

                /* Assign a new job ID */
                pJob->JobId = dwNextJobId++;
                dwJobCount++;

                /* Append the new job to the job list */
                InsertTailList(&JobListHead, &pJob->JobEntry);

                /* Calculate the next start time */
                CalculateNextStartTime(pJob);

#if 0
                DumpStartList(&StartListHead);
#endif

                /* Release the job list lock */
                RtlReleaseResource(&JobListLock);

                pJob = NULL;
            }
        }

        RegCloseKey(hJobKey);
        hJobKey = NULL;
    }

done:
    if (pJob != NULL)
        HeapFree(GetProcessHeap(), 0, pJob);

    if (hJobKey != NULL)
        RegCloseKey(hJobKey);

    if (hJobsKey != NULL)
        RegCloseKey(hJobsKey);

    return lError;
}


/* The next start time (local time) of a job: the first day, from today on, that is one of the
 * requested days of the week or month (any day if none is requested) and where the job time of
 * day has not passed yet. Times of day are compared to the millisecond, and a start time that just
 * passed (the timer may fire early) is never chosen again. */
VOID
CalculateNextStartTime(
    _In_ PJOB pJob)
{
    SYSTEMTIME Now, Day;
    FILETIME FileTime;
    ULARGE_INTEGER Midnight, Candidate;
    DWORD_PTR NowMs;
    UINT i;

    TRACE("CalculateNextStartTime(%p)\n", pJob);
    TRACE("JobTime: %lu\n", pJob->JobTime);
    TRACE("DaysOfWeek: 0x%x\n", pJob->DaysOfWeek);
    TRACE("DaysOfMonth: 0x%x\n", pJob->DaysOfMonth);

    GetLocalTime(&Now);

    if (pJob->StartTime.dwLowDateTime != 0 || pJob->StartTime.dwHighDateTime != 0)
    {
        SystemTimeToFileTime(&Now, &FileTime);
        if (CompareFileTime(&FileTime, &pJob->StartTime) < 0)
            FileTimeToSystemTime(&pJob->StartTime, &Now);
    }

    NowMs = (DWORD_PTR)Now.wHour * 3600000 + (DWORD_PTR)Now.wMinute * 60000 +
            (DWORD_PTR)Now.wSecond * 1000 + Now.wMilliseconds;

    Day = Now;
    Day.wHour = Day.wMinute = Day.wSecond = Day.wMilliseconds = 0;
    SystemTimeToFileTime(&Day, &FileTime);
    Midnight.LowPart = FileTime.dwLowDateTime;
    Midnight.HighPart = FileTime.dwHighDateTime;

    /* Every requested day of the month or week comes within a year */
    for (i = 0; i < 366; i++)
    {
        Candidate.QuadPart = Midnight.QuadPart + (ULONGLONG)i * 24 * 60 * 60 * 10000000;

        if (i == 0 && NowMs >= pJob->JobTime)
            continue;

        FileTime.dwLowDateTime = Candidate.LowPart;
        FileTime.dwHighDateTime = Candidate.HighPart;
        FileTimeToSystemTime(&FileTime, &Day);

        /* Bit 0 of DaysOfWeek is Monday, wDayOfWeek 0 is Sunday */
        if ((pJob->DaysOfWeek == 0 && pJob->DaysOfMonth == 0) ||
            (pJob->DaysOfMonth & (1UL << (Day.wDay - 1))) ||
            (pJob->DaysOfWeek & (1 << ((Day.wDayOfWeek + 6) % 7))))
        {
            break;
        }
    }

    Candidate.QuadPart += (ULONGLONG)pJob->JobTime * 10000;

    pJob->StartTime.dwLowDateTime = Candidate.LowPart;
    pJob->StartTime.dwHighDateTime = Candidate.HighPart;
}

#if 0
VOID
DumpStartList(
    _In_ PLIST_ENTRY StartListHead)
{
    PLIST_ENTRY CurrentEntry;
    PJOB CurrentJob;

    CurrentEntry = JobListHead->Flink;
    while (CurrentEntry != &JobListHead)
    {
        CurrentJob = CONTAINING_RECORD(CurrentEntry, JOB, StartEntry);

        TRACE("%3lu: %016I64x\n", CurrentJob->JobId, CurrentJob->StartTime.QuadPart);

        CurrentEntry = CurrentEntry->Flink;
    }
}
#endif
/* EOF */
