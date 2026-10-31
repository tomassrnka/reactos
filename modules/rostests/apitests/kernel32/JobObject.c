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

/* The limits a job was given come back from a query, in the caller's layout */
static
VOID
test_LimitQuery(VOID)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Set, Got;
    JOBOBJECT_BASIC_LIMIT_INFORMATION Basic;
    HANDLE hJob = CreateJobObjectW(NULL, NULL);
    DWORD Length = 0;

    ok(hJob != NULL, "CreateJobObject failed: %lu\n", GetLastError());
    if (!hJob)
        return;
    ZeroMemory(&Set, sizeof(Set));
    Set.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                           JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    Set.BasicLimitInformation.ActiveProcessLimit = 1;
    Set.ProcessMemoryLimit = 0x10000000;
    ok(SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &Set, sizeof(Set)),
       "SetInformationJobObject failed: %lu\n", GetLastError());
    memset(&Got, 0xcc, sizeof(Got));
    ok(QueryInformationJobObject(hJob, JobObjectExtendedLimitInformation, &Got, sizeof(Got), &Length),
       "QueryInformationJobObject failed: %lu\n", GetLastError());
    ok(Length == sizeof(Got), "Length %lu\n", Length);
    ok(Got.BasicLimitInformation.LimitFlags == Set.BasicLimitInformation.LimitFlags, "Flags 0x%lx\n",
       Got.BasicLimitInformation.LimitFlags);
    ok(Got.BasicLimitInformation.ActiveProcessLimit == 1, "Active process limit %lu\n",
       Got.BasicLimitInformation.ActiveProcessLimit);
    ok(Got.ProcessMemoryLimit == 0x10000000, "Process memory limit 0x%Ix\n", Got.ProcessMemoryLimit);
    memset(&Basic, 0xcc, sizeof(Basic));
    Length = 0;
    ok(QueryInformationJobObject(hJob, JobObjectBasicLimitInformation, &Basic, sizeof(Basic), &Length),
       "QueryInformationJobObject(basic) failed: %lu\n", GetLastError());
    ok(Length == sizeof(Basic), "Length %lu\n", Length);
    ok(Basic.LimitFlags == Set.BasicLimitInformation.LimitFlags, "Basic flags 0x%lx\n", Basic.LimitFlags);
    SetLastError(0xdeadbeef);
    ok(!QueryInformationJobObject(hJob, JobObjectExtendedLimitInformation, &Got, sizeof(Got) - 1, &Length),
       "A short query succeeded\n");
    ok(GetLastError() == ERROR_BAD_LENGTH, "Error %lu\n", GetLastError());
    CloseHandle(hJob);
}

/*
 * Regression test for bugcheck 0x20 (KERNEL_APC_PENDING_DURING_EXIT).
 *
 * A process that self-assigns to a job with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
 * and then closes the last job handle makes the kernel terminate the current
 * process from the job-close kill path. If that termination runs the current
 * threads exit from inside the job lock critical region, PspExitThread exits
 * with APCs disabled and the machine bugchecks (0x20). Nodes child_process
 * and Chromium use exactly this pattern on teardown.
 *
 * This runs in a child process: on an unfixed kernel the child (and the whole
 * VM) bugchecks, so the parents wait never returns. On a fixed kernel the
 * child is cleanly killed by kill-on-close and the parent observes its exit.
 */
static
int
KillOnCloseChild(void)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Eli;
    HANDLE hJob;

    hJob = CreateJobObjectW(NULL, NULL);
    if (hJob == NULL)
        return 11;

    ZeroMemory(&Eli, sizeof(Eli));
    Eli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &Eli, sizeof(Eli)))
        return 12;

    if (!AssignProcessToJobObject(hJob, GetCurrentProcess()))
        return 13;

    /* Last handle: kill-on-close fires and should terminate us right here. */
    CloseHandle(hJob);

    /* Reaching this point means kill-on-close did not kill us. That is itself
       a behavior bug, but it is not the crash this test guards against. */
    return 123;
}

static
void
test_KillOnClose(void)
{
    WCHAR Self[MAX_PATH];
    WCHAR CmdLine[MAX_PATH + 32];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD Length, Wait, Code = 0xDEADBEEF;

    Length = GetModuleFileNameW(NULL, Self, ARRAYSIZE(Self));
    ok(Length != 0 && Length < ARRAYSIZE(Self), "GetModuleFileNameW failed: %lu\n", GetLastError());
    if (Length == 0 || Length >= ARRAYSIZE(Self))
        return;

    StringCchPrintfW(CmdLine, ARRAYSIZE(CmdLine), L"\"%s\" JobObject killchild", Self);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(Self, CmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        ok(FALSE, "CreateProcessW failed: %lu\n", GetLastError());
        return;
    }

    Wait = WaitForSingleObject(pi.hProcess, 30000);
    /* The important assertion: the child terminated (and the machine survived).
       On an unfixed kernel the kill-on-close path bugchecks and we never get
       here at all. */
    ok(Wait == WAIT_OBJECT_0, "kill-on-close child did not terminate (hang/bugcheck), wait=%lu\n", Wait);
    if (Wait == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(pi.hProcess, &Code);
        ok(Code != 123, "kill-on-close did not terminate the self-assigned process (exit=%lu)\n", Code);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

/*
 * Nested jobs (Windows 8 and later): a process that already is in a job can be
 * assigned to a second one, which then becomes a child of the first. Chromium
 * and Firefox need this: their sandbox puts every child process into a job of
 * its own, and the children already are in the job of whatever started the
 * browser (a CI agent, Playwright, a test runner).
 *
 * The expected results were taken from Windows 10 22H2.
 */
static
BOOL
CreateSleepChild(
    _In_ DWORD Flags,
    _Out_ PROCESS_INFORMATION *pi)
{
    WCHAR Self[MAX_PATH];
    WCHAR CmdLine[MAX_PATH + 32];
    STARTUPINFOW si;
    DWORD Length;

    Length = GetModuleFileNameW(NULL, Self, ARRAYSIZE(Self));
    if (Length == 0 || Length >= ARRAYSIZE(Self))
        return FALSE;

    StringCchPrintfW(CmdLine, ARRAYSIZE(CmdLine), L"\"%s\" JobObject sleepchild", Self);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    return CreateProcessW(Self, CmdLine, NULL, NULL, FALSE, Flags | CREATE_SUSPENDED,
                          NULL, NULL, &si, pi);
}

static
HANDLE
CreateJobWithFlags(
    _In_ DWORD LimitFlags,
    _In_ DWORD ActiveProcessLimit)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Eli;
    HANDLE hJob;

    hJob = CreateJobObjectW(NULL, NULL);
    ok(hJob != NULL, "CreateJobObject failed with %lu\n", GetLastError());
    if (hJob != NULL && LimitFlags != 0)
    {
        ZeroMemory(&Eli, sizeof(Eli));
        Eli.BasicLimitInformation.LimitFlags = LimitFlags;
        Eli.BasicLimitInformation.ActiveProcessLimit = ActiveProcessLimit;
        ok(SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &Eli, sizeof(Eli)),
           "SetInformationJobObject failed with %lu\n", GetLastError());
    }
    return hJob;
}

static
BOOL
InJob(
    _In_ HANDLE hProcess,
    _In_ HANDLE hJob)
{
    BOOL Result = FALSE;

    ok(IsProcessInJob(hProcess, hJob, &Result), "IsProcessInJob failed with %lu\n", GetLastError());
    return Result;
}

static
void
CheckAccounting(
    _In_ HANDLE hJob,
    _In_ ULONG Total,
    _In_ ULONG Active,
    _In_ ULONG Line)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Info;

    ZeroMemory(&Info, sizeof(Info));
    ok(QueryInformationJobObject(hJob, JobObjectBasicAccountingInformation, &Info, sizeof(Info), NULL),
       "line %lu: QueryInformationJobObject failed with %lu\n", Line, GetLastError());
    ok(Info.TotalProcesses == Total, "line %lu: TotalProcesses = %lu, expected %lu\n", Line, Info.TotalProcesses, Total);
    ok(Info.ActiveProcesses == Active, "line %lu: ActiveProcesses = %lu, expected %lu\n", Line, Info.ActiveProcesses, Active);
}

static
void
CheckProcessCount(
    _In_ HANDLE hJob,
    _In_ ULONG Count,
    _In_ ULONG Line)
{
    struct
    {
        JOBOBJECT_BASIC_PROCESS_ID_LIST List;
        ULONG_PTR More[7];
    } Buffer;

    ZeroMemory(&Buffer, sizeof(Buffer));
    ok(QueryInformationJobObject(hJob, JobObjectBasicProcessIdList, &Buffer, sizeof(Buffer), NULL),
       "line %lu: QueryInformationJobObject failed with %lu\n", Line, GetLastError());
    ok(Buffer.List.NumberOfAssignedProcesses == Count,
       "line %lu: NumberOfAssignedProcesses = %lu, expected %lu\n", Line, Buffer.List.NumberOfAssignedProcesses, Count);
    ok(Buffer.List.NumberOfProcessIdsInList == Count,
       "line %lu: NumberOfProcessIdsInList = %lu, expected %lu\n", Line, Buffer.List.NumberOfProcessIdsInList, Count);
}

static
void
KillAndClose(PROCESS_INFORMATION *pi)
{
    TerminateProcess(pi->hProcess, 1);
    CloseHandle(pi->hThread);
    CloseHandle(pi->hProcess);
}

static
void
test_NestedAssignment(void)
{
    PROCESS_INFORMATION Child, Other, Third;
    HANDLE hOuter, hInner, hRoot, hLimit;
    DWORD Wait, Code;
    BOOL Success;

    hOuter = CreateJobWithFlags(0, 0);
    hInner = CreateJobWithFlags(0, 0);
    hRoot = CreateJobWithFlags(0, 0);
    hLimit = CreateJobWithFlags(JOB_OBJECT_LIMIT_ACTIVE_PROCESS, 1);

    /* Whatever job the test itself runs in, the child is in it too. */
    ok(CreateSleepChild(0, &Child), "CreateProcess failed with %lu\n", GetLastError());
    ok(CreateSleepChild(0, &Other), "CreateProcess failed with %lu\n", GetLastError());
    ok(CreateSleepChild(0, &Third), "CreateProcess failed with %lu\n", GetLastError());

    /* The outer job takes the child (nesting under the job of the test, if there is one) */
    Success = AssignProcessToJobObject(hOuter, Child.hProcess);
    ok(Success, "assigning to the outer job failed with %lu\n", GetLastError());
    ok(InJob(Child.hProcess, hOuter), "child is not in the outer job\n");
    CheckAccounting(hOuter, 1, 1, __LINE__);

    /* The child is in a job already, the second one nests below the first */
    SetLastError(0xDEADBEEF);
    Success = AssignProcessToJobObject(hInner, Child.hProcess);
    ok(Success, "assigning a process in a job to a second job failed with %lu\n", GetLastError());
    ok(InJob(Child.hProcess, hInner), "child is not in the inner job\n");
    ok(InJob(Child.hProcess, hOuter), "child left the outer job\n");
    ok(InJob(Child.hProcess, NULL), "child is in no job\n");
    ok(!InJob(GetCurrentProcess(), hInner), "the test process is in the inner job\n");

    /* Members of a child job count in the parent too, once */
    CheckAccounting(hInner, 1, 1, __LINE__);
    CheckAccounting(hOuter, 1, 1, __LINE__);
    CheckProcessCount(hInner, 1, __LINE__);
    CheckProcessCount(hOuter, 1, __LINE__);

    /* Assigning to a job the process is in already, or to a parent of it, succeeds */
    Success = AssignProcessToJobObject(hInner, Child.hProcess);
    ok(Success, "assigning to the job it is in failed with %lu\n", GetLastError());
    Success = AssignProcessToJobObject(hOuter, Child.hProcess);
    ok(Success, "assigning to the parent job failed with %lu\n", GetLastError());
    CheckAccounting(hInner, 1, 1, __LINE__);
    CheckAccounting(hOuter, 1, 1, __LINE__);

    /* The inner job has a parent, so only members of that parent may join it */
    Success = AssignProcessToJobObject(hRoot, Other.hProcess);
    ok(Success, "assigning to an unrelated job failed with %lu\n", GetLastError());
    SetLastError(0xDEADBEEF);
    Success = AssignProcessToJobObject(hInner, Other.hProcess);
    ok(!Success, "assigning to a job whose parent the process is not in succeeded\n");
    ok_err(ERROR_ACCESS_DENIED);
    ok(!InJob(Other.hProcess, hInner), "process joined the inner job\n");

    KillAndClose(&Other);

    /* The active process limit counts the members of the job itself */
    ok(CreateSleepChild(0, &Other), "CreateProcess failed with %lu\n", GetLastError());
    Success = AssignProcessToJobObject(hOuter, Third.hProcess);
    ok(Success, "assigning to the outer job failed with %lu\n", GetLastError());
    Success = AssignProcessToJobObject(hOuter, Other.hProcess);
    ok(Success, "assigning to the outer job failed with %lu\n", GetLastError());
    Success = AssignProcessToJobObject(hLimit, Third.hProcess);
    ok(Success, "assigning below the outer job to the limited job failed with %lu\n", GetLastError());
    SetLastError(0xDEADBEEF);
    Success = AssignProcessToJobObject(hLimit, Other.hProcess);
    ok(!Success, "assigning beyond the active process limit succeeded\n");
    ok_err(ERROR_NOT_ENOUGH_QUOTA);
    ok(!InJob(Other.hProcess, hLimit), "process joined the full job\n");
    CheckAccounting(hLimit, 1, 1, __LINE__);

    /* Terminating the outer job terminates the members of the inner job */
    ResumeThread(Child.hThread);
    Success = TerminateJobObject(hOuter, 77);
    ok(Success, "TerminateJobObject failed with %lu\n", GetLastError());
    Wait = WaitForSingleObject(Child.hProcess, 5000);
    ok(Wait == WAIT_OBJECT_0, "member of the inner job survived TerminateJobObject on the outer job (wait=%lu)\n", Wait);
    Code = 0xDEADBEEF;
    GetExitCodeProcess(Child.hProcess, &Code);
    ok(Code == 77, "exit code = %lu, expected 77\n", Code);
    Wait = WaitForSingleObject(Third.hProcess, 5000);
    ok(Wait == WAIT_OBJECT_0, "member of the limited job survived TerminateJobObject on the outer job (wait=%lu)\n", Wait);
    Wait = WaitForSingleObject(Other.hProcess, 5000);
    ok(Wait == WAIT_OBJECT_0, "member of the outer job survived TerminateJobObject (wait=%lu)\n", Wait);
    CheckAccounting(hOuter, 3, 0, __LINE__);
    CheckAccounting(hInner, 1, 0, __LINE__);
    CheckProcessCount(hOuter, 0, __LINE__);
    CheckProcessCount(hInner, 0, __LINE__);

    KillAndClose(&Child);
    KillAndClose(&Other);
    KillAndClose(&Third);
    CloseHandle(hLimit);
    CloseHandle(hRoot);
    CloseHandle(hInner);
    CloseHandle(hOuter);
}

static
void
test_NestedKillOnClose(void)
{
    PROCESS_INFORMATION Child;
    HANDLE hOuter, hInner;
    DWORD Wait;

    hOuter = CreateJobWithFlags(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, 0);
    hInner = CreateJobWithFlags(0, 0);

    ok(CreateSleepChild(0, &Child), "CreateProcess failed with %lu\n", GetLastError());
    ok(AssignProcessToJobObject(hOuter, Child.hProcess), "assign failed with %lu\n", GetLastError());
    ok(AssignProcessToJobObject(hInner, Child.hProcess), "nested assign failed with %lu\n", GetLastError());
    ResumeThread(Child.hThread);

    /* The last handle of the outer job is closed; its members include those of the inner job */
    CloseHandle(hOuter);
    Wait = WaitForSingleObject(Child.hProcess, 5000);
    ok(Wait == WAIT_OBJECT_0, "member of the inner job survived kill-on-close of the outer job (wait=%lu)\n", Wait);

    KillAndClose(&Child);
    CloseHandle(hInner);
}

/*
 * What a process of nested jobs gives to its children. Run in a process of its
 * own, as it joins the jobs for good. The exit code encodes the outcome:
 * bit 0 the child was created, bit 1 it is in the outer job, bit 2 in the
 * inner job, bit 7 something else went wrong.
 */
static
int
NestedCaseChild(
    _In_ DWORD OuterFlags,
    _In_ DWORD InnerFlags,
    _In_ DWORD CreateFlags)
{
    PROCESS_INFORMATION Child;
    HANDLE hOuter, hInner;
    BOOL Result;
    int Code = 0;

    hOuter = CreateJobWithFlags(OuterFlags, 0);
    hInner = CreateJobWithFlags(InnerFlags, 0);
    if (hOuter == NULL || hInner == NULL)
        return 0x80;

    if (!AssignProcessToJobObject(hOuter, GetCurrentProcess()) ||
        !AssignProcessToJobObject(hInner, GetCurrentProcess()))
        return 0x80;

    if (!CreateSleepChild(CreateFlags, &Child))
        return GetLastError() == ERROR_ACCESS_DENIED ? 0 : 0x80;

    Code |= 1;
    if (IsProcessInJob(Child.hProcess, hOuter, &Result) && Result)
        Code |= 2;
    if (IsProcessInJob(Child.hProcess, hInner, &Result) && Result)
        Code |= 4;

    TerminateProcess(Child.hProcess, 0);
    return Code;
}

static
void
CheckNestedCase(
    _In_ DWORD OuterFlags,
    _In_ DWORD InnerFlags,
    _In_ DWORD CreateFlags,
    _In_ int Expected,
    _In_ PCSTR Name)
{
    WCHAR Self[MAX_PATH];
    WCHAR CmdLine[MAX_PATH + 64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD Wait, Code = 0xDEADBEEF;

    if (GetModuleFileNameW(NULL, Self, ARRAYSIZE(Self)) == 0)
        return;

    StringCchPrintfW(CmdLine, ARRAYSIZE(CmdLine), L"\"%s\" JobObject nestedcase %lu %lu %lu",
                     Self, OuterFlags, InnerFlags, CreateFlags);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(Self, CmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        ok(FALSE, "%s: CreateProcessW failed: %lu\n", Name, GetLastError());
        return;
    }

    Wait = WaitForSingleObject(pi.hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "%s: child did not terminate, wait=%lu\n", Name, Wait);
    if (Wait == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(pi.hProcess, &Code);
        ok(Code == (DWORD)Expected, "%s: result 0x%lx, expected 0x%x\n", Name, Code, Expected);
    }
    else
    {
        TerminateProcess(pi.hProcess, 1);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

static
void
test_NestedBreakaway(void)
{
    /* Without a request the new process leaves every job that allows silent
       breakaway, starting with the innermost, and stays in the first that does not */
    CheckNestedCase(0, 0, 0, 1 | 2 | 4, "inherit");
    CheckNestedCase(0, JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK, 0, 1 | 2, "silent inner");
    CheckNestedCase(JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK, 0, 0, 1 | 2 | 4, "silent outer");
    CheckNestedCase(JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK, JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK, 0, 1, "silent both");

    /* A requested breakaway fails unless the innermost job allows it, and
       then leaves the jobs that allow it, again from the inside */
    CheckNestedCase(0, 0, CREATE_BREAKAWAY_FROM_JOB, 0, "breakaway none");
    CheckNestedCase(JOB_OBJECT_LIMIT_BREAKAWAY_OK, 0, CREATE_BREAKAWAY_FROM_JOB, 0, "breakaway outer only");
    CheckNestedCase(0, JOB_OBJECT_LIMIT_BREAKAWAY_OK, CREATE_BREAKAWAY_FROM_JOB, 1 | 2, "breakaway inner only");
    CheckNestedCase(JOB_OBJECT_LIMIT_BREAKAWAY_OK, JOB_OBJECT_LIMIT_BREAKAWAY_OK, CREATE_BREAKAWAY_FROM_JOB, 1, "breakaway both");
}

/*
 * Two threads assign a process of job X to job Y and a process of job Y to
 * job X, over and over. Both always fail (a job that has processes cannot be
 * nested), but taking the job locks in opposite orders used to deadlock.
 */
typedef struct _CROSS_ASSIGN
{
    HANDLE hProcess;
    HANDLE hTarget;
    DWORD EndTime;
    ULONG Count;
    ULONG Unexpected;
} CROSS_ASSIGN, *PCROSS_ASSIGN;

static
DWORD
WINAPI
CrossAssignThread(LPVOID Param)
{
    PCROSS_ASSIGN Info = Param;

    while ((LONG)(Info->EndTime - GetTickCount()) > 0)
    {
        SetLastError(0);
        if (AssignProcessToJobObject(Info->hTarget, Info->hProcess) ||
            GetLastError() != ERROR_ACCESS_DENIED)
        {
            Info->Unexpected++;
        }
        Info->Count++;
    }
    return 0;
}

static
void
test_CrossAssignStress(void)
{
    PROCESS_INFORMATION P1, P2;
    HANDLE hX, hY, Threads[2];
    CROSS_ASSIGN A, B;
    DWORD Wait;

    hX = CreateJobWithFlags(0, 0);
    hY = CreateJobWithFlags(0, 0);
    ok(CreateSleepChild(0, &P1), "CreateProcess failed with %lu\n", GetLastError());
    ok(CreateSleepChild(0, &P2), "CreateProcess failed with %lu\n", GetLastError());
    ok(AssignProcessToJobObject(hX, P1.hProcess), "assign failed with %lu\n", GetLastError());
    ok(AssignProcessToJobObject(hY, P2.hProcess), "assign failed with %lu\n", GetLastError());

    A.hProcess = P1.hProcess; A.hTarget = hY;
    B.hProcess = P2.hProcess; B.hTarget = hX;
    A.Count = A.Unexpected = B.Count = B.Unexpected = 0;
    A.EndTime = B.EndTime = GetTickCount() + 10000;

    Threads[0] = CreateThread(NULL, 0, CrossAssignThread, &A, 0, NULL);
    Threads[1] = CreateThread(NULL, 0, CrossAssignThread, &B, 0, NULL);
    ok(Threads[0] != NULL && Threads[1] != NULL, "CreateThread failed\n");
    if (Threads[0] != NULL && Threads[1] != NULL)
    {
        Wait = WaitForMultipleObjects(2, Threads, TRUE, 60000);
        ok(Wait == WAIT_OBJECT_0, "cross-assigning threads hung (deadlock), wait=%lu\n", Wait);
        if (Wait == WAIT_OBJECT_0)
        {
            ok(A.Unexpected == 0 && B.Unexpected == 0,
               "unexpected results: %lu of %lu, %lu of %lu\n", A.Unexpected, A.Count, B.Unexpected, B.Count);
            trace("cross assignments: %lu + %lu\n", A.Count, B.Count);
        }
        else
        {
            /* The threads are stuck for good; do not wait for them again */
            return;
        }
    }
    if (Threads[0]) CloseHandle(Threads[0]);
    if (Threads[1]) CloseHandle(Threads[1]);

    CheckAccounting(hX, 1, 1, __LINE__);
    CheckAccounting(hY, 1, 1, __LINE__);

    KillAndClose(&P1);
    KillAndClose(&P2);
    CloseHandle(hX);
    CloseHandle(hY);
}

START_TEST(JobObject)
{
    char **argv;
    int argc = winetest_get_mainargs(&argv);

    /* Child mode for test_KillOnClose: self-assign to a KILL_ON_JOB_CLOSE job
       and close the last handle. */
    if (argc >= 3 && !strcmp(argv[2], "killchild"))
    {
        ExitProcess(KillOnCloseChild());
    }

    /* Child mode for test_NestedAssignment and others: just stay around */
    if (argc >= 3 && !strcmp(argv[2], "sleepchild"))
    {
        Sleep(60000);
        ExitProcess(0);
    }

    /* Child mode for test_NestedBreakaway */
    if (argc >= 6 && !strcmp(argv[2], "nestedcase"))
    {
        ExitProcess(NestedCaseChild(strtoul(argv[3], NULL, 10),
                                    strtoul(argv[4], NULL, 10),
                                    strtoul(argv[5], NULL, 10)));
    }

    test_LimitQuery();
    test_CompletionPort();
    test_RoundTrip();
    test_InvalidParameters();
    test_Access();
    test_SandboxPolicies();
    test_KillOnClose();
    test_NestedAssignment();
    test_NestedKillOnClose();
    test_NestedBreakaway();
    test_CrossAssignStress();
}
