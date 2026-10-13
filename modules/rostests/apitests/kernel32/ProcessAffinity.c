/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that a new process keeps the affinity set by its creator or inherited from it
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <ndk/psfuncs.h>
#include <ndk/pstypes.h>

typedef struct _CHILD_REPORT
{
    DWORD_PTR Process;
    DWORD_PTR Peb;
    DWORD_PTR MainRun;
    DWORD_PTR ThreadRun;
} CHILD_REPORT, *PCHILD_REPORT;

static ULONG (NTAPI *pNtGetCurrentProcessorNumber)(VOID);

/* Full width: GetProcessAffinityMask truncates to 32 bits */
static DWORD_PTR
QueryProcessMask(HANDLE hProcess)
{
    PROCESS_BASIC_INFORMATION Info;

    if (!NT_SUCCESS(NtQueryInformationProcess(hProcess, ProcessBasicInformation, &Info, sizeof(Info), NULL)))
        return 0;
    return (DWORD_PTR)Info.AffinityMask;
}

static DWORD WINAPI
SpinThread(LPVOID Param)
{
    PDWORD_PTR RunMask = Param;
    DWORD Start = GetTickCount();

    do
    {
        *RunMask |= (DWORD_PTR)1 << pNtGetCurrentProcessorNumber();
        if ((GetTickCount() & 7) == 0)
            Sleep(0);
    } while (GetTickCount() - Start < 300);
    return 0;
}

/* Child: report the process mask, the PEB mask and where each of its two threads ran */
static VOID
Child(VOID)
{
    CHILD_REPORT Report = { 0 };
    HANDLE hThread = NULL;
    CHAR Buffer[160];
    DWORD Written;

    Report.Process = QueryProcessMask(GetCurrentProcess());
    Report.Peb = (DWORD_PTR)NtCurrentPeb()->ImageProcessAffinityMask;
    if (pNtGetCurrentProcessorNumber)
    {
        hThread = CreateThread(NULL, 0, SpinThread, &Report.ThreadRun, 0, NULL);
        SpinThread(&Report.MainRun);
        if (hThread)
        {
            /* The parent's 30 s timeout bounds this wait */
            WaitForSingleObject(hThread, INFINITE);
            CloseHandle(hThread);
        }
    }
    StringCbPrintfA(Buffer, sizeof(Buffer), "process=%Ix peb=%Ix main=%Ix thread=%Ix\n",
                    Report.Process, Report.Peb, Report.MainRun, Report.ThreadRun);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), Buffer, (DWORD)strlen(Buffer), &Written, NULL);
}

static BOOL
ParseField(
    _In_ PCSTR Buffer,
    _In_ PCSTR Name,
    _Out_ PDWORD_PTR Value)
{
    PCSTR Field = strstr(Buffer, Name);

    if (!Field)
        return FALSE;
    *Value = (DWORD_PTR)_strtoui64(Field + strlen(Name), NULL, 16);
    return TRUE;
}

static BOOL
RunChild(
    _In_ DWORD_PTR SetMask,
    _Out_ PCHILD_REPORT Report)
{
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 64];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    SECURITY_ATTRIBUTES Sa = { sizeof(Sa), NULL, TRUE };
    HANDLE hRead, hWrite;
    CHAR Buffer[160];
    DWORD Read, Total = 0, Error;
    BOOL Success, Exited;

    RtlZeroMemory(Report, sizeof(*Report));
    if (!CreatePipe(&hRead, &hWrite, &Sa, 0))
    {
        skip("CreatePipe failed with %lu\n", GetLastError());
        return FALSE;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" ProcessAffinity child", FileName);
    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    StartupInfo.hStdOutput = hWrite;
    StartupInfo.hStdError = hWrite;

    /* The same sequence as cmd's start /affinity: create suspended, set the mask, resume */
    Success = CreateProcessW(FileName, CommandLine, NULL, NULL, TRUE,
                             SetMask ? CREATE_SUSPENDED : 0,
                             NULL, NULL, &StartupInfo, &ProcessInfo);
    Error = GetLastError();
    CloseHandle(hWrite);
    if (!Success)
    {
        skip("CreateProcess failed with %lu\n", Error);
        CloseHandle(hRead);
        return FALSE;
    }
    if (SetMask)
    {
        Success = SetProcessAffinityMask(ProcessInfo.hProcess, SetMask);
        Error = GetLastError();
        ok(Success, "SetProcessAffinityMask failed with %lu\n", Error);
        if (ResumeThread(ProcessInfo.hThread) == (DWORD)-1)
            ok(0, "ResumeThread failed with %lu\n", GetLastError());
    }

    /* The report fits in the pipe buffer, so the child never blocks on it */
    Exited = (WaitForSingleObject(ProcessInfo.hProcess, 30000) == WAIT_OBJECT_0);
    ok(Exited, "Child did not exit\n");
    if (!Exited)
    {
        TerminateProcess(ProcessInfo.hProcess, 1);
        WaitForSingleObject(ProcessInfo.hProcess, 5000);
    }
    while (Exited && Total < sizeof(Buffer) - 1 &&
           ReadFile(hRead, Buffer + Total, sizeof(Buffer) - 1 - Total, &Read, NULL) && Read)
    {
        Total += Read;
    }
    Buffer[Total] = ANSI_NULL;
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
    CloseHandle(hRead);

    if (!Exited ||
        !ParseField(Buffer, "process=", &Report->Process) ||
        !ParseField(Buffer, "peb=", &Report->Peb) ||
        !ParseField(Buffer, "main=", &Report->MainRun) ||
        !ParseField(Buffer, "thread=", &Report->ThreadRun))
    {
        ok(0, "Unexpected child output '%s'\n", Buffer);
        return FALSE;
    }
    return TRUE;
}

static VOID
CheckChild(PCSTR Case, const CHILD_REPORT *Report, DWORD_PTR Expected)
{
    ok(Report->Process == Expected, "%s: process mask %Ix, expected %Ix\n", Case, Report->Process, Expected);
    /* Windows shows the process affinity in the PEB; ReactOS leaves 0 unless the image asks for a mask */
    ok(Report->Peb == 0 || Report->Peb == Report->Process, "%s: PEB mask %Ix\n", Case, Report->Peb);
    if (!pNtGetCurrentProcessorNumber)
        return;
    ok(Report->MainRun != 0 && (Report->MainRun & ~Expected) == 0,
       "%s: main thread ran on %Ix, expected only %Ix\n", Case, Report->MainRun, Expected);
    ok(Report->ThreadRun != 0 && (Report->ThreadRun & ~Expected) == 0,
       "%s: second thread ran on %Ix, expected only %Ix\n", Case, Report->ThreadRun, Expected);
}

START_TEST(ProcessAffinity)
{
    DWORD_PTR OrigMask, TargetMask, PebMask;
    CHILD_REPORT Report;
    BOOL Restored;
    int argc;
    char **argv;

    pNtGetCurrentProcessorNumber = (PVOID)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                         "NtGetCurrentProcessorNumber");
    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "child"))
    {
        Child();
        return;
    }

    OrigMask = QueryProcessMask(GetCurrentProcess());
    if (!(OrigMask & (OrigMask - 1)))
    {
        skip("Needs at least two processors (mask %Ix)\n", OrigMask);
        return;
    }
    if (!pNtGetCurrentProcessorNumber)
        skip("No NtGetCurrentProcessorNumber: the processors the threads ran on are not checked\n");
    PebMask = (DWORD_PTR)NtCurrentPeb()->ImageProcessAffinityMask;
    ok(PebMask == 0 || PebMask == OrigMask, "PEB mask %Ix, process mask %Ix\n", PebMask, OrigMask);

    /* The highest processor, so that a child that ignores the mask is not on it by chance */
    TargetMask = OrigMask;
    while (TargetMask & (TargetMask - 1))
        TargetMask &= TargetMask - 1;

    /* No mask: the child gets the parent's affinity */
    if (RunChild(0, &Report))
        CheckChild("Default", &Report, OrigMask);

    /* Mask set by the creator while the child is suspended */
    if (RunChild(TargetMask, &Report))
        CheckChild("Creator-set", &Report, TargetMask);

    /* Mask inherited from the parent */
    if (!SetProcessAffinityMask(GetCurrentProcess(), TargetMask))
    {
        skip("SetProcessAffinityMask(self) failed with %lu\n", GetLastError());
        return;
    }
    if (RunChild(0, &Report))
        CheckChild("Inherited", &Report, TargetMask);
    Restored = SetProcessAffinityMask(GetCurrentProcess(), OrigMask);
    ok(Restored, "Restoring the process mask failed with %lu\n", GetLastError());
}
