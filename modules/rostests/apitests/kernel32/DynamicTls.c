/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     TLS of a DLL that is loaded after the process started
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ndk/pstypes.h>
#include <stdio.h>

typedef ULONG (WINAPI *FN_TlsDynGetValue)(VOID);
typedef VOID (WINAPI *FN_TlsDynSetValue)(ULONG);
typedef PCSTR (WINAPI *FN_TlsDynGetEvents)(VOID);
typedef VOID (WINAPI *FN_TlsDynSetLog)(char *, LONG *);
typedef ULONG (WINAPI *FN_TlsDynGetIndex)(VOID);
typedef PVOID (WINAPI *FN_TlsDynGetTemplate)(VOID);

#define TEMPLATE_VALUE 0x5EEDF00D
#define AFTER_UNLOAD_CODE 0x1234

static FN_TlsDynGetValue pTlsDynGetValue;
static FN_TlsDynSetValue pTlsDynSetValue;

/* A thread started before the first load that lives until the end, doing what it is told */
enum { CMD_GET, CMD_SET, CMD_EXIT };
static HANDLE CommandEvent, DoneEvent;
static LONG Command;
static ULONG CommandValue;

static DWORD WINAPI Worker(LPVOID Parameter)
{
    for (;;)
    {
        SetEvent(DoneEvent);
        WaitForSingleObject(CommandEvent, INFINITE);
        if (Command == CMD_EXIT)
            return 0;
        if (Command == CMD_GET)
            CommandValue = pTlsDynGetValue();
        else
            pTlsDynSetValue(CommandValue);
    }
}

static BOOL RunCommand(LONG Cmd, ULONG *Value)
{
    Command = Cmd;
    CommandValue = *Value;
    SetEvent(CommandEvent);
    if (WaitForSingleObject(DoneEvent, 10000) != WAIT_OBJECT_0)
        return FALSE;
    *Value = CommandValue;
    return TRUE;
}

static ULONG WorkerGet(VOID)
{
    ULONG Value = 0xDEAD;
    ok(RunCommand(CMD_GET, &Value), "The worker did not answer\n");
    return Value;
}

static VOID WorkerSet(ULONG Value)
{
    ok(RunCommand(CMD_SET, &Value), "The worker did not answer\n");
}

static DWORD WINAPI NewThread(LPVOID Parameter)
{
    return Parameter ? AFTER_UNLOAD_CODE : pTlsDynGetValue();
}

static DWORD RunThread(LPVOID Parameter)
{
    HANDLE hThread;
    DWORD Code = 0xDEAD;

    hThread = CreateThread(NULL, 0, NewThread, Parameter, 0, NULL);
    ok(hThread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!hThread)
        return Code;
    ok(WaitForSingleObject(hThread, 10000) == WAIT_OBJECT_0, "The thread did not finish\n");
    ok(GetExitCodeThread(hThread, &Code), "GetExitCodeThread failed: %lu\n", GetLastError());
    CloseHandle(hThread);
    return Code;
}

static HMODULE LoadTlsDll(VOID)
{
    HMODULE hDll;

    hDll = LoadLibraryW(L"kernel32_tlsdyn.dll");
    ok(hDll != NULL, "kernel32_tlsdyn.dll did not load: %lu\n", GetLastError());
    if (!hDll)
        return NULL;
    pTlsDynGetValue = (FN_TlsDynGetValue)GetProcAddress(hDll, "TlsDynGetValue");
    pTlsDynSetValue = (FN_TlsDynSetValue)GetProcAddress(hDll, "TlsDynSetValue");
    ok(pTlsDynGetValue && pTlsDynSetValue, "Exports missing\n");
    if (!pTlsDynGetValue || !pTlsDynSetValue)
    {
        FreeLibrary(hDll);
        return NULL;
    }
    return hDll;
}

/* Child process: the template becomes unreadable after the load, then a thread starts */
#define THREAD_HAD_BLOCK 1
#define THREAD_HAD_NO_BLOCK 2

static ULONG ChildTlsIndex;

static DWORD WINAPI CheckBlockThread(LPVOID Parameter)
{
    PVOID *TlsVector = (PVOID *)NtCurrentTeb()->ThreadLocalStoragePointer;
    return (TlsVector && TlsVector[ChildTlsIndex]) ? THREAD_HAD_BLOCK : THREAD_HAD_NO_BLOCK;
}

static VOID ProtectedTemplateChild(VOID)
{
    FN_TlsDynGetIndex pGetIndex;
    FN_TlsDynGetTemplate pGetTemplate;
    HANDLE hThread;
    HMODULE hDll;
    DWORD Old, Code = 0xDEAD;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    hDll = LoadLibraryW(L"kernel32_tlsdyn.dll");
    if (!hDll)
        ExitProcess(0xD1);
    pGetIndex = (FN_TlsDynGetIndex)GetProcAddress(hDll, "TlsDynGetIndex");
    pGetTemplate = (FN_TlsDynGetTemplate)GetProcAddress(hDll, "TlsDynGetTemplate");
    if (!pGetIndex || !pGetTemplate)
        ExitProcess(0xD2);
    ChildTlsIndex = pGetIndex();
    if (!VirtualProtect(pGetTemplate(), 1, PAGE_NOACCESS, &Old))
        ExitProcess(0xD3);
    hThread = CreateThread(NULL, 0, CheckBlockThread, NULL, 0, NULL);
    if (!hThread)
        ExitProcess(0xD4);
    WaitForSingleObject(hThread, 10000);
    GetExitCodeThread(hThread, &Code);
    ExitProcess(Code);
}

static VOID TestProtectedTemplate(PCSTR Self)
{
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    char CommandLine[MAX_PATH + 32];
    DWORD Code = 0;

    StringCchPrintfA(CommandLine, ARRAYSIZE(CommandLine), "\"%s\" DynamicTls protected", Self);
    if (!CreateProcessA(NULL, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcess failed: %lu\n", GetLastError());
        return;
    }
    if (WaitForSingleObject(ProcessInfo.hProcess, 30000) != WAIT_OBJECT_0)
    {
        trace("The child did not exit\n");
        TerminateProcess(ProcessInfo.hProcess, 0xDEAD);
        WaitForSingleObject(ProcessInfo.hProcess, 10000);
    }
    GetExitCodeProcess(ProcessInfo.hProcess, &Code);
    trace("Unreadable template, new thread: child exit code 0x%lx\n", Code);
    ok(Code < 0xD1 || Code > 0xD4, "The child failed to set up: 0x%lx\n", Code);
    /* The thread may fail to start, or the process may end: it must not run without its block */
    ok(Code != THREAD_HAD_NO_BLOCK, "A thread ran without its TLS block\n");
    CloseHandle(ProcessInfo.hProcess);
    CloseHandle(ProcessInfo.hThread);
}

/* Copies of the DLL whose TLS directory, or image size, wraps around the address space on i386 */
static VOID TestBadImage(PCWSTR DllPath, BOOL BadSize)
{
    WCHAR TempDir[MAX_PATH], Path[MAX_PATH];
    PIMAGE_DOS_HEADER DosHeader;
    PIMAGE_NT_HEADERS32 Nt32;
    PIMAGE_NT_HEADERS64 Nt64;
    PUCHAR Data = NULL;
    DWORD Size, Written;
    HANDLE hFile;
    HMODULE hDll;
    BOOL Ret;

    hFile = CreateFileW(DllPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "Cannot open %S: %lu\n", DllPath, GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    Size = GetFileSize(hFile, NULL);
    Data = HeapAlloc(GetProcessHeap(), 0, Size);
    Ret = Data && ReadFile(hFile, Data, Size, &Written, NULL) && Written == Size;
    CloseHandle(hFile);
    ok(Ret, "Cannot read %S\n", DllPath);
    if (!Ret)
        goto Exit;

    DosHeader = (PIMAGE_DOS_HEADER)Data;
    Nt32 = (PIMAGE_NT_HEADERS32)(Data + DosHeader->e_lfanew);
    Nt64 = (PIMAGE_NT_HEADERS64)Nt32;
    if (Nt32->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        /* Base plus RVA (or plus size) is 0 at the preferred base */
        ULONG Wrap = 0 - Nt32->OptionalHeader.ImageBase;
        if (BadSize)
            Nt32->OptionalHeader.SizeOfImage = Wrap;
        else
            Nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress = Wrap;
    }
    else
    {
        if (BadSize)
            Nt64->OptionalHeader.SizeOfImage = 0xF0000000;
        else
            Nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress = 0xFFFFF000;
    }

    GetTempPathW(ARRAYSIZE(TempDir), TempDir);
    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%stlsdyn_bad%u.dll", TempDir, BadSize);
    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "Cannot create %S: %lu\n", Path, GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        goto Exit;
    Ret = WriteFile(hFile, Data, Size, &Written, NULL);
    CloseHandle(hFile);
    ok(Ret, "Cannot write %S\n", Path);

    SetLastError(0xdeadbeef);
    hDll = LoadLibraryW(Path);
    trace("%s: LoadLibrary %p, error %lu\n", BadSize ? "Wrapping image size" : "Wrapping TLS directory", hDll, GetLastError());
    if (hDll)
    {
        /* A module that loads must have working TLS */
        FN_TlsDynGetValue pGetValue = (FN_TlsDynGetValue)GetProcAddress(hDll, "TlsDynGetValue");
        if (!BadSize)
            ok(!is_reactos(), "A TLS directory outside the image was accepted\n");
        else if (pGetValue)
            ok(pGetValue() == TEMPLATE_VALUE, "TLS of a module with a wrapping image size: 0x%lx\n", pGetValue());
        FreeLibrary(hDll);
    }
    else if (is_reactos() && !BadSize)
    {
        ok(GetLastError() == ERROR_BAD_EXE_FORMAT, "Error %lu\n", GetLastError());
    }
    DeleteFileW(Path);

Exit:
    HeapFree(GetProcessHeap(), 0, Data);
}

START_TEST(DynamicTls)
{
    FN_TlsDynGetEvents pTlsDynGetEvents;
    FN_TlsDynSetLog pTlsDynSetLog;
    static char HostLog[64];
    static LONG HostCount;
    HANDLE hWorker;
    HMODULE hDll;
    ULONG Value;
    PCSTR Events;
    size_t Length;
    WCHAR DllPath[MAX_PATH], *Slash;
    char **argv;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "protected"))
    {
        ProtectedTemplateChild();
        return;
    }

    CommandEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    DoneEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(CommandEvent && DoneEvent, "CreateEventW failed: %lu\n", GetLastError());
    if (!CommandEvent || !DoneEvent)
        return;

    /* A thread that has initialized (its TLS too) before the load */
    hWorker = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    ok(hWorker != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!hWorker)
        return;
    ok(WaitForSingleObject(DoneEvent, 10000) == WAIT_OBJECT_0, "The worker did not start\n");

    hDll = LoadTlsDll();
    if (!hDll)
        goto Cleanup;

    /* TLS callbacks run for the process attach, before DllMain */
    pTlsDynGetEvents = (FN_TlsDynGetEvents)GetProcAddress(hDll, "TlsDynGetEvents");
    pTlsDynSetLog = (FN_TlsDynSetLog)GetProcAddress(hDll, "TlsDynSetLog");
    ok(pTlsDynGetEvents && pTlsDynSetLog, "Exports missing\n");
    Events = pTlsDynGetEvents ? pTlsDynGetEvents() : "";
    ok(!strncmp(Events, "T1M1", 4), "Events at load: '%s'\n", Events);

    /* The loading thread sees the template */
    Value = pTlsDynGetValue();
    ok(Value == TEMPLATE_VALUE, "Loading thread: 0x%lx\n", Value);
    pTlsDynSetValue(0xAAAA);
    ok(pTlsDynGetValue() == 0xAAAA, "Loading thread after set: 0x%lx\n", pTlsDynGetValue());

    /* The thread that existed before the load has its own copy of the template */
    ok(WorkerGet() == TEMPLATE_VALUE, "Thread started before the load\n");
    WorkerSet(0x0D0D);
    ok(WorkerGet() == 0x0D0D, "Thread started before the load, after its set\n");
    ok(pTlsDynGetValue() == 0xAAAA, "The other thread's set changed this thread's copy: 0x%lx\n", pTlsDynGetValue());

    /* A thread started after the load gets the template too */
    Value = RunThread(NULL);
    ok(Value == TEMPLATE_VALUE, "Thread started after the load: 0x%lx\n", Value);

    /* The unload runs the TLS callbacks, then DllMain, for the process detach */
    if (pTlsDynSetLog)
        pTlsDynSetLog(HostLog, &HostCount);
    ok(FreeLibrary(hDll), "FreeLibrary failed: %lu\n", GetLastError());
    ok(GetModuleHandleW(L"kernel32_tlsdyn.dll") == NULL, "The DLL is still loaded\n");
    Length = strlen(HostLog);
    ok(Length >= 4 && !strcmp(HostLog + Length - 4, "T0M0"), "Events at unload: '%s'\n", HostLog);

    /* A thread started after the unload does not get the gone DLL's TLS */
    Value = RunThread((LPVOID)1);
    ok(Value == AFTER_UNLOAD_CODE, "Thread started after the unload: 0x%lx\n", Value);

    /* A second load starts from the template again, in the old thread and the loading one */
    hDll = LoadTlsDll();
    if (!hDll)
        goto Cleanup;
    ok(pTlsDynGetValue() == TEMPLATE_VALUE, "Loading thread after a reload: 0x%lx\n", pTlsDynGetValue());
    ok(WorkerGet() == TEMPLATE_VALUE, "Thread started before the first load, after a reload\n");
    WorkerSet(0x0E0E);
    ok(WorkerGet() == 0x0E0E, "Thread started before the first load, after a reload and a set\n");
    ok(pTlsDynGetValue() == TEMPLATE_VALUE, "The other thread's set changed this thread's copy: 0x%lx\n", pTlsDynGetValue());
    ok(FreeLibrary(hDll), "FreeLibrary failed: %lu\n", GetLastError());

    /* Failures */
    TestProtectedTemplate(argv[0]);
    GetModuleFileNameW(NULL, DllPath, ARRAYSIZE(DllPath));
    Slash = wcsrchr(DllPath, L'\\');
    if (Slash)
    {
        StringCchCopyW(Slash + 1, ARRAYSIZE(DllPath) - (Slash + 1 - DllPath), L"kernel32_tlsdyn.dll");
        TestBadImage(DllPath, FALSE);
        TestBadImage(DllPath, TRUE);
    }

Cleanup:
    Command = CMD_EXIT;
    SetEvent(CommandEvent);
    ok(WaitForSingleObject(hWorker, 10000) == WAIT_OBJECT_0, "The worker did not exit\n");
    CloseHandle(hWorker);
    CloseHandle(CommandEvent);
    CloseHandle(DoneEvent);
}
