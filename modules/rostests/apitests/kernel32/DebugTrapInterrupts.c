/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test processor exceptions reported to a user-mode debugger
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#include <ctype.h>
#include <intrin.h>
#ifdef _M_AMD64
#include <xmmintrin.h>
#endif

/*
 * A debugged child raises #GP (a privileged instruction), INT 2Ch and, on
 * x64, #XM (an unmasked SSE divide by zero) and handles each one itself.
 * The parent debugs it and passes every first-chance exception on.
 * While the debug port waits for the parent, the faulting thread must be
 * able to take interrupts: other processors may be waiting for it to serve
 * an IPI. "once" raises each exception one time, "stress" raises them on
 * several threads while other threads allocate, protect and free memory; on
 * a kernel that flushes the TLB of other processors with IPIs, this sends
 * them to the processors that take the exceptions. TRAPDBG_SECONDS sets the length
 * of the stress phase (1 to 3600 seconds, default 10).
 */

#define TRAP_KINDS 3

static volatile LONG StopChild;
static volatile LONG ChildFailures;
static volatile LONG ChildRaised[TRAP_KINDS];
static volatile LONG ChildMemoryRounds;

static const NTSTATUS ExpectedCode[TRAP_KINDS] =
{
    STATUS_PRIVILEGED_INSTRUCTION,
    STATUS_ASSERTION_FAILURE,
    STATUS_FLOAT_DIVIDE_BY_ZERO,
};

static
VOID
RaisePrivilegedInstruction(VOID)
{
    _disable();
}

static
VOID
RaiseXmmException(VOID)
{
#ifdef _M_AMD64
    volatile float Zero = 0.0f;
    volatile float One = 1.0f;
    volatile float Result;

    _mm_setcsr(_mm_getcsr() & ~(_MM_MASK_DIV_ZERO | _MM_EXCEPT_MASK));
    Result = _mm_cvtss_f32(_mm_div_ss(_mm_set_ss(One), _mm_set_ss(Zero)));
    (void)Result;
#endif
}

static
VOID
RaiseTrap(
    _In_ ULONG Kind)
{
    NTSTATUS Code = STATUS_SUCCESS;
#ifdef _M_AMD64
    unsigned int MxCsr = _mm_getcsr();
#endif

    _SEH2_TRY
    {
        if (Kind == 0)
            RaisePrivilegedInstruction();
        else if (Kind == 1)
            __int2c();
        else
            RaiseXmmException();
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Code = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

#ifdef _M_AMD64
    _mm_setcsr(MxCsr);
#endif

    if (Code == ExpectedCode[Kind])
        InterlockedIncrement(&ChildRaised[Kind]);
    else
        InterlockedIncrement(&ChildFailures);
}

static
ULONG
TrapKindCount(VOID)
{
#ifdef _M_AMD64
    return 3;
#else
    return 2;
#endif
}

static
DWORD
WINAPI
FaultThread(
    _In_ PVOID Parameter)
{
    ULONG Kind = PtrToUlong(Parameter);

    while (!StopChild)
    {
        RaiseTrap(Kind % TrapKindCount());
        Kind++;
    }
    return 0;
}

static
DWORD
WINAPI
MemoryThread(
    _In_ PVOID Parameter)
{
    const SIZE_T Size = 16 * PAGE_SIZE;
    PUCHAR Buffer;
    SIZE_T Offset;
    DWORD OldProtect;

    UNREFERENCED_PARAMETER(Parameter);

    while (!StopChild)
    {
        Buffer = VirtualAlloc(NULL, Size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!Buffer)
        {
            InterlockedIncrement(&ChildFailures);
            Sleep(10);
            continue;
        }
        for (Offset = 0; Offset < Size; Offset += PAGE_SIZE)
            Buffer[Offset] = 1;
        if (!VirtualProtect(Buffer, Size, PAGE_READONLY, &OldProtect))
            InterlockedIncrement(&ChildFailures);
        if (!VirtualFree(Buffer, 0, MEM_RELEASE))
            InterlockedIncrement(&ChildFailures);
        InterlockedIncrement(&ChildMemoryRounds);
    }
    return 0;
}

static
DWORD
RunChild(
    _In_ BOOL Stress,
    _In_ ULONG Seconds)
{
    SYSTEM_INFO SystemInfo;
    HANDLE Threads[64];
    ULONG ThreadCount = 0;
    ULONG FaultThreads, MemoryThreads, i;

    if (!Stress)
    {
        for (i = 0; i < TrapKindCount(); i++)
            RaiseTrap(i);
        return ChildFailures;
    }

    GetSystemInfo(&SystemInfo);
    FaultThreads = max(2, min(SystemInfo.dwNumberOfProcessors, 16));
    MemoryThreads = max(1, FaultThreads / 2);

    for (i = 0; i < FaultThreads + MemoryThreads; i++)
    {
        Threads[ThreadCount] = CreateThread(NULL,
                                            0,
                                            i < FaultThreads ? FaultThread : MemoryThread,
                                            ULongToPtr(i),
                                            0,
                                            NULL);
        if (Threads[ThreadCount])
            ThreadCount++;
        else
            InterlockedIncrement(&ChildFailures);
    }

    Sleep(Seconds * 1000);
    InterlockedExchange(&StopChild, 1);
    WaitForMultipleObjects(ThreadCount, Threads, TRUE, INFINITE);
    for (i = 0; i < ThreadCount; i++)
        CloseHandle(Threads[i]);

    for (i = 0; i < TrapKindCount(); i++)
    {
        if (ChildRaised[i] == 0)
            InterlockedIncrement(&ChildFailures);
    }
    if (ChildMemoryRounds == 0)
        InterlockedIncrement(&ChildFailures);
    return ChildFailures;
}

static
VOID
DebugChild(
    _In_ BOOL Stress,
    _In_ ULONG Seconds)
{
    WCHAR FileName[MAX_PATH];
    WCHAR CommandLine[MAX_PATH + 64];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    DEBUG_EVENT Event;
    DWORD ContinueStatus, ExitCode = (DWORD)-1;
    DWORD LastEvent, Start;
    ULONG Seen[TRAP_KINDS] = { 0 };
    ULONG Other = 0, Traced = 0, SecondChance = 0, Breakpoints = 0, i;
    BOOL Exited = FALSE, Hung = FALSE;
    ULONG_PTR ModuleBase[32];
    ULONG Modules = 0;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine,
                    sizeof(CommandLine),
                    L"\"%ls\" DebugTrapInterrupts child %ls %lu",
                    FileName,
                    Stress ? L"stress" : L"once",
                    Seconds);

    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;

    if (!CreateProcessW(FileName,
                        CommandLine,
                        NULL,
                        NULL,
                        FALSE,
                        DEBUG_ONLY_THIS_PROCESS,
                        NULL,
                        NULL,
                        &StartupInfo,
                        &ProcessInfo))
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
        return;
    }

    Start = LastEvent = GetTickCount();
    while (!Exited)
    {
        /* Bound the whole run, in case events keep coming but the child never ends */
        if (GetTickCount() - Start > (Seconds + 120) * 1000)
        {
            Hung = TRUE;
            break;
        }

        if (!WaitForDebugEvent(&Event, 1000))
        {
            /* The child raises an exception at least every few milliseconds */
            if (GetTickCount() - LastEvent > 60 * 1000)
            {
                Hung = TRUE;
                break;
            }
            continue;
        }
        LastEvent = GetTickCount();

        ContinueStatus = DBG_CONTINUE;
        switch (Event.dwDebugEventCode)
        {
            case CREATE_PROCESS_DEBUG_EVENT:
                ModuleBase[Modules++] = (ULONG_PTR)Event.u.CreateProcessInfo.lpBaseOfImage;
                if (Event.u.CreateProcessInfo.hFile)
                    CloseHandle(Event.u.CreateProcessInfo.hFile);
                break;

            case LOAD_DLL_DEBUG_EVENT:
                if (Modules < _countof(ModuleBase))
                    ModuleBase[Modules++] = (ULONG_PTR)Event.u.LoadDll.lpBaseOfDll;
                if (Event.u.LoadDll.hFile)
                    CloseHandle(Event.u.LoadDll.hFile);
                break;

            case EXCEPTION_DEBUG_EVENT:
            {
                NTSTATUS Code = Event.u.Exception.ExceptionRecord.ExceptionCode;

                if (Code == STATUS_BREAKPOINT && Event.u.Exception.dwFirstChance)
                {
                    /* The loader breakpoint */
                    Breakpoints++;
                    break;
                }

                ContinueStatus = DBG_EXCEPTION_NOT_HANDLED;
                if (!Event.u.Exception.dwFirstChance)
                {
                    SecondChance++;
                    break;
                }

                for (i = 0; i < TRAP_KINDS; i++)
                {
                    if (Code == ExpectedCode[i])
                    {
                        Seen[i]++;
                        break;
                    }
                }
                if (i == TRAP_KINDS)
                {
                    ULONG_PTR Address = (ULONG_PTR)Event.u.Exception.ExceptionRecord.ExceptionAddress;
                    ULONG Best = 0, j;

                    /* Other modules may raise and handle exceptions of their own */
                    for (j = 1; j < Modules; j++)
                    {
                        if (ModuleBase[j] <= Address && ModuleBase[j] > ModuleBase[Best])
                            Best = j;
                    }
                    if (Best == 0)
                        Other++;
                    if (Traced < 8)
                    {
                        trace("Exception 0x%lx at %p (module %lu at %p), parameters %p %p\n",
                              Code,
                              (PVOID)Address,
                              Best,
                              (PVOID)ModuleBase[Best],
                              (PVOID)Event.u.Exception.ExceptionRecord.ExceptionInformation[0],
                              (PVOID)Event.u.Exception.ExceptionRecord.ExceptionInformation[1]);
                        Traced++;
                    }
                }
                break;
            }

            case EXIT_PROCESS_DEBUG_EVENT:
                ExitCode = Event.u.ExitProcess.dwExitCode;
                Exited = TRUE;
                break;
        }

        ContinueDebugEvent(Event.dwProcessId, Event.dwThreadId, ContinueStatus);
    }

    ok(!Hung, "No debug event for 60 seconds or the child did not end in time\n");
    if (Hung)
    {
        TerminateProcess(ProcessInfo.hProcess, 1);
        DebugActiveProcessStop(ProcessInfo.dwProcessId);
    }

    ok(Breakpoints == 1, "Breakpoints = %lu\n", Breakpoints);
    ok(SecondChance == 0, "SecondChance = %lu\n", SecondChance);
    ok(Other == 0, "Other exceptions in the test = %lu\n", Other);
    for (i = 0; i < TrapKindCount(); i++)
    {
        if (Stress)
            ok(Seen[i] > 0, "Exception 0x%lx seen %lu times\n", ExpectedCode[i], Seen[i]);
        else
            ok(Seen[i] == 1, "Exception 0x%lx seen %lu times\n", ExpectedCode[i], Seen[i]);
    }
    ok(Exited && ExitCode == 0, "Exited %d, exit code %lu\n", Exited, ExitCode);
    trace("%ls: %lu privileged instruction, %lu assertion, %lu SSE exceptions\n",
          Stress ? L"stress" : L"once", Seen[0], Seen[1], Seen[2]);

    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
}

START_TEST(DebugTrapInterrupts)
{
    char **Argv;
    int Argc;
    char Buffer[16];
    char *End;
    DWORD Length;
    ULONG Seconds = 10, Value;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 5 && !strcmp(Argv[2], "child"))
    {
        BOOL Stress = !strcmp(Argv[3], "stress");

        Value = strtoul(Argv[4], &End, 10);
        if (!isdigit((unsigned char)Argv[4][0]) || *End || Value > 3600 || (Stress && Value == 0))
            ExitProcess(1);
        ExitProcess(RunChild(Stress, Value));
    }

    Length = GetEnvironmentVariableA("TRAPDBG_SECONDS", Buffer, sizeof(Buffer));
    if (Length > 0 && Length < sizeof(Buffer) && isdigit((unsigned char)Buffer[0]))
    {
        Value = strtoul(Buffer, &End, 10);
        if (!*End && Value > 0 && Value <= 3600)
            Seconds = Value;
    }

    DebugChild(FALSE, 0);
    DebugChild(TRUE, Seconds);
}
