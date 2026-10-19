/*
 * PROJECT:     ReactOS WoW64 API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     An idle message loop must block. Typical loops run on an empty queue for two seconds each and
 *              count their iterations and the CPU time they take. 32-bit Windows code keeps its stack only
 *              4-byte aligned (MSVC, Delphi), so the waits are made at both stack alignments. Build as PE32;
 *              the same source also runs natively (i386, amd64) and on Windows.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#define RUN_MS 2000
#define TIMER_MS 100
#define WM_END_RUN (WM_APP + 1)

static unsigned Checks, Failures;
#define CHECK(x, ...) do { ++Checks; if (!(x)) { ++Failures; printf("FAIL line %u: %s: ", __LINE__, #x); printf(__VA_ARGS__); printf("\n"); } } while (0)

static HWND Window;
static DWORD MainThreadId;
static DWORD StackOffset; /* the stack is this many bytes past an 8-byte boundary when the wait is called */

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT Message, WPARAM wParam, LPARAM lParam)
{
    if (Message == WM_PAINT)
    {
        PAINTSTRUCT Paint;
        BeginPaint(hwnd, &Paint);
        EndPaint(hwnd, &Paint);
        return 0;
    }
    return DefWindowProcW(hwnd, Message, wParam, lParam);
}

static DWORD MsgWait(DWORD Count, const HANDLE *Handles, DWORD Milliseconds, DWORD WakeMask, DWORD Flags)
{
#if defined(__GNUC__) && defined(__i386__)
    DWORD Args[6] = { Count, (DWORD)Handles, Milliseconds, WakeMask, Flags, StackOffset };
    DWORD Result;
    /* 16-byte boundary, minus 12 + StackOffset, minus 5 arguments: the call is made StackOffset past 8 bytes */
    __asm__ __volatile__(
        "movl %%esp, %%edi\n\t"
        "andl $-16, %%esp\n\t"
        "subl 20(%1), %%esp\n\t"
        "subl $12, %%esp\n\t"
        "pushl 16(%1)\n\t"
        "pushl 12(%1)\n\t"
        "pushl 8(%1)\n\t"
        "pushl 4(%1)\n\t"
        "pushl (%1)\n\t"
        "call *%2\n\t"
        "movl %%edi, %%esp"
        : "=a"(Result)
        : "r"(Args), "r"(MsgWaitForMultipleObjectsEx)
        : "ecx", "edx", "edi", "memory", "cc");
    return Result;
#else
    return MsgWaitForMultipleObjectsEx(Count, Handles, Milliseconds, WakeMask, Flags);
#endif
}

static ULONGLONG ThreadCpuMs(void)
{
    FILETIME Create, Exit, Kernel, User;
    if (!GetThreadTimes(GetCurrentThread(), &Create, &Exit, &Kernel, &User))
        return 0;
    return ((((ULONGLONG)Kernel.dwHighDateTime << 32) | Kernel.dwLowDateTime) +
            (((ULONGLONG)User.dwHighDateTime << 32) | User.dwLowDateTime)) / 10000;
}

static void Drain(void)
{
    MSG Msg;
    while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&Msg);
}

typedef struct
{
    const char *Name;
    unsigned Iterations, Messages, Timers, WaitFailed;
    DWORD Elapsed, LastError, LastWait;
    ULONGLONG Cpu;
} RESULT;

/* One loop iteration: the wait (or poll) under test. Like real message loops, a failed wait does not end the loop */
typedef void (*STEP)(RESULT *r, DWORD Remaining);

static void Waited(RESULT *r, DWORD Result)
{
    r->LastWait = Result;
    if (Result == WAIT_FAILED)
    {
        ++r->WaitFailed;
        r->LastError = GetLastError();
    }
}

static void StepMsgWait(RESULT *r, DWORD Remaining)
{
    /* The common message-pump wait: any input, and input already seen counts (MWMO_INPUTAVAILABLE) */
    Waited(r, MsgWait(0, NULL, Remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE));
}

static void StepMsgWaitInfinite(RESULT *r, DWORD Remaining)
{
    /* As above with no timeout: the run ends with a message that a helper thread posts */
    Waited(r, MsgWait(0, NULL, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE));
}

static void StepMsgWaitEvent(RESULT *r, DWORD Remaining)
{
    /* A worker-style wait for an event or a message (Delphi, Total Commander) */
    static HANDLE Event;
    if (!Event) Event = CreateEventW(NULL, FALSE, FALSE, NULL);
    Waited(r, MsgWait(1, &Event, INFINITE, QS_ALLINPUT, 0));
}

static void StepMsgWaitAlertable(RESULT *r, DWORD Remaining)
{
    static HANDLE Event;
    if (!Event) Event = CreateEventW(NULL, FALSE, FALSE, NULL);
    Waited(r, MsgWait(1, &Event, Remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE | MWMO_ALERTABLE));
}

static void StepFirefox(RESULT *r, DWORD Remaining)
{
    /* Firefox's WinUtils::WaitForMessage: wait, then decide with GetQueueStatus and PeekMessage(PM_NOREMOVE) */
    MSG Msg;
    Waited(r, MsgWait(0, NULL, INFINITE, QS_ALLINPUT | QS_ALLPOSTMESSAGE, MWMO_INPUTAVAILABLE));
    if (r->LastWait != WAIT_TIMEOUT && !(HIWORD(GetQueueStatus(QS_SENDMESSAGE)) & QS_SENDMESSAGE) &&
        !PeekMessageW(&Msg, NULL, 0, 0, PM_NOREMOVE))
        SwitchToThread();
}

static void StepChromium(RESULT *r, DWORD Remaining)
{
    /* The Chromium-style UI pump (Firefox's GPU process): wait, then look at the mouse change bits */
    MSG Msg;
    Waited(r, MsgWait(0, NULL, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE));
    if (r->LastWait == WAIT_OBJECT_0 && (HIWORD(GetQueueStatus(QS_MOUSE)) & QS_MOUSE) &&
        !PeekMessageW(&Msg, NULL, WM_MOUSEFIRST, WM_MOUSELAST, PM_NOREMOVE))
        WaitMessage();
}

static void StepWaitMessage(RESULT *r, DWORD Remaining)
{
    /* Bounded by the timer or the end-of-run message */
    r->LastWait = WaitMessage();
    if (!r->LastWait)
    {
        ++r->WaitFailed;
        r->LastError = GetLastError();
    }
}

static void StepQueueStatus(RESULT *r, DWORD Remaining)
{
    /* A poller that sleeps unless GetQueueStatus or GetInputState claims there is something to do */
    if (!(HIWORD(GetQueueStatus(QS_ALLINPUT)) & QS_ALLINPUT) && !GetInputState())
        Sleep(50);
}

static DWORD WINAPI EndRunThread(PVOID Param)
{
    /* An INFINITE wait on an idle queue needs a wake: one posted message when the run is over */
    unsigned Try;
    Sleep(RUN_MS);
    for (Try = 0; Try < 50 && !PostThreadMessageW(MainThreadId, WM_END_RUN, 0, 0); ++Try)
        Sleep(10);
    return 0;
}

static DWORD WINAPI WatchdogThread(PVOID Param)
{
    /* A wait that never returns must not hang the test run */
    Sleep(180000);
    printf("FAIL: watchdog, the test did not finish in 180 s\n");
    printf("IDLE summary: %u checks, %u failures\n", Checks + 1, Failures + 1);
    ExitProcess(2);
}

/* One run; FALSE when input that really arrived (the window is on a live desktop) spoiled an otherwise good run */
static BOOL RunOnce(const char *Name, STEP Step, BOOL Timer, unsigned MaxIterations, BOOL Last)
{
    RESULT Result, *r = &Result;
    MSG Msg;
    DWORD Start, Status;
    ULONGLONG Cpu0;
    HANDLE Thread;
    BOOL Ended = FALSE;

    ZeroMemory(r, sizeof(*r));
    r->Name = Name;
    Drain();
    if (Timer)
        CHECK(SetTimer(Window, 1, TIMER_MS, NULL) != 0, "%s: SetTimer error %lu", Name, GetLastError());
    Thread = CreateThread(NULL, 0, EndRunThread, NULL, 0, NULL);
    if (!Thread)
    {
        CHECK(FALSE, "%s: CreateThread error %lu", Name, GetLastError());
        if (Timer) KillTimer(Window, 1);
        return TRUE;
    }
    Cpu0 = ThreadCpuMs();
    Start = GetTickCount();
    while (!Ended && (r->Elapsed = GetTickCount() - Start) < RUN_MS + 1000 && r->Iterations < 1000000)
    {
        ++r->Iterations;
        Step(r, RUN_MS + 1000 - r->Elapsed);
        while (PeekMessageW(&Msg, NULL, 0, 0, PM_REMOVE))
        {
            if (Msg.message == WM_END_RUN && !Msg.hwnd)
            {
                Ended = TRUE;
                continue;
            }
            ++r->Messages;
            if (Msg.message == WM_TIMER) ++r->Timers;
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
    }
    r->Elapsed = GetTickCount() - Start;
    r->Cpu = ThreadCpuMs() - Cpu0;
    if (Timer) KillTimer(Window, 1);
    WaitForSingleObject(Thread, INFINITE);
    CloseHandle(Thread);
    Drain();
    Status = GetQueueStatus(QS_ALLINPUT);
    if (!Last && !r->WaitFailed && r->Messages > r->Timers && Ended &&
        (r->Iterations > MaxIterations || r->Cpu > RUN_MS / 4))
    {
        printf("IDLE %-15s align=%lu retry: %u input messages arrived\n", Name, StackOffset, r->Messages - r->Timers);
        return FALSE;
    }
    printf("IDLE %-15s align=%lu iterations=%u messages=%u timers=%u waitfailed=%u error=%lu elapsed=%lu cpu=%lums status=%08lx\n",
           Name, StackOffset, r->Iterations, r->Messages, r->Timers, r->WaitFailed, r->LastError, r->Elapsed,
           (unsigned long)r->Cpu, Status);
    /* The run must end by the posted message: a wait that missed it would sit out the deadline */
    CHECK(Ended && r->Elapsed >= RUN_MS - 50 && r->Elapsed < RUN_MS + 900, "%s (stack +%lu): ended=%d after %lu ms",
          Name, StackOffset, Ended, r->Elapsed);
    CHECK(r->WaitFailed == 0, "%s (stack +%lu): %u failed waits, error %lu", Name, StackOffset, r->WaitFailed, r->LastError);
    CHECK(r->Iterations <= MaxIterations, "%s (stack +%lu): %u iterations in %lu ms (max %u)", Name, StackOffset,
          r->Iterations, r->Elapsed, MaxIterations);
    CHECK(r->Cpu <= RUN_MS / 4, "%s (stack +%lu): %lu ms CPU in %lu ms", Name, StackOffset, (unsigned long)r->Cpu, r->Elapsed);
    if (Timer)
        CHECK(r->Timers >= RUN_MS / TIMER_MS / 4 && r->Timers <= RUN_MS / TIMER_MS + 2,
              "%s (stack +%lu): %u WM_TIMER in %lu ms", Name, StackOffset, r->Timers, r->Elapsed);
    return TRUE;
}

static void RunLoop(const char *Name, STEP Step, BOOL Timer, unsigned MaxIterations)
{
    unsigned Try;
    for (Try = 0; Try < 3 && !RunOnce(Name, Step, Timer, MaxIterations, Try == 2); ++Try)
        ;
}

int main(void)
{
    WNDCLASSW Class = {0};
    DWORD Status, Start, Elapsed;
    HANDLE Watchdog;
    unsigned Try;
    BOOL Wow = FALSE;

    setvbuf(stdout, NULL, _IONBF, 0);
    MainThreadId = GetCurrentThreadId();
    Watchdog = CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);
    if (!Watchdog)
    {
        printf("FAIL: watchdog thread not created, error %lu\nIDLE summary: 1 checks, 1 failures\n", GetLastError());
        return 1;
    }
    CloseHandle(Watchdog);
    IsWow64Process(GetCurrentProcess(), &Wow);
    printf("IDLE start ptr=%u wow64=%d\n", (unsigned)(sizeof(void *) * 8), Wow);

    Class.lpfnWndProc = WindowProc;
    Class.hInstance = GetModuleHandleW(NULL);
    Class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    Class.lpszClassName = L"wow64_idle_loop";
    CHECK(RegisterClassW(&Class), "error %lu", GetLastError());
    Window = CreateWindowW(Class.lpszClassName, L"Idle loop", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           20, 20, 240, 180, NULL, NULL, Class.hInstance, NULL);
    CHECK(Window != NULL, "error %lu", GetLastError());
    UpdateWindow(Window);

    /* Let creation, activation and painting settle */
    Start = GetTickCount();
    while (GetTickCount() - Start < 500)
    {
        MsgWaitForMultipleObjectsEx(0, NULL, 50, QS_ALLINPUT, 0);
        Drain();
    }

    /* A drained queue reports nothing, and no input. Input that really arrives on the live desktop is drained
       and the check made again */
    for (Try = 0; Try < 3; ++Try)
    {
        Drain();
        Status = GetQueueStatus(QS_ALLINPUT);
        if (!LOWORD(Status) && !HIWORD(GetQueueStatus(QS_ALLINPUT)) && !GetInputState())
            break;
        Sleep(200);
    }
    Status = GetQueueStatus(QS_ALLINPUT);
    printf("IDLE drained status=%08lx inputstate=%d\n", Status, GetInputState());
    CHECK(LOWORD(Status) == 0, "wake bits %04x left after the queue was drained", LOWORD(Status));
    Status = GetQueueStatus(QS_ALLINPUT);
    CHECK(HIWORD(Status) == 0, "change bits %04x not cleared by the previous GetQueueStatus", HIWORD(Status));
    CHECK(!GetInputState(), "GetInputState reports input on an idle queue");

    for (StackOffset = 0; StackOffset <= 4; StackOffset += 4)
    {
        /* A zero-timeout poll on an idle queue times out at once; a 300 ms wait takes 300 ms. A message that
           really arrives (the session is live) wakes the wait: drain it and wait again */
        for (Try = 0; Try < 3; ++Try)
        {
            Drain();
            Status = MsgWait(0, NULL, 0, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (Status != WAIT_OBJECT_0) break;
        }
        CHECK(Status == WAIT_TIMEOUT, "stack +%lu: zero-timeout wait returned %lx, error %lu", StackOffset, Status, GetLastError());
        for (Try = 0; Try < 3; ++Try)
        {
            Drain();
            Start = GetTickCount();
            Status = MsgWait(0, NULL, 300, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            Elapsed = GetTickCount() - Start;
            if (Status != WAIT_OBJECT_0) break;
        }
        CHECK(Status == WAIT_TIMEOUT && Elapsed >= 250, "stack +%lu: 300 ms wait returned %lx after %lu ms, error %lu",
              StackOffset, Status, Elapsed, GetLastError());

        /* Idle loops: a wait for input blocks until the run ends, so a handful of iterations is all there is */
        RunLoop("msgwait", StepMsgWait, FALSE, 5);
        RunLoop("msgwait-inf", StepMsgWaitInfinite, FALSE, 5);
        RunLoop("msgwait-event", StepMsgWaitEvent, FALSE, 5);
        RunLoop("msgwait-alert", StepMsgWaitAlertable, FALSE, 5);
        RunLoop("firefox", StepFirefox, FALSE, 5);
        RunLoop("chromium", StepChromium, FALSE, 5);
        /* Timer loops: one or two wakes per WM_TIMER */
        RunLoop("msgwait-timer", StepMsgWait, TRUE, 2 * RUN_MS / TIMER_MS + 5);
        RunLoop("firefox-timer", StepFirefox, TRUE, 2 * RUN_MS / TIMER_MS + 5);
        RunLoop("chromium-timer", StepChromium, TRUE, 2 * RUN_MS / TIMER_MS + 5);
    }
    StackOffset = 0;
    /* Calls without a wait structure: the WaitMessage and GetQueueStatus/GetInputState loops */
    RunLoop("waitmsg-timer", StepWaitMessage, TRUE, 2 * RUN_MS / TIMER_MS + 5);
    RunLoop("queuestatus", StepQueueStatus, FALSE, RUN_MS / 50 + 25);

    DestroyWindow(Window);
    printf("IDLE summary: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
