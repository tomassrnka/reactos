/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for AddClipboardFormatListener, RemoveClipboardFormatListener and WM_CLIPBOARDUPDATE
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2 (x64 and WOW64): the viewer chain gets WM_DRAWCLIPBOARD
 * (wParam = clipboard owner) first, then WM_CLIPBOARDUPDATE (wParam = number of formats)
 * is posted to every listener, newest registration first. A window of another process
 * is not registered and the call returns ERROR_ACCESS_DENIED as its result.
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <winuser.h>

typedef BOOL (WINAPI *PFN_LISTENER)(HWND);

static PFN_LISTENER pAdd, pRemove;
static HWND hViewer, hListener1, hListener2, hOwner, hThreadWnd, hNextViewer;
static DWORD dwThread2;
static HANDLE hThreadReady, hThreadDone;

#define MAX_EVENTS 32
static struct
{
    HWND hwnd;
    UINT msg;
    WPARAM wParam;
    LPARAM lParam;
    BOOL bUpdatePending;
    DWORD dwSequence;
} Events[MAX_EVENTS];
static LONG cEvents;
static BOOL bRender;

static void AddEvent(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, BOOL bPending)
{
    LONG i = InterlockedIncrement(&cEvents) - 1;

    if (i >= MAX_EVENTS)
        return;
    Events[i].hwnd = hwnd;
    Events[i].msg = msg;
    Events[i].wParam = wParam;
    Events[i].lParam = lParam;
    Events[i].bUpdatePending = bPending;
    Events[i].dwSequence = GetClipboardSequenceNumber();
}

static HGLOBAL MakeText(const char *Text)
{
    HGLOBAL Mem = GlobalAlloc(GMEM_MOVEABLE, strlen(Text) + 1);

    if (Mem)
    {
        strcpy(GlobalLock(Mem), Text);
        GlobalUnlock(Mem);
    }
    return Mem;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    MSG Msg;

    switch (msg)
    {
        case WM_DRAWCLIPBOARD:
            AddEvent(hwnd, msg, wParam, lParam,
                     PeekMessageA(&Msg, NULL, WM_CLIPBOARDUPDATE, WM_CLIPBOARDUPDATE, PM_NOREMOVE | PM_NOYIELD));
            if (hwnd == hViewer && hNextViewer)
                SendMessageA(hNextViewer, msg, wParam, lParam);
            return 0;
        case WM_CHANGECBCHAIN:
            if (hwnd == hViewer)
            {
                if ((HWND)wParam == hNextViewer)
                    hNextViewer = (HWND)lParam;
                else if (hNextViewer)
                    SendMessageA(hNextViewer, msg, wParam, lParam);
            }
            return 0;
        case WM_CLIPBOARDUPDATE:
            ok(InSendMessageEx(NULL) == ISMEX_NOSEND, "WM_CLIPBOARDUPDATE was sent\n");
            AddEvent(hwnd, msg, wParam, lParam, FALSE);
            return 0;
        case WM_RENDERFORMAT:
            AddEvent(hwnd, msg, wParam, lParam, FALSE);
            if (bRender)
            {
                DWORD dwSeq = GetClipboardSequenceNumber();
                ok(SetClipboardData((UINT)wParam, MakeText("rendered")) != NULL, "Render failed\n");
                ok(GetClipboardSequenceNumber() == dwSeq, "Rendering changed the sequence number\n");
            }
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void Pump(void)
{
    DWORD dwEnd = GetTickCount() + 150;
    MSG Msg;

    do
    {
        while (PeekMessageA(&Msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&Msg);
        MsgWaitForMultipleObjects(0, NULL, FALSE, 10, QS_ALLINPUT);
    } while ((LONG)(dwEnd - GetTickCount()) > 0);
}

static void ResetEvents(void)
{
    Pump();
    cEvents = 0;
}

static LONG CountEvents(HWND hwnd, UINT msg)
{
    LONG i, Count = 0;

    for (i = 0; i < cEvents && i < MAX_EVENTS; i++)
    {
        if (Events[i].hwnd == hwnd && Events[i].msg == msg)
            Count++;
    }
    return Count;
}

static LONG FindEvent(HWND hwnd, UINT msg)
{
    LONG i;

    for (i = 0; i < cEvents && i < MAX_EVENTS; i++)
    {
        if (Events[i].hwnd == hwnd && Events[i].msg == msg)
            return i;
    }
    return -1;
}

static HWND CreateTestWindow(HWND hParent, DWORD dwStyle)
{
    return CreateWindowExA(0, "ClipListenerTest", NULL, dwStyle, 0, 0, 10, 10,
                           hParent, NULL, GetModuleHandleA(NULL), NULL);
}

#define CMD_GETDATA (WM_APP + 1)

static DWORD WINAPI Thread2(PVOID Param)
{
    MSG Msg;

    hThreadWnd = CreateTestWindow(NULL, WS_OVERLAPPED);
    PeekMessageA(&Msg, NULL, 0, 0, PM_NOREMOVE);
    SetEvent(hThreadReady);
    while (GetMessageA(&Msg, NULL, 0, 0) > 0)
    {
        if (!Msg.hwnd && Msg.message == CMD_GETDATA)
        {
            if (OpenClipboard(NULL))
            {
                ok(GetClipboardData((UINT)Msg.wParam) != NULL, "GetClipboardData(%u) failed\n", (UINT)Msg.wParam);
                CloseClipboard();
            }
            SetEvent(hThreadDone);
            continue;
        }
        DispatchMessageA(&Msg);
    }
    DestroyWindow(hThreadWnd);
    return 0;
}

static void Thread2GetData(UINT Format)
{
    ResetEvent(hThreadDone);
    PostThreadMessageA(dwThread2, CMD_GETDATA, Format, 0);
    while (MsgWaitForMultipleObjects(1, &hThreadDone, FALSE, 5000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1)
    {
        MSG Msg;
        while (PeekMessageA(&Msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&Msg);
    }
}

static void ChangeClipboard(HWND hOpen, BOOL bEmpty, UINT Format, const char *Text)
{
    ok(OpenClipboard(hOpen), "OpenClipboard failed, error %lu\n", GetLastError());
    if (bEmpty)
        EmptyClipboard();
    if (Format)
        SetClipboardData(Format, Text ? MakeText(Text) : NULL);
    CloseClipboard();
}

#define TEST_CALL(Call, ExpectedRet, ExpectedError) do { \
        BOOL _Ret; DWORD _Error; \
        SetLastError(0xdeadbeef); \
        _Ret = (Call); \
        _Error = GetLastError(); \
        ok(_Ret == (ExpectedRet), "%s returned %d, expected %d\n", #Call, _Ret, (ExpectedRet)); \
        ok(_Error == (ExpectedError), "%s error %lu, expected %lu\n", #Call, _Error, (DWORD)(ExpectedError)); \
    } while (0)

static void TestRegistration(void)
{
    HWND hMessage, hChild;

    TEST_CALL(pAdd(NULL), FALSE, ERROR_INVALID_WINDOW_HANDLE);
    TEST_CALL(pAdd((HWND)(ULONG_PTR)0xdead), FALSE, ERROR_INVALID_WINDOW_HANDLE);
    TEST_CALL(pRemove(NULL), FALSE, ERROR_INVALID_WINDOW_HANDLE);
    TEST_CALL(pRemove((HWND)(ULONG_PTR)0xdead), FALSE, ERROR_INVALID_WINDOW_HANDLE);
    TEST_CALL(pRemove(hListener1), FALSE, ERROR_INVALID_PARAMETER);
    TEST_CALL(pAdd(hListener1), TRUE, 0xdeadbeef);
    TEST_CALL(pAdd(hListener1), FALSE, ERROR_INVALID_PARAMETER);
    TEST_CALL(pRemove(hListener1), TRUE, 0xdeadbeef);
    TEST_CALL(pRemove(hListener1), FALSE, ERROR_INVALID_PARAMETER);

    /* The desktop window belongs to another process: not registered, the result is ERROR_ACCESS_DENIED */
    TEST_CALL(pAdd(GetDesktopWindow()), ERROR_ACCESS_DENIED, 0xdeadbeef);
    TEST_CALL(pRemove(GetDesktopWindow()), ERROR_ACCESS_DENIED, 0xdeadbeef);

    /* Windows of another thread of this process, message-only and child windows */
    TEST_CALL(pAdd(hThreadWnd), TRUE, 0xdeadbeef);
    TEST_CALL(pRemove(hThreadWnd), TRUE, 0xdeadbeef);
    hMessage = CreateTestWindow(HWND_MESSAGE, 0);
    hChild = CreateTestWindow(hViewer, WS_CHILD);
    ok(hMessage && hChild, "CreateWindow failed\n");
    TEST_CALL(pAdd(hMessage), TRUE, 0xdeadbeef);
    TEST_CALL(pRemove(hMessage), TRUE, 0xdeadbeef);
    TEST_CALL(pAdd(hChild), TRUE, 0xdeadbeef);
    TEST_CALL(pRemove(hChild), TRUE, 0xdeadbeef);

    /* A destroyed listener is gone */
    TEST_CALL(pAdd(hMessage), TRUE, 0xdeadbeef);
    TEST_CALL(pAdd(hChild), TRUE, 0xdeadbeef);
    DestroyWindow(hMessage);
    DestroyWindow(hChild);
    TEST_CALL(pRemove(hMessage), FALSE, ERROR_INVALID_WINDOW_HANDLE);
    TEST_CALL(pRemove(hChild), FALSE, ERROR_INVALID_WINDOW_HANDLE);
    ChangeClipboard(NULL, TRUE, CF_TEXT, "after destroy");
    Pump();
}

static void TestMessages(void)
{
    DWORD dwSeq, dwSeq2;
    LONG iDraw, iUpdate1, iUpdate2;

    ok(pAdd(hListener1), "Add failed\n");

    /* SetClipboardViewer notifies the listeners too */
    ResetEvents();
    hNextViewer = SetClipboardViewer(hViewer);
    Pump();
    ok(CountEvents(hViewer, WM_DRAWCLIPBOARD) == 1, "No WM_DRAWCLIPBOARD for the new viewer\n");
    ok(CountEvents(hListener1, WM_CLIPBOARDUPDATE) == 1, "SetClipboardViewer: %ld WM_CLIPBOARDUPDATE\n",
       CountEvents(hListener1, WM_CLIPBOARDUPDATE));

    /* An invalid next window fails and leaves the viewer in place */
    TEST_CALL(ChangeClipboardChain(hViewer, (HWND)(ULONG_PTR)0xdead), FALSE, ERROR_INVALID_WINDOW_HANDLE);
    TEST_CALL(ChangeClipboardChain((HWND)(ULONG_PTR)0xdead, hNextViewer), FALSE, ERROR_INVALID_WINDOW_HANDLE);
    ok(GetClipboardViewer() == hViewer, "Viewer is %p, expected %p\n", GetClipboardViewer(), hViewer);

    ok(pAdd(hListener2), "Add failed\n");

    /* Open/Empty/Set/Close: the sequence number and the order of the notifications */
    ResetEvents();
    dwSeq = GetClipboardSequenceNumber();
    ok(OpenClipboard(hOwner), "OpenClipboard failed\n");
    EmptyClipboard();
    ok(GetClipboardSequenceNumber() == dwSeq + 1, "EmptyClipboard: sequence +%lu\n", GetClipboardSequenceNumber() - dwSeq);
    SetClipboardData(CF_UNICODETEXT, NULL);
    ok(GetClipboardSequenceNumber() == dwSeq + 2, "Delayed SetClipboardData: sequence +%lu\n", GetClipboardSequenceNumber() - dwSeq);
    SetClipboardData(CF_TEXT, MakeText("listener"));
    ok(GetClipboardSequenceNumber() == dwSeq + 3, "SetClipboardData: sequence +%lu\n", GetClipboardSequenceNumber() - dwSeq);
    ok(cEvents == 0, "%ld notifications before CloseClipboard\n", cEvents);
    CloseClipboard();
    /* CF_LOCALE and CF_OEMTEXT are synthesized: one step each */
    ok(GetClipboardSequenceNumber() == dwSeq + 5, "After CloseClipboard: sequence +%lu, expected +5\n", GetClipboardSequenceNumber() - dwSeq);
    iDraw = FindEvent(hViewer, WM_DRAWCLIPBOARD);
    ok(iDraw == 0, "WM_DRAWCLIPBOARD was not first (%ld)\n", iDraw);
    if (iDraw == 0)
    {
        ok((HWND)Events[0].wParam == hOwner, "WM_DRAWCLIPBOARD wParam %p, expected the owner %p\n", (HWND)Events[0].wParam, hOwner);
        ok(!Events[0].bUpdatePending, "WM_CLIPBOARDUPDATE was posted before WM_DRAWCLIPBOARD\n");
        ok(Events[0].dwSequence == dwSeq + 5, "Sequence at WM_DRAWCLIPBOARD +%lu\n", Events[0].dwSequence - dwSeq);
    }
    Pump();
    iUpdate1 = FindEvent(hListener1, WM_CLIPBOARDUPDATE);
    iUpdate2 = FindEvent(hListener2, WM_CLIPBOARDUPDATE);
    ok(iUpdate1 > 0 && iUpdate2 > 0, "Missing WM_CLIPBOARDUPDATE: %ld %ld\n", iUpdate1, iUpdate2);
    ok(iUpdate2 < iUpdate1, "The newest listener was not notified first\n");
    if (iUpdate1 > 0)
    {
        ok(Events[iUpdate1].wParam == 4, "WM_CLIPBOARDUPDATE wParam %Iu, expected 4 formats\n", Events[iUpdate1].wParam);
        ok(Events[iUpdate1].lParam == 0, "WM_CLIPBOARDUPDATE lParam %Id\n", Events[iUpdate1].lParam);
    }
    ok(cEvents == 3, "%ld notifications\n", cEvents);

    /* Nothing changed */
    ResetEvents();
    dwSeq = GetClipboardSequenceNumber();
    ChangeClipboard(NULL, FALSE, 0, NULL);
    Pump();
    ok(cEvents == 0, "Open/Close: %ld notifications\n", cEvents);
    ok(GetClipboardSequenceNumber() == dwSeq, "Open/Close changed the sequence number\n");

    /* Delayed rendering for another thread is not a change */
    ResetEvents();
    dwSeq = GetClipboardSequenceNumber();
    bRender = TRUE;
    Thread2GetData(CF_UNICODETEXT);
    bRender = FALSE;
    Pump();
    ok(CountEvents(hOwner, WM_RENDERFORMAT) == 1, "No WM_RENDERFORMAT\n");
    ok(CountEvents(hViewer, WM_DRAWCLIPBOARD) == 0, "Rendering notified the viewer\n");
    ok(CountEvents(hListener1, WM_CLIPBOARDUPDATE) == 0, "Rendering notified the listener\n");
    ok(GetClipboardSequenceNumber() == dwSeq, "Rendering: sequence +%lu\n", GetClipboardSequenceNumber() - dwSeq);

    /* A synthesized format is not a change either */
    ResetEvents();
    Thread2GetData(CF_OEMTEXT);
    Pump();
    ok(cEvents == 0, "Synthesized GetClipboardData: %ld notifications\n", cEvents);
    ok(GetClipboardSequenceNumber() == dwSeq, "Synthesized GetClipboardData: sequence +%lu\n", GetClipboardSequenceNumber() - dwSeq);

    /* Emptying without an owner window: wParam is the owner (none) and the count 0 */
    ResetEvents();
    dwSeq = GetClipboardSequenceNumber();
    ChangeClipboard(NULL, TRUE, 0, NULL);
    Pump();
    ok(GetClipboardSequenceNumber() == dwSeq + 1, "Empty: sequence +%lu\n", GetClipboardSequenceNumber() - dwSeq);
    iDraw = FindEvent(hViewer, WM_DRAWCLIPBOARD);
    iUpdate1 = FindEvent(hListener1, WM_CLIPBOARDUPDATE);
    ok(iDraw >= 0 && Events[iDraw].wParam == 0, "WM_DRAWCLIPBOARD after emptying: %ld\n", iDraw);
    ok(iUpdate1 >= 0 && Events[iUpdate1].wParam == 0, "WM_CLIPBOARDUPDATE after emptying: %ld\n", iUpdate1);

    /* Changes are not coalesced */
    ResetEvents();
    ChangeClipboard(NULL, TRUE, CF_TEXT, "one");
    ChangeClipboard(NULL, TRUE, CF_TEXT, "two");
    ChangeClipboard(NULL, TRUE, CF_TEXT, "three");
    Pump();
    ok(CountEvents(hViewer, WM_DRAWCLIPBOARD) == 3, "%ld WM_DRAWCLIPBOARD\n", CountEvents(hViewer, WM_DRAWCLIPBOARD));
    ok(CountEvents(hListener1, WM_CLIPBOARDUPDATE) == 3, "%ld WM_CLIPBOARDUPDATE for listener 1\n", CountEvents(hListener1, WM_CLIPBOARDUPDATE));
    ok(CountEvents(hListener2, WM_CLIPBOARDUPDATE) == 3, "%ld WM_CLIPBOARDUPDATE for listener 2\n", CountEvents(hListener2, WM_CLIPBOARDUPDATE));

    /* Adding a format to the clipboard without emptying it */
    ResetEvents();
    dwSeq = GetClipboardSequenceNumber();
    ChangeClipboard(NULL, FALSE, CF_WAVE, NULL);
    dwSeq2 = GetClipboardSequenceNumber();
    Pump();
    ok(dwSeq2 == dwSeq + 1, "Set without Empty: sequence +%lu\n", dwSeq2 - dwSeq);
    iUpdate1 = FindEvent(hListener1, WM_CLIPBOARDUPDATE);
    ok(iUpdate1 >= 0 && Events[iUpdate1].wParam == 5, "WM_CLIPBOARDUPDATE after adding a format: %ld\n", iUpdate1);

    /* ChangeClipboardChain does not notify the listeners */
    ResetEvents();
    ok(ChangeClipboardChain(hViewer, hNextViewer) || !hNextViewer, "ChangeClipboardChain failed\n");
    Pump();
    ok(CountEvents(hListener1, WM_CLIPBOARDUPDATE) == 0, "ChangeClipboardChain notified the listener\n");

    /* Removed listeners and listeners of another thread */
    ok(pRemove(hListener1), "Remove failed\n");
    ok(pAdd(hThreadWnd), "Add failed\n");
    ResetEvents();
    ChangeClipboard(NULL, TRUE, CF_TEXT, "removed");
    Pump();
    ok(CountEvents(hListener1, WM_CLIPBOARDUPDATE) == 0, "A removed listener was notified\n");
    ok(CountEvents(hListener2, WM_CLIPBOARDUPDATE) == 1, "Listener 2 was not notified\n");
    ok(pRemove(hListener2), "Remove failed\n");
    ok(pRemove(hThreadWnd), "Remove failed\n");
}

/* A window freed by a thread whose process window station is not the window's own one
   must not stay behind as the clipboard owner, open window or viewer of its window
   station (the freeing thread has no business with the clipboard of that station, but
   the next clipboard change from the original station would use a stale pointer) and
   must be dropped from the listeners. */
static void TestWindowFreedFromOtherWinsta(void)
{
    HWINSTA hOldWinsta = GetProcessWindowStation(), hNewWinsta;
    HWND hWndOwner, hWndView, hWndListener;
    BOOL bSwitched;

    hNewWinsta = CreateWindowStationA(NULL, 0, WINSTA_ALL_ACCESS, NULL);
    if (!hNewWinsta)
    {
        skip("CreateWindowStation failed, error %lu\n", GetLastError());
        return;
    }

    hWndOwner = CreateTestWindow(NULL, WS_OVERLAPPED);
    hWndView = CreateTestWindow(NULL, WS_OVERLAPPED);
    hWndListener = CreateTestWindow(NULL, WS_OVERLAPPED);
    ok(hWndOwner && hWndView && hWndListener, "CreateWindow failed\n");
    if (!hWndOwner || !hWndView || !hWndListener || !pAdd(hWndListener))
    {
        skip("Setup failed\n");
        goto cleanup;
    }

    /* The window becomes owner and viewer of the clipboard of the original station */
    SetClipboardViewer(hWndView);
    ChangeClipboard(hWndOwner, TRUE, CF_UNICODETEXT, "winsta");
    ok(GetClipboardOwner() == hWndOwner, "Owner is %p, expected %p\n", GetClipboardOwner(), hWndOwner);
    ok(GetClipboardViewer() == hWndView, "Viewer is %p, expected %p\n", GetClipboardViewer(), hWndView);

    /* Free them from another window station */
    bSwitched = SetProcessWindowStation(hNewWinsta);
    ok(bSwitched, "SetProcessWindowStation failed, error %lu\n", GetLastError());
    DestroyWindow(hWndOwner);
    DestroyWindow(hWndView);
    DestroyWindow(hWndListener);
    if (bSwitched)
        ok(SetProcessWindowStation(hOldWinsta), "SetProcessWindowStation failed, error %lu\n", GetLastError());
    if (!bSwitched)
    {
        skip("Cannot switch the window station\n");
        return;
    }

    ok(GetClipboardOwner() == NULL, "Owner is %p after it was destroyed\n", GetClipboardOwner());
    ok(GetClipboardViewer() == NULL, "Viewer is %p after it was destroyed\n", GetClipboardViewer());
    ok(GetOpenClipboardWindow() == NULL, "Open window is %p\n", GetOpenClipboardWindow());

    /* A change from the original station: this notifies viewer and listeners and sends
       WM_DESTROYCLIPBOARD to the previous owner; it must not touch freed windows */
    ResetEvents();
    ChangeClipboard(hOwner, TRUE, CF_UNICODETEXT, "after");
    ChangeClipboard(hOwner, TRUE, CF_UNICODETEXT, "after again");
    Pump();
    ok(GetClipboardOwner() == hOwner, "Owner is %p, expected %p\n", GetClipboardOwner(), hOwner);

cleanup:
    if (hOldWinsta && GetProcessWindowStation() != hOldWinsta)
        SetProcessWindowStation(hOldWinsta);
    if (GetClipboardViewer() == hWndView && hWndView)
        ChangeClipboardChain(hWndView, NULL);
    CloseWindowStation(hNewWinsta);
}

START_TEST(ClipboardFormatListener)
{
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    WNDCLASSA Class = {0};
    HANDLE hThread;
    HGLOBAL hSaved = NULL;
    SIZE_T cbSaved = 0;

    pAdd = (PFN_LISTENER)GetProcAddress(hUser32, "AddClipboardFormatListener");
    pRemove = (PFN_LISTENER)GetProcAddress(hUser32, "RemoveClipboardFormatListener");
    if (!pAdd || !pRemove)
    {
        skip("No clipboard format listeners\n");
        return;
    }

    Class.lpfnWndProc = WndProc;
    Class.hInstance = GetModuleHandleA(NULL);
    Class.lpszClassName = "ClipListenerTest";
    RegisterClassA(&Class);
    hViewer = CreateTestWindow(NULL, WS_OVERLAPPED);
    hListener1 = CreateTestWindow(NULL, WS_OVERLAPPED);
    hListener2 = CreateTestWindow(NULL, WS_OVERLAPPED);
    hOwner = CreateTestWindow(NULL, WS_OVERLAPPED);
    hThreadReady = CreateEventA(NULL, FALSE, FALSE, NULL);
    hThreadDone = CreateEventA(NULL, FALSE, FALSE, NULL);
    hThread = CreateThread(NULL, 0, Thread2, NULL, 0, &dwThread2);
    WaitForSingleObject(hThreadReady, 5000);
    if (!hViewer || !hListener1 || !hListener2 || !hOwner || !hThreadWnd)
    {
        skip("CreateWindow failed\n");
        return;
    }

    /* Keep the text of the clipboard */
    if (OpenClipboard(NULL))
    {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData && (cbSaved = GlobalSize(hData)) != 0 && (hSaved = GlobalAlloc(GMEM_MOVEABLE, cbSaved)) != NULL)
        {
            memcpy(GlobalLock(hSaved), GlobalLock(hData), cbSaved);
            GlobalUnlock(hData);
            GlobalUnlock(hSaved);
        }
        CloseClipboard();
    }

    TestRegistration();
    TestMessages();

    if (GetClipboardViewer() == hViewer)
        ChangeClipboardChain(hViewer, hNextViewer);
    TestWindowFreedFromOtherWinsta();
    if (OpenClipboard(NULL))
    {
        EmptyClipboard();
        if (hSaved)
            SetClipboardData(CF_UNICODETEXT, hSaved);
        CloseClipboard();
    }
    PostThreadMessageA(dwThread2, WM_QUIT, 0, 0);
    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    DestroyWindow(hViewer);
    DestroyWindow(hListener1);
    DestroyWindow(hListener2);
    DestroyWindow(hOwner);
}
