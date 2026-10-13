/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the LocalDumps crash dumps of unhandled exceptions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <winver.h>
#include <dbghelp.h>
#include <shlobj.h>

#define LOCALDUMPS_KEY L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps"
#define APP_KEY        LOCALDUMPS_KEY L"\\kernel32_apitest.exe"
#define WER_KEY        L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting"
#define WINDOWS_KEY    L"SYSTEM\\CurrentControlSet\\Control\\Windows"

/* What Windows 7 writes for DumpType 1 (and when DumpType is absent) */
#define MINI_DUMP_FLAGS (MiniDumpWithDataSegs | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData)

typedef BOOL (WINAPI *PFN_MINIDUMPREADDUMPSTREAM)(PVOID, ULONG, PMINIDUMP_DIRECTORY *, PVOID *, ULONG *);

static PFN_MINIDUMPREADDUMPSTREAM pMiniDumpReadDumpStream;
static WCHAR DumpFolder[MAX_PATH];
static BOOL IsReactOS;
static volatile LONG *NullPointer;

/* Child side ****************************************************************/

static LONG WINAPI FilterExecuteHandler(PEXCEPTION_POINTERS ExceptionInfo)
{
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI FilterContinueSearch(PEXCEPTION_POINTERS ExceptionInfo)
{
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI FilterTerminate(PEXCEPTION_POINTERS ExceptionInfo)
{
    TerminateProcess(GetCurrentProcess(), 0xE0E0);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Faults at address 0x10, so the report shows which exception it holds */
static LONG WINAPI FilterFault(PEXCEPTION_POINTERS ExceptionInfo)
{
    NullPointer[4] = 2;
    return EXCEPTION_CONTINUE_SEARCH;
}

static int Recurse(int Depth)
{
    volatile char Buffer[4096];
    Buffer[0] = (char)Depth;
    Buffer[sizeof(Buffer) - 1] = (char)Depth;
    return Recurse(Depth + 1) + Buffer[0] + Buffer[sizeof(Buffer) - 1];
}

static DWORD WINAPI CrashThread(PVOID Parameter)
{
    *NullPointer = 1;
    return 0;
}

static VOID RunChild(PCSTR Mode)
{
    /* The test framework turns off the fault error box, which also turns off the report */
    SetErrorMode(0);

    if (!strcmp(Mode, "nogp"))
        SetErrorMode(SEM_NOGPFAULTERRORBOX);
    else if (!strcmp(Mode, "nogpthread"))
        RtlSetThreadErrorMode(RTL_SEM_NOGPFAULTERRORBOX, NULL);
    else if (!strcmp(Mode, "filtexec"))
        SetUnhandledExceptionFilter(FilterExecuteHandler);
    else if (!strcmp(Mode, "filtcont"))
        SetUnhandledExceptionFilter(FilterContinueSearch);
    else if (!strcmp(Mode, "filtterm"))
        SetUnhandledExceptionFilter(FilterTerminate);
    else if (!strcmp(Mode, "filtfault"))
        SetUnhandledExceptionFilter(FilterFault);

    if (!strcmp(Mode, "so"))
        ExitProcess(Recurse(0));
    if (!strcmp(Mode, "raise"))
        RaiseException(0xE0000001, 0, 0, NULL);
    if (!strcmp(Mode, "thread"))
    {
        HANDLE Thread = CreateThread(NULL, 0, CrashThread, NULL, 0, NULL);
        WaitForSingleObject(Thread, INFINITE);
    }
    *NullPointer = 1;
    ExitProcess(0x5EE);
}

/* Parent side ***************************************************************/

/* Under WOW64 the settings must go to the 64-bit view, which the reporter reads */
static REGSAM RegistryView;

static BOOL SetDword(HKEY Root, PCWSTR Key, PCWSTR Name, DWORD Value)
{
    HKEY hKey;
    LONG Error = RegCreateKeyExW(Root, Key, 0, NULL, 0, KEY_SET_VALUE | RegistryView, NULL, &hKey, NULL);
    if (Error != ERROR_SUCCESS)
        return FALSE;
    Error = RegSetValueExW(hKey, Name, 0, REG_DWORD, (PBYTE)&Value, sizeof(Value));
    RegCloseKey(hKey);
    return Error == ERROR_SUCCESS;
}

static BOOL GetDword(HKEY Root, PCWSTR Key, PCWSTR Name, PDWORD Value)
{
    HKEY hKey;
    DWORD Type, Size = sizeof(*Value);
    LONG Error;

    if (RegOpenKeyExW(Root, Key, 0, KEY_QUERY_VALUE | RegistryView, &hKey) != ERROR_SUCCESS)
        return FALSE;
    Error = RegQueryValueExW(hKey, Name, NULL, &Type, (PBYTE)Value, &Size);
    RegCloseKey(hKey);
    return Error == ERROR_SUCCESS && Type == REG_DWORD;
}

static VOID DeleteValue(HKEY Root, PCWSTR Key, PCWSTR Name)
{
    HKEY hKey;
    if (RegOpenKeyExW(Root, Key, 0, KEY_SET_VALUE | RegistryView, &hKey) == ERROR_SUCCESS)
    {
        RegDeleteValueW(hKey, Name);
        RegCloseKey(hKey);
    }
}

static VOID RestoreDword(HKEY Root, PCWSTR Key, PCWSTR Name, BOOL Had, DWORD Value)
{
    if (Had)
        SetDword(Root, Key, Name, Value);
    else
        DeleteValue(Root, Key, Name);
}

static BOOL GetStringValue(HKEY Root, PCWSTR Key, PCWSTR Name)
{
    HKEY hKey;
    LONG Error;

    if (RegOpenKeyExW(Root, Key, 0, KEY_QUERY_VALUE | RegistryView, &hKey) != ERROR_SUCCESS)
        return FALSE;
    Error = RegQueryValueExW(hKey, Name, NULL, NULL, NULL, NULL);
    RegCloseKey(hKey);
    return Error == ERROR_SUCCESS;
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

static HANDLE StartChild(PCSTR Mode, PDWORD ProcessId)
{
    CHAR FileName[MAX_PATH], CommandLine[MAX_PATH + 64];
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;

    GetModuleFileNameA(NULL, FileName, _countof(FileName));
    StringCbPrintfA(CommandLine, sizeof(CommandLine), "\"%s\" LocalDumps %s", FileName, Mode);
    /* Keep the child off our console handles */
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

static VOID BuildPath(PWSTR Path, PCWSTR Folder, DWORD ProcessId, PCWSTR Extension)
{
    StringCchPrintfW(Path, MAX_PATH, L"%s\\kernel32_apitest.exe.%lu%s", Folder, ProcessId, Extension);
}

static BOOL FileExists(PCWSTR Path)
{
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

static ULONG CountDumps(PCWSTR Folder)
{
    WCHAR Pattern[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE hFind;
    ULONG Count = 0;

    StringCchPrintfW(Pattern, _countof(Pattern), L"%s\\*.dmp", Folder);
    hFind = FindFirstFileW(Pattern, &FindData);
    if (hFind == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        Count++;
    } while (FindNextFileW(hFind, &FindData));
    FindClose(hFind);
    return Count;
}

static VOID EmptyFolder(PCWSTR Folder)
{
    WCHAR Pattern[MAX_PATH], Path[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE hFind;

    StringCchPrintfW(Pattern, _countof(Pattern), L"%s\\kernel32_apitest.exe.*", Folder);
    hFind = FindFirstFileW(Pattern, &FindData);
    if (hFind == INVALID_HANDLE_VALUE)
        return;
    do
    {
        StringCchPrintfW(Path, _countof(Path), L"%s\\%s", Folder, FindData.cFileName);
        DeleteFileW(Path);
    } while (FindNextFileW(hFind, &FindData));
    FindClose(hFind);
}

typedef struct _EXPECTED_DUMP
{
    DWORD Code;
    BOOL CheckParameter;
    ULONG_PTR Parameter1;
    ULONG MinThreads;
} EXPECTED_DUMP;

static VOID CheckDump(PCWSTR Path, const EXPECTED_DUMP *Expected, int Line)
{
    HANDLE hFile, hMapping;
    PBYTE Base;
    PMINIDUMP_HEADER Header;
    PMINIDUMP_DIRECTORY Directory;
    PMINIDUMP_EXCEPTION_STREAM Exception = NULL;
    PMINIDUMP_THREAD_LIST Threads = NULL;
    PMINIDUMP_MODULE_LIST Modules = NULL;
    PVOID Stream;
    ULONG Size, i;
    BOOL Found;

    hFile = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok_(__FILE__, Line)(hFile != INVALID_HANDLE_VALUE, "Cannot open %ls: %lu\n", Path, GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    hMapping = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    Base = hMapping ? MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0) : NULL;
    ok_(__FILE__, Line)(Base != NULL, "Cannot map %ls: %lu\n", Path, GetLastError());
    if (!Base)
        goto Close;

    Header = (PMINIDUMP_HEADER)Base;
    ok_(__FILE__, Line)(Header->Signature == MINIDUMP_SIGNATURE, "Signature 0x%lx\n", Header->Signature);
    ok_(__FILE__, Line)(Header->Flags == MINI_DUMP_FLAGS, "Flags 0x%I64x\n", (ULONGLONG)Header->Flags);

    if (pMiniDumpReadDumpStream(Base, ExceptionStream, &Directory, &Stream, &Size))
        Exception = Stream;
    if (pMiniDumpReadDumpStream(Base, ThreadListStream, &Directory, &Stream, &Size))
        Threads = Stream;
    if (pMiniDumpReadDumpStream(Base, ModuleListStream, &Directory, &Stream, &Size))
        Modules = Stream;

    ok_(__FILE__, Line)(Exception != NULL, "No exception stream\n");
    if (Exception)
    {
        ok_(__FILE__, Line)(Exception->ExceptionRecord.ExceptionCode == Expected->Code,
                            "Exception code 0x%lx\n", Exception->ExceptionRecord.ExceptionCode);
        if (Expected->CheckParameter)
        {
            ok_(__FILE__, Line)(Exception->ExceptionRecord.NumberParameters >= 2 &&
                                Exception->ExceptionRecord.ExceptionInformation[1] == Expected->Parameter1,
                                "Parameters %lu, [1] 0x%I64x\n", Exception->ExceptionRecord.NumberParameters,
                                Exception->ExceptionRecord.ExceptionInformation[1]);
        }
        ok_(__FILE__, Line)(Exception->ThreadContext.DataSize == sizeof(CONTEXT),
                            "Context size %lu\n", Exception->ThreadContext.DataSize);
    }

    ok_(__FILE__, Line)(Threads != NULL, "No thread list\n");
    if (Threads && Exception)
    {
        ok_(__FILE__, Line)(Threads->NumberOfThreads >= Expected->MinThreads,
                            "%lu threads\n", Threads->NumberOfThreads);
        for (Found = FALSE, i = 0; i < Threads->NumberOfThreads; i++)
            Found = Found || (Threads->Threads[i].ThreadId == Exception->ThreadId);
        ok_(__FILE__, Line)(Found, "Thread %lu not in the thread list\n", Exception->ThreadId);
    }

    ok_(__FILE__, Line)(Modules != NULL, "No module list\n");
    if (Modules)
    {
        PMINIDUMP_STRING Name = NULL;
        ok_(__FILE__, Line)(Modules->NumberOfModules >= 3, "%lu modules\n", Modules->NumberOfModules);
        if (Modules->NumberOfModules)
            Name = (PMINIDUMP_STRING)(Base + Modules->Modules[0].ModuleNameRva);
        ok_(__FILE__, Line)(Name && wcsstr(Name->Buffer, L"kernel32_apitest.exe") != NULL,
                            "First module %ls\n", Name ? Name->Buffer : L"(none)");
    }

    UnmapViewOfFile(Base);
Close:
    if (hMapping)
        CloseHandle(hMapping);
    CloseHandle(hFile);
}

/* OrielOS also writes a text report next to the dump */
static VOID CheckReport(PCWSTR Path, DWORD Code, int Line)
{
    CHAR Buffer[4096], Expected[64];
    DWORD Read = 0;
    HANDLE hFile;

    hFile = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok_(__FILE__, Line)(hFile != INVALID_HANDLE_VALUE, "Cannot open %ls: %lu\n", Path, GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    ReadFile(hFile, Buffer, sizeof(Buffer) - 1, &Read, NULL);
    CloseHandle(hFile);
    Buffer[Read] = ANSI_NULL;

    StringCbPrintfA(Expected, sizeof(Expected), "Exception number: 0x%08lx", Code);
    ok_(__FILE__, Line)(strstr(Buffer, Expected) != NULL, "'%s' not in the report\n", Expected);
    ok_(__FILE__, Line)(strstr(Buffer, "kernel32_apitest.exe") != NULL, "Program name not in the report\n");
    ok_(__FILE__, Line)(strstr(Buffer, "Exception module: ") != NULL, "No faulting module in the report\n");
}

static VOID TestCrash_(PCSTR Mode, DWORD ExitCode, const EXPECTED_DUMP *Expected, PCWSTR Folder, int Line)
{
    WCHAR DumpPath[MAX_PATH], ReportPath[MAX_PATH];
    DWORD ProcessId, Wait, Code = 0;
    HANDLE hProcess;

    /* Process ids are reused, and so are the file names */
    if (Folder == DumpFolder)
        EmptyFolder(Folder);

    hProcess = StartChild(Mode, &ProcessId);
    if (!hProcess)
        return;
    Wait = WaitForSingleObject(hProcess, 60000);
    ok_(__FILE__, Line)(Wait == WAIT_OBJECT_0, "%s: child did not exit (%lu)\n", Mode, Wait);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(hProcess, 1);
        WaitForSingleObject(hProcess, 5000);
    }
    GetExitCodeProcess(hProcess, &Code);
    CloseHandle(hProcess);
    ok_(__FILE__, Line)(Code == ExitCode, "%s: exit code 0x%lx, expected 0x%lx\n", Mode, Code, ExitCode);

    BuildPath(DumpPath, Folder, ProcessId, L".dmp");
    BuildPath(ReportPath, Folder, ProcessId, L".txt");
    if (!Expected)
    {
        ok_(__FILE__, Line)(!FileExists(DumpPath), "%s: unexpected dump %ls\n", Mode, DumpPath);
        ok_(__FILE__, Line)(!FileExists(ReportPath), "%s: unexpected report %ls\n", Mode, ReportPath);
        return;
    }

    ok_(__FILE__, Line)(FileExists(DumpPath), "%s: no dump %ls\n", Mode, DumpPath);
    if (FileExists(DumpPath))
        CheckDump(DumpPath, Expected, Line);
    if (IsReactOS)
        CheckReport(ReportPath, Expected->Code, Line);

    /* Elsewhere remove only what this child left */
    if (Folder != DumpFolder)
    {
        DeleteFileW(DumpPath);
        DeleteFileW(ReportPath);
    }
}
#define TestCrash(Mode, ExitCode, Expected) TestCrash_(Mode, ExitCode, Expected, DumpFolder, __LINE__)

static const EXPECTED_DUMP AccessViolation = { STATUS_ACCESS_VIOLATION, TRUE, 0, 1 };
static const EXPECTED_DUMP FilterAccessViolation = { STATUS_ACCESS_VIOLATION, TRUE, 4 * sizeof(LONG), 1 };
static const EXPECTED_DUMP ThreadAccessViolation = { STATUS_ACCESS_VIOLATION, TRUE, 0, 2 };
static const EXPECTED_DUMP StackOverflow = { STATUS_STACK_OVERFLOW, FALSE, 0, 1 };
static const EXPECTED_DUMP Raised = { 0xE0000001, FALSE, 0, 1 };

/* With a write time DaysAgo days back, which FAT stores as exactly as NTFS */
static VOID CreateDumpFile(PCWSTR Name, ULONG DaysAgo)
{
    WCHAR Path[MAX_PATH];
    HANDLE hFile;
    FILETIME Now;
    ULARGE_INTEGER Time;

    StringCchPrintfW(Path, _countof(Path), L"%s\\%s", DumpFolder, Name);
    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "Cannot create %ls: %lu\n", Path, GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    GetSystemTimeAsFileTime(&Now);
    Time.LowPart = Now.dwLowDateTime;
    Time.HighPart = Now.dwHighDateTime;
    Time.QuadPart -= DaysAgo * 24ULL * 3600 * 10000000;
    Now.dwLowDateTime = Time.LowPart;
    Now.dwHighDateTime = Time.HighPart;
    ok(SetFileTime(hFile, NULL, NULL, &Now), "SetFileTime failed: %lu\n", GetLastError());
    CloseHandle(hFile);
}

static BOOL DumpFileExists(PCWSTR Name)
{
    WCHAR Path[MAX_PATH];

    StringCchPrintfW(Path, _countof(Path), L"%s\\%s", DumpFolder, Name);
    return FileExists(Path);
}

static VOID DeleteDumpFile(PCWSTR Name)
{
    WCHAR Path[MAX_PATH];

    StringCchPrintfW(Path, _countof(Path), L"%s\\%s", DumpFolder, Name);
    DeleteFileW(Path);
}

static VOID TestDumpCount(VOID)
{
    /* Every .dmp file in the folder counts, and the oldest go first */
    SetDword(HKEY_LOCAL_MACHINE, APP_KEY, L"DumpCount", 2);
    CreateDumpFile(L"older.dmp", 2);
    CreateDumpFile(L"newer.dmp", 1);
    TestCrash("av", STATUS_ACCESS_VIOLATION, &AccessViolation);
    ok(!DumpFileExists(L"older.dmp"), "older.dmp was kept\n");
    ok(DumpFileExists(L"newer.dmp"), "newer.dmp was deleted\n");
    ok_int(CountDumps(DumpFolder), 2);
    DeleteDumpFile(L"older.dmp");
    DeleteDumpFile(L"newer.dmp");

    /* No dump at all with a count of 0 */
    SetDword(HKEY_LOCAL_MACHINE, APP_KEY, L"DumpCount", 0);
    TestCrash("av", STATUS_ACCESS_VIOLATION, NULL);
    SetDword(HKEY_LOCAL_MACHINE, APP_KEY, L"DumpCount", 50);
}

static VOID TestDefaultFolder(VOID)
{
    typedef HRESULT (WINAPI *PFN_SHGETFOLDERPATHW)(HWND, int, HANDLE, DWORD, LPWSTR);
    PFN_SHGETFOLDERPATHW pSHGetFolderPathW;
    WCHAR Folder[MAX_PATH];
    HMODULE hShell32;

    hShell32 = LoadLibraryW(L"shell32.dll");
    pSHGetFolderPathW = (PFN_SHGETFOLDERPATHW)GetProcAddress(hShell32, "SHGetFolderPathW");
    if (!pSHGetFolderPathW ||
        FAILED(pSHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, Folder)))
    {
        skip("No local application data folder\n");
        return;
    }
    StringCchCatW(Folder, _countof(Folder), L"\\CrashDumps");

    /* A global DumpFolder would apply instead; existing dumps there could be pruned */
    if (GetStringValue(HKEY_LOCAL_MACHINE, LOCALDUMPS_KEY, L"DumpFolder"))
    {
        skip("LocalDumps has a DumpFolder\n");
        return;
    }
    if (CountDumps(Folder))
    {
        skip("%ls has dumps already\n", Folder);
        return;
    }

    /* Without DumpFolder the dump goes to %LOCALAPPDATA%\CrashDumps */
    DeleteValue(HKEY_LOCAL_MACHINE, APP_KEY, L"DumpFolder");
    TestCrash_("av", STATUS_ACCESS_VIOLATION, &AccessViolation, Folder, __LINE__);
}

START_TEST(LocalDumps)
{
    PCHAR *Argv;
    int Argc;
    HKEY hKey;
    LONG Error;
    DWORD OldErrorMode = 0, OldDontShowUI = 0;
    BOOL HadErrorMode, HadDontShowUI, HadLocalDumps, IsWow64;
    DWORD Disposition;
    HMODULE hDbgHelp;

    Argc = winetest_get_mainargs(&Argv);
    if (Argc >= 3)
    {
        RunChild(Argv[2]);
        return;
    }

    hDbgHelp = LoadLibraryW(L"dbghelp.dll");
    pMiniDumpReadDumpStream = (PFN_MINIDUMPREADDUMPSTREAM)GetProcAddress(hDbgHelp, "MiniDumpReadDumpStream");
    if (!pMiniDumpReadDumpStream)
    {
        skip("No MiniDumpReadDumpStream\n");
        return;
    }

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ReactOS", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        IsReactOS = TRUE;
        RegCloseKey(hKey);
    }

    if (IsWow64Process(GetCurrentProcess(), &IsWow64) && IsWow64)
        RegistryView = KEY_WOW64_64KEY;

    HadLocalDumps = (RegOpenKeyExW(HKEY_LOCAL_MACHINE, LOCALDUMPS_KEY, 0, KEY_READ | RegistryView, &hKey) == ERROR_SUCCESS);
    if (HadLocalDumps)
        RegCloseKey(hKey);

    /* Do not overwrite settings someone made for this program */
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, APP_KEY, 0, KEY_READ | RegistryView, &hKey) == ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        skip("LocalDumps has settings for kernel32_apitest.exe already\n");
        return;
    }

    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, APP_KEY, 0, NULL, 0, KEY_SET_VALUE | RegistryView, NULL, &hKey, &Disposition);
    if (Error != ERROR_SUCCESS)
    {
        skip("Cannot create the LocalDumps key: %ld\n", Error);
        return;
    }
    /* Another run of this test created it meanwhile */
    if (Disposition != REG_CREATED_NEW_KEY)
    {
        RegCloseKey(hKey);
        skip("LocalDumps has settings for kernel32_apitest.exe already\n");
        return;
    }

    GetTempPathW(_countof(DumpFolder), DumpFolder);
    StringCchPrintfW(DumpFolder + wcslen(DumpFolder), _countof(DumpFolder) - wcslen(DumpFolder),
                     L"k32localdumps.%lu", GetCurrentProcessId());
    CreateDirectoryW(DumpFolder, NULL);
    EmptyFolder(DumpFolder);
    RegSetValueExW(hKey, L"DumpFolder", 0, REG_EXPAND_SZ, (PBYTE)DumpFolder,
                   (DWORD)(wcslen(DumpFolder) + 1) * sizeof(WCHAR));
    RegCloseKey(hKey);
    SetDword(HKEY_LOCAL_MACHINE, APP_KEY, L"DumpCount", 50);
    SetDword(HKEY_LOCAL_MACHINE, APP_KEY, L"DumpType", 1);

    /* No message boxes: the classic hard error (ErrorMode) and the Windows Error Reporting dialog */
    HadErrorMode = GetDword(HKEY_LOCAL_MACHINE, WINDOWS_KEY, L"ErrorMode", &OldErrorMode);
    HadDontShowUI = GetDword(HKEY_LOCAL_MACHINE, WER_KEY, L"DontShowUI", &OldDontShowUI);
    SetDword(HKEY_LOCAL_MACHINE, WINDOWS_KEY, L"ErrorMode", 2);
    SetDword(HKEY_LOCAL_MACHINE, WER_KEY, L"DontShowUI", 1);

    TestCrash("av", STATUS_ACCESS_VIOLATION, &AccessViolation);
    TestCrash("thread", STATUS_ACCESS_VIOLATION, &ThreadAccessViolation);
    TestCrash("raise", 0xE0000001, &Raised);
    TestCrash("so", STATUS_STACK_OVERFLOW, &StackOverflow);
    /* An unhandled-exception filter decides first */
    TestCrash("filtcont", STATUS_ACCESS_VIOLATION, &AccessViolation);
    TestCrash("filtexec", STATUS_ACCESS_VIOLATION, NULL);
    TestCrash("filtterm", 0xE0E0, NULL);
    /* A fault in the filter is reported instead */
    TestCrash("filtfault", STATUS_ACCESS_VIOLATION, &FilterAccessViolation);
    /* No report when the program asked for no fault error box */
    TestCrash("nogp", STATUS_ACCESS_VIOLATION, NULL);
    TestCrash("nogpthread", STATUS_ACCESS_VIOLATION, NULL);

    TestDumpCount();
    TestDefaultFolder();

    EmptyFolder(DumpFolder);
    RemoveDirectoryW(DumpFolder);
    RestoreDword(HKEY_LOCAL_MACHINE, WINDOWS_KEY, L"ErrorMode", HadErrorMode, OldErrorMode);
    RestoreDword(HKEY_LOCAL_MACHINE, WER_KEY, L"DontShowUI", HadDontShowUI, OldDontShowUI);
    DeleteKey(HKEY_LOCAL_MACHINE, APP_KEY);
    if (!HadLocalDumps)
        DeleteKey(HKEY_LOCAL_MACHINE, LOCALDUMPS_KEY);
}
