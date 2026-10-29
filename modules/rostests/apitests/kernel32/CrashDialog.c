/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the "has stopped working" dialog of unhandled exceptions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <winuser.h>

/*
 * A child crashes; the test finds the dialog by its title and answers it.
 * Windows draws its dialog without child windows, so there the test uses the
 * keyboard only; on ReactOS it also checks the controls of drwtsn32's dialog.
 */

#define WER_KEY         L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting"
#define WER_POLICY_KEY  L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Error Reporting"
#define LOCALDUMPS_KEY  WER_KEY L"\\LocalDumps"
#define LOCALDUMPS_APP  LOCALDUMPS_KEY L"\\kernel32_apitest.exe"
#define AEDEBUG_KEY     L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AeDebug"

/* The title is the file description of the program */
#define PROGRAM_NAME    L"Kernel32 Regtests"

/* Controls of drwtsn32's dialog */
#define IDC_CRASH_INSTRUCTION       201
#define IDC_CRASH_CONTENT           202
#define IDC_CRASH_EXPANDER          203
#define IDC_CRASH_EXPANDER_LABEL    204
#define IDC_CRASH_DETAILS           205
#define IDC_CRASH_DEBUG             206
#define IDC_CRASH_CLOSE             207

#define DEBUGGER_EXIT_CODE 0xDEB0DEB0
/* The child could not load uhexit_dll.dll: no dialog, and the wrong exit code */
#define NO_DLL_EXIT_CODE   0x0BADD11

static BOOL IsReactOS;
static REGSAM RegistryView;
static WCHAR DumpFolder[MAX_PATH];
static WCHAR MarkerPath[MAX_PATH];
static volatile LONG *NullPointer;

/* Child side ****************************************************************/

static LONG WINAPI FilterFault(PEXCEPTION_POINTERS ExceptionInfo)
{
    NullPointer[4] = 2;
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Started as the AeDebug debugger: end the crashed process with a code of its own and leave a mark */
static VOID FakeDebugger(PCSTR ProcessId, PCSTR Event)
{
    DWORD Pid = strtoul(ProcessId, NULL, 0);
    HANDLE hProcess, hFile;
    CHAR Line[64];
    DWORD Written;

    /* The mark first: the test looks for it as soon as the process has ended */
    hFile = CreateFileW(MarkerPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        StringCbPrintfA(Line, sizeof(Line), "%lu", Pid);
        WriteFile(hFile, Line, (DWORD)strlen(Line), &Written, NULL);
        CloseHandle(hFile);
    }
    hProcess = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, Pid);
    if (hProcess)
    {
        TerminateProcess(hProcess, DEBUGGER_EXIT_CODE);
        WaitForSingleObject(hProcess, 10000);
        CloseHandle(hProcess);
    }
    SetEvent((HANDLE)(ULONG_PTR)_strtoui64(Event, NULL, 0));
    ExitProcess(0);
}

static VOID RunChild(PCSTR Mode)
{
    /* Its DLL_PROCESS_DETACH leaves a marker: a crashed process must not run it */
    if (!LoadLibraryW(L"uhexit_dll.dll"))
        ExitProcess(NO_DLL_EXIT_CODE);
    /* The test framework turns off the fault error box */
    SetErrorMode(0);
    if (!strcmp(Mode, "nogp"))
        SetErrorMode(SEM_NOGPFAULTERRORBOX);
    else if (!strcmp(Mode, "filtfault"))
        SetUnhandledExceptionFilter(FilterFault);
    *NullPointer = 1;
    ExitProcess(0x5EE);
}

/* Parent side ***************************************************************/

typedef struct _SAVED_VALUE
{
    HKEY Root;
    PCWSTR Key;
    PCWSTR Name;
    REGSAM View;
    BOOL Had;
    DWORD Type;
    PBYTE Data;
    DWORD Size;
} SAVED_VALUE;

static SAVED_VALUE SavedValues[] =
{
    { HKEY_LOCAL_MACHINE, WER_KEY, L"DontShowUI" },
    { HKEY_LOCAL_MACHINE, WER_POLICY_KEY, L"DontShowUI" },
    { HKEY_CURRENT_USER, WER_KEY, L"DontShowUI" },
    { HKEY_LOCAL_MACHINE, AEDEBUG_KEY, L"Debugger" },
    { HKEY_LOCAL_MACHINE, AEDEBUG_KEY, L"Auto" },
};

/* FALSE when the value exists but could not be saved completely */
static BOOL SaveValue(SAVED_VALUE *Value)
{
    HKEY hKey;
    LONG Error;

    Value->Had = FALSE;
    Error = RegOpenKeyExW(Value->Root, Value->Key, 0, KEY_QUERY_VALUE | Value->View, &hKey);
    if (Error == ERROR_FILE_NOT_FOUND)
        return TRUE;
    if (Error != ERROR_SUCCESS)
        return FALSE;
    Value->Size = 0;
    Error = RegQueryValueExW(hKey, Value->Name, NULL, &Value->Type, NULL, &Value->Size);
    if (Error == ERROR_SUCCESS)
    {
        Value->Data = HeapAlloc(GetProcessHeap(), 0, Value->Size + sizeof(WCHAR));
        Error = Value->Data ? RegQueryValueExW(hKey, Value->Name, NULL, &Value->Type, Value->Data, &Value->Size)
                            : ERROR_OUTOFMEMORY;
        Value->Had = (Error == ERROR_SUCCESS);
    }
    RegCloseKey(hKey);
    return Value->Had || Error == ERROR_FILE_NOT_FOUND;
}

static VOID SetValue(HKEY Root, PCWSTR Key, PCWSTR Name, REGSAM View, DWORD Type, const VOID *Data, DWORD Size)
{
    HKEY hKey;
    LONG Error = RegCreateKeyExW(Root, Key, 0, NULL, 0, KEY_SET_VALUE | View, NULL, &hKey, NULL);
    ok(Error == ERROR_SUCCESS, "Cannot create %ls: %ld\n", Key, Error);
    if (Error != ERROR_SUCCESS)
        return;
    Error = RegSetValueExW(hKey, Name, 0, Type, Data, Size);
    ok(Error == ERROR_SUCCESS, "Cannot set %ls: %ld\n", Name, Error);
    RegCloseKey(hKey);
}

static VOID DeleteValue(HKEY Root, PCWSTR Key, PCWSTR Name, REGSAM View)
{
    HKEY hKey;
    if (RegOpenKeyExW(Root, Key, 0, KEY_SET_VALUE | View, &hKey) == ERROR_SUCCESS)
    {
        RegDeleteValueW(hKey, Name);
        RegCloseKey(hKey);
    }
}

static VOID DeleteKey(HKEY Root, PCWSTR Key)
{
    typedef LONG (WINAPI *PFN_REGDELETEKEYEXW)(HKEY, LPCWSTR, REGSAM, DWORD);
    PFN_REGDELETEKEYEXW pRegDeleteKeyExW;

    pRegDeleteKeyExW = (PFN_REGDELETEKEYEXW)GetProcAddress(GetModuleHandleW(L"advapi32.dll"), "RegDeleteKeyExW");
    if (RegistryView && pRegDeleteKeyExW)
        pRegDeleteKeyExW(Root, Key, RegistryView, 0);
    else
        RegDeleteKeyW(Root, Key);
}

static VOID RestoreValue(const SAVED_VALUE *Value)
{
    if (Value->Had)
        SetValue(Value->Root, Value->Key, Value->Name, Value->View, Value->Type, Value->Data, Value->Size);
    else
        DeleteValue(Value->Root, Value->Key, Value->Name, Value->View);
}

static VOID SetDontShowUI(HKEY Root, PCWSTR Key, DWORD Value)
{
    SetValue(Root, Key, L"DontShowUI", RegistryView, REG_DWORD, &Value, sizeof(Value));
}

/* Auto: NULL for no debugger */
static VOID SetDebugger(PCWSTR Auto)
{
    WCHAR FileName[MAX_PATH], Command[MAX_PATH + 64];

    if (!Auto)
    {
        DeleteValue(HKEY_LOCAL_MACHINE, AEDEBUG_KEY, L"Debugger", 0);
        DeleteValue(HKEY_LOCAL_MACHINE, AEDEBUG_KEY, L"Auto", 0);
        return;
    }
    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCchPrintfW(Command, _countof(Command), L"\"%s\" CrashDialog fakedbg %%ld %%ld", FileName);
    SetValue(HKEY_LOCAL_MACHINE, AEDEBUG_KEY, L"Debugger", 0, REG_SZ, Command,
             (DWORD)(wcslen(Command) + 1) * sizeof(WCHAR));
    SetValue(HKEY_LOCAL_MACHINE, AEDEBUG_KEY, L"Auto", 0, REG_SZ, Auto, (DWORD)(wcslen(Auto) + 1) * sizeof(WCHAR));
}

static HANDLE StartChild(PCSTR Mode, PDWORD ProcessId)
{
    CHAR FileName[MAX_PATH], CommandLine[MAX_PATH + 64];
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;

    GetModuleFileNameA(NULL, FileName, _countof(FileName));
    StringCbPrintfA(CommandLine, sizeof(CommandLine), "\"%s\" CrashDialog %s", FileName, Mode);
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    if (!CreateProcessA(FileName, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
        return NULL;
    }
    CloseHandle(ProcessInfo.hThread);
    *ProcessId = ProcessInfo.dwProcessId;
    return ProcessInfo.hProcess;
}

typedef struct _FIND_DIALOG
{
    DWORD ChildId;
    HWND Found;
} FIND_DIALOG;

/* Dialogs of earlier cases that are still closing do not count */
static HWND OldDialogs[16];
static ULONG OldDialogCount;

static BOOL CALLBACK FindDialogProc(HWND hWnd, LPARAM lParam)
{
    FIND_DIALOG *Find = (FIND_DIALOG *)lParam;
    WCHAR Title[64], Class[16];
    DWORD Pid = 0;
    ULONG i;

    if (!IsWindowVisible(hWnd) ||
        !GetWindowTextW(hWnd, Title, _countof(Title)) || wcscmp(Title, PROGRAM_NAME) ||
        !GetClassNameW(hWnd, Class, _countof(Class)) || wcscmp(Class, L"#32770"))
    {
        return TRUE;
    }
    /* Not a window of the crashed program itself */
    GetWindowThreadProcessId(hWnd, &Pid);
    if (Pid == Find->ChildId)
        return TRUE;
    for (i = 0; i < OldDialogCount; i++)
    {
        if (OldDialogs[i] == hWnd)
            return TRUE;
    }
    if (!Find->ChildId && OldDialogCount < _countof(OldDialogs))
    {
        /* Collecting the old dialogs */
        OldDialogs[OldDialogCount++] = hWnd;
        return TRUE;
    }
    Find->Found = hWnd;
    return FALSE;
}

static VOID RememberOldDialogs(VOID)
{
    FIND_DIALOG Find = { 0, NULL };

    OldDialogCount = 0;
    EnumWindows(FindDialogProc, (LPARAM)&Find);
}

static BOOL CALLBACK CloseHardErrorProc(HWND hWnd, LPARAM lParam)
{
    WCHAR Title[64];

    if (IsWindowVisible(hWnd) && GetWindowTextW(hWnd, Title, _countof(Title)) &&
        !_wcsicmp(Title, L"kernel32_apitest.exe - Application Error"))
    {
        PostMessageW(hWnd, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

/* A case that failed may have left the hard error box of the fallback */
static VOID CloseHardErrorBoxes(VOID)
{
    EnumWindows(CloseHardErrorProc, 0);
}

/* The dialog for the child, or NULL when the child ended or Wait milliseconds passed first */
static HWND WaitForDialog(HANDLE hProcess, DWORD ProcessId, DWORD Wait)
{
    FIND_DIALOG Find;
    DWORD Start = GetTickCount();

    do
    {
        Find.ChildId = ProcessId;
        Find.Found = NULL;
        EnumWindows(FindDialogProc, (LPARAM)&Find);
        if (Find.Found)
            return Find.Found;
        if (WaitForSingleObject(hProcess, 100) == WAIT_OBJECT_0)
            return NULL;
    } while (GetTickCount() - Start < Wait);
    return NULL;
}

static DWORD WaitForExit(HANDLE hProcess, DWORD Wait)
{
    DWORD Code = 0;

    if (WaitForSingleObject(hProcess, Wait) != WAIT_OBJECT_0)
    {
        TerminateProcess(hProcess, 0xDEAD);
        WaitForSingleObject(hProcess, 5000);
    }
    GetExitCodeProcess(hProcess, &Code);
    return Code;
}

/* A key press into the dialog, as the user would give it */
static BOOL PressKey(HWND hDlg, WORD Key)
{
    HWND hForeground = GetForegroundWindow();
    DWORD ForegroundThread = hForeground ? GetWindowThreadProcessId(hForeground, NULL) : 0;
    DWORD Thread = GetCurrentThreadId();
    int Try;

    for (Try = 0; Try < 20 && GetForegroundWindow() != hDlg; Try++)
    {
        if (ForegroundThread && ForegroundThread != Thread)
            AttachThreadInput(Thread, ForegroundThread, TRUE);
        /* Windows lets a process take the foreground after it had the last input */
        if (Try)
        {
            keybd_event(VK_MENU, 0, 0, 0);
            keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        }
        SetForegroundWindow(hDlg);
        if (ForegroundThread && ForegroundThread != Thread)
            AttachThreadInput(Thread, ForegroundThread, FALSE);
        Sleep(100);
    }
    if (GetForegroundWindow() != hDlg)
    {
        skip("The dialog did not get the foreground\n");
        return FALSE;
    }
    keybd_event((BYTE)Key, 0, 0, 0);
    keybd_event((BYTE)Key, 0, KEYEVENTF_KEYUP, 0);
    return TRUE;
}

/* A hung dialog must not hang the test */
static LRESULT SendBounded(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    DWORD_PTR Result = 0;
    if (!SendMessageTimeoutW(hWnd, Msg, wParam, lParam, SMTO_ABORTIFHUNG, 5000, &Result))
        return 0;
    return (LRESULT)Result;
}

/* GetWindowText does not ask a control of another process (an edit control) for its text */
static VOID GetText(HWND hDlg, int Id, PWSTR Text, int Count)
{
    Text[0] = UNICODE_NULL;
    SendBounded(GetDlgItem(hDlg, Id), WM_GETTEXT, Count, (LPARAM)Text);
}

static BOOL FileExists(PCWSTR Path)
{
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

/* Written by uhexit_dll.dll when the child runs DLL detach */
static BOOL DetachRan(DWORD ProcessId)
{
    WCHAR Path[MAX_PATH], Temp[MAX_PATH];
    BOOL Ran;

    GetTempPathW(_countof(Temp), Temp);
    StringCchPrintfW(Path, _countof(Path), L"%suhexit.%lu.txt", Temp, ProcessId);
    Ran = FileExists(Path);
    DeleteFileW(Path);
    return Ran;
}

static VOID DumpPath(PWSTR Path, DWORD ProcessId)
{
    StringCchPrintfW(Path, MAX_PATH, L"%s\\kernel32_apitest.exe.%lu.dmp", DumpFolder, ProcessId);
}

/* Process ids are reused, and so are the file names: start each case with an empty folder */
static VOID EmptyFolder(VOID)
{
    WCHAR Pattern[MAX_PATH], Path[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE hFind;

    StringCchPrintfW(Pattern, _countof(Pattern), L"%s\\kernel32_apitest.exe.*", DumpFolder);
    hFind = FindFirstFileW(Pattern, &FindData);
    if (hFind == INVALID_HANDLE_VALUE)
        return;
    do
    {
        StringCchPrintfW(Path, _countof(Path), L"%s\\%s", DumpFolder, FindData.cFileName);
        DeleteFileW(Path);
    } while (FindNextFileW(hFind, &FindData));
    FindClose(hFind);
}

static VOID DeleteReport(DWORD ProcessId)
{
    WCHAR Path[MAX_PATH];

    DumpPath(Path, ProcessId);
    DeleteFileW(Path);
    Path[wcslen(Path) - 4] = UNICODE_NULL;
    StringCchCatW(Path, _countof(Path), L".txt");
    DeleteFileW(Path);
}

/* What the user sees on ReactOS before answering */
static VOID CheckControls(HWND hDlg, DWORD ProcessId, BOOL Debug)
{
    WCHAR Text[2048], Expected[MAX_PATH + 32];
    HWND hDetails = GetDlgItem(hDlg, IDC_CRASH_DETAILS);

    GetText(hDlg, IDC_CRASH_INSTRUCTION, Text, _countof(Text));
    ok(!wcscmp(Text, PROGRAM_NAME L" has stopped working"), "Instruction '%ls'\n", Text);
    GetText(hDlg, IDC_CRASH_CONTENT, Text, _countof(Text));
    ok(!wcscmp(Text, L"A problem caused the program to stop working correctly. Please close the program."),
       "Content '%ls'\n", Text);
    GetText(hDlg, IDC_CRASH_CLOSE, Text, _countof(Text));
    ok(!wcscmp(Text, L"&Close program"), "Close button '%ls'\n", Text);
    ok(IsWindowVisible(GetDlgItem(hDlg, IDC_CRASH_CLOSE)), "Close button hidden\n");
    ok(IsWindowVisible(GetDlgItem(hDlg, IDC_CRASH_DEBUG)) == Debug, "Debug button %s\n", Debug ? "hidden" : "shown");
    ok(LOWORD(SendBounded(hDlg, DM_GETDEFID, 0, 0)) == IDC_CRASH_CLOSE,
       "Default button %u\n", LOWORD(SendBounded(hDlg, DM_GETDEFID, 0, 0)));

    /* The problem details open below */
    GetText(hDlg, IDC_CRASH_EXPANDER_LABEL, Text, _countof(Text));
    ok(!wcscmp(Text, L"View problem details"), "Expander '%ls'\n", Text);
    ok(hDetails && !IsWindowVisible(hDetails), "Details shown before the expander is used\n");
    SendBounded(GetDlgItem(hDlg, IDC_CRASH_EXPANDER), BM_CLICK, 0, 0);
    ok(IsWindowVisible(hDetails), "Details hidden after the expander is used\n");
    GetText(hDlg, IDC_CRASH_EXPANDER_LABEL, Text, _countof(Text));
    ok(!wcscmp(Text, L"Hide problem details"), "Expander '%ls'\n", Text);

    GetText(hDlg, IDC_CRASH_DETAILS, Text, _countof(Text));
    ok(!wcsncmp(Text, L"Problem signature:\r\n  Problem Event Name:\tAPPCRASH\r\n", 51), "Details '%.60ls'\n", Text);
    ok(wcsstr(Text, L"  Application Name:\tkernel32_apitest.exe\r\n") != NULL, "No application name\n");
    ok(wcsstr(Text, L"  Fault Module Name:\tkernel32_apitest.exe\r\n") != NULL, "No module name\n");
    ok(wcsstr(Text, L"  Exception Code:\tc0000005\r\n") != NULL, "No exception code\n");
    ok(wcsstr(Text, L"  Exception Offset:\t") != NULL, "No offset\n");
    ok(wcsstr(Text, L"  OS Version:\t") != NULL, "No system version\n");
    ok(wcsstr(Text, L"Files that help describe the problem:") != NULL, "No files\n");
    DumpPath(Expected, ProcessId);
    ok(wcsstr(Text, Expected) != NULL, "Dump path %ls not in the details\n", Expected);

    SendBounded(GetDlgItem(hDlg, IDC_CRASH_EXPANDER), BM_CLICK, 0, 0);
    ok(!IsWindowVisible(hDetails), "Details shown after the expander is used twice\n");
}

typedef enum _ANSWER
{
    AnswerCloseButton,      /* ReactOS only: the Close program button */
    AnswerDebugButton,      /* ReactOS only: the Debug button */
    AnswerEnter,
    AnswerEscape,
    AnswerEndProcess,       /* ReactOS only: the program ends meanwhile */
} ANSWER;

static VOID TestDialog_(PCSTR Mode, BOOL Debug, ANSWER Answer, DWORD ExpectedCode, int Line)
{
    DWORD ProcessId, Code;
    HANDLE hProcess;
    HWND hDlg;

    DeleteFileW(MarkerPath);
    EmptyFolder();
    RememberOldDialogs();
    hProcess = StartChild(Mode, &ProcessId);
    if (!hProcess)
        return;

    hDlg = WaitForDialog(hProcess, ProcessId, 30000);
    ok_(__FILE__, Line)(hDlg != NULL, "%s: no dialog\n", Mode);
    if (!hDlg)
    {
        WaitForExit(hProcess, 0);
        CloseHandle(hProcess);
        CloseHardErrorBoxes();
        return;
    }
    ok_(__FILE__, Line)(WaitForSingleObject(hProcess, 0) == WAIT_TIMEOUT, "%s: the program ended before the answer\n", Mode);

    /* The report is written before the dialog is shown */
    if (IsReactOS)
    {
        WCHAR Path[MAX_PATH];
        DumpPath(Path, ProcessId);
        ok_(__FILE__, Line)(FileExists(Path), "%s: no dump before the answer\n", Mode);
        CheckControls(hDlg, ProcessId, Debug);
    }

    switch (Answer)
    {
        case AnswerCloseButton:
            PostMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_CRASH_CLOSE, BN_CLICKED),
                         (LPARAM)GetDlgItem(hDlg, IDC_CRASH_CLOSE));
            break;
        case AnswerDebugButton:
            PostMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDC_CRASH_DEBUG, BN_CLICKED),
                         (LPARAM)GetDlgItem(hDlg, IDC_CRASH_DEBUG));
            break;
        case AnswerEnter:
            PressKey(hDlg, VK_RETURN);
            break;
        case AnswerEscape:
            PressKey(hDlg, VK_ESCAPE);
            break;
        case AnswerEndProcess:
            TerminateProcess(hProcess, 0x1234);
            break;
    }

    Code = WaitForExit(hProcess, 20000);
    ok_(__FILE__, Line)(Code == ExpectedCode, "%s: exit code 0x%lx, expected 0x%lx\n", Mode, Code, ExpectedCode);
    ok_(__FILE__, Line)(FileExists(MarkerPath) == (ExpectedCode == DEBUGGER_EXIT_CODE),
                        "%s: debugger %s\n", Mode, FileExists(MarkerPath) ? "started" : "not started");
    ok_(__FILE__, Line)(!DetachRan(ProcessId), "%s: the crashed program ran DLL detach\n", Mode);

    /* The dialog goes away with its answer, or with the program */
    if (IsReactOS)
    {
        DWORD Start = GetTickCount();
        while (IsWindow(hDlg) && GetTickCount() - Start < 5000)
            Sleep(100);
        ok_(__FILE__, Line)(!IsWindow(hDlg), "%s: the dialog stayed\n", Mode);
    }
    if (IsWindow(hDlg))
        PostMessageW(hDlg, WM_CLOSE, 0, 0);

    CloseHandle(hProcess);
    DeleteReport(ProcessId);
}
#define TestDialog(Mode, Debug, Answer, Code) TestDialog_(Mode, Debug, Answer, Code, __LINE__)

/* No dialog: the program ends at once with ExpectedCode; Reported tells whether a dump is written */
static VOID TestNoDialog_(PCSTR Mode, DWORD ExpectedCode, BOOL Reported, int Line)
{
    WCHAR Path[MAX_PATH];
    DWORD ProcessId, Code;
    HANDLE hProcess;
    HWND hDlg;

    DeleteFileW(MarkerPath);
    EmptyFolder();
    RememberOldDialogs();
    hProcess = StartChild(Mode, &ProcessId);
    if (!hProcess)
        return;

    hDlg = WaitForDialog(hProcess, ProcessId, 15000);
    ok_(__FILE__, Line)(hDlg == NULL, "%s: unexpected dialog\n", Mode);
    if (hDlg)
        PostMessageW(hDlg, WM_CLOSE, 0, 0);

    Code = WaitForExit(hProcess, 20000);
    CloseHardErrorBoxes();
    ok_(__FILE__, Line)(Code == ExpectedCode, "%s: exit code 0x%lx, expected 0x%lx\n", Mode, Code, ExpectedCode);
    ok_(__FILE__, Line)(FileExists(MarkerPath) == (ExpectedCode == DEBUGGER_EXIT_CODE),
                        "%s: debugger %s\n", Mode, FileExists(MarkerPath) ? "started" : "not started");
    ok_(__FILE__, Line)(!DetachRan(ProcessId), "%s: the crashed program ran DLL detach\n", Mode);
    DumpPath(Path, ProcessId);
    ok_(__FILE__, Line)(FileExists(Path) == Reported, "%s: dump %s\n", Mode, FileExists(Path) ? "written" : "missing");

    CloseHandle(hProcess);
    DeleteReport(ProcessId);
}
#define TestNoDialog(Mode, Code, Reported) TestNoDialog_(Mode, Code, Reported, __LINE__)

START_TEST(CrashDialog)
{
    PCHAR *Argv;
    int Argc;
    HKEY hKey;
    USEROBJECTFLAGS Flags;
    BOOL IsWow64, HadLocalDumps;
    LONG Error;
    DWORD Count = 50, Type = 1;
    size_t i;

    GetTempPathW(_countof(MarkerPath), MarkerPath);
    StringCchCatW(MarkerPath, _countof(MarkerPath), L"k32crashdlg.dbg");

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 5 && !strcmp(Argv[2], "fakedbg"))
    {
        FakeDebugger(Argv[3], Argv[4]);
        return;
    }
    if (Argc >= 3)
    {
        RunChild(Argv[2]);
        return;
    }

    /* Without the DLL the checks for DLL detach would pass for nothing */
    ok(SearchPathW(NULL, L"uhexit_dll.dll", NULL, _countof(DumpFolder), DumpFolder, NULL) != 0,
       "uhexit_dll.dll not found: %lu\n", GetLastError());

    if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &Flags, sizeof(Flags), NULL) ||
        !(Flags.dwFlags & WSF_VISIBLE))
    {
        skip("No interactive window station\n");
        return;
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ReactOS", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        IsReactOS = TRUE;
        RegCloseKey(hKey);
    }
    if (IsWow64Process(GetCurrentProcess(), &IsWow64) && IsWow64)
        RegistryView = KEY_WOW64_64KEY;

    /* Do not overwrite settings someone made for this program */
    /* Do not change settings that could not be saved; keys this test creates are deleted at the end */
    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, LOCALDUMPS_APP, 0, KEY_READ | RegistryView, &hKey);
    if (Error != ERROR_FILE_NOT_FOUND)
    {
        if (Error == ERROR_SUCCESS)
            RegCloseKey(hKey);
        skip("LocalDumps has settings for kernel32_apitest.exe already (%ld)\n", Error);
        return;
    }
    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, LOCALDUMPS_KEY, 0, KEY_READ | RegistryView, &hKey);
    if (Error != ERROR_SUCCESS && Error != ERROR_FILE_NOT_FOUND)
    {
        skip("Cannot read the LocalDumps key: %ld\n", Error);
        return;
    }
    HadLocalDumps = (Error == ERROR_SUCCESS);
    if (HadLocalDumps)
        RegCloseKey(hKey);

    for (i = 0; i < _countof(SavedValues); i++)
    {
        if (wcscmp(SavedValues[i].Key, AEDEBUG_KEY))
            SavedValues[i].View = RegistryView;
        if (!SaveValue(&SavedValues[i]))
        {
            skip("Cannot save %ls\\%ls\n", SavedValues[i].Key, SavedValues[i].Name);
            return;
        }
    }

    /* Dumps go to a folder of this run; their paths are in the problem details */
    GetTempPathW(_countof(DumpFolder), DumpFolder);
    StringCchPrintfW(DumpFolder + wcslen(DumpFolder), _countof(DumpFolder) - wcslen(DumpFolder),
                     L"k32crashdlg.%lu", GetCurrentProcessId());
    CreateDirectoryW(DumpFolder, NULL);
    SetValue(HKEY_LOCAL_MACHINE, LOCALDUMPS_APP, L"DumpFolder", RegistryView, REG_EXPAND_SZ, DumpFolder,
             (DWORD)(wcslen(DumpFolder) + 1) * sizeof(WCHAR));
    SetValue(HKEY_LOCAL_MACHINE, LOCALDUMPS_APP, L"DumpCount", RegistryView, REG_DWORD, &Count, sizeof(Count));
    SetValue(HKEY_LOCAL_MACHINE, LOCALDUMPS_APP, L"DumpType", RegistryView, REG_DWORD, &Type, sizeof(Type));

    SetDontShowUI(HKEY_LOCAL_MACHINE, WER_KEY, 0);
    DeleteValue(HKEY_LOCAL_MACHINE, WER_POLICY_KEY, L"DontShowUI", RegistryView);
    DeleteValue(HKEY_CURRENT_USER, WER_KEY, L"DontShowUI", RegistryView);
    SetDebugger(NULL);

    /* Close program, and Escape, end the program with the exception code */
    if (IsReactOS)
        TestDialog("av", FALSE, AnswerCloseButton, STATUS_ACCESS_VIOLATION);
    TestDialog("av", FALSE, AnswerEscape, STATUS_ACCESS_VIOLATION);
    TestDialog("av", FALSE, AnswerEnter, STATUS_ACCESS_VIOLATION);
    /* A fault in the unhandled-exception filter gets the dialog too */
    TestDialog("filtfault", FALSE, AnswerEscape, STATUS_ACCESS_VIOLATION);
    /* No dialog, and no report, when the program turned the fault error box off */
    TestNoDialog("nogp", STATUS_ACCESS_VIOLATION, FALSE);
    if (IsReactOS)
        TestDialog("av", FALSE, AnswerEndProcess, 0x1234);

    /* With a debugger set, Debug starts it; Close program stays the default */
    SetDebugger(L"0");
    TestDialog("av", TRUE, AnswerEnter, STATUS_ACCESS_VIOLATION);
    TestDialog("av", TRUE, AnswerEscape, STATUS_ACCESS_VIOLATION);
    if (IsReactOS)
    {
        TestDialog("av", TRUE, AnswerDebugButton, DEBUGGER_EXIT_CODE);
        TestDialog("av", TRUE, AnswerCloseButton, STATUS_ACCESS_VIOLATION);
    }

    /* DontShowUI: no dialog, no debugger offered, the report is still written */
    SetDontShowUI(HKEY_LOCAL_MACHINE, WER_KEY, 1);
    TestNoDialog("av", STATUS_ACCESS_VIOLATION, TRUE);
    SetDontShowUI(HKEY_LOCAL_MACHINE, WER_KEY, 0);
    SetDontShowUI(HKEY_LOCAL_MACHINE, WER_POLICY_KEY, 1);
    TestNoDialog("av", STATUS_ACCESS_VIOLATION, TRUE);
    DeleteValue(HKEY_LOCAL_MACHINE, WER_POLICY_KEY, L"DontShowUI", RegistryView);
    /* Windows does not read it from the user's key */
    SetDontShowUI(HKEY_CURRENT_USER, WER_KEY, 1);
    TestDialog("av", TRUE, AnswerEscape, STATUS_ACCESS_VIOLATION);
    DeleteValue(HKEY_CURRENT_USER, WER_KEY, L"DontShowUI", RegistryView);

    /* A debugger that starts by itself gets no dialog first; Windows starts it before it writes the dump */
    SetDebugger(L"1");
    TestNoDialog("av", DEBUGGER_EXIT_CODE, IsReactOS);

    for (i = 0; i < _countof(SavedValues); i++)
    {
        RestoreValue(&SavedValues[i]);
        HeapFree(GetProcessHeap(), 0, SavedValues[i].Data);
    }
    DeleteFileW(MarkerPath);
    EmptyFolder();
    RemoveDirectoryW(DumpFolder);
    DeleteKey(HKEY_LOCAL_MACHINE, LOCALDUMPS_APP);
    /* An empty LocalDumps key would turn on dumps for every program */
    if (!HadLocalDumps)
        DeleteKey(HKEY_LOCAL_MACHINE, LOCALDUMPS_KEY);
}
