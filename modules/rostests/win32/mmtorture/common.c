/*
 * PROJECT:     ReactOS memory manager torture suite
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Logging, shared progress counters, patterns, process helpers
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "mmtorture.h"

MMT_SHARED *MmtShared;
HANDLE MmtStopEvent;
WCHAR MmtExePath[MAX_PATH];

static VOID
MmtOutput(const char *Prefix, const char *Format, va_list Args)
{
    char Buffer[400];
    int Length;

    Length = _snprintf(Buffer, sizeof(Buffer) - 2, "MMT:%s[%lu] ", Prefix, GetCurrentProcessId());
    if (Length < 0)
        Length = 0;
    _vsnprintf(Buffer + Length, sizeof(Buffer) - 2 - Length, Format, Args);
    Buffer[sizeof(Buffer) - 2] = 0;
    Length = (int)strlen(Buffer);
    if (Length == 0 || Buffer[Length - 1] != '\n')
    {
        Buffer[Length] = '\n';
        Buffer[Length + 1] = 0;
    }
    OutputDebugStringA(Buffer);
    fputs(Buffer, stdout);
    fflush(stdout);
}

VOID
MmtLog(const char *Format, ...)
{
    va_list Args;

    va_start(Args, Format);
    MmtOutput("", Format, Args);
    va_end(Args);
}

VOID
MmtFail(const char *Format, ...)
{
    va_list Args;

    if (MmtShared)
        InterlockedIncrement(&MmtShared->Failures);
    va_start(Args, Format);
    MmtOutput("FAIL", Format, Args);
    va_end(Args);
}

BOOL
MmtOpenShared(BOOL Create)
{
    HANDLE Section;

    if (Create)
    {
        Section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                     sizeof(MMT_SHARED), MMT_SHARED_NAME);
        MmtStopEvent = CreateEventW(NULL, TRUE, FALSE, MMT_STOP_EVENT);
    }
    else
    {
        Section = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, MMT_SHARED_NAME);
        MmtStopEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, MMT_STOP_EVENT);
    }
    if (!Section)
        return FALSE;
    MmtShared = MapViewOfFile(Section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MMT_SHARED));
    /* Keep the handle: the name goes away with the last handle */
    return MmtShared != NULL;
}

LONG
MmtAllocSlot(const char *Name)
{
    static MMT_SHARED Private;
    LONG Slot;

    if (!MmtShared)
    {
        /* Standalone run: count locally */
        MmtShared = &Private;
    }
    Slot = InterlockedIncrement(&MmtShared->SlotCount) - 1;
    if (Slot >= MMT_MAX_SLOTS)
        Slot = MMT_MAX_SLOTS - 1;
    MmtShared->Slot[Slot].ProcessId = (LONG)GetCurrentProcessId();
    lstrcpynA(MmtShared->Slot[Slot].Name, Name, sizeof(MmtShared->Slot[Slot].Name));
    MmtShared->Slot[Slot].LastOpTick = (LONG)GetTickCount();
    MmtShared->Slot[Slot].InUse = 1;
    return Slot;
}

VOID
MmtProgress(LONG Slot)
{
    InterlockedIncrement(&MmtShared->Slot[Slot].Ops);
    MmtShared->Slot[Slot].LastOpTick = (LONG)GetTickCount();
}

BOOL
MmtShouldStop(VOID)
{
    if (!MmtStopEvent)
        return FALSE;
    return WaitForSingleObject(MmtStopEvent, 0) == WAIT_OBJECT_0;
}

VOID
MmtRngInit(MMT_RNG *Rng, ULONGLONG Seed)
{
    Rng->State = Seed ? Seed : 0x9E3779B97F4A7C15ULL;
    MmtRand(Rng);
}

ULONG
MmtRand(MMT_RNG *Rng)
{
    ULONGLONG X = Rng->State;

    X ^= X << 13;
    X ^= X >> 7;
    X ^= X << 17;
    Rng->State = X;
    return (ULONG)(X >> 16);
}

ULONG
MmtRandRange(MMT_RNG *Rng, ULONG Low, ULONG High)
{
    if (High <= Low)
        return Low;
    return Low + MmtRand(Rng) % (High - Low + 1);
}

ULONGLONG
MmtPatternWord(ULONGLONG Seed, SIZE_T Page, SIZE_T Word)
{
    ULONGLONG V = Seed ^ ((ULONGLONG)Page * 0x9E3779B97F4A7C15ULL) ^ ((ULONGLONG)Word << 48);

    V ^= V >> 29;
    return V | 1;
}

VOID
MmtFillPage(PVOID Page, ULONGLONG Seed, SIZE_T PageIndex)
{
    ULONGLONG *P = Page;
    SIZE_T i;

    for (i = 0; i < PAGE_SIZE / sizeof(ULONGLONG); i++)
        P[i] = MmtPatternWord(Seed, PageIndex, i);
}

BOOL
MmtCheckPage(const VOID *Page, ULONGLONG Seed, SIZE_T PageIndex, BOOL Full, const char *What)
{
    const ULONGLONG *P = Page;
    SIZE_T i, Step = Full ? 1 : 61;

    for (i = 0; i < PAGE_SIZE / sizeof(ULONGLONG); i += Step)
    {
        ULONGLONG Expected = MmtPatternWord(Seed, PageIndex, i);
        if (P[i] != Expected)
        {
            MmtFail("%s: data mismatch at %p word %lu page %lu: got %08lx%08lx expected %08lx%08lx",
                    What, &P[i], (ULONG)i, (ULONG)PageIndex,
                    (ULONG)(P[i] >> 32), (ULONG)P[i],
                    (ULONG)(Expected >> 32), (ULONG)Expected);
            return FALSE;
        }
    }
    if (!Full)
    {
        i = PAGE_SIZE / sizeof(ULONGLONG) - 1;
        if (P[i] != MmtPatternWord(Seed, PageIndex, i))
        {
            MmtFail("%s: data mismatch at %p (last word) page %lu", What, &P[i], (ULONG)PageIndex);
            return FALSE;
        }
    }
    return TRUE;
}

BOOL
MmtIsZeroPage(const VOID *Page)
{
    const ULONG_PTR *P = Page;
    SIZE_T i;

    for (i = 0; i < PAGE_SIZE / sizeof(ULONG_PTR); i++)
    {
        if (P[i])
            return FALSE;
    }
    return TRUE;
}

BOOL
MmtSpawn(PCWSTR Arguments, BOOL Wait, DWORD TimeoutMs, PDWORD ExitCode, PHANDLE ProcessHandle)
{
    WCHAR CommandLine[512];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    PCWSTR Exe = MmtExePath;

    if (Arguments[0] == L'!')
    {
        /* An external program: the command line is given as is */
        lstrcpynW(CommandLine, Arguments + 1, _countof(CommandLine));
        Exe = NULL;
    }
    else
    {
        _snwprintf(CommandLine, _countof(CommandLine) - 1, L"\"%s\" %s", MmtExePath, Arguments);
        CommandLine[_countof(CommandLine) - 1] = 0;
    }
    ZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    if (!CreateProcessW(Exe, CommandLine, NULL, NULL, TRUE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
        return FALSE;
    CloseHandle(ProcessInfo.hThread);
    if (Wait)
    {
        if (WaitForSingleObject(ProcessInfo.hProcess, TimeoutMs) != WAIT_OBJECT_0)
        {
            if (ExitCode)
                *ExitCode = STILL_ACTIVE;
            if (ProcessHandle)
                *ProcessHandle = ProcessInfo.hProcess;
            else
                CloseHandle(ProcessInfo.hProcess);
            return TRUE;
        }
        if (ExitCode)
            GetExitCodeProcess(ProcessInfo.hProcess, ExitCode);
    }
    if (ProcessHandle)
        *ProcessHandle = ProcessInfo.hProcess;
    else
        CloseHandle(ProcessInfo.hProcess);
    return TRUE;
}

static LONG WINAPI
MmtUnhandledFilter(PEXCEPTION_POINTERS Pointers)
{
    MmtFail("unhandled exception %08lx at %p (info %p %p)",
            Pointers->ExceptionRecord->ExceptionCode,
            Pointers->ExceptionRecord->ExceptionAddress,
            (PVOID)Pointers->ExceptionRecord->ExceptionInformation[0],
            (PVOID)Pointers->ExceptionRecord->ExceptionInformation[1]);
    return EXCEPTION_EXECUTE_HANDLER;
}

VOID
MmtSetupProcess(VOID)
{
    /* A crash must end the process with its status, never wait on a message box */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(MmtUnhandledFilter);
    GetModuleFileNameW(NULL, MmtExePath, _countof(MmtExePath));
}

ULONG
MmtArgUlong(int argc, char **argv, int Index, ULONG Default)
{
    if (Index < argc)
        return strtoul(argv[Index], NULL, 0);
    return Default;
}
