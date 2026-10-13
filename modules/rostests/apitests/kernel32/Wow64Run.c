/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "precomp.h"

static BOOL RunHello(const WCHAR *Image, HANDLE Output, BOOL Fault)
{
    WCHAR Command[MAX_PATH + 6];
    STARTUPINFOW Startup = {sizeof(Startup)};
    PROCESS_INFORMATION Process;
    DWORD ExitCode = 0, Wait;
    BOOL Result, Wow64;

    Startup.dwFlags = STARTF_USESTDHANDLES;
    Startup.hStdOutput = Startup.hStdError = Output;
    Startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    wcscpy(Command, L"\"");
    wcscat(Command, Image);
    wcscat(Command, L"\"");
    if (Fault) wcscat(Command, L" !");
    Result = CreateProcessW(Image, Command, NULL, NULL, TRUE, CREATE_SUSPENDED,
                            NULL, NULL, &Startup, &Process);
    ok(Result, "PE32 CreateProcess failed %lu\n", GetLastError());
    if (!Result) return FALSE;
    Wow64 = FALSE;
    ok(IsWow64Process(Process.hProcess, &Wow64), "IsWow64Process failed\n");
#ifdef _WIN64
    ok(Wow64, "PE32 child is not a WoW64 process\n");
#else
    ok(!Wow64, "Native i386 child reported as WoW64\n");
#endif
    ok(ResumeThread(Process.hThread) == 1, "ResumeThread failed\n");
    Wait = WaitForSingleObject(Process.hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "Child wait %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(Process.hProcess, 0xdead);
        WaitForSingleObject(Process.hProcess, 5000);
    }
    ok(GetExitCodeProcess(Process.hProcess, &ExitCode), "GetExitCodeProcess failed\n");
#ifdef _WIN64
    ok(ExitCode == (Fault ? (DWORD)STATUS_ACCESS_VIOLATION : 32), "Child exit %lx\n", ExitCode);
#else
    ok(ExitCode == 0, "Child exit %lx\n", ExitCode);
#endif
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
    return TRUE;
}

START_TEST(Wow64Run)
{
    WCHAR Image[MAX_PATH], Output[MAX_PATH];
    SECURITY_ATTRIBUTES Attributes = {sizeof(Attributes), NULL, TRUE};
    HANDLE File, Console;
    DWORD Count, i;
    BOOL Result;
    COORD Origin = {0, 0};
    char Text[128];
    static const char Expected[] = "Hello from 32-bit ReactOS!\r\n";

    GetWindowsDirectoryW(Image, RTL_NUMBER_OF(Image));
#ifdef _WIN64
    wcscat(Image, L"\\SysWOW64\\wow64_hello.exe");
#else
    wcscat(Image, L"\\system32\\wow64_hello.exe");
#endif
    if (GetFileAttributesW(Image) == INVALID_FILE_ATTRIBUTES)
    {
        skip("Install the i386 wow64_hello.exe fixture to run this test\n");
        return;
    }
    GetTempPathW(RTL_NUMBER_OF(Output), Output);
    wcscat(Output, L"wow64-hello-output.txt");
    File = CreateFileW(Output, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &Attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    ok(File != INVALID_HANDLE_VALUE, "CreateFile failed %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE) return;
    for (i = 0; i < 4; ++i)
    {
        SetFilePointer(File, 0, NULL, FILE_BEGIN);
        SetEndOfFile(File);
        if (!RunHello(Image, File, FALSE)) break;
        SetFilePointer(File, 0, NULL, FILE_BEGIN);
        ZeroMemory(Text, sizeof(Text));
        Count = 0;
        Result = ReadFile(File, Text, sizeof(Text), &Count, NULL);
        ok(Result, "ReadFile failed %lu\n", GetLastError());
        ok(Count == sizeof(Expected) - 1 && !memcmp(Text, Expected, sizeof(Expected) - 1),
           "PE32 stdout differs: %lu bytes, '%.*s'\n", Count, (int)min(Count, sizeof(Text)), Text);
    }
#ifdef _WIN64
    RunHello(Image, File, TRUE);
#endif
    CloseHandle(File);

    /* An inactive screen buffer isolates visible-console verification from
     * the test harness's redirected stdout and from other console clients. */
    Console = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &Attributes, CONSOLE_TEXTMODE_BUFFER, NULL);
    ok(Console != INVALID_HANDLE_VALUE, "CreateConsoleScreenBuffer failed %lu\n", GetLastError());
    if (Console != INVALID_HANDLE_VALUE)
    {
        if (RunHello(Image, Console, FALSE))
        {
            ZeroMemory(Text, sizeof(Text));
            Count = 0;
            Result = ReadConsoleOutputCharacterA(Console, Text, sizeof(Expected) - 3, Origin, &Count);
            ok(Result, "ReadConsoleOutputCharacter failed %lu\n", GetLastError());
            ok(Count == sizeof(Expected) - 3 && !memcmp(Text, Expected, sizeof(Expected) - 3),
               "PE32 console text differs: %lu bytes, '%.*s'\n", Count, (int)min(Count, sizeof(Text)), Text);
        }
        CloseHandle(Console);
    }
}

static VOID RunReviewChild(const WCHAR *Image, const WCHAR *Arguments)
{
    WCHAR Command[2 * MAX_PATH];
    STARTUPINFOW Startup = {sizeof(Startup)};
    PROCESS_INFORMATION Process;
    DWORD Wait, ExitCode = 0;
    BOOL Result;

    _snwprintf(Command, RTL_NUMBER_OF(Command), L"\"%ls\" %ls", Image, Arguments);
    Result = CreateProcessW(Image, Command, NULL, NULL, FALSE, 0,
                            NULL, NULL, &Startup, &Process);
    ok(Result, "CreateProcess %ls failed: %lu\n", Command, GetLastError());
    if (!Result) return;
    Wait = WaitForSingleObject(Process.hProcess, 20000);
    ok(Wait == WAIT_OBJECT_0, "Child %ls wait: %lu\n", Command, Wait);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(Process.hProcess, 0xdead);
        WaitForSingleObject(Process.hProcess, 5000);
    }
    ok(GetExitCodeProcess(Process.hProcess, &ExitCode), "GetExitCodeProcess failed\n");
    ok(ExitCode == 0, "Child %ls exit: %lx\n", Command, ExitCode);
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
}

START_TEST(Wow64ReviewFixes)
{
    WCHAR Source[MAX_PATH], Image[MAX_PATH], Local[MAX_PATH], Dll[MAX_PATH];
    WCHAR Temp[MAX_PATH];
    BOOL Copied;

    GetSystemDirectoryW(Image, RTL_NUMBER_OF(Image));
    wcscat(Image, L"\\wow64_native.exe");
    RunReviewChild(Image, L"");

    GetWindowsDirectoryW(Source, RTL_NUMBER_OF(Source));
#ifdef _WIN64
    wcscat(Source, L"\\SysWOW64\\");
#else
    wcscat(Source, L"\\system32\\");
#endif
    wcscpy(Image, Source);
    wcscat(Image, L"wow64_errors.exe");
    RunReviewChild(Image, L"h");
    RunReviewChild(Image, L"a");
    RunReviewChild(Image, L"m");

    /* The PE32 image and its static import live outside every system search
     * directory. The parent current directory is deliberately unchanged. */
    GetTempPathW(RTL_NUMBER_OF(Temp), Temp);
    _snwprintf(Local, RTL_NUMBER_OF(Local), L"%lswow64-local-%lu", Temp, GetCurrentProcessId());
    ok(CreateDirectoryW(Local, NULL), "CreateDirectory failed: %lu\n", GetLastError());
    _snwprintf(Image, RTL_NUMBER_OF(Image), L"%ls\\wow64_localhello.exe", Local);
    wcscpy(Temp, Source);
    wcscat(Temp, L"wow64_localhello.exe");
    Copied = CopyFileW(Temp, Image, FALSE);
    ok(Copied, "Copy PE32 executable failed: %lu\n", GetLastError());
    _snwprintf(Dll, RTL_NUMBER_OF(Dll), L"%ls\\wow64_local.dll", Local);
    wcscpy(Temp, Source);
    wcscat(Temp, L"wow64_local.dll");
    Copied = CopyFileW(Temp, Dll, FALSE) && Copied;
    ok(Copied, "Copy adjacent DLL failed: %lu\n", GetLastError());
    if (Copied) RunReviewChild(Image, L"");
    DeleteFileW(Image);
    DeleteFileW(Dll);
    RemoveDirectoryW(Local);
}
