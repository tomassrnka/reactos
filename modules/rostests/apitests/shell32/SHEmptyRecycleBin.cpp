/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the confirmation box of SHEmptyRecycleBinW
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "shelltest.h"

/* The box must show up this long after the call starts, or the call is taken as hung */
#define BOX_TIMEOUT_MS 30000
/* Nothing in this test may take longer than this */
#define TEST_TIMEOUT_MS 150000

typedef struct _BOX_INFO
{
    UINT DefId;
    UINT FocusId;
    UINT Buttons;
    UINT ButtonIds[8];
    HWND hNo;
    WCHAR Title[128];
} BOX_INFO;

typedef struct _ROUND
{
    BOX_INFO Box;
    HANDLE hSeen;
    volatile LONG Returned;
} ROUND;

static HANDLE g_hTestDone;

/* Ends the process if the test hangs somewhere the round watcher cannot see */
static DWORD WINAPI TestWatchdog(PVOID Param)
{
    if (WaitForSingleObject(g_hTestDone, TEST_TIMEOUT_MS) == WAIT_TIMEOUT)
    {
        trace("The test hangs\n");
        TerminateProcess(GetCurrentProcess(), 1);
    }
    return 0;
}

static BOOL CALLBACK FindBoxProc(HWND hwnd, LPARAM lParam)
{
    DWORD Pid;
    WCHAR Class[16];

    GetWindowThreadProcessId(hwnd, &Pid);
    if (Pid != GetCurrentProcessId() || !IsWindowVisible(hwnd))
        return TRUE;
    if (!GetClassNameW(hwnd, Class, _countof(Class)) || lstrcmpiW(Class, L"#32770"))
        return TRUE;

    *(HWND *)lParam = hwnd;
    return FALSE;
}

static BOOL CALLBACK ButtonProc(HWND hwnd, LPARAM lParam)
{
    BOX_INFO *Box = (BOX_INFO *)lParam;
    WCHAR Class[16];

    if (!GetClassNameW(hwnd, Class, _countof(Class)) || lstrcmpiW(Class, L"Button"))
        return TRUE;

    if (Box->Buttons < _countof(Box->ButtonIds))
        Box->ButtonIds[Box->Buttons] = GetDlgCtrlID(hwnd);
    Box->Buttons++;
    if (GetDlgCtrlID(hwnd) == IDNO || GetDlgCtrlID(hwnd) == IDCANCEL)
        Box->hNo = hwnd;
    if ((GetWindowLongW(hwnd, GWL_STYLE) & BS_TYPEMASK) == BS_DEFPUSHBUTTON)
        Box->DefId = GetDlgCtrlID(hwnd);
    return TRUE;
}

/* Find the confirmation box of the main thread, describe it, and answer No */
static DWORD WINAPI WatchThread(PVOID Param)
{
    ROUND *Round = (ROUND *)Param;
    DWORD Start = GetTickCount();
    HWND hBox = NULL;
    GUITHREADINFO Gui = { sizeof(Gui) };

    while (!Round->Returned)
    {
        hBox = NULL;
        EnumWindows(FindBoxProc, (LPARAM)&hBox);
        if (hBox)
            break;
        if (GetTickCount() - Start > BOX_TIMEOUT_MS)
        {
            trace("No confirmation box and no return from SHEmptyRecycleBinW\n");
            TerminateProcess(GetCurrentProcess(), 1);
        }
        Sleep(50);
    }
    if (!hBox)
        return 1;

    Sleep(300);
    GetWindowTextW(hBox, Round->Box.Title, _countof(Round->Box.Title));
    EnumChildWindows(hBox, ButtonProc, (LPARAM)&Round->Box);
    if (GetGUIThreadInfo(GetWindowThreadProcessId(hBox, NULL), &Gui) && Gui.hwndFocus)
        Round->Box.FocusId = GetDlgCtrlID(Gui.hwndFocus);
    SetEvent(Round->hSeen);

    /* Answer No (or Cancel): the bin must stay as it is */
    if (Round->Box.hNo)
        PostMessageW(Round->Box.hNo, BM_CLICK, 0, 0);
    else
        PostMessageW(hBox, WM_CLOSE, 0, 0);

    for (Start = GetTickCount(); IsWindow(hBox); Sleep(50))
    {
        if (GetTickCount() - Start > 5000)
        {
            trace("The confirmation box did not close\n");
            TerminateProcess(GetCurrentProcess(), 1);
        }
    }
    return 0;
}

static UINT64 BinItems(PCWSTR Root)
{
    SHQUERYRBINFO Info = { sizeof(Info) };

    if (FAILED(SHQueryRecycleBinW(Root, &Info)))
        return (UINT64)-1;
    return Info.i64NumItems;
}

/* Recycle a new file of this test, whose name nothing else uses; FALSE if the bin did not take it */
static BOOL MakeBinItem(PCWSTR Root)
{
    WCHAR Dir[MAX_PATH], Path[MAX_PATH + 1];
    SHFILEOPSTRUCTW Op = { 0 };
    UINT64 Before = BinItems(Root);
    DWORD Length = GetTempPathW(_countof(Dir), Dir);
    HANDLE hFile;
    DWORD Written;

    if (!Length || Length > MAX_PATH - 14 || !GetTempFileNameW(Dir, L"ebt", 0, Path))
        return FALSE;
    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        WriteFile(hFile, "recycle me", 10, &Written, NULL);
        CloseHandle(hFile);
    }

    Path[lstrlenW(Path) + 1] = 0;
    Op.wFunc = FO_DELETE;
    Op.pFrom = Path;
    Op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (SHFileOperationW(&Op) || Op.fAnyOperationsAborted ||
        GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES)
    {
        DeleteFileW(Path);
        return FALSE;
    }

    /* A file that was deleted for good, not recycled, leaves the count as it was */
    return Before != (UINT64)-1 && BinItems(Root) == Before + 1;
}

static void RunRound(PCWSTR Root, PCSTR What)
{
    ROUND Round = { { 0 } };
    UINT64 Before, After;
    HANDLE hThread;
    HRESULT hr;
    DWORD Wait;

    Before = BinItems(Root);
    Round.hSeen = CreateEventW(NULL, TRUE, FALSE, NULL);
    hThread = CreateThread(NULL, 0, WatchThread, &Round, 0, NULL);
    if (!Round.hSeen || !hThread)
    {
        skip("%s: cannot start the watcher, error %lu\n", What, GetLastError());
        if (hThread)
        {
            InterlockedExchange(&Round.Returned, 1);
            WaitForSingleObject(hThread, INFINITE);
            CloseHandle(hThread);
        }
        if (Round.hSeen)
            CloseHandle(Round.hSeen);
        return;
    }

    hr = SHEmptyRecycleBinW(NULL, NULL, SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    InterlockedExchange(&Round.Returned, 1);
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
    Wait = WaitForSingleObject(Round.hSeen, 0);
    CloseHandle(Round.hSeen);

    ok(Wait == WAIT_OBJECT_0, "%s: no confirmation box appeared\n", What);
    if (Wait != WAIT_OBJECT_0)
        return;

    trace("%s: title '%ls', %u buttons (first ids %u %u), default %u, focus %u\n",
          What, Round.Box.Title, Round.Box.Buttons, Round.Box.ButtonIds[0], Round.Box.ButtonIds[1],
          Round.Box.DefId, Round.Box.FocusId);
    ok(Round.Box.DefId == IDYES, "%s: default button is %u, expected Yes (%u)\n", What, Round.Box.DefId, IDYES);
    ok(Round.Box.FocusId == IDYES, "%s: focused button is %u, expected Yes (%u)\n", What, Round.Box.FocusId, IDYES);
    ok(hr == S_OK, "%s: answering No returned %lx\n", What, hr);
    After = BinItems(Root);
    ok(After == Before, "%s: answering No changed the recycle bin from %I64u to %I64u items\n",
       What, Before, After);
}

START_TEST(SHEmptyRecycleBin)
{
    WCHAR Root[MAX_PATH];
    HANDLE hWatchdog;
    DWORD Length;

    Length = GetTempPathW(_countof(Root), Root);
    if (!Length || Length >= _countof(Root) || !PathStripToRootW(Root))
    {
        skip("The temporary path is unknown\n");
        return;
    }
    PathAddBackslashW(Root);

    g_hTestDone = CreateEventW(NULL, TRUE, FALSE, NULL);
    hWatchdog = g_hTestDone ? CreateThread(NULL, 0, TestWatchdog, NULL, 0, NULL) : NULL;
    if (!hWatchdog)
    {
        skip("Cannot start the watchdog, error %lu\n", GetLastError());
        if (g_hTestDone)
            CloseHandle(g_hTestDone);
        return;
    }

    if (BinItems(Root) == (UINT64)-1)
    {
        skip("The recycle bin of %ls cannot be queried\n", Root);
        goto Done;
    }
    if (!MakeBinItem(Root))
    {
        skip("The test file did not go to the recycle bin\n");
        goto Done;
    }
    RunRound(Root, "one item or more");

    /* The multiple-item box has its own text */
    if (MakeBinItem(Root))
        RunRound(Root, "several items");
    else
        skip("The second test file did not go to the recycle bin\n");

    /* The two small files of this test stay in the recycle bin: emptying it would also delete files
       that something else recycled meanwhile */

Done:
    SetEvent(g_hTestDone);
    WaitForSingleObject(hWatchdog, INFINITE);
    CloseHandle(hWatchdog);
    CloseHandle(g_hTestDone);
}
