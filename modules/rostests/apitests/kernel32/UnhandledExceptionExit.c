/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test that a process ended by an unhandled exception does not run DLL detach
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

/*
 * A child loads uhexit_dll.dll, whose DLL_PROCESS_DETACH writes a marker
 * file, and then dies of an unhandled exception. Windows ends such a process
 * at once (NtTerminateProcess with the exception code): no DLL detach and no
 * atexit handlers run. A process that calls ExitProcess still gets them.
 */

#define CUSTOM_CODE     0xE0012345
#define EXIT_CODE       0x00C0FFEE
#define NO_DLL_CODE     0x0BADD11
#define FALLTHROUGH_CODE 0x0000FA11

#define WER_KEY         L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting"
#define AEDEBUG_KEY     L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\AeDebug"

static volatile LONG *NullPointer;

/* Child side ****************************************************************/

static DWORD WINAPI CrashThread(PVOID Parameter)
{
    if (Parameter)
        RaiseException(CUSTOM_CODE, 0, 0, NULL);
    else
        NullPointer[2] = 1;
    return 0;
}

static LONG WINAPI ExecuteHandlerFilter(PEXCEPTION_POINTERS ExceptionInfo)
{
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI ExitProcessFilter(PEXCEPTION_POINTERS ExceptionInfo)
{
    ExitProcess(EXIT_CODE);
}

static VOID RunChild(PCSTR Mode)
{
    BOOL Raise = (strstr(Mode, "raise") != NULL);
    HANDLE hThread;

    if (!LoadLibraryW(L"uhexit_dll.dll"))
        ExitProcess(NO_DLL_CODE);

    /* No error box: the default filter lets the startup handler end the process */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    if (!strcmp(Mode, "exit"))
        ExitProcess(EXIT_CODE);
    if (!strncmp(Mode, "filter", 6))
        SetUnhandledExceptionFilter(ExecuteHandlerFilter);
    else if (!strncmp(Mode, "exitfilter", 10))
        SetUnhandledExceptionFilter(ExitProcessFilter);

    if (strstr(Mode, "thread"))
    {
        hThread = CreateThread(NULL, 0, CrashThread, (PVOID)(ULONG_PTR)Raise, 0, NULL);
        if (hThread)
            WaitForSingleObject(hThread, 30000);
    }
    else
    {
        CrashThread((PVOID)(ULONG_PTR)Raise);
    }
    ExitProcess(FALLTHROUGH_CODE);
}

/* Parent side ***************************************************************/

typedef struct _SAVED_VALUE
{
    PCWSTR Key;
    PCWSTR Name;
    REGSAM View;
    BOOL Had;
    DWORD Type;
    PBYTE Data;
    DWORD Size;
} SAVED_VALUE;

/* A post-mortem debugger, or the error reporting UI, would take the crashes of the children */
static SAVED_VALUE SavedValues[] =
{
    { AEDEBUG_KEY, L"Debugger" },
    { AEDEBUG_KEY, L"Auto" },
    { WER_KEY, L"DontShowUI" },
};

/* FALSE when the value exists but could not be saved */
static BOOL SaveValue(SAVED_VALUE *Value)
{
    HKEY hKey;
    LONG Error;

    Value->Had = FALSE;
    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, Value->Key, 0, KEY_QUERY_VALUE | Value->View, &hKey);
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

static LONG SetValue(const SAVED_VALUE *Value, DWORD Type, const VOID *Data, DWORD Size)
{
    HKEY hKey;
    LONG Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Value->Key, 0, NULL, 0, KEY_SET_VALUE | Value->View,
                                 NULL, &hKey, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = RegSetValueExW(hKey, Value->Name, 0, Type, Data, Size);
    RegCloseKey(hKey);
    return Error;
}

static LONG DeleteValue(const SAVED_VALUE *Value)
{
    HKEY hKey;
    LONG Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, Value->Key, 0, KEY_SET_VALUE | Value->View, &hKey);
    if (Error == ERROR_FILE_NOT_FOUND)
        return ERROR_SUCCESS;
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = RegDeleteValueW(hKey, Value->Name);
    RegCloseKey(hKey);
    return (Error == ERROR_FILE_NOT_FOUND) ? ERROR_SUCCESS : Error;
}

static VOID RestoreValues(VOID)
{
    size_t i;

    for (i = 0; i < _countof(SavedValues); i++)
    {
        if (SavedValues[i].Had)
            SetValue(&SavedValues[i], SavedValues[i].Type, SavedValues[i].Data, SavedValues[i].Size);
        else
            DeleteValue(&SavedValues[i]);
        HeapFree(GetProcessHeap(), 0, SavedValues[i].Data);
        SavedValues[i].Data = NULL;
    }
}

/* No post-mortem debugger and no error reporting UI for the crashing children; FALSE to skip them */
static BOOL PrepareRegistry(VOID)
{
    BOOL IsWow64;
    DWORD One = 1;
    LONG Error;
    size_t i;

    /* Windows reads the error reporting settings from the native view */
    if (IsWow64Process(GetCurrentProcess(), &IsWow64) && IsWow64)
        SavedValues[2].View = KEY_WOW64_64KEY;

    for (i = 0; i < _countof(SavedValues); i++)
    {
        if (!SaveValue(&SavedValues[i]))
        {
            skip("Cannot save %ls\\%ls\n", SavedValues[i].Key, SavedValues[i].Name);
            while (i--)
                HeapFree(GetProcessHeap(), 0, SavedValues[i].Data);
            return FALSE;
        }
    }

    /* A debugger that is set must go, or the crashes are its */
    for (i = 0; i < 2; i++)
    {
        if (!SavedValues[i].Had)
            continue;
        Error = DeleteValue(&SavedValues[i]);
        if (Error != ERROR_SUCCESS)
        {
            skip("AeDebug %ls is set and cannot be cleared (%ld)\n", SavedValues[i].Name, Error);
            RestoreValues();
            return FALSE;
        }
    }

    /* The children also turn off the fault error box, so this is only a second guard */
    SetValue(&SavedValues[2], REG_DWORD, &One, sizeof(One));
    return TRUE;
}

static VOID MarkerPath(PWSTR Path, DWORD ProcessId)
{
    WCHAR Temp[MAX_PATH];

    GetTempPathW(_countof(Temp), Temp);
    StringCchPrintfW(Path, MAX_PATH, L"%suhexit.%lu.txt", Temp, ProcessId);
}

static VOID TestExit_(PCSTR Mode, DWORD ExpectedCode, BOOL ExpectDetach, int Line)
{
    CHAR FileName[MAX_PATH], CommandLine[MAX_PATH + 64];
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    WCHAR Marker[MAX_PATH];
    DWORD Wait, Code = 0x55555555;
    BOOL Detached;

    GetModuleFileNameA(NULL, FileName, _countof(FileName));
    StringCbPrintfA(CommandLine, sizeof(CommandLine), "\"%s\" UnhandledExceptionExit %s", FileName, Mode);
    if (!CreateProcessA(FileName, CommandLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL,
                        &StartupInfo, &ProcessInfo))
    {
        ok_(__FILE__, Line)(FALSE, "%s: CreateProcess failed with %lu\n", Mode, GetLastError());
        return;
    }
    /* Process ids are reused: no marker of an older process with this id */
    MarkerPath(Marker, ProcessInfo.dwProcessId);
    DeleteFileW(Marker);
    ResumeThread(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hThread);

    /* A process that runs DLL detach after its threads were killed can hang there */
    Wait = WaitForSingleObject(ProcessInfo.hProcess, 30000);
    ok_(__FILE__, Line)(Wait == WAIT_OBJECT_0, "%s: the process did not end (wait %lu)\n", Mode, Wait);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(ProcessInfo.hProcess, 0xDEAD);
        WaitForSingleObject(ProcessInfo.hProcess, 5000);
    }
    GetExitCodeProcess(ProcessInfo.hProcess, &Code);
    CloseHandle(ProcessInfo.hProcess);

    Detached = (GetFileAttributesW(Marker) != INVALID_FILE_ATTRIBUTES);
    ok_(__FILE__, Line)(Code == ExpectedCode, "%s: exit code 0x%08lx, expected 0x%08lx\n", Mode, Code, ExpectedCode);
    ok_(__FILE__, Line)(Detached == ExpectDetach, "%s: DLL detach %s\n", Mode, Detached ? "ran" : "did not run");
    DeleteFileW(Marker);
}
#define TestExit(Mode, Code, Detach) TestExit_(Mode, Code, Detach, __LINE__)

START_TEST(UnhandledExceptionExit)
{
    PCHAR *Argv;
    int Argc;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 3)
    {
        RunChild(Argv[2]);
        return;
    }

    /* The marker works: ExitProcess runs DLL detach */
    TestExit("exit", EXIT_CODE, TRUE);
    /* So does ExitProcess called from the unhandled-exception filter */
    TestExit("exitfilter-thread", EXIT_CODE, TRUE);

    if (!PrepareRegistry())
        return;

    /* An unhandled exception ends the process with its code, without DLL detach */
    TestExit("thread", STATUS_ACCESS_VIOLATION, FALSE);
    TestExit("main", STATUS_ACCESS_VIOLATION, FALSE);
    TestExit("raise-thread", CUSTOM_CODE, FALSE);
    TestExit("raise-main", CUSTOM_CODE, FALSE);
    /* Also when a filter set with SetUnhandledExceptionFilter returns EXCEPTION_EXECUTE_HANDLER */
    TestExit("filter-thread", STATUS_ACCESS_VIOLATION, FALSE);
    TestExit("filter-main", STATUS_ACCESS_VIOLATION, FALSE);
    TestExit("filter-raise-thread", CUSTOM_CODE, FALSE);
    TestExit("filter-raise-main", CUSTOM_CODE, FALSE);

    RestoreValues();
}
