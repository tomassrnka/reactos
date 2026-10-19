/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the job object UI restrictions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

/*
 * Chromium 109, sandbox/win/src/job.cc. The JobLevel cases there fall through
 * into each other, so each level takes the flags of the ones below it too:
 * kLockdown adds the four here to kLimitedUser's, which comes to all eight.
 */
#define JOB_LIMITEDUSER_UI (JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | \
                            JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS| \
                            JOB_OBJECT_UILIMIT_DESKTOP         | \
                            JOB_OBJECT_UILIMIT_EXITWINDOWS)

#define JOB_LOCKDOWN_UI (JOB_LIMITEDUSER_UI                 | \
                         JOB_OBJECT_UILIMIT_WRITECLIPBOARD  | \
                         JOB_OBJECT_UILIMIT_READCLIPBOARD   | \
                         JOB_OBJECT_UILIMIT_HANDLES         | \
                         JOB_OBJECT_UILIMIT_GLOBALATOMS)

static const ULONG SingleRestrictions[] =
{
    JOB_OBJECT_UILIMIT_HANDLES,
    JOB_OBJECT_UILIMIT_READCLIPBOARD,
    JOB_OBJECT_UILIMIT_WRITECLIPBOARD,
    JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS,
    JOB_OBJECT_UILIMIT_DISPLAYSETTINGS,
    JOB_OBJECT_UILIMIT_GLOBALATOMS,
    JOB_OBJECT_UILIMIT_DESKTOP,
    JOB_OBJECT_UILIMIT_EXITWINDOWS,
};

static
BOOL
SetRestrictions(
    _In_ HANDLE hJob,
    _In_ ULONG Restrictions)
{
    JOBOBJECT_BASIC_UI_RESTRICTIONS Info;

    Info.UIRestrictionsClass = Restrictions;
    return SetInformationJobObject(hJob,
                                   JobObjectBasicUIRestrictions,
                                   &Info,
                                   sizeof(Info));
}

static
void
test_RoundTrip(void)
{
    JOBOBJECT_BASIC_UI_RESTRICTIONS Info;
    HANDLE hJob;
    DWORD Returned;
    BOOL Success;
    ULONG i;

    hJob = CreateJobObjectW(NULL, NULL);
    ok(hJob != NULL, "CreateJobObject failed with %lu\n", GetLastError());
    if (hJob == NULL)
        return;

    /* A fresh job restricts nothing */
    memset(&Info, 0xAA, sizeof(Info));
    Returned = 0;
    SetLastError(0xDEADBEEF);
    Success = QueryInformationJobObject(hJob,
                                        JobObjectBasicUIRestrictions,
                                        &Info,
                                        sizeof(Info),
                                        &Returned);
    ok(Success != FALSE, "QueryInformationJobObject failed with %lu\n", GetLastError());
    ok_long(Info.UIRestrictionsClass, 0);
    ok_long(Returned, sizeof(Info));

    /* Every flag on its own, so one mistake cannot hide behind the others */
    for (i = 0; i < _countof(SingleRestrictions); i++)
    {
        SetLastError(0xDEADBEEF);
        Success = SetRestrictions(hJob, SingleRestrictions[i]);
        ok(Success != FALSE, "Setting 0x%lx failed with %lu\n",
           SingleRestrictions[i], GetLastError());
        if (Success == FALSE)
        {
            /* Everything below only means something once it is set */
            skip("0x%lx could not be set\n", SingleRestrictions[i]);
            continue;
        }

        memset(&Info, 0xAA, sizeof(Info));
        Returned = 0;
        Success = QueryInformationJobObject(hJob,
                                            JobObjectBasicUIRestrictions,
                                            &Info,
                                            sizeof(Info),
                                            &Returned);
        ok(Success != FALSE, "Querying 0x%lx failed with %lu\n",
           SingleRestrictions[i], GetLastError());
        if (Success == FALSE)
        {
            skip("0x%lx could not be queried\n", SingleRestrictions[i]);
        }
        else
        {
            ok_long(Info.UIRestrictionsClass, SingleRestrictions[i]);
            ok_long(Returned, sizeof(Info));
        }

        /* Back to nothing, which drops the per-job state win32k keeps */
        SetLastError(0xDEADBEEF);
        Success = SetRestrictions(hJob, 0);
        ok(Success != FALSE, "Clearing 0x%lx failed with %lu\n",
           SingleRestrictions[i], GetLastError());
        if (Success == FALSE)
        {
            /* The job stays restricted, so the round after this one would be
               starting from the wrong place */
            skip("0x%lx could not be cleared\n", SingleRestrictions[i]);
            continue;
        }

        memset(&Info, 0xAA, sizeof(Info));
        Success = QueryInformationJobObject(hJob,
                                            JobObjectBasicUIRestrictions,
                                            &Info,
                                            sizeof(Info),
                                            NULL);
        ok(Success != FALSE, "QueryInformationJobObject failed with %lu\n", GetLastError());
        if (Success != FALSE)
            ok_long(Info.UIRestrictionsClass, 0);
    }

    /* All of them at once */
    SetLastError(0xDEADBEEF);
    Success = SetRestrictions(hJob, JOB_OBJECT_UILIMIT_ALL);
    ok(Success != FALSE, "Setting JOB_OBJECT_UILIMIT_ALL failed with %lu\n", GetLastError());

    memset(&Info, 0xAA, sizeof(Info));
    Success = QueryInformationJobObject(hJob,
                                        JobObjectBasicUIRestrictions,
                                        &Info,
                                        sizeof(Info),
                                        NULL);
    ok(Success != FALSE, "QueryInformationJobObject failed with %lu\n", GetLastError());
    ok_long(Info.UIRestrictionsClass, JOB_OBJECT_UILIMIT_ALL);

    /* Setting the same value twice is not an error */
    SetLastError(0xDEADBEEF);
    Success = SetRestrictions(hJob, JOB_OBJECT_UILIMIT_ALL);
    ok(Success != FALSE, "Setting the same restrictions again failed with %lu\n",
       GetLastError());

    /* Close it while still restricted, to exercise the delete path */
    CloseHandle(hJob);
}

static
void
test_InvalidParameters(void)
{
    JOBOBJECT_BASIC_UI_RESTRICTIONS Info;
    HANDLE hJob;
    DWORD Returned;
    BOOL Success;

    hJob = CreateJobObjectW(NULL, NULL);
    ok(hJob != NULL, "CreateJobObject failed with %lu\n", GetLastError());
    if (hJob == NULL)
        return;

    /* Undefined bits must be rejected, and must not be stored */
    SetLastError(0xDEADBEEF);
    Success = SetRestrictions(hJob, 0xDEAD0000);
    ok(Success == FALSE, "Setting undefined restrictions succeeded\n");
    ok_err(ERROR_INVALID_PARAMETER);

    SetLastError(0xDEADBEEF);
    Success = SetRestrictions(hJob, JOB_OBJECT_UILIMIT_ALL | 0x100);
    ok(Success == FALSE, "Setting one undefined bit succeeded\n");
    ok_err(ERROR_INVALID_PARAMETER);

    memset(&Info, 0xAA, sizeof(Info));
    Success = QueryInformationJobObject(hJob,
                                        JobObjectBasicUIRestrictions,
                                        &Info,
                                        sizeof(Info),
                                        NULL);
    ok(Success != FALSE, "QueryInformationJobObject failed with %lu\n", GetLastError());
    ok_long(Info.UIRestrictionsClass, 0);

    /* The class is fixed length in both directions */
    Info.UIRestrictionsClass = JOB_OBJECT_UILIMIT_HANDLES;
    SetLastError(0xDEADBEEF);
    Success = SetInformationJobObject(hJob,
                                      JobObjectBasicUIRestrictions,
                                      &Info,
                                      sizeof(Info) - 1);
    ok(Success == FALSE, "SetInformationJobObject with a short buffer succeeded\n");
    ok_err(ERROR_BAD_LENGTH);

    SetLastError(0xDEADBEEF);
    Success = SetInformationJobObject(hJob,
                                      JobObjectBasicUIRestrictions,
                                      &Info,
                                      sizeof(Info) + 1);
    ok(Success == FALSE, "SetInformationJobObject with a long buffer succeeded\n");
    ok_err(ERROR_BAD_LENGTH);

    Returned = 0;
    SetLastError(0xDEADBEEF);
    Success = QueryInformationJobObject(hJob,
                                        JobObjectBasicUIRestrictions,
                                        &Info,
                                        sizeof(Info) - 1,
                                        &Returned);
    ok(Success == FALSE, "QueryInformationJobObject with a short buffer succeeded\n");
    ok_err(ERROR_BAD_LENGTH);

    /* An inaccessible buffer must be reported, not raised */
    SetLastError(0xDEADBEEF);
    Success = SetInformationJobObject(hJob,
                                      JobObjectBasicUIRestrictions,
                                      NULL,
                                      sizeof(Info));
    ok(Success == FALSE, "SetInformationJobObject with a NULL buffer succeeded\n");
    ok_err(ERROR_NOACCESS);

    CloseHandle(hJob);
}

/* A handle without JOB_OBJECT_SET_ATTRIBUTES may not change the restrictions */
static
void
test_Access(void)
{
    JOBOBJECT_BASIC_UI_RESTRICTIONS Info;
    HANDLE hJob, hQueryOnly;
    BOOL Success;

    hJob = CreateJobObjectW(NULL, NULL);
    ok(hJob != NULL, "CreateJobObject failed with %lu\n", GetLastError());
    if (hJob == NULL)
        return;

    hQueryOnly = NULL;
    Success = DuplicateHandle(GetCurrentProcess(),
                              hJob,
                              GetCurrentProcess(),
                              &hQueryOnly,
                              JOB_OBJECT_QUERY,
                              FALSE,
                              0);
    ok(Success != FALSE, "DuplicateHandle failed with %lu\n", GetLastError());
    if (Success)
    {
        SetLastError(0xDEADBEEF);
        Success = SetRestrictions(hQueryOnly, JOB_OBJECT_UILIMIT_HANDLES);
        ok(Success == FALSE, "Setting restrictions through a query handle succeeded\n");
        ok_err(ERROR_ACCESS_DENIED);

        memset(&Info, 0xAA, sizeof(Info));
        Success = QueryInformationJobObject(hQueryOnly,
                                            JobObjectBasicUIRestrictions,
                                            &Info,
                                            sizeof(Info),
                                            NULL);
        ok(Success != FALSE, "QueryInformationJobObject failed with %lu\n", GetLastError());
        ok_long(Info.UIRestrictionsClass, 0);

        CloseHandle(hQueryOnly);
    }

    CloseHandle(hJob);
}

/*
 * The two policies the Chromium sandbox builds, in the order it builds them.
 * The GPU one excepts every UI restriction away again, which has to end up
 * clearing them rather than failing.
 */
static
void
test_SandboxPolicies(void)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ExtendedLimit;
    JOBOBJECT_BASIC_UI_RESTRICTIONS Info;
    HANDLE hJob;
    BOOL Success;

    /* sandbox::Job::Init(JOB_LOCKDOWN) */
    hJob = CreateJobObjectW(NULL, NULL);
    ok(hJob != NULL, "CreateJobObject failed with %lu\n", GetLastError());
    if (hJob == NULL)
        return;

    memset(&ExtendedLimit, 0, sizeof(ExtendedLimit));
    ExtendedLimit.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ExtendedLimit.BasicLimitInformation.ActiveProcessLimit = 1;

    SetLastError(0xDEADBEEF);
    Success = SetInformationJobObject(hJob,
                                      JobObjectExtendedLimitInformation,
                                      &ExtendedLimit,
                                      sizeof(ExtendedLimit));
    ok(Success != FALSE, "Setting the lockdown limits failed with %lu\n", GetLastError());

    SetLastError(0xDEADBEEF);
    Success = SetRestrictions(hJob, JOB_LOCKDOWN_UI);
    ok(Success != FALSE, "Setting the lockdown restrictions failed with %lu\n",
       GetLastError());

    memset(&Info, 0xAA, sizeof(Info));
    Success = QueryInformationJobObject(hJob,
                                        JobObjectBasicUIRestrictions,
                                        &Info,
                                        sizeof(Info),
                                        NULL);
    ok(Success != FALSE, "QueryInformationJobObject failed with %lu\n", GetLastError());
    ok_long(Info.UIRestrictionsClass, JOB_LOCKDOWN_UI);

    CloseHandle(hJob);

    /* Job::Init(JobLevel::kLimitedUser), which does not take kLockdown's four */
    hJob = CreateJobObjectW(NULL, NULL);
    ok(hJob != NULL, "CreateJobObject failed with %lu\n", GetLastError());
    if (hJob == NULL)
        return;

    memset(&ExtendedLimit, 0, sizeof(ExtendedLimit));
    ExtendedLimit.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ExtendedLimit.BasicLimitInformation.ActiveProcessLimit = 1;

    SetLastError(0xDEADBEEF);
    Success = SetInformationJobObject(hJob,
                                      JobObjectExtendedLimitInformation,
                                      &ExtendedLimit,
                                      sizeof(ExtendedLimit));
    ok(Success != FALSE, "Setting the limited user limits failed with %lu\n", GetLastError());

    SetLastError(0xDEADBEEF);
    Success = SetRestrictions(hJob, JOB_LIMITEDUSER_UI);
    ok(Success != FALSE, "Setting the limited user restrictions failed with %lu\n",
       GetLastError());

    memset(&Info, 0xAA, sizeof(Info));
    Success = QueryInformationJobObject(hJob,
                                        JobObjectBasicUIRestrictions,
                                        &Info,
                                        sizeof(Info),
                                        NULL);
    ok(Success != FALSE, "QueryInformationJobObject failed with %lu\n", GetLastError());
    ok_long(Info.UIRestrictionsClass, JOB_LIMITEDUSER_UI);

    CloseHandle(hJob);

    /*
     * job.cc applies the caller's exceptions as
     * jbur.UIRestrictionsClass &= ~ui_exceptions before its single
     * SetInformationJobObject, so a delegate that excepts everything away asks
     * for a mask of zero. That has to be accepted, not refused.
     */
    hJob = CreateJobObjectW(NULL, NULL);
    ok(hJob != NULL, "CreateJobObject failed with %lu\n", GetLastError());
    if (hJob == NULL)
        return;

    SetLastError(0xDEADBEEF);
    Success = SetRestrictions(hJob, JOB_LOCKDOWN_UI & ~JOB_OBJECT_UILIMIT_ALL);
    ok(Success != FALSE, "Excepting every restriction away failed with %lu\n",
       GetLastError());

    memset(&Info, 0xAA, sizeof(Info));
    Success = QueryInformationJobObject(hJob,
                                        JobObjectBasicUIRestrictions,
                                        &Info,
                                        sizeof(Info),
                                        NULL);
    ok(Success != FALSE, "QueryInformationJobObject failed with %lu\n", GetLastError());
    ok_long(Info.UIRestrictionsClass, 0);

    CloseHandle(hJob);
}

/* A completion port associated with a job reports a process that is assigned to it,
 * its exit and the end of the last process, with the key given at association. */
static
VOID
test_CompletionPort(VOID)
{
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT Port;
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Child = { 0 };
    WCHAR Command[MAX_PATH + 32];
    HANDLE hJob, hPort;
    DWORD Message, i;
    ULONG_PTR Key;
    LPOVERLAPPED Overlapped;
    BOOL SeenNew = FALSE, SeenExit = FALSE, SeenZero = FALSE;

    hJob = CreateJobObjectW(NULL, NULL);
    hPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    ok(hJob != NULL && hPort != NULL, "CreateJobObject/CreateIoCompletionPort failed: %lu\n", GetLastError());
    if (!hJob || !hPort)
        goto Cleanup;

    Port.CompletionKey = (PVOID)(ULONG_PTR)0x1234;
    Port.CompletionPort = hPort;
    ok(SetInformationJobObject(hJob, JobObjectAssociateCompletionPortInformation, &Port, sizeof(Port)),
       "Associating the port failed: %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    ok(!SetInformationJobObject(hJob, JobObjectAssociateCompletionPortInformation, &Port, sizeof(Port) - 1),
       "A short association succeeded\n");
    ok(GetLastError() == ERROR_BAD_LENGTH, "Error %lu\n", GetLastError());

    GetSystemDirectoryW(Command, MAX_PATH);
    StringCchCatW(Command, RTL_NUMBER_OF(Command), L"\\cmd.exe /c exit 3");
    if (!CreateProcessW(NULL, Command, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, NULL, &Startup, &Child))
    {
        skip("CreateProcess failed: %lu\n", GetLastError());
        goto Cleanup;
    }
    ok(AssignProcessToJobObject(hJob, Child.hProcess), "AssignProcessToJobObject failed: %lu\n", GetLastError());
    ResumeThread(Child.hThread);
    WaitForSingleObject(Child.hProcess, 30000);

    for (i = 0; i < 16 && !SeenZero; i++)
    {
        if (!GetQueuedCompletionStatus(hPort, &Message, &Key, &Overlapped, 5000))
            break;
        ok(Key == 0x1234, "Message %lu with key 0x%Ix\n", Message, Key);
        /* Windows 10 also reports the console host the child starts in the job */
        if (Message == JOB_OBJECT_MSG_NEW_PROCESS && (ULONG_PTR)Overlapped == Child.dwProcessId)
            SeenNew = TRUE;
        else if ((Message == JOB_OBJECT_MSG_EXIT_PROCESS || Message == JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS) &&
                 (ULONG_PTR)Overlapped == Child.dwProcessId)
            SeenExit = TRUE;
        else if (Message == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO)
        {
            SeenZero = TRUE;
        }
    }
    ok(SeenNew, "No JOB_OBJECT_MSG_NEW_PROCESS for the child\n");
    ok(SeenExit, "No exit message for the child\n");
    ok(SeenZero, "No JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO\n");
    CloseHandle(Child.hThread);
    CloseHandle(Child.hProcess);

Cleanup:
    if (hJob)
        CloseHandle(hJob);
    if (hPort)
        CloseHandle(hPort);
}

START_TEST(JobObject)
{
    test_CompletionPort();
    test_RoundTrip();
    test_InvalidParameters();
    test_Access();
    test_SandboxPolicies();
}
