/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     Explicit handle inheritance, including concurrent parent changes
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The two-handle exclusion scenario complements Wine's kernel32 process test
 * (Eric Pouech, Dmitry Timoshkov, Michael Mueller). The child signals events and
 * the parent checks their state, so reuse of an excluded numeric slot cannot
 * accidentally pass an object-identity assertion.
 */

#include "precomp.h"
#include <stdlib.h>
#include <ndk/pstypes.h>

#ifndef EXTENDED_STARTUPINFO_PRESENT
#define EXTENDED_STARTUPINFO_PRESENT 0x00080000
#endif

#define LARGE_LIST_COUNT 8193
#define RACE_COUNT 256
#define REPORT_COMPLETE 0x484c4358

typedef BOOL (WINAPI *INITIALIZE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD, PSIZE_T);
typedef BOOL (WINAPI *UPDATE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR,
                                 PVOID, SIZE_T, PVOID, PSIZE_T);
typedef VOID (WINAPI *DELETE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST);

static INITIALIZE_LIST pInitialize;
static UPDATE_LIST pUpdate;
static DELETE_LIST pDelete;
static WCHAR Image[MAX_PATH], MappingName[80];
static BOOL IsReactOS, IsWin8;

typedef struct
{
    ULONG Complete, Count, FlagFailures;
    HANDLE Handles[LARGE_LIST_COUNT + RACE_COUNT + 2];
    ULONG ConsoleMode[3], StandardInputMode, StandardOutputMode;
    BOOL WriteStandardOutput, StandardOutputWritten;
    HANDLE ConsoleHandles[3];
    HANDLE StandardOutput;
} CHILD_REPORT;

static CHILD_REPORT *Report;
static SECURITY_ATTRIBUTES Inheritable = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};

static VOID Child(PCSTR Name)
{
    HANDLE Mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, Name);
    CHILD_REPORT *Data;
    ULONG i;
    DWORD Flags, Mode;

    if (!Mapping) ExitProcess(90);
    Data = MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*Data));
    if (!Data) ExitProcess(91);
    for (i = 0; i < Data->Count; ++i)
    {
        if (GetHandleInformation(Data->Handles[i], &Flags) &&
            !(Flags & HANDLE_FLAG_INHERIT))
            ++Data->FlagFailures;
        SetEvent(Data->Handles[i]);
    }
    for (i = 0; i < 3; ++i)
        Data->ConsoleMode[i] = GetConsoleMode(Data->ConsoleHandles[i], &Mode);
    Data->StandardOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    Data->StandardOutputMode = GetConsoleMode(Data->StandardOutput, &Mode);
    if (Data->WriteStandardOutput)
    {
        DWORD Written = 0;
        Data->StandardOutputWritten = WriteConsoleW(Data->StandardOutput, L"H", 1, &Written, NULL) && Written == 1;
    }
    Data->StandardInputMode = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &Mode);
    Data->Complete = REPORT_COMPLETE;
    ExitProcess(0);
}

/* Create suspended: the race worker can finish publishing its handles before
 * the child reads the report, without changing the inheritance operation. */
static BOOL CreateChild(PHANDLE Handles, SIZE_T Count, BOOL Inherit,
                        BOOL Attribute, HANDLE StdOut, PPROCESS_INFORMATION Pi,
                        PDWORD Error)
{
    STARTUPINFOEXW Si;
    SIZE_T Size = 0;
    WCHAR Command[2 * MAX_PATH];
    BOOL Ret;

    ZeroMemory(&Si, sizeof(Si));
    ZeroMemory(Pi, sizeof(*Pi));
    Si.StartupInfo.cb = sizeof(Si);
    if (StdOut)
    {
        Si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        Si.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        Si.StartupInfo.hStdOutput = StdOut;
        Si.StartupInfo.hStdError = StdOut;
    }
    if (Attribute)
    {
        pInitialize(NULL, 1, 0, &Size);
        Si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, Size);
        if (!Si.lpAttributeList) return FALSE;
        Ret = pInitialize(Si.lpAttributeList, 1, 0, &Size);
        ok(Ret, "InitializeProcThreadAttributeList failed: %lu\n", GetLastError());
        Ret = pUpdate(Si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                      Handles, Count * sizeof(HANDLE), NULL, NULL);
        *Error = GetLastError();
        ok(Ret, "UpdateProcThreadAttribute failed: %lu\n", *Error);
        if (!Ret)
        {
            pDelete(Si.lpAttributeList);
            HeapFree(GetProcessHeap(), 0, Si.lpAttributeList);
            return FALSE;
        }
    }
    StringCchPrintfW(Command, _countof(Command), L"\"%s\" HandleList child %s",
                     Image, MappingName);
    SetLastError(0xdeadbeef);
    Ret = CreateProcessW(NULL, Command, NULL, NULL, Inherit,
                         CREATE_SUSPENDED | (Attribute ? EXTENDED_STARTUPINFO_PRESENT : 0),
                         NULL, NULL, &Si.StartupInfo, Pi);
    *Error = GetLastError();
    if (Si.lpAttributeList)
    {
        pDelete(Si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, Si.lpAttributeList);
    }
    return Ret;
}

static VOID FinishChild(PPROCESS_INFORMATION Pi)
{
    DWORD Exit = 0, Wait;
    ResumeThread(Pi->hThread);
    Wait = WaitForSingleObject(Pi->hProcess, 30000);
    ok(Wait == WAIT_OBJECT_0, "Child wait: %lu\n", Wait);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(Pi->hProcess, 92);
        WaitForSingleObject(Pi->hProcess, 10000);
    }
    GetExitCodeProcess(Pi->hProcess, &Exit);
    ok(Exit == 0, "Child exit: %lx\n", Exit);
    ok(Report->Complete == REPORT_COMPLETE, "No child report: %lx\n", Report->Complete);
    CloseHandle(Pi->hThread);
    CloseHandle(Pi->hProcess);
}

static VOID ExpectFailure(PHANDLE Handles, SIZE_T Count, BOOL Inherit, DWORD Error)
{
    PROCESS_INFORMATION Pi;
    DWORD Actual = 0;
    BOOL Ret = CreateChild(Handles, Count, Inherit, TRUE, NULL, &Pi, &Actual);
    ok(!Ret && Actual == Error, "CreateProcess returned %d, error %lu, expected %lu\n",
       Ret, Actual, Error);
    if (Ret)
    {
        TerminateProcess(Pi.hProcess, 93);
        WaitForSingleObject(Pi.hProcess, 10000);
        CloseHandle(Pi.hThread);
        CloseHandle(Pi.hProcess);
    }
}

static VOID TestSelection(BOOL Attribute, HANDLE StdOut)
{
    PROCESS_INFORMATION Pi;
    DWORD Error = 0;
    BOOL Ret;
    ResetEvent(Report->Handles[0]);
    ResetEvent(Report->Handles[1]);
    Report->Count = 2;
    Report->Complete = Report->FlagFailures = 0;
    Ret = CreateChild(Report->Handles, 1, TRUE, Attribute, StdOut, &Pi, &Error);
    ok(Ret, "CreateProcess selection failed: %lu\n", Error);
    if (!Ret) return;
    FinishChild(&Pi);
    if (!StdOut)
        ok(Report->StandardInputMode, "Default standard console input is unusable\n");
    ok(WaitForSingleObject(Report->Handles[0], 0) == WAIT_OBJECT_0,
       "Listed event missing at its original handle value\n");
    ok(WaitForSingleObject(Report->Handles[1], 0) == (Attribute ? WAIT_TIMEOUT : WAIT_OBJECT_0),
       "Incorrect inheritance of the unlisted event (attribute=%d)\n", Attribute);
    if (StdOut && !IsWin8)
        ok(Report->StandardOutput == StdOut, "Standard output value changed: %p / %p\n",
           Report->StandardOutput, StdOut);
}

static DWORD WINAPI CreateRacingHandles(PVOID Parameter)
{
    ULONG i;
    UNREFERENCED_PARAMETER(Parameter);
    for (i = 0; i < RACE_COUNT; ++i)
    {
        Report->Handles[2 + i] = CreateEventW(&Inheritable, TRUE, FALSE, NULL);
        SetHandleInformation(Report->Handles[1], HANDLE_FLAG_INHERIT,
                             (i & 1) ? HANDLE_FLAG_INHERIT : 0);
        SwitchToThread();
    }
    SetHandleInformation(Report->Handles[1], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    return 0;
}

static VOID TestRace(VOID)
{
    ULONG Iteration, i;
    for (Iteration = 0; Iteration < 8; ++Iteration)
    {
        PROCESS_INFORMATION Pi;
        DWORD Error = 0;
        HANDLE Thread;
        BOOL Ret;
        Report->Count = 2 + RACE_COUNT;
        Report->Complete = Report->FlagFailures = 0;
        ResetEvent(Report->Handles[0]);
        ResetEvent(Report->Handles[1]);
        Thread = CreateThread(NULL, 0, CreateRacingHandles, NULL, 0, NULL);
        ok(Thread != NULL, "Race thread: %lu\n", GetLastError());
        if (!Thread) return;
        Ret = CreateChild(Report->Handles, 1, TRUE, TRUE, NULL, &Pi, &Error);
        ok(Ret, "Concurrent CreateProcess failed: %lu\n", Error);
        ok(WaitForSingleObject(Thread, 30000) == WAIT_OBJECT_0, "Race worker timeout\n");
        CloseHandle(Thread);
        if (Ret) FinishChild(&Pi);
        if (Ret)
            ok(WaitForSingleObject(Report->Handles[0], 0) == WAIT_OBJECT_0,
               "Race lost the listed handle\n");
        for (i = 1; i < 2 + RACE_COUNT; ++i)
        {
            ok(Report->Handles[i] != NULL, "Event allocation %lu failed\n", i);
            if (Ret)
                ok(WaitForSingleObject(Report->Handles[i], 0) == WAIT_TIMEOUT,
                   "Concurrent unlisted event %lu leaked into child\n", i);
            if (i >= 2) CloseHandle(Report->Handles[i]);
        }
    }
}

static VOID TestLargeList(VOID)
{
    PROCESS_INFORMATION Pi;
    PHANDLE Handles = HeapAlloc(GetProcessHeap(), 0, LARGE_LIST_COUNT * sizeof(HANDLE));
    DWORD Error = 0;
    ULONG i;
    BOOL Ret;
    if (!Handles) return;
    for (i = 0; i < LARGE_LIST_COUNT; ++i)
    {
        Handles[i] = CreateEventW(&Inheritable, TRUE, FALSE, NULL);
        Report->Handles[i] = Handles[i];
        ok(Handles[i] != NULL, "Large event allocation %lu failed\n", i);
    }
    Report->Count = LARGE_LIST_COUNT;
    Report->Complete = Report->FlagFailures = 0;
    Ret = CreateChild(Handles, LARGE_LIST_COUNT, TRUE, TRUE, NULL, &Pi, &Error);
    ok(Ret, "CreateProcess with %u handles failed: %lu\n", LARGE_LIST_COUNT, Error);
    if (Ret)
    {
        FinishChild(&Pi);
        ok(!Report->FlagFailures, "Inheritable flags lost: %lu\n", Report->FlagFailures);
    }
    for (i = 0; i < LARGE_LIST_COUNT; ++i)
    {
        if (Ret) ok(WaitForSingleObject(Handles[i], 0) == WAIT_OBJECT_0,
                    "Large list: event %lu missing\n", i);
        CloseHandle(Handles[i]);
    }
    HeapFree(GetProcessHeap(), 0, Handles);
}

static VOID TestConsole(VOID)
{
    PROCESS_INFORMATION Pi;
    HANDLE Handles[2];
    DWORD Error = 0;
    BOOL Ret;
    ULONG i;
    for (i = 0; i < 3; ++i)
        Report->ConsoleHandles[i] = CreateFileW(i ? L"CONOUT$" : L"CONIN$",
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &Inheritable, OPEN_EXISTING, 0, NULL);
    if (Report->ConsoleHandles[0] == INVALID_HANDLE_VALUE ||
        Report->ConsoleHandles[1] == INVALID_HANDLE_VALUE ||
        Report->ConsoleHandles[2] == INVALID_HANDLE_VALUE)
    {
        skip("No attached console\n");
        goto Cleanup;
    }
    Handles[0] = Report->ConsoleHandles[0];
    Handles[1] = Report->ConsoleHandles[1];
    Report->Count = 0;
    Report->Complete = 0;
    Ret = CreateChild(Handles, 2, TRUE, TRUE, Handles[1], &Pi, &Error);
    if (!IsReactOS && !IsWin8)
    {
        /* Server 2008 R2 rejects console pseudo-handles in the native list and
         * inherits console handles separately. The task explicitly requires
         * ReactOS to support listed console handles, a deliberate extension. */
        ok(!Ret && Error == ERROR_NO_SYSTEM_RESOURCES,
           "Win7 console list: %d / %lu\n", Ret, Error);
    }
    else
        ok(Ret, "Console list failed: %lu\n", Error);
    if (Ret)
    {
        FinishChild(&Pi);
        ok(Report->ConsoleMode[0] && Report->ConsoleMode[1], "Listed console handles unusable\n");
        ok(Report->ConsoleMode[2] == !IsWin8,
           "Unlisted console inheritance: %lu\n", Report->ConsoleMode[2]);
    }
    Handles[1] = Handles[0];
    ExpectFailure(Handles, 2, TRUE, (!IsReactOS && !IsWin8) ?
                  ERROR_NO_SYSTEM_RESOURCES : ERROR_INVALID_PARAMETER);
    Handles[1] = Report->ConsoleHandles[1];
    SetHandleInformation(Handles[1], HANDLE_FLAG_INHERIT, 0);
    ExpectFailure(Handles, 2, TRUE, (!IsReactOS && !IsWin8) ?
                  ERROR_NO_SYSTEM_RESOURCES : ERROR_INVALID_PARAMETER);
Cleanup:
    for (i = 0; i < 3; ++i) CloseHandle(Report->ConsoleHandles[i]);
}

/* Move all three standard handles away from slots 0-2. Output goes to a private
 * screen buffer so checking its contents also detects accidental slot aliasing. */
static VOID TestMovedStandardHandles(VOID)
{
    HANDLE Saved[3], Moved[3];
    DWORD Error = 0, Read;
    ULONG i, Explicit;
    COORD Origin = {0, 0};
    WCHAR Character;
    PROCESS_INFORMATION Pi;
    BOOL Ret;

    Moved[0] = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, &Inheritable,
                          OPEN_EXISTING, 0, NULL);
    Moved[1] = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &Inheritable, CONSOLE_TEXTMODE_BUFFER, NULL);
    Moved[2] = INVALID_HANDLE_VALUE;
    if (Moved[0] == INVALID_HANDLE_VALUE || Moved[1] == INVALID_HANDLE_VALUE)
    {
        skip("No console for moved standard handles\n");
        goto Cleanup;
    }
    Ret = DuplicateHandle(GetCurrentProcess(), Moved[1], GetCurrentProcess(),
                           &Moved[2], 0, TRUE, DUPLICATE_SAME_ACCESS);
    ok(Ret, "Duplicate output: %lu\n", GetLastError());
    if (!Ret) goto Cleanup;
    for (i = 0; i < 3; ++i)
    {
        Saved[i] = GetStdHandle(STD_INPUT_HANDLE - i);
        SetStdHandle(STD_INPUT_HANDLE - i, Moved[i]);
    }
    Report->Count = 0;
    Report->WriteStandardOutput = TRUE;
    for (Explicit = 0; Explicit < 2; ++Explicit)
    {
        FillConsoleOutputCharacterW(Moved[1], L' ', 1, Origin, &Read);
        SetConsoleCursorPosition(Moved[1], Origin);
        Report->Complete = Report->StandardOutputWritten = 0;
        Ret = CreateChild(Report->Handles, 1, TRUE, TRUE,
                           Explicit ? Moved[1] : NULL, &Pi, &Error);
        ok(Ret, "Moved std handles (%lu): %lu\n", Explicit, Error);
        if (!Ret) continue;
        FinishChild(&Pi);
        /* Windows 8+ filters console handles along with native handles. */
        if (!IsWin8)
        {
            ok(Report->StandardOutput == Moved[1], "Moved output value changed\n");
            ok(Report->StandardInputMode, "Moved input is unusable\n");
            ok(Report->StandardOutputMode && Report->StandardOutputWritten,
               "Moved output is unusable (%lu)\n", Explicit);
            Character = 0;
            ReadConsoleOutputCharacterW(Moved[1], &Character, 1, Origin, &Read);
            ok(Read == 1 && Character == L'H', "Output went to the wrong buffer: %x\n", Character);
        }
        else if (Explicit)
            ok(!Report->StandardOutputMode && !Report->StandardOutputWritten,
               "Unlisted explicit output inherited on Windows 8+\n");
        else
            ok(Report->StandardOutputMode && Report->StandardOutputWritten,
               "Default Windows 8+ output is unusable\n");
    }
    Report->WriteStandardOutput = FALSE;
    for (i = 0; i < 3; ++i) SetStdHandle(STD_INPUT_HANDLE - i, Saved[i]);
Cleanup:
    for (i = 0; i < 3; ++i)
        if (Moved[i] != INVALID_HANDLE_VALUE) CloseHandle(Moved[i]);
}

static BOOL QueryCsrUsage(SIZE_T *PrivateBytes, ULONG *Handles)
{
    ULONG Size = 0x10000, Needed;
    PVOID Buffer;
    PSYSTEM_PROCESS_INFORMATION Entry;
    NTSTATUS Status;
    BOOL Found = FALSE;

    for (;;)
    {
        Buffer = HeapAlloc(GetProcessHeap(), 0, Size);
        if (!Buffer) return FALSE;
        Status = NtQuerySystemInformation(SystemProcessInformation, Buffer, Size, &Needed);
        if (Status != (NTSTATUS)0xc0000004) break;
        HeapFree(GetProcessHeap(), 0, Buffer);
        Size *= 2;
    }
    *PrivateBytes = 0;
    *Handles = 0;
    if (NT_SUCCESS(Status))
    {
        Entry = Buffer;
        for (;;)
        {
            if (Entry->ImageName.Length == 9 * sizeof(WCHAR) &&
                !_wcsnicmp(Entry->ImageName.Buffer, L"csrss.exe", 9))
            {
                *PrivateBytes += Entry->PrivatePageCount;
                *Handles += Entry->HandleCount;
                Found = TRUE;
            }
            if (!Entry->NextEntryOffset) break;
            Entry = (PVOID)((PBYTE)Entry + Entry->NextEntryOffset);
        }
    }
    HeapFree(GetProcessHeap(), 0, Buffer);
    return Found;
}

static VOID TestGuiExit(VOID)
{
    enum { ConsoleCount = 4096, Warmup = 16, Iterations = 96 };
    HANDLE Console, *Copies, Selection;
    WCHAR GuiImage[MAX_PATH], Command[2 * MAX_PATH];
    STARTUPINFOEXW Si;
    PROCESS_INFORMATION Pi;
    SIZE_T Size = 0, Before = 0, After = 0;
    ULONG i, BeforeHandles = 0, AfterHandles = 0;
    DWORD Wait, Exit;
    BOOL Ret, Measured = FALSE;

    wcscpy(GuiImage, Image);
    wcscpy(wcsrchr(GuiImage, L'\\') + 1, L"handlelist_gui.exe");
    if (GetFileAttributesW(GuiImage) == INVALID_FILE_ATTRIBUTES)
    {
        skip("handlelist_gui.exe is unavailable\n");
        return;
    }
    Console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, &Inheritable,
                          OPEN_EXISTING, 0, NULL);
    if (Console == INVALID_HANDLE_VALUE) return;
    Copies = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ConsoleCount * sizeof(HANDLE));
    if (!Copies) { CloseHandle(Console); return; }
    for (i = 0; i < ConsoleCount; ++i)
    {
        if (!DuplicateHandle(GetCurrentProcess(), Console, GetCurrentProcess(),
                              &Copies[i], 0, TRUE, DUPLICATE_SAME_ACCESS)) break;
    }
    ok(i == ConsoleCount, "Only %lu console copies: %lu\n", i, GetLastError());
    ZeroMemory(&Si, sizeof(Si));
    Si.StartupInfo.cb = sizeof(Si);
    pInitialize(NULL, 1, 0, &Size);
    Si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Si.lpAttributeList) goto Cleanup;
    pInitialize(Si.lpAttributeList, 1, 0, &Size);
    Selection = CreateEventW(&Inheritable, TRUE, FALSE, NULL);
    Ret = pUpdate(Si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                   &Selection, sizeof(HANDLE), NULL, NULL);
    ok(Ret, "GUI attribute: %lu\n", GetLastError());
    if (!Ret) goto FreeList;
    for (i = 0; i < Warmup + Iterations; ++i)
    {
        StringCchPrintfW(Command, _countof(Command), L"\"%s\"", GuiImage);
        Ret = CreateProcessW(NULL, Command, NULL, NULL, TRUE,
                              EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                              &Si.StartupInfo, &Pi);
        ok(Ret, "GUI child %lu: %lu\n", i, GetLastError());
        if (!Ret) break;
        Wait = WaitForSingleObject(Pi.hProcess, 30000);
        ok(Wait == WAIT_OBJECT_0, "GUI child timeout %lu\n", i);
        if (Wait != WAIT_OBJECT_0) TerminateProcess(Pi.hProcess, 94);
        GetExitCodeProcess(Pi.hProcess, &Exit);
        ok(Exit == 0, "GUI child %lu exit %lu\n", i, Exit);
        CloseHandle(Pi.hThread);
        CloseHandle(Pi.hProcess);
        if (i + 1 == Warmup || (i + 1 > Warmup && (i + 1 - Warmup) % 32 == 0))
        {
            Sleep(100); /* Allow CSR disconnect to finish before sampling. */
            if (QueryCsrUsage(&After, &AfterHandles))
            {
                trace("GUI exits %lu: csrss private bytes %Iu, handles %lu\n", i + 1, After, AfterHandles);
                if (i + 1 == Warmup)
                {
                    Before = After;
                    BeforeHandles = AfterHandles;
                    Measured = TRUE;
                }
            }
        }
    }
    if (Measured)
    {
        trace("GUI loop csrss delta: bytes %Id, handles %ld\n",
              (SSIZE_T)After - Before, (LONG)AfterHandles - BeforeHandles);
        ok(After <= Before + 4 * 1024 * 1024,
           "GUI exits leaked csrss memory: %Iu -> %Iu\n", Before, After);
    }
    else skip("csrss memory counters unavailable\n");
FreeList:
    CloseHandle(Selection);
    pDelete(Si.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, Si.lpAttributeList);
Cleanup:
    for (i = 0; i < ConsoleCount; ++i) if (Copies[i]) CloseHandle(Copies[i]);
    HeapFree(GetProcessHeap(), 0, Copies);
    CloseHandle(Console);
}

static VOID TestListCapacity(VOID)
{
    typedef NTSTATUS (NTAPI *CREATE_WITH_ATTRIBUTES)(PHANDLE, ACCESS_MASK,
        POBJECT_ATTRIBUTES, HANDLE, ULONG, HANDLE, HANDLE, HANDLE, BOOLEAN,
        PROS_PS_ATTRIBUTE_LIST);
    CREATE_WITH_ATTRIBUTES CreateNative;
    ROS_PS_ATTRIBUTE_LIST Attributes;
    HANDLE Process = NULL;
    NTSTATUS Status;
    SIZE_T Count = (SIZE_T)ROS_PS_MAX_HANDLE_COUNT + 1, i;
    PHANDLE Handles;
    BOOL Wow64 = FALSE;

    Handles = VirtualAlloc(NULL, Count * sizeof(HANDLE),
                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (Handles)
    {
        for (i = 0; i < Count; ++i) Handles[i] = Report->Handles[0];
        ExpectFailure(Handles, Count, TRUE, ERROR_INVALID_PARAMETER);
        VirtualFree(Handles, 0, MEM_RELEASE);
    }
    else skip("Memory for capacity test unavailable: %lu\n", GetLastError());

    /* A private native service: a 32-bit process under WoW64 creates processes through the WoW64 layer */
    if (IsWow64Process(GetCurrentProcess(), &Wow64) && Wow64)
    {
        skip("NtCreateProcessWithAttributes is not a WoW64 service\n");
        return;
    }
    CreateNative = (CREATE_WITH_ATTRIBUTES)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                         "NtCreateProcessWithAttributes");
    if (!CreateNative) return;
    ZeroMemory(&Attributes, sizeof(Attributes));
    Attributes.TotalLength = sizeof(Attributes);
    Attributes.Attributes[0].Attribute = ROS_PS_ATTRIBUTE_HANDLE_LIST;
    Attributes.Attributes[0].Size = Count * sizeof(HANDLE);
    /* An unreadable list must be rejected by count before probing/allocation. */
    Attributes.Attributes[0].ValuePtr = NULL;
    Status = CreateNative(&Process, PROCESS_ALL_ACCESS, NULL, GetCurrentProcess(),
                           4 /* PROCESS_CREATE_FLAGS_INHERIT_HANDLES */,
                           NULL, NULL, NULL, FALSE, &Attributes);
    ok(Status == (NTSTATUS)0xc000000d, "Native over-capacity status: %lx\n", Status);
    ok(Process == NULL, "Native over-capacity call returned a process\n");
    if (NT_SUCCESS(Status) && Process) CloseHandle(Process);
}

START_TEST(HandleList)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    HANDLE Mapping, Pair[2];
    SIZE_T Size = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST List;
    OSVERSIONINFOW Version = {sizeof(Version)};
    char **Argv;
    int Argc = winetest_get_mainargs(&Argv);
    BOOL Ret;

    if (Argc == 4 && !strcmp(Argv[2], "child")) Child(Argv[3]);
    pInitialize = (INITIALIZE_LIST)GetProcAddress(Kernel32, "InitializeProcThreadAttributeList");
    pUpdate = (UPDATE_LIST)GetProcAddress(Kernel32, "UpdateProcThreadAttribute");
    pDelete = (DELETE_LIST)GetProcAddress(Kernel32, "DeleteProcThreadAttributeList");
    if (!pInitialize || !pUpdate || !pDelete)
    {
        skip("Process attributes unavailable\n");
        return;
    }
    GetVersionExW(&Version);
    IsReactOS = is_reactos();
    IsWin8 = Version.dwMajorVersion > 6 || (Version.dwMajorVersion == 6 && Version.dwMinorVersion >= 2);
    GetModuleFileNameW(NULL, Image, _countof(Image));
    StringCchPrintfW(MappingName, _countof(MappingName), L"Local\\HandleList-%lu", GetCurrentProcessId());
    Mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(*Report), MappingName);
    ok(Mapping != NULL, "CreateFileMapping: %lu\n", GetLastError());
    if (!Mapping) return;
    Report = MapViewOfFile(Mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*Report));
    if (!Report) { CloseHandle(Mapping); return; }
    Report->Handles[0] = Pair[0] = CreateEventW(&Inheritable, TRUE, FALSE, NULL);
    Report->Handles[1] = Pair[1] = CreateEventW(&Inheritable, TRUE, FALSE, NULL);

    TestSelection(FALSE, NULL);
    TestSelection(TRUE, NULL);
    TestSelection(TRUE, Pair[1]);
    SetHandleInformation(Pair[0], HANDLE_FLAG_INHERIT, 0);
    ExpectFailure(Pair, 1, TRUE, ERROR_INVALID_PARAMETER);
    SetHandleInformation(Pair[0], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    Pair[1] = Pair[0];
    ExpectFailure(Pair, 2, TRUE, ERROR_INVALID_PARAMETER);
    Pair[1] = CreateEventW(&Inheritable, TRUE, FALSE, NULL);
    CloseHandle(Pair[1]);
    ExpectFailure(Pair, 2, TRUE, ERROR_INVALID_PARAMETER);
    Pair[1] = INVALID_HANDLE_VALUE;
    ExpectFailure(Pair, 2, TRUE, ERROR_INVALID_PARAMETER);
    ExpectFailure(Pair, 1, FALSE, ERROR_INVALID_PARAMETER);

    pInitialize(NULL, 1, 0, &Size);
    List = HeapAlloc(GetProcessHeap(), 0, Size);
    if (List)
    {
        pInitialize(List, 1, 0, &Size);
        SetLastError(0xdeadbeef);
        Ret = pUpdate(List, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, Pair, 0, NULL, NULL);
        ok(!Ret && GetLastError() == ERROR_BAD_LENGTH, "Empty list: %d / %lu\n", Ret, GetLastError());
        pDelete(List);
        HeapFree(GetProcessHeap(), 0, List);
    }
    {
        PROCESS_INFORMATION Pi;
        DWORD Error = 0;
        HANDLE NullList[2] = {Report->Handles[0], NULL};
        Report->Count = 2;
        Report->Complete = 0;
        ResetEvent(Report->Handles[0]);
        ResetEvent(Report->Handles[1]);
        Ret = CreateChild(NullList, 2, TRUE, TRUE, NULL, &Pi, &Error);
        ok(Ret, "NULL entry: CreateProcess failed %lu\n", Error);
        if (Ret)
        {
            FinishChild(&Pi);
            ok(WaitForSingleObject(Report->Handles[0], 0) == WAIT_TIMEOUT &&
               WaitForSingleObject(Report->Handles[1], 0) == WAIT_TIMEOUT,
               "NULL entry did not select an empty table\n");
        }
    }
    TestRace();
    TestConsole();
    TestMovedStandardHandles();
    TestListCapacity();
    CloseHandle(Report->Handles[0]);
    CloseHandle(Report->Handles[1]);
    TestLargeList();
    /* Enlarging a Windows 7 console table can affect later console attaches. */
    TestGuiExit();
    UnmapViewOfFile(Report);
    CloseHandle(Mapping);
}
