/*
 * PROJECT:         ReactOS API tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Test for IsHungAppWindow and SMTO_ABORTIFHUNG / SMTO_NOTIMEOUTIFNOTHUNG
 *
 * A thread counts as hung when it has not read its message queue for about
 * five seconds and is not waiting for input. A thread starts the clock when
 * it is created. The tests run the victim threads side by side to keep the
 * run time near ten seconds.
 */

#include "precomp.h"

#include <stdlib.h>

#ifndef SMTO_ERRORONEXIT
#define SMTO_ERRORONEXIT 0x20
#endif

#define HUNG_MIN 4000
#define HUNG_MAX 6500
#define LAST_ERROR_MARK 0xDEAD

typedef enum
{
    V_STALL,
    V_NEVER_READS,
    V_IDLE_GETMESSAGE,
    V_IDLE_MSGWAIT,
    V_STALL_REFRESH,
    V_IDLE_MSGWAIT_ALERTABLE,
    V_WAKE_STALL,
    V_TIMEOUT_THEN_HEAP
} VKIND;

static HANDLE g_hHeap;

typedef enum
{
    API_MSGWAIT_ALLINPUT,
    API_PEEK_ALL,
    API_PEEK_NARROW,
    API_MSGWAIT_POSTMESSAGE,
    API_GETQUEUESTATUS
} REFRESHAPI;

typedef struct
{
    VKIND Kind;
    HWND hWnd;
    DWORD ThreadId;
    HANDLE hReady;
    HANDLE hRelease;
    HANDLE hThread;
    HANDLE hQuit;
    HANDLE hWake;
    BOOL Alertable;
    volatile DWORD LastRead;
    int Case;
    REFRESHAPI Api;
} VICTIM;

static
LRESULT
CALLBACK
HungTestProc(
    _In_ HWND hWnd,
    _In_ UINT message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    if (message == WM_APP + 3)
        ExitThread(0);
    if (message == WM_APP + 4)
    {
        DestroyWindow(hWnd);
        return 1;
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

static
void
PumpMessages(void)
{
    MSG msg;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&msg);
}

static
DWORD
WINAPI
VictimThread(
    _Inout_ PVOID Parameter)
{
    VICTIM *v = Parameter;
    MSG msg;

    v->hWnd = CreateWindowExW(0, L"HungTest", NULL, 0, 10, 10, 20, 20, NULL, NULL, 0, NULL);
    if (v->Kind == V_NEVER_READS)
    {
        v->LastRead = GetTickCount();
        SetEvent(v->hReady);
        WaitForSingleObject(v->hRelease, INFINITE);
        return 0;
    }

    PumpMessages();
    switch (v->Kind)
    {
    case V_STALL:
        v->LastRead = GetTickCount();
        SetEvent(v->hReady);
        WaitForSingleObject(v->hRelease, INFINITE);
        break;
    case V_IDLE_GETMESSAGE:
        v->LastRead = GetTickCount();
        SetEvent(v->hReady);
        while (GetMessageW(&msg, NULL, 0, 0) > 0)
            DispatchMessageW(&msg);
        break;
    case V_IDLE_MSGWAIT:
        v->LastRead = GetTickCount();
        SetEvent(v->hReady);
        while (MsgWaitForMultipleObjects(1, &v->hRelease, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0)
            PumpMessages();
        break;
    case V_IDLE_MSGWAIT_ALERTABLE:
        v->LastRead = GetTickCount();
        SetEvent(v->hReady);
        while (MsgWaitForMultipleObjectsEx(1, &v->hRelease, INFINITE, QS_ALLINPUT, MWMO_ALERTABLE) != WAIT_OBJECT_0)
            PumpMessages();
        break;
    case V_WAKE_STALL:
        /* An idle wait that ends by an object, with nothing read, then a stall */
        SetEvent(v->hReady);
        while (MsgWaitForMultipleObjectsEx(1, &v->hWake, INFINITE, QS_ALLINPUT,
                                           v->Alertable ? MWMO_ALERTABLE : 0) != WAIT_OBJECT_0)
            PumpMessages();
        v->LastRead = GetTickCount();
        WaitForSingleObject(v->hRelease, INFINITE);
        break;
    case V_TIMEOUT_THEN_HEAP:
        /* An idle wait that ends by its timeout, then a block on a lock: not waiting for input */
        SetEvent(v->hReady);
        while (MsgWaitForMultipleObjectsEx(0, NULL, 1000, QS_ALLINPUT, 0) != WAIT_TIMEOUT)
            PumpMessages();
        v->LastRead = GetTickCount();
        HeapFree(g_hHeap, 0, HeapAlloc(g_hHeap, 0, 16));
        break;
    case V_STALL_REFRESH:
        v->LastRead = GetTickCount();
        SetEvent(v->hReady);
        if (WaitForSingleObject(v->hRelease, 3000) == WAIT_TIMEOUT)
        {
            switch (v->Api)
            {
            case API_MSGWAIT_ALLINPUT:
                MsgWaitForMultipleObjects(0, NULL, FALSE, 0, QS_ALLINPUT);
                break;
            case API_PEEK_ALL:
                PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
                break;
            case API_PEEK_NARROW:
                PeekMessageW(&msg, NULL, WM_USER, WM_USER + 1, PM_NOREMOVE);
                break;
            case API_MSGWAIT_POSTMESSAGE:
                MsgWaitForMultipleObjects(0, NULL, FALSE, 0, QS_POSTMESSAGE);
                break;
            case API_GETQUEUESTATUS:
                GetQueueStatus(QS_ALLINPUT);
                break;
            }
            WaitForSingleObject(v->hRelease, INFINITE);
        }
        break;
    default:
        break;
    }

    PumpMessages();
    return 0;
}

static
VICTIM *
StartVictim(
    _In_ VKIND Kind,
    _In_ int Case,
    _In_ REFRESHAPI Api)
{
    VICTIM *v = calloc(1, sizeof(*v));

    v->Kind = Kind;
    v->Case = Case;
    v->Api = Api;
    v->Alertable = Api != 0;
    v->hReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    v->hRelease = CreateEventW(NULL, TRUE, FALSE, NULL);
    v->hWake = CreateEventW(NULL, TRUE, FALSE, NULL);
    v->hThread = CreateThread(NULL, 0, VictimThread, v, 0, &v->ThreadId);
    WaitForSingleObject(v->hReady, 10000);
    return v;
}

static
void
StopVictim(
    _In_ VICTIM *v)
{
    SetEvent(v->hRelease);
    PostThreadMessageW(v->ThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(v->hThread, 10000);
    CloseHandle(v->hThread);
    CloseHandle(v->hReady);
    CloseHandle(v->hRelease);
    CloseHandle(v->hWake);
    free(v);
}

static
void
SleepUntilAge(
    _In_ VICTIM *v,
    _In_ DWORD Age)
{
    DWORD Now;

    while ((Now = GetTickCount()) - v->LastRead < Age)
        Sleep(10);
}

/* Returns the age of the thread in ms when IsHungAppWindow first returned TRUE, or -1 */
static
int
WaitForHung(
    _In_ VICTIM *v,
    _In_ DWORD MaxAge)
{
    DWORD Age;

    for (;;)
    {
        Age = GetTickCount() - v->LastRead;
        if (IsHungAppWindow(v->hWnd))
            return (int)Age;
        if (Age > MaxAge)
            return -1;
        Sleep(20);
    }
}

typedef struct
{
    BOOL Ok;
    LRESULT Ret;
    DWORD Error;
    DWORD Elapsed;
} SENDRESULT;

static
void
DoSend(
    _In_ HWND hWnd,
    _In_ UINT Flags,
    _In_ UINT Timeout,
    _Out_ SENDRESULT *Result)
{
    DWORD_PTR dwResult = 0;
    DWORD Start;

    SetLastError(LAST_ERROR_MARK);
    Start = GetTickCount();
    Result->Ret = SendMessageTimeoutW(hWnd, WM_NULL, 0, 0, Flags, Timeout, &dwResult);
    Result->Elapsed = GetTickCount() - Start;
    Result->Error = GetLastError();
}

#define EXPECT_TIMED_OUT(r, name, minms) do { \
    ok((r).Ret == 0, "%s: ret = %Id\n", name, (r).Ret); \
    ok((r).Error == ERROR_TIMEOUT, "%s: error = %lu\n", name, (r).Error); \
    ok((r).Elapsed >= (minms), "%s: returned after %lu ms\n", name, (r).Elapsed); \
} while (0)

#define EXPECT_ABORTED(r, name) do { \
    ok((r).Ret == 0, "%s: ret = %Id\n", name, (r).Ret); \
    ok((r).Error == LAST_ERROR_MARK, "%s: error = %lu\n", name, (r).Error); \
    ok((r).Elapsed < 1000, "%s: returned after %lu ms\n", name, (r).Elapsed); \
} while (0)

static
DWORD
WINAPI
IdleCase(
    _In_ PVOID Parameter)
{
    VICTIM *v = Parameter;
    SENDRESULT r;
    const char *name = v->Kind == V_IDLE_GETMESSAGE ? "GetMessage" : "MsgWait";

    SleepUntilAge(v, 6300);
    ok(!IsHungAppWindow(v->hWnd), "%s: a thread waiting for input is hung\n", name);
    DoSend(v->hWnd, SMTO_ABORTIFHUNG, 3000, &r);
    ok(r.Ret != 0, "%s: SMTO_ABORTIFHUNG to a waiting thread failed, error %lu\n", name, r.Error);
    ok(r.Elapsed < 1000, "%s: returned after %lu ms\n", name, r.Elapsed);
    return 0;
}

static
DWORD
WINAPI
StallCase(
    _In_ PVOID Parameter)
{
    VICTIM *v = Parameter;
    SENDRESULT r;
    int Hung;

    SleepUntilAge(v, 1500);
    ok(!IsHungAppWindow(v->hWnd), "Stalled for 1.5 s, but hung\n");
    DoSend(v->hWnd, SMTO_ABORTIFHUNG, 200, &r);
    EXPECT_TIMED_OUT(r, "SMTO_ABORTIFHUNG before the hung time", 150);
    DoSend(v->hWnd, SMTO_NORMAL, 200, &r);
    EXPECT_TIMED_OUT(r, "SMTO_NORMAL before the hung time", 150);

    Hung = WaitForHung(v, 8000);
    ok(Hung >= HUNG_MIN && Hung <= HUNG_MAX, "Hung after %d ms\n", Hung);

    SleepUntilAge(v, 7000);
    DoSend(v->hWnd, SMTO_ABORTIFHUNG, 3000, &r);
    EXPECT_ABORTED(r, "SMTO_ABORTIFHUNG after the hung time");
    DoSend(v->hWnd, SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 3000, &r);
    EXPECT_ABORTED(r, "SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG after the hung time");
    DoSend(v->hWnd, SMTO_NOTIMEOUTIFNOTHUNG, 100, &r);
    EXPECT_TIMED_OUT(r, "SMTO_NOTIMEOUTIFNOTHUNG after the hung time", 80);
    ok(r.Elapsed < 1000, "SMTO_NOTIMEOUTIFNOTHUNG after the hung time returned after %lu ms\n", r.Elapsed);
    DoSend(v->hWnd, SMTO_NORMAL, 100, &r);
    EXPECT_TIMED_OUT(r, "SMTO_NORMAL after the hung time", 80);
    return 0;
}

static
DWORD
WINAPI
NoTimeoutIfNotHungCase(
    _In_ PVOID Parameter)
{
    VICTIM *v = Parameter;
    SENDRESULT r;

    /* The timeout does not apply while the thread is not hung: the call ends when it becomes hung */
    SleepUntilAge(v, 500);
    DoSend(v->hWnd, SMTO_NOTIMEOUTIFNOTHUNG, 100, &r);
    EXPECT_TIMED_OUT(r, "SMTO_NOTIMEOUTIFNOTHUNG before the hung time", 100);
    ok(r.Elapsed >= HUNG_MIN - 1000 && r.Elapsed <= HUNG_MAX, "SMTO_NOTIMEOUTIFNOTHUNG returned after %lu ms\n", r.Elapsed);
    return 0;
}

static
DWORD
WINAPI
NeverReadsCase(
    _In_ PVOID Parameter)
{
    VICTIM *v = Parameter;
    SENDRESULT r;
    int Hung;

    /* The clock of a new thread starts at its creation: it is not hung at first, and hung later */
    SleepUntilAge(v, 1000);
    ok(!IsHungAppWindow(v->hWnd), "A window of a new thread is hung after 1 s\n");
    DoSend(v->hWnd, SMTO_ABORTIFHUNG, 200, &r);
    EXPECT_TIMED_OUT(r, "SMTO_ABORTIFHUNG to a new thread", 150);

    Hung = WaitForHung(v, 8000);
    ok(Hung >= HUNG_MIN && Hung <= HUNG_MAX, "A thread that never reads is hung after %d ms\n", Hung);
    SleepUntilAge(v, 7000);
    DoSend(v->hWnd, SMTO_ABORTIFHUNG, 3000, &r);
    EXPECT_ABORTED(r, "SMTO_ABORTIFHUNG to a thread that never reads");
    return 0;
}

static
DWORD
WINAPI
RefreshCase(
    _In_ PVOID Parameter)
{
    VICTIM *v = Parameter;
    BOOL Refreshes = v->Api == API_MSGWAIT_ALLINPUT || v->Api == API_PEEK_ALL;
    int Hung;

    /* A call at 3 s that counts as reading the queue moves the hung time from 5 s to 8 s */
    Hung = WaitForHung(v, 12000);
    if (Refreshes)
        ok(Hung >= 7000 && Hung <= 9500, "API %d: hung after %d ms, expected about 8000\n", v->Api, Hung);
    else
        ok(Hung >= HUNG_MIN && Hung <= HUNG_MAX, "API %d: hung after %d ms, expected about 5000\n", v->Api, Hung);
    return 0;
}

static
void
TestOwnThread(void)
{
    HWND hWnd;

    hWnd = CreateWindowExW(0, L"HungTest", NULL, 0, 10, 10, 20, 20, NULL, NULL, 0, NULL);
    ok(hWnd != NULL, "CreateWindow failed\n");
    PumpMessages();
    Sleep(6300);
    ok(IsHungAppWindow(hWnd), "The window of the calling thread is not hung after 6 s without reading\n");
    PumpMessages();
    ok(!IsHungAppWindow(hWnd), "Still hung after PeekMessage\n");
    DestroyWindow(hWnd);
}

static
void
TestBasics(void)
{
    SENDRESULT r;
    DWORD Error;

    SetLastError(LAST_ERROR_MARK);
    ok(IsHungAppWindow(NULL) == FALSE, "IsHungAppWindow(NULL) is TRUE\n");
    Error = GetLastError();
    trace("IsHungAppWindow(NULL): last error %lu\n", Error);
    ok(IsHungAppWindow(GetDesktopWindow()) == FALSE, "The desktop window is hung\n");

    DoSend(GetDesktopWindow(), SMTO_ABORTIFHUNG, 5000, &r);
    ok(r.Ret != 0, "SMTO_ABORTIFHUNG to the desktop window failed, error %lu\n", r.Error);
}

static
void
TestFlags(void)
{
    static const struct
    {
        UINT Flags;
        BOOL Valid;
    } Tests[] =
    {
        { SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, TRUE },
        { 0x04, TRUE },
        { 0x10, TRUE },
        { SMTO_ERRORONEXIT, TRUE },
        { 0x80, FALSE },
        { 0x100, FALSE },
        { 0x80000000, FALSE },
    };
    HWND hWnd;
    SENDRESULT r;
    DWORD_PTR Result;
    LRESULT Ret;
    DWORD Error;
    int i;

    hWnd = CreateWindowExW(0, L"HungTest", NULL, 0, 10, 10, 20, 20, NULL, NULL, 0, NULL);
    ok(hWnd != NULL, "CreateWindow failed\n");
    for (i = 0; i < ARRAYSIZE(Tests); i++)
    {
        Result = 0xBEEF;
        SetLastError(LAST_ERROR_MARK);
        Ret = SendMessageTimeoutW(hWnd, WM_NULL, 0, 0, Tests[i].Flags, 1000, &Result);
        Error = GetLastError();
        if (Tests[i].Valid)
            ok(Ret != 0, "Flags 0x%x rejected, error %lu\n", Tests[i].Flags, Error);
        else
        {
            ok(Ret == 0, "Flags 0x%x accepted\n", Tests[i].Flags);
            ok(Error == ERROR_INVALID_PARAMETER, "Flags 0x%x: error = %lu\n", Tests[i].Flags, Error);
        }
    }
    DestroyWindow(hWnd);
    (void)r;
}

static
void
TestReceiverExits(void)
{
    VICTIM *v;
    SENDRESULT r;

    v = StartVictim(V_IDLE_GETMESSAGE, 0, 0);
    DoSend(v->hWnd, SMTO_ERRORONEXIT, 5000, &r);
    ok(r.Ret != 0, "SMTO_ERRORONEXIT to a pumping thread failed, error %lu\n", r.Error);
    SetLastError(LAST_ERROR_MARK);
    r.Ret = SendMessageTimeoutW(v->hWnd, WM_APP + 3, 0, 0, SMTO_ERRORONEXIT, 5000, NULL);
    r.Error = GetLastError();
    ok(r.Ret == 0, "SMTO_ERRORONEXIT: ret = %Id\n", r.Ret);
    ok(r.Error == ERROR_INVALID_WINDOW_HANDLE, "SMTO_ERRORONEXIT: error = %lu\n", r.Error);
    StopVictim(v);
}

static
void
TestReceiverDestroysWindow(void)
{
    /* Windows 7 and 10 return 1 here for every flag, SMTO_ERRORONEXIT included, in both paths */
    static const UINT Flags[] = { 0, SMTO_ERRORONEXIT, SMTO_BLOCK | SMTO_ERRORONEXIT };
    VICTIM *v;
    HWND hWnd;
    SENDRESULT r;
    int i;

    for (i = 0; i < ARRAYSIZE(Flags); i++)
    {
        v = StartVictim(V_IDLE_GETMESSAGE, 0, 0);
        SetLastError(LAST_ERROR_MARK);
        r.Ret = SendMessageTimeoutW(v->hWnd, WM_APP + 4, 0, 0, Flags[i], 3000, NULL);
        r.Error = GetLastError();
        ok(r.Ret == 1, "Cross-thread, handler destroys the window, flags 0x%x: ret = %Id\n", Flags[i], r.Ret);
        ok(!IsWindow(v->hWnd), "Window still exists\n");
        StopVictim(v);
    }

    for (i = 0; i < 2; i++)
    {
        hWnd = CreateWindowExW(0, L"HungTest", NULL, 0, 10, 10, 20, 20, NULL, NULL, 0, NULL);
        r.Ret = SendMessageTimeoutW(hWnd, WM_APP + 4, 0, 0, i ? SMTO_ERRORONEXIT : 0, 3000, NULL);
        ok(r.Ret == 1, "Same thread, handler destroys the window, flags %d: ret = %Id\n", i, r.Ret);
        ok(!IsWindow(hWnd), "Window still exists\n");
    }
}

/* An APC that blocks runs inside an alertable wait for input; the thread is not waiting then */
static VOID CALLBACK BlockingApc(ULONG_PTR Parameter)
{
    VICTIM *v = (VICTIM *)Parameter;

    v->LastRead = GetTickCount();
    WaitForSingleObject(v->hRelease, INFINITE);
}

static
DWORD
WINAPI
AlertableThread(
    _In_ PVOID Parameter)
{
    VICTIM *v = Parameter;

    v->hWnd = CreateWindowExW(0, L"HungTest", NULL, 0, 10, 10, 20, 20, NULL, NULL, 0, NULL);
    PumpMessages();
    SetEvent(v->hReady);
    while (MsgWaitForMultipleObjectsEx(1, &v->hQuit, INFINITE, QS_ALLINPUT, MWMO_ALERTABLE) != WAIT_OBJECT_0)
        PumpMessages();
    return 0;
}

static
DWORD
WINAPI
BlockedApcCase(
    _In_ PVOID Parameter)
{
    VICTIM *v;
    int Hung;

    /* Windows 7 keeps such a thread out of the hung state; Windows 10 and ReactOS do not */
    if (!is_reactos() && GetNTVersion() < _WIN32_WINNT_WIN8)
    {
        skip("Windows 7 does not report a thread that blocks in an APC as hung\n");
        return 0;
    }

    v = calloc(1, sizeof(*v));
    v->hReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    v->hRelease = CreateEventW(NULL, TRUE, FALSE, NULL);
    v->hQuit = CreateEventW(NULL, TRUE, FALSE, NULL);
    v->hThread = CreateThread(NULL, 0, AlertableThread, v, 0, &v->ThreadId);
    WaitForSingleObject(v->hReady, 10000);

    /* Idle in an alertable wait for more than the hung time: responsive */
    Sleep(6300);
    ok(!IsHungAppWindow(v->hWnd), "Hung while waiting in an alertable wait for input\n");

    /* The APC starts, blocks, and the clock starts at the APC */
    QueueUserAPC(BlockingApc, v->hThread, (ULONG_PTR)v);
    while (!v->LastRead)
        Sleep(5);
    Hung = WaitForHung(v, 10000);
    ok(Hung >= HUNG_MIN && Hung <= HUNG_MAX, "A thread blocked in an APC is hung after %d ms\n", Hung);

    SetEvent(v->hRelease);
    SetEvent(v->hQuit);
    WaitForSingleObject(v->hThread, 10000);
    CloseHandle(v->hThread);
    CloseHandle(v->hReady);
    CloseHandle(v->hRelease);
    CloseHandle(v->hQuit);
    free(v);
    return 0;
}

static
DWORD
WINAPI
WakeStallCase(
    _In_ PVOID Parameter)
{
    VICTIM *v = Parameter;
    int Hung;

    Sleep(6300);
    ok(!IsHungAppWindow(v->hWnd), "Alertable %d: hung while idle for 6.3 s\n", v->Alertable);
    SetEvent(v->hWake);
    while (!v->LastRead)
        Sleep(5);
    /* The wake counts as a read; with no read after it the thread is hung 5 s later */
    Hung = WaitForHung(v, 10000);
    ok(Hung >= HUNG_MIN && Hung <= HUNG_MAX, "Alertable %d: hung %d ms after the wake\n", v->Alertable, Hung);
    return 0;
}

static
DWORD
WINAPI
HeapCase(
    _In_ PVOID Parameter)
{
    VICTIM *v = Parameter;
    int Hung;

    while (!v->LastRead)
        Sleep(5);
    Hung = WaitForHung(v, 10000);
    ok(Hung >= HUNG_MIN && Hung <= HUNG_MAX, "Blocked on a lock after a wait timed out: hung after %d ms\n", Hung);
    return 0;
}

#define REFRESH_CASES 5
#define VICTIM_COUNT (5 + REFRESH_CASES + 4)
#define CHILD_COUNT 5
#define CASE_COUNT (VICTIM_COUNT + CHILD_COUNT + 1)

static
DWORD
WINAPI
ShowerThread(
    _In_ PVOID Parameter)
{
    VICTIM *v = Parameter;
    MSG msg;

    /* Activating a window ends the startup of the test process. SMTO_ABORTIFHUNG spares a starting process */
    v->hWnd = CreateWindowExW(0, L"HungTest", NULL, 0, 10, 10, 20, 20, NULL, NULL, 0, NULL);
    SetActiveWindow(v->hWnd);
    SetEvent(v->hReady);
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
        DispatchMessageW(&msg);
    DestroyWindow(v->hWnd);
    return 0;
}

static
void
WidenAscii(
    _Out_ WCHAR *Dest,
    _In_ PCSTR Src)
{
    while ((*Dest++ = (UCHAR)*Src++))
        ;
}

static
void
ChildProcess(
    _In_ PCSTR Mode,
    _In_ PCSTR Title)
{
    WCHAR wTitle[64];
    BOOL Visible = !strcmp(Mode, "visible");
    HWND hWnd;
    BOOL ShowNoActivate = !strcmp(Mode, "noactivate-then-swp");
    BOOL ShowNaThenShow = !strcmp(Mode, "showna-then-show");
    BOOL ShowNoActivatePlain = !strcmp(Mode, "noactivate-then-plain-swp");

    WidenAscii(wTitle, Title);
    RegisterSimpleClass(HungTestProc, L"HungTestChild");
    hWnd = CreateWindowExW(0, L"HungTestChild", wTitle, Visible ? (WS_OVERLAPPEDWINDOW | WS_VISIBLE) : 0,
                           20, 20, 100, 60, NULL, NULL, 0, NULL);
    PumpMessages();
    if (ShowNoActivate)
    {
        /* Shown without activation, then a request to activate the visible window */
        ShowWindow(hWnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    if (ShowNoActivatePlain)
    {
        /* A plain SetWindowPos without SWP_NOACTIVATE activates a visible window */
        ShowWindow(hWnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    }
    if (ShowNaThenShow)
    {
        /* SW_SHOW on a window that is already visible changes nothing */
        ShowWindow(hWnd, SW_SHOWNA);
        ShowWindow(hWnd, SW_SHOW);
    }
    Sleep(INFINITE);
    DestroyWindow(hWnd);
}

typedef struct
{
    PCSTR Mode;
    BOOL Visible;
} CHILDCASE;

static
DWORD
WINAPI
ChildCase(
    _In_ PVOID Parameter)
{
    CHILDCASE *c = Parameter;
    CHAR Path[MAX_PATH], CmdLine[MAX_PATH + 100], Title[64];
    WCHAR wTitle[64];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD Start;
    HWND hWnd = NULL;
    SENDRESULT r;

    GetModuleFileNameA(NULL, Path, ARRAYSIZE(Path));
    sprintf(Title, "HungChild_%s_%lu", c->Mode, GetCurrentProcessId());
    sprintf(CmdLine, "\"%s\" HungWindow child %s %s", Path, c->Mode, Title);
    WidenAscii(wTitle, Title);
    Start = GetTickCount();
    if (!CreateProcessA(NULL, CmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
        return 0;
    }

    while (!hWnd && GetTickCount() - Start < 15000)
    {
        hWnd = FindWindowW(L"HungTestChild", wTitle);
        if (!hWnd)
            Sleep(20);
    }
    ok(hWnd != NULL, "%s child: window not found\n", c->Mode);
    if (hWnd)
    {
        /* The process age counts from its creation. A process that shows no window is not judged for 30 s */
        while (GetTickCount() - Start < 7000)
            Sleep(10);
        ok(IsHungAppWindow(hWnd), "%s child: not hung after 7 s\n", c->Mode);
        DoSend(hWnd, SMTO_ABORTIFHUNG, 300, &r);
        if (c->Visible)
            EXPECT_ABORTED(r, "SMTO_ABORTIFHUNG to a process with a visible window");
        else
            EXPECT_TIMED_OUT(r, "SMTO_ABORTIFHUNG to a starting process", 250);
    }

    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

START_TEST(HungWindow)
{
    VICTIM *Victims[VICTIM_COUNT];
    HANDLE Threads[CASE_COUNT];
    VICTIM *Shower;
    static CHILDCASE Children[CHILD_COUNT] = { { "invisible", FALSE }, { "visible", TRUE },
                                               { "noactivate-then-swp", TRUE }, { "showna-then-show", FALSE },
                                               { "noactivate-then-plain-swp", TRUE } };
    char **argv;
    int argc, i, n = 0;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 5 && !strcmp(argv[2], "child"))
    {
        ChildProcess(argv[3], argv[4]);
        return;
    }

    RegisterSimpleClass(HungTestProc, L"HungTest");

    Shower = calloc(1, sizeof(*Shower));
    Shower->hReady = CreateEventW(NULL, TRUE, FALSE, NULL);
    Shower->hThread = CreateThread(NULL, 0, ShowerThread, Shower, 0, &Shower->ThreadId);
    WaitForSingleObject(Shower->hReady, 10000);

    TestBasics();
    TestFlags();
    TestReceiverExits();
    TestReceiverDestroysWindow();

    /* Held for the whole test: the victim that blocks on it must be hung 5 s after its last wait */
    g_hHeap = HeapCreate(0, 0, 0);
    HeapLock(g_hHeap);

    Victims[0] = StartVictim(V_IDLE_GETMESSAGE, 0, 0);
    Victims[1] = StartVictim(V_IDLE_MSGWAIT, 1, 0);
    Victims[2] = StartVictim(V_STALL, 2, 0);
    Victims[3] = StartVictim(V_STALL, 3, 0);
    Victims[4] = StartVictim(V_NEVER_READS, 4, 0);
    for (i = 0; i < REFRESH_CASES; i++)
        Victims[5 + i] = StartVictim(V_STALL_REFRESH, 5 + i, i);
    Victims[10] = StartVictim(V_IDLE_MSGWAIT_ALERTABLE, 10, 0);
    Victims[11] = StartVictim(V_WAKE_STALL, 11, 0);
    Victims[12] = StartVictim(V_WAKE_STALL, 12, 1);
    Victims[13] = StartVictim(V_TIMEOUT_THEN_HEAP, 13, 0);

    Threads[n++] = CreateThread(NULL, 0, IdleCase, Victims[0], 0, NULL);
    Threads[n++] = CreateThread(NULL, 0, IdleCase, Victims[1], 0, NULL);
    Threads[n++] = CreateThread(NULL, 0, StallCase, Victims[2], 0, NULL);
    Threads[n++] = CreateThread(NULL, 0, NoTimeoutIfNotHungCase, Victims[3], 0, NULL);
    Threads[n++] = CreateThread(NULL, 0, NeverReadsCase, Victims[4], 0, NULL);
    for (i = 0; i < REFRESH_CASES; i++)
        Threads[n++] = CreateThread(NULL, 0, RefreshCase, Victims[5 + i], 0, NULL);
    Threads[n++] = CreateThread(NULL, 0, IdleCase, Victims[10], 0, NULL);
    Threads[n++] = CreateThread(NULL, 0, WakeStallCase, Victims[11], 0, NULL);
    Threads[n++] = CreateThread(NULL, 0, WakeStallCase, Victims[12], 0, NULL);
    Threads[n++] = CreateThread(NULL, 0, HeapCase, Victims[13], 0, NULL);
    for (i = 0; i < CHILD_COUNT; i++)
        Threads[n++] = CreateThread(NULL, 0, ChildCase, &Children[i], 0, NULL);
    Threads[n++] = CreateThread(NULL, 0, BlockedApcCase, NULL, 0, NULL);

    TestOwnThread();

    WaitForMultipleObjects(n, Threads, TRUE, 60000);
    for (i = 0; i < n; i++)
        CloseHandle(Threads[i]);
    HeapUnlock(g_hHeap);
    for (i = 0; i < VICTIM_COUNT; i++)
        StopVictim(Victims[i]);
    HeapDestroy(g_hHeap);

    PostThreadMessageW(Shower->ThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(Shower->hThread, 10000);
    CloseHandle(Shower->hThread);
    CloseHandle(Shower->hReady);
    free(Shower);
}
