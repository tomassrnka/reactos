/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     RtlQueryProcessDebugInformation reports the 32-bit modules of a WOW64 target
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The module list a 64-bit caller gets for a 32-bit process was measured on Windows 10 22H2:
 * the native walk of the 64-bit PEB gives the native ntdll and the wow64 support DLLs, and the
 * 32-bit loader list adds the executable and the 32-bit libraries, all below 4 GB. Toolhelp
 * (TH32CS_SNAPMODULE) and tasklist /M read this same list, so they report those 32-bit modules
 * too. This is the behaviour a 64-bit process must reproduce; a 32-bit process has no WOW64
 * target to read, so the cross-bitness part is a 64-bit test only.
 */

#include "precomp.h"

#ifdef _WIN64

#define FOUR_GB 0x100000000ULL

typedef struct
{
    PROCESS_INFORMATION Info;
} WOW64_CHILD;

/*
 * A 32-bit command interpreter from the WOW64 system directory that echoes "ready" once it
 * runs, then idles: the signal comes from the child, so the exact set of libraries it has
 * loaded by then does not matter. Modeled on the psapi EnumProcessModulesEx test.
 */
static BOOL StartWow64Child(WOW64_CHILD *Child)
{
    char Directory[MAX_PATH], Command[MAX_PATH + 96], Text[16];
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    SECURITY_ATTRIBUTES Security = { sizeof(Security), NULL, TRUE };
    HANDLE Read, Write;
    DWORD Deadline, Got = 0, Total = 0;

    if (!GetSystemWow64DirectoryA(Directory, sizeof(Directory)))
    {
        skip("no 32-bit programs here\n");
        return FALSE;
    }
    if (!CreatePipe(&Read, &Write, &Security, 0))
    {
        skip("no pipe (%lu)\n", GetLastError());
        return FALSE;
    }
    SetHandleInformation(Read, HANDLE_FLAG_INHERIT, 0);

    lstrcpyA(Command, "\"");
    lstrcatA(Command, Directory);
    lstrcatA(Command, "\\cmd.exe\" /c echo ready& ping -n 30 127.0.0.1 > nul");
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    StartupInfo.hStdOutput = Write;
    StartupInfo.hStdError = Write;
    StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (!CreateProcessA(NULL, Command, NULL, NULL, TRUE, 0, NULL, NULL, &StartupInfo, &Child->Info))
    {
        skip("the 32-bit process did not start (%lu)\n", GetLastError());
        CloseHandle(Read);
        CloseHandle(Write);
        return FALSE;
    }
    CloseHandle(Write);

    memset(Text, 0, sizeof(Text));
    Deadline = GetTickCount() + 30000;
    while (Total < 5 && GetTickCount() < Deadline)
    {
        DWORD Available = 0;

        if (!PeekNamedPipe(Read, NULL, 0, NULL, &Available, NULL))
            break;
        if (Available && ReadFile(Read, Text + Total, min(Available, 5 - Total), &Got, NULL))
            Total += Got;
        else
            Sleep(50);
    }
    CloseHandle(Read);

    if (strncmp(Text, "ready", 5))
    {
        skip("the 32-bit process did not signal in time\n");
        TerminateProcess(Child->Info.hProcess, 0);
        CloseHandle(Child->Info.hThread);
        CloseHandle(Child->Info.hProcess);
        return FALSE;
    }
    return TRUE;
}

static void StopWow64Child(WOW64_CHILD *Child)
{
    TerminateProcess(Child->Info.hProcess, 0);
    CloseHandle(Child->Info.hThread);
    CloseHandle(Child->Info.hProcess);
}

/* One module query of a target: the number of modules below 4 GB, and whether the 32-bit
   ntdll (there is also a native one above 4 GB) and the 32-bit executable (cmd.exe) are
   among them. For a WOW64 process the native walk yields only modules at or above 4 GB, so
   anything below 4 GB can only have come from the new 32-bit loader walk. */
static BOOL QueryModules32(ULONG ProcessId, ULONG *Below4Gb, BOOL *HasNtdll32, BOOL *HasExe)
{
    PRTL_DEBUG_INFORMATION Buffer;
    PRTL_PROCESS_MODULES Modules;
    NTSTATUS Status;
    ULONG i;

    *Below4Gb = 0;
    *HasNtdll32 = FALSE;
    *HasExe = FALSE;

    Buffer = RtlCreateQueryDebugBuffer(0, FALSE);
    if (!Buffer)
        return FALSE;

    Status = RtlQueryProcessDebugInformation(ProcessId, RTL_DEBUG_QUERY_MODULES, Buffer);
    if (!NT_SUCCESS(Status) || Buffer->Modules == NULL)
    {
        RtlDestroyQueryDebugBuffer(Buffer);
        return FALSE;
    }

    Modules = Buffer->Modules;
    for (i = 0; i < Modules->NumberOfModules; i++)
    {
        PRTL_PROCESS_MODULE_INFORMATION Mod = &Modules->Modules[i];
        const char *Name = &Mod->FullPathName[Mod->OffsetToFileName];

        if ((ULONGLONG)(ULONG_PTR)Mod->ImageBase < FOUR_GB)
        {
            (*Below4Gb)++;
            if (!_stricmp(Name, "ntdll.dll"))
                *HasNtdll32 = TRUE;
            if (!_stricmp(Name, "cmd.exe"))
                *HasExe = TRUE;
        }
    }

    RtlDestroyQueryDebugBuffer(Buffer);
    return TRUE;
}

/* The module list of a 32-bit process, from a 64-bit caller: the native walk sees only the
   64-bit modules (all at or above 4 GB), and the fix adds the 32-bit loader list below 4 GB. */
static void TestWow64Target(void)
{
    WOW64_CHILD Child;
    ULONG Below4Gb = 0;
    BOOL Wow64 = FALSE, HasNtdll32 = FALSE, HasExe = FALSE, Have = FALSE;
    DWORD Deadline;

    if (!StartWow64Child(&Child))
        return;

    IsWow64Process(Child.Info.hProcess, &Wow64);
    if (!Wow64)
    {
        skip("the process is not a 32-bit one\n");
        StopWow64Child(&Child);
        return;
    }

    /* The 32-bit libraries may still be loading just after the signal, so the query is
       retried for a while until the list holds the ntdll and a few other 32-bit modules. */
    Deadline = GetTickCount() + 20000;
    do
    {
        if (QueryModules32(Child.Info.dwProcessId, &Below4Gb, &HasNtdll32, &HasExe) &&
            HasNtdll32 && HasExe && Below4Gb >= 3)
        {
            Have = TRUE;
            break;
        }
        Sleep(200);
    } while (GetTickCount() < Deadline);

    ok(Have, "no 32-bit modules were reported for the WOW64 process (%lu below 4 GB, 32-bit ntdll %d, exe %d)\n",
       Below4Gb, HasNtdll32, HasExe);
    ok(HasNtdll32, "the 32-bit ntdll.dll of the WOW64 process is missing from the module list\n");
    ok(HasExe, "the 32-bit executable (cmd.exe) of the WOW64 process is missing from the module list\n");
    ok(Below4Gb >= 3, "only %lu modules below 4 GB, expected the executable and its 32-bit libraries\n", Below4Gb);

    StopWow64Child(&Child);
}

#endif /* _WIN64 */

/* A process can read its own module list: at least the executable and ntdll are there. */
static void TestSelf(void)
{
    PRTL_DEBUG_INFORMATION Buffer;
    NTSTATUS Status;

    Buffer = RtlCreateQueryDebugBuffer(0, FALSE);
    ok(Buffer != NULL, "RtlCreateQueryDebugBuffer failed\n");
    if (!Buffer)
        return;

    Status = RtlQueryProcessDebugInformation((ULONG)(ULONG_PTR)NtCurrentTeb()->ClientId.UniqueProcess,
                                             RTL_DEBUG_QUERY_MODULES, Buffer);
    ok(NT_SUCCESS(Status), "RtlQueryProcessDebugInformation failed, 0x%lx\n", Status);
    if (NT_SUCCESS(Status) && Buffer->Modules != NULL)
        ok(Buffer->Modules->NumberOfModules >= 2, "only %lu modules for this process\n",
           Buffer->Modules->NumberOfModules);

    RtlDestroyQueryDebugBuffer(Buffer);
}

START_TEST(RtlQueryProcessDebugModules)
{
    TestSelf();
#ifdef _WIN64
    TestWow64Target();
#endif
}
