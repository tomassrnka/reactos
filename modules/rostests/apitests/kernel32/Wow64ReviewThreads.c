/* SPDX-License-Identifier: GPL-2.0-or-later
 * Native controller plus an identical PE32 child. The startup and transition
 * schedules use the probes in wow64/phase3a_review.py in a throwaway test build.
 * The exit/allocator race also runs on ordinary images and native i386. */
#include "precomp.h"
#include <pseh/pseh2.h>
#include <ndk/psfuncs.h>
#include <ndk/kefuncs.h>
#include <ndk/exfuncs.h>
#include <reactos/wow64.h>
#include <winuser.h>

static HANDLE Finish;
static LONG Allocations;

static VOID CALLBACK NestedApc(ULONG_PTR Depth)
{
    if (!Depth)
    {
        ok(QueueUserAPC(NestedApc, GetCurrentThread(), 1), "queue nested APC\n");
        ok(SleepEx(0, TRUE) == WAIT_IO_COMPLETION, "nested APC delivery\n");
    }
}

static BOOL WaitUntilBlocked(PROCESS_INFORMATION *Child)
{
    BYTE *Buffer = HeapAlloc(GetProcessHeap(), 0, 1024 * 1024);
    ULONG Attempt, i;
    BOOL Found = FALSE;
    if (!Buffer) return FALSE;
    for (Attempt = 0; Attempt < 1000 && !Found; ++Attempt)
    {
        PSYSTEM_PROCESS_INFORMATION Process = (PVOID)Buffer;
        if (NT_SUCCESS(NtQuerySystemInformation(SystemProcessInformation, Buffer, 1024 * 1024, NULL)))
        {
            for (;;)
            {
                if (HandleToULong(Process->UniqueProcessId) == Child->dwProcessId)
                {
                    PSYSTEM_THREAD_INFORMATION Thread = (PVOID)(Process + 1);
                    for (i = 0; i < Process->NumberOfThreads; ++i)
                        if (HandleToULong(Thread[i].ClientId.UniqueThread) == Child->dwThreadId &&
                            Thread[i].ThreadState == 5 /* Waiting */ &&
                            Thread[i].WaitReason == 6 /* UserRequest */) Found = TRUE;
                    break;
                }
                if (!Process->NextEntryOffset) break;
                Process = (PVOID)((BYTE *)Process + Process->NextEntryOffset);
            }
        }
        if (!Found) Sleep(10);
    }
    HeapFree(GetProcessHeap(), 0, Buffer);
    return Found;
}

static VOID CheckRoundTrip(PROCESS_INFORMATION *Child)
{
#ifdef _WIN64
    WOW64_CONTEXT Context;
#else
    CONTEXT Context;
#endif
    ok(WaitUntilBlocked(Child), "child did not block in its wait\n");
    ok(SuspendThread(Child->hThread) == 0, "suspend blocked child\n");
    memset(&Context, 0, sizeof(Context));
#ifdef _WIN64
    Context.ContextFlags = WOW64_CONTEXT_FULL;
    ok_ntstatus(NtQueryInformationThread(Child->hThread, ThreadWow64Context,
                                        &Context, sizeof(Context), NULL), STATUS_SUCCESS);
    ok_ntstatus(NtSetInformationThread(Child->hThread, ThreadWow64Context,
                                      &Context, sizeof(Context)), STATUS_SUCCESS);
#else
    Context.ContextFlags = CONTEXT_FULL;
    ok(GetThreadContext(Child->hThread, &Context), "get blocked context\n");
    ok(SetThreadContext(Child->hThread, &Context), "restore blocked context\n");
#endif
    ok(ResumeThread(Child->hThread) == 1, "resume blocked child\n");
}

/* Shared with the controller: [0] guest unwound past the popped callback,
 * [1] controller finished inspecting it. */
#define POP_SECTION "wow64-r1-callbackpop-%lx"
static volatile LONG *PopShared;
static volatile BOOL PopArmed;
static LRESULT (WINAPI *pDefWindowProcW)(HWND, UINT, WPARAM, LPARAM);

static LRESULT CALLBACK PopWindowProc(HWND Window, UINT Message, WPARAM Wparam, LPARAM Lparam)
{
    if (Message == WM_WINDOWPOSCHANGING && PopArmed)
    {
        PopArmed = FALSE;
        RaiseException(0xe042643c, 0, 0, NULL);
    }
    return pDefWindowProcW(Window, Message, Wparam, Lparam);
}

/* The finally block needs its own registration frame. RtlUnwind calls it
 * right after the popped callback frame's handler, while the frame of the
 * catching function only runs its scopes after RtlUnwind's NtContinue. */
static DECLSPEC_NOINLINE BOOL
PopSetWindowPos(BOOL (WINAPI *pSetWindowPos)(HWND, HWND, int, int, int, int, UINT),
                HWND Window, volatile BOOL *Unwound)
{
    volatile BOOL Returned = FALSE;
    ULONG Spin;
    _SEH2_TRY
    {
        pSetWindowPos(Window, NULL, 40, 40, 160, 160, SWP_NOZORDER | SWP_NOACTIVATE);
        Returned = TRUE;
    }
    _SEH2_FINALLY
    {
        if (!Returned)
        {
            /* No service call between here and the popped frame. */
            *Unwound = TRUE;
            InterlockedExchange(&PopShared[0], 1);
            for (Spin = 0; !PopShared[1] && Spin < 0x40000000; ++Spin) YieldProcessor();
        }
    }
    _SEH2_END;
    return Returned;
}

/* Unwind a guest exception through a win32k callback, which pops the nested
 * callback frame with STATUS_CALLBACK_POP_STACK. The finally block runs in
 * the middle of that unwind, before the guest makes any further service call,
 * and waits there in pure guest code while the controller inspects the CPU
 * state the guest now runs on. */
static VOID ChildCallbackPop(void)
{
    HMODULE User32 = LoadLibraryW(L"user32.dll");
    ATOM (WINAPI *pRegisterClassW)(const WNDCLASSW *);
    HWND (WINAPI *pCreateWindowExW)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, PVOID);
    BOOL (WINAPI *pSetWindowPos)(HWND, HWND, int, int, int, int, UINT);
    BOOL (WINAPI *pDestroyWindow)(HWND);
    WNDCLASSW Class = {0};
    char Name[64];
    HANDLE Map;
    HWND Window = NULL;
    volatile BOOL Returned = FALSE, Unwound = FALSE;
    DWORD Code = 0;

    ok(User32 != NULL, "load user32: %lu\n", GetLastError());
    if (!User32) return;
    pRegisterClassW = (PVOID)GetProcAddress(User32, "RegisterClassW");
    pCreateWindowExW = (PVOID)GetProcAddress(User32, "CreateWindowExW");
    pSetWindowPos = (PVOID)GetProcAddress(User32, "SetWindowPos");
    pDestroyWindow = (PVOID)GetProcAddress(User32, "DestroyWindow");
    pDefWindowProcW = (PVOID)GetProcAddress(User32, "DefWindowProcW");
    sprintf(Name, POP_SECTION, GetCurrentProcessId());
    Map = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, Name);
    ok(Map != NULL, "open controller section: %lu\n", GetLastError());
    if (!Map) return;
    PopShared = MapViewOfFile(Map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    ok(PopShared != NULL, "map controller section: %lu\n", GetLastError());
    Class.lpfnWndProc = PopWindowProc;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.lpszClassName = L"Wow64ReviewCallbackPop";
    if (PopShared && pRegisterClassW(&Class))
        Window = pCreateWindowExW(0, Class.lpszClassName, L"pop", WS_OVERLAPPEDWINDOW,
                                  10, 10, 150, 150, NULL, NULL, Class.hInstance, NULL);
    ok(Window != NULL, "create window: %lu\n", GetLastError());
    if (Window)
    {
        PopArmed = TRUE;
        _SEH2_TRY
        {
            Returned = PopSetWindowPos(pSetWindowPos, Window, &Unwound);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Code = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
        ok(!PopArmed, "window procedure not entered\n");
        ok(!Returned && Unwound, "callback not unwound: returned %d unwound %d\n", Returned, Unwound);
        ok(Code == 0xe042643c, "outer exception code %lx\n", Code);
        ok(PopShared[1], "controller did not inspect the popped frame\n");
        /* Reenter win32k after the unwind. */
        ok(pSetWindowPos(Window, NULL, 50, 50, 180, 180, SWP_NOZORDER | SWP_NOACTIVATE), "set position after unwind\n");
        ok(pDestroyWindow(Window), "destroy window\n");
    }
    if (PopShared) UnmapViewOfFile((PVOID)PopShared);
    CloseHandle(Map);
}

static VOID CheckCallbackPop(PROCESS_INFORMATION *Child)
{
#ifdef _WIN64
    THREAD_BASIC_INFORMATION Basic;
    WOW64_CPU_STATE State;
    WOW64_CONTEXT Context;
    PVOID Cpu = NULL;
#endif
    volatile LONG *Shared = NULL;
    char Name[64];
    HANDLE Map;
    DWORD Error;
    ULONG i;
#ifdef _WIN64
    ok_ntstatus(NtQueryInformationThread(Child->hThread, ThreadBasicInformation,
                                        &Basic, sizeof(Basic), NULL), STATUS_SUCCESS);
#endif
    /* Named after the suspended child, so only this run uses it. */
    sprintf(Name, POP_SECTION, Child->dwProcessId);
    Map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, Name);
    Error = GetLastError();
    ok(Map != NULL && Error != ERROR_ALREADY_EXISTS, "create section: %lu\n", Error);
    if (Map && Error == ERROR_ALREADY_EXISTS) { CloseHandle(Map); Map = NULL; }
    if (Map) Shared = MapViewOfFile(Map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    ok(Shared != NULL, "map section: %lu\n", GetLastError());
    ResumeThread(Child->hThread);
    if (!Shared) { if (Map) CloseHandle(Map); return; }
    ok(!Shared[0] && !Shared[1], "fresh section is not zero\n");
    for (i = 0; i < 3000 && !Shared[0]; ++i)
    {
        if (WaitForSingleObject(Child->hProcess, 10) == WAIT_OBJECT_0) break;
    }
    ok(Shared[0], "guest did not unwind through the callback\n");
#ifdef _WIN64
    if (Shared[0])
    {
        ok(SuspendThread(Child->hThread) == 0, "suspend unwinding child\n");
        ok(ReadProcessMemory(Child->hProcess, &((PTEB)Basic.TebBaseAddress)->TlsSlots[WOW64_TLS_CPU],
                             &Cpu, sizeof(Cpu), NULL) && Cpu, "CPU state unavailable\n");
        memset(&State, 0, sizeof(State));
        if (Cpu)
            ok(ReadProcessMemory(Child->hProcess, Cpu, &State, sizeof(State), NULL), "read CPU state\n");
        /* The guest runs on the surviving frame. Its guard must stay set
         * until its next service entry has saved the complete context. */
        ok(State.InTransition == 1, "guest runs with InTransition %ld after a popped callback\n",
           State.InTransition);
        ok(State.CallbackFrame == NULL, "surviving frame still has a callback frame %p\n", State.CallbackFrame);
        memset(&Context, 0, sizeof(Context));
        Context.ContextFlags = WOW64_CONTEXT_FULL;
        ok_ntstatus(NtQueryInformationThread(Child->hThread, ThreadWow64Context,
                                            &Context, sizeof(Context), NULL), STATUS_SUCCESS);
        ok(ResumeThread(Child->hThread) == 1, "resume unwinding child\n");
    }
#endif
    InterlockedExchange(&Shared[1], 1);
    UnmapViewOfFile((PVOID)Shared);
    CloseHandle(Map);
}

static DWORD WINAPI AllocateWorker(PVOID Parameter)
{
    ULONG i;
    UNREFERENCED_PARAMETER(Parameter);
    while (WaitForSingleObject(Finish, 0) == WAIT_TIMEOUT)
    {
        volatile ULONG *Memory = VirtualAlloc(NULL, 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!Memory) continue;
        _SEH2_TRY
        {
            for (i = 0; i < 256; ++i) Memory[i * 1024] = 0x35a12345;
            SwitchToThread();
            for (i = 0; i < 256; ++i)
                if (Memory[i * 1024] != 0x35a12345) { ok(FALSE, "allocation overwritten\n"); break; }
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        { ok(FALSE, "live allocation released: %lx\n", _SEH2_GetExceptionCode()); }
        _SEH2_END;
        ok(VirtualFree((PVOID)Memory, 0, MEM_RELEASE), "live allocation no longer owned\n");
        InterlockedIncrement(&Allocations);
    }
    return 0;
}

static HANDLE Event(const char *Suffix, BOOL Manual)
{
    char Name[80];
    sprintf(Name, "wow64-r1-%s", Suffix);
    return CreateEventA(NULL, Manual, FALSE, Name);
}

static VOID ExitNative(PROCESS_INFORMATION *Child, BOOL Apc)
{
    DWORD Id, Exit = 0, Wait;
    CLIENT_ID Client;
    NTSTATUS Status;
    HANDLE Thread = NULL;
    LPTHREAD_START_ROUTINE Routine = (LPTHREAD_START_ROUTINE)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlExitUserThread");
    /* The PE32 target need not have native kernel32 mapped. Enter ntdll
     * directly instead of using kernel32's BaseThreadStartup trampoline. */
    Status = RtlCreateUserThread(Child->hProcess, NULL, TRUE, 0, 0, 0,
                                 (PTHREAD_START_ROUTINE)Routine, (PVOID)0x35a,
                                 &Thread, &Client);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    Id = HandleToULong(Client.UniqueThread);
    trace("R1 native exit pid=%lx tid=%lx\n", Child->dwProcessId, Id);
#ifdef _WIN64
    if (Apc)
    {
        Status = NtQueueApcThread(Thread, (PKNORMAL_ROUTINE)(LONG_PTR)-0x48d0,
                                           NULL, NULL, NULL);
        ok_ntstatus(Status, STATUS_SUCCESS);
    }
#endif
    ResumeThread(Thread);
    Wait = WaitForSingleObject(Thread, 5000);
    ok(Wait == WAIT_OBJECT_0, "native helper wait: %lu\n", Wait);
    if (Wait == WAIT_OBJECT_0)
    {
        GetExitCodeThread(Thread, &Exit);
        ok(Exit == 0x35a, "native helper ran guest startup: %lx\n", Exit);
    }
    CloseHandle(Thread);
}

#ifdef _WIN64
static VOID CheckTransition(PROCESS_INFORMATION *Child, BOOL Apc)
{
    WOW64_CONTEXT Get, Set, Saved;
    THREAD_BASIC_INFORMATION Basic;
    WOW64_CPU_STATE Before, After;
    PVOID Cpu;
    HANDLE Arrived = Event(Apc ? "apc-transition" : "transition", FALSE);
    HANDLE Proceed = Event(Apc ? "apc-continue" : "continue", FALSE);
    ULONG Expected = Apc ? 4 : 160;
    ULONG Count = 0;
    NTSTATUS Status;
    HANDLE Waits[2] = {Arrived, Child->hProcess};
    ok_ntstatus(NtQueryInformationThread(Child->hThread, ThreadBasicInformation,
                                        &Basic, sizeof(Basic), NULL), STATUS_SUCCESS);
    ResumeThread(Child->hThread);
    while (WaitForMultipleObjects(2, Waits, FALSE, 30000) == WAIT_OBJECT_0)
    {
        ++Count;
        Cpu = NULL;
        ok(ReadProcessMemory(Child->hProcess, &((PTEB)Basic.TebBaseAddress)->TlsSlots[WOW64_TLS_CPU],
                             &Cpu, sizeof(Cpu), NULL) && Cpu, "CPU state unavailable at stop %lu\n", Count);
        if (!Cpu) break;
        ok(ReadProcessMemory(Child->hProcess, Cpu, &Before, sizeof(Before), NULL), "read CPU\n");
        memset(&Get, 0x5a, sizeof(Get));
        Get.ContextFlags = WOW64_CONTEXT_ALL;
        Saved = Get;
        Status = NtQueryInformationThread(Child->hThread, ThreadWow64Context, &Get, sizeof(Get), NULL);
        ok_ntstatus(Status, STATUS_RETRY);
        ok(!memcmp(&Get, &Saved, sizeof(Get)), "failed get changed the output at stop %lu\n", Count);
        Set = Get;
        Set.ContextFlags = WOW64_CONTEXT_INTEGER;
        Set.Ebx = 0xa135a135;
        Status = NtSetInformationThread(Child->hThread, ThreadWow64Context, &Set, sizeof(Set));
        ok_ntstatus(Status, STATUS_RETRY);
        ok(ReadProcessMemory(Child->hProcess, Cpu, &After, sizeof(After), NULL), "reread CPU\n");
        ok(!memcmp(&Before, &After, sizeof(Before)), "set partly changed transition state at stop %lu\n", Count);
        /* Restore on the unfixed image so all subsequent boundaries run. */
        if (memcmp(&Before, &After, sizeof(Before)))
            WriteProcessMemory(Child->hProcess, Cpu, &Before, sizeof(Before), NULL);
        SetEvent(Proceed);
        if (Count == Expected) break;
    }
    ok(Count == Expected, "transition boundaries visited: %lu/%lu\n", Count, Expected);
    CloseHandle(Arrived);
    CloseHandle(Proceed);
}
#endif

START_TEST(Wow64ReviewThreads)
{
    char **Argv, Command[2 * MAX_PATH];
    int Argc = winetest_get_mainargs(&Argv);
    HANDLE Ready, Entered = NULL, Release = NULL;
    STARTUPINFOA Startup = {sizeof(Startup)};
    PROCESS_INFORMATION Child;
    BOOL Guest = FALSE;
    DWORD Exit, i;

    Finish = Event("finish", TRUE);
    Ready = Event("ready", TRUE);
    if (Argc >= 3 && (!strcmp(Argv[2], "childapc") || !strcmp(Argv[2], "childwait")))
    {
        if (!strcmp(Argv[2], "childapc"))
        {
            ok(QueueUserAPC(NestedApc, GetCurrentThread(), 0), "queue APC\n");
            ok(SleepEx(0, TRUE) == WAIT_IO_COMPLETION, "APC delivery\n");
        }
        SetEvent(Ready);
        Exit = WaitForSingleObject(Finish, 60000);
        ok(Exit == WAIT_OBJECT_0, "context round trip changed wait result: %lx\n", Exit);
        CloseHandle(Ready); CloseHandle(Finish);
        return;
    }
    if (Argc >= 3 && !strcmp(Argv[2], "childpop"))
    {
        ChildCallbackPop();
        SetEvent(Ready);
        Exit = WaitForSingleObject(Finish, 60000);
        ok(Exit == WAIT_OBJECT_0, "finish wait: %lx\n", Exit);
        CloseHandle(Ready); CloseHandle(Finish);
        return;
    }
    if (Argc >= 3 && !strcmp(Argv[2], "child"))
    {
        HANDLE Workers[2];
        Workers[0] = CreateThread(NULL, 0, AllocateWorker, NULL, 0, NULL);
        Workers[1] = CreateThread(NULL, 0, AllocateWorker, NULL, 0, NULL);
        ok(Workers[0] && Workers[1], "allocator threads\n");
        SetEvent(Ready);
        WaitForSingleObject(Finish, 60000);
        if (Workers[0] && Workers[1])
        {
            ok(WaitForMultipleObjects(2, Workers, TRUE, 10000) == WAIT_OBJECT_0, "allocator exit\n");
            CloseHandle(Workers[0]); CloseHandle(Workers[1]);
        }
        trace("R1 allocations %ld\n", Allocations);
        CloseHandle(Ready); CloseHandle(Finish);
        return;
    }
    IsWow64Process(GetCurrentProcess(), &Guest);
    if (Guest || Argc < 4)
    {
        skip("Native controller requires PE32 fixture path and exit/startup/transition/apc/apccontext/roundtrip/callbackpop mode\n");
        goto Done;
    }
    if (!strcmp(Argv[3], "startup"))
    {
        Entered = Event("init-entered", TRUE);
        Release = Event("init-release", TRUE);
    }
    _snprintf(Command, sizeof(Command), "\"%s\" Wow64ReviewThreads %s", Argv[2],
              !strcmp(Argv[3], "apccontext") ? "childapc" :
              !strcmp(Argv[3], "roundtrip") ? "childwait" :
              !strcmp(Argv[3], "callbackpop") ? "childpop" : "child");
    if (!CreateProcessA(Argv[2], Command, NULL, NULL, FALSE, CREATE_SUSPENDED,
                        NULL, NULL, &Startup, &Child))
    { ok(FALSE, "PE32 child: %lu\n", GetLastError()); goto Done; }
#ifdef _WIN64
    if (!strcmp(Argv[3], "transition") || !strcmp(Argv[3], "apccontext"))
        CheckTransition(&Child, !strcmp(Argv[3], "apccontext"));
    else
#endif
    if (!strcmp(Argv[3], "callbackpop"))
        CheckCallbackPop(&Child);
    else
    {
        ResumeThread(Child.hThread);
        if (Entered)
        {
            ok(WaitForSingleObject(Entered, 10000) == WAIT_OBJECT_0, "startup probe not reached\n");
            ExitNative(&Child, FALSE);
            ok(WaitForSingleObject(Ready, 0) == WAIT_TIMEOUT, "guest entry ran on a native helper\n");
            SetEvent(Release);
        }
    }
    ok(WaitForSingleObject(Ready, 30000) == WAIT_OBJECT_0, "guest entry not reached\n");
    if (!strcmp(Argv[3], "exit"))
        for (i = 0; i < 128; ++i) ExitNative(&Child, FALSE);
    if (!strcmp(Argv[3], "apc")) ExitNative(&Child, TRUE);
    if (!strcmp(Argv[3], "roundtrip")) CheckRoundTrip(&Child);
    SetEvent(Finish);
    if (WaitForSingleObject(Child.hProcess, 10000) != WAIT_OBJECT_0)
    { ok(FALSE, "child did not exit\n"); TerminateProcess(Child.hProcess, 0xdead); WaitForSingleObject(Child.hProcess, 5000); }
    GetExitCodeProcess(Child.hProcess, &Exit);
    ok(Exit == 0, "child exit %lx\n", Exit);
    CloseHandle(Child.hThread); CloseHandle(Child.hProcess);
Done:
    if (Entered) CloseHandle(Entered);
    if (Release) CloseHandle(Release);
    CloseHandle(Ready); CloseHandle(Finish);
}
