/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for application restart and recovery registration
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef RESTART_MAX_CMD_LINE
#define RESTART_MAX_CMD_LINE 1024
#define RESTART_NO_CRASH 1
#define RESTART_NO_HANG 2
#define RESTART_NO_PATCH 4
#define RESTART_NO_REBOOT 8
#endif
#ifndef RECOVERY_DEFAULT_PING_INTERVAL
#define RECOVERY_DEFAULT_PING_INTERVAL 5000
#define RECOVERY_MAX_PING_INTERVAL (5 * 60 * 1000)
#endif

typedef DWORD (WINAPI *RECOVERY_CALLBACK)(PVOID);

#define HR_NOT_FOUND           HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
#define HR_INSUFFICIENT_BUFFER HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)

typedef HRESULT (WINAPI *FN_RegisterApplicationRestart)(PCWSTR, DWORD);
typedef HRESULT (WINAPI *FN_UnregisterApplicationRestart)(VOID);
typedef HRESULT (WINAPI *FN_GetApplicationRestartSettings)(HANDLE, PWSTR, PDWORD, PDWORD);
typedef HRESULT (WINAPI *FN_RegisterApplicationRecoveryCallback)(RECOVERY_CALLBACK, PVOID, DWORD, DWORD);
typedef HRESULT (WINAPI *FN_UnregisterApplicationRecoveryCallback)(VOID);
typedef HRESULT (WINAPI *FN_GetApplicationRecoveryCallback)(HANDLE, RECOVERY_CALLBACK *, PVOID *, PDWORD, PDWORD);
typedef HRESULT (WINAPI *FN_ApplicationRecoveryInProgress)(PBOOL);

static FN_RegisterApplicationRestart pRegisterApplicationRestart;
static FN_UnregisterApplicationRestart pUnregisterApplicationRestart;
static FN_GetApplicationRestartSettings pGetApplicationRestartSettings;
static FN_RegisterApplicationRecoveryCallback pRegisterApplicationRecoveryCallback;
static FN_UnregisterApplicationRecoveryCallback pUnregisterApplicationRecoveryCallback;
static FN_GetApplicationRecoveryCallback pGetApplicationRecoveryCallback;
static FN_ApplicationRecoveryInProgress pApplicationRecoveryInProgress;

#define CHILD_COMMAND_LINE L"/child restart \"x y\""

static DWORD WINAPI RecoveryCallback(PVOID Parameter)
{
    return 0;
}

static void TestRestartArguments(void)
{
    WCHAR Long[RESTART_MAX_CMD_LINE + 2];
    WCHAR Buffer[RESTART_MAX_CMD_LINE + 2];
    DWORD Size, Flags, i;
    HRESULT hr;
    static const DWORD OtherFlags[] = { 0x10, 0x20, 0x80000000, 0xFFFFFFFF };

    /* Flags are not checked: undefined ones are kept as given */
    for (i = 0; i < ARRAYSIZE(OtherFlags); i++)
    {
        hr = pRegisterApplicationRestart(L"x", OtherFlags[i]);
        ok(hr == S_OK, "Flags 0x%lx: hr 0x%lx\n", OtherFlags[i], hr);
        Size = ARRAYSIZE(Buffer);
        Flags = 0xdeadbeef;
        hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
        ok(hr == S_OK, "Flags 0x%lx: Get hr 0x%lx\n", OtherFlags[i], hr);
        ok(Flags == OtherFlags[i], "Flags 0x%lx returned as 0x%lx\n", OtherFlags[i], Flags);
    }
    hr = pRegisterApplicationRestart(L"x", RESTART_NO_CRASH | RESTART_NO_HANG | RESTART_NO_PATCH | RESTART_NO_REBOOT);
    ok(hr == S_OK, "All flags: hr 0x%lx\n", hr);
    Size = ARRAYSIZE(Buffer);
    Flags = 0xdeadbeef;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    ok(Flags == 15, "Flags 0x%lx\n", Flags);

    /* Command line length: counted without the terminator */
    for (i = 0; i < ARRAYSIZE(Long) - 1; i++)
        Long[i] = L'a' + (WCHAR)(i % 26);
    Long[RESTART_MAX_CMD_LINE - 1] = UNICODE_NULL;
    hr = pRegisterApplicationRestart(Long, 0);
    ok(hr == S_OK, "%u characters: hr 0x%lx\n", RESTART_MAX_CMD_LINE - 1, hr);
    Size = ARRAYSIZE(Buffer);
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, NULL);
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    ok(Size == RESTART_MAX_CMD_LINE, "Size %lu\n", Size);
    ok(hr == S_OK && !wcscmp(Buffer, Long), "Command line differs\n");

    Long[RESTART_MAX_CMD_LINE - 1] = L'z';
    Long[RESTART_MAX_CMD_LINE] = UNICODE_NULL;
    hr = pRegisterApplicationRestart(Long, 0);
    ok(hr == E_INVALIDARG, "%u characters: hr 0x%lx\n", RESTART_MAX_CMD_LINE, hr);
    Long[RESTART_MAX_CMD_LINE] = L'z';
    Long[RESTART_MAX_CMD_LINE + 1] = UNICODE_NULL;
    hr = pRegisterApplicationRestart(Long, 0);
    ok(hr == E_INVALIDARG, "%u characters: hr 0x%lx\n", RESTART_MAX_CMD_LINE + 1, hr);

    /* A failed registration keeps the previous one */
    Size = ARRAYSIZE(Buffer);
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, NULL);
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    ok(Size == RESTART_MAX_CMD_LINE, "Size %lu after failed registrations\n", Size);

    /* No command line and an empty one */
    hr = pRegisterApplicationRestart(NULL, RESTART_NO_HANG);
    ok(hr == S_OK, "NULL: hr 0x%lx\n", hr);
    Size = ARRAYSIZE(Buffer);
    Buffer[0] = L'?';
    Flags = 0xdeadbeef;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
    ok(hr == S_OK, "NULL: hr 0x%lx\n", hr);
    ok(Size == 1, "NULL: Size %lu\n", Size);
    ok(Buffer[0] == UNICODE_NULL, "NULL: Buffer 0x%x\n", Buffer[0]);
    ok(Flags == RESTART_NO_HANG, "NULL: Flags 0x%lx\n", Flags);

    hr = pRegisterApplicationRestart(L"", 0);
    ok(hr == S_OK, "Empty: hr 0x%lx\n", hr);
    Size = ARRAYSIZE(Buffer);
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, NULL);
    ok(hr == S_OK, "Empty: hr 0x%lx\n", hr);
    ok(Size == 1, "Empty: Size %lu\n", Size);

    hr = pUnregisterApplicationRestart();
    ok(hr == S_OK, "hr 0x%lx\n", hr);
}

static void TestRestart(void)
{
    WCHAR Buffer[64];
    DWORD Size, Flags;
    HRESULT hr;

    /* Nothing registered */
    hr = pUnregisterApplicationRestart();
    ok(hr == S_OK, "Unregister, nothing registered: hr 0x%lx\n", hr);
    Size = 0;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), NULL, &Size, NULL);
    ok(hr == HR_NOT_FOUND, "hr 0x%lx\n", hr);
    ok(Size == 0, "Size %lu\n", Size);
    Size = ARRAYSIZE(Buffer);
    Flags = 0xdeadbeef;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
    ok(hr == HR_NOT_FOUND, "hr 0x%lx\n", hr);
    ok(Size == 0, "Size %lu\n", Size);
    ok(Flags == 0xdeadbeef, "Flags 0x%lx\n", Flags);

    /* Without a buffer only the size can be asked for */
    Size = 100;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), NULL, &Size, NULL);
    ok(hr == E_INVALIDARG, "NULL buffer with size, nothing registered: hr 0x%lx\n", hr);
    ok(Size == 100, "NULL buffer with size, nothing registered: Size %lu\n", Size);

    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, NULL, &Flags);
    ok(hr == E_INVALIDARG, "No size: hr 0x%lx\n", hr);

    /* Registered */
    hr = pRegisterApplicationRestart(L"-a b", RESTART_NO_CRASH | RESTART_NO_HANG);
    ok(hr == S_OK, "hr 0x%lx\n", hr);

    /* What Firefox does: size query without a buffer */
    Size = 0;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), NULL, &Size, NULL);
    ok(hr == S_OK, "Size query: hr 0x%lx\n", hr);
    ok(Size == 5, "Size query: Size %lu\n", Size);

    Size = 4;
    wcscpy(Buffer, L"????????");
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, NULL);
    ok(hr == HR_INSUFFICIENT_BUFFER, "Small buffer: hr 0x%lx\n", hr);
    ok(Size == 5, "Small buffer: Size %lu\n", Size);
    ok(Buffer[0] == UNICODE_NULL, "Small buffer: Buffer[0] 0x%x\n", Buffer[0]);
    ok(Buffer[1] == L'?', "Small buffer: Buffer[1] 0x%x\n", Buffer[1]);

    Size = 0;
    wcscpy(Buffer, L"????????");
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, NULL);
    ok(hr == HR_INSUFFICIENT_BUFFER, "Zero size with buffer: hr 0x%lx\n", hr);
    ok(Size == 5, "Zero size with buffer: Size %lu\n", Size);
    /* Windows writes a terminator even into this zero-sized buffer; ReactOS writes nothing */
    if (is_reactos())
        ok(Buffer[0] == L'?', "Zero size with buffer: Buffer[0] 0x%x\n", Buffer[0]);
    else
        ok(Buffer[0] == UNICODE_NULL, "Zero size with buffer: Buffer[0] 0x%x\n", Buffer[0]);
    ok(Buffer[1] == L'?', "Zero size with buffer: Buffer[1] 0x%x\n", Buffer[1]);

    Size = 100;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), NULL, &Size, NULL);
    ok(hr == E_INVALIDARG, "NULL buffer with size: hr 0x%lx\n", hr);
    ok(Size == 100, "NULL buffer with size: Size %lu\n", Size);

    Size = 5;
    Flags = 0xdeadbeef;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    ok(Size == 5, "Size %lu\n", Size);
    ok(!wcscmp(Buffer, L"-a b"), "Buffer '%S'\n", Buffer);
    ok(Flags == (RESTART_NO_CRASH | RESTART_NO_HANG), "Flags 0x%lx\n", Flags);

    Size = ARRAYSIZE(Buffer);
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, NULL);
    ok(hr == S_OK, "Large buffer: hr 0x%lx\n", hr);
    ok(Size == 5, "Large buffer: Size %lu\n", Size);

    /* A second registration replaces the first */
    hr = pRegisterApplicationRestart(L"second", RESTART_NO_PATCH);
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    Size = ARRAYSIZE(Buffer);
    Flags = 0;
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, &Flags);
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    ok(!wcscmp(Buffer, L"second"), "Buffer '%S'\n", Buffer);
    ok(Flags == RESTART_NO_PATCH, "Flags 0x%lx\n", Flags);

    /* Unregistered */
    hr = pUnregisterApplicationRestart();
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    Size = ARRAYSIZE(Buffer);
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, NULL);
    ok(hr == HR_NOT_FOUND, "After unregister: hr 0x%lx\n", hr);
    hr = pUnregisterApplicationRestart();
    ok(hr == S_OK, "Second unregister: hr 0x%lx\n", hr);

    TestRestartArguments();
}

static void TestRestartHandles(void)
{
    WCHAR Buffer[64];
    DWORD Size;
    HANDLE hProcess;
    HRESULT hr;

    hr = pRegisterApplicationRestart(L"handles", 0);
    ok(hr == S_OK, "hr 0x%lx\n", hr);

    hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    ok(hProcess != NULL, "OpenProcess failed: %lu\n", GetLastError());
    if (hProcess)
    {
        Size = ARRAYSIZE(Buffer);
        hr = pGetApplicationRestartSettings(hProcess, Buffer, &Size, NULL);
        ok(hr == S_OK, "Own process handle: hr 0x%lx\n", hr);
        ok(hr == S_OK && !wcscmp(Buffer, L"handles"), "Own process handle: Buffer '%S'\n", Buffer);
        CloseHandle(hProcess);
    }

    hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    ok(hProcess != NULL, "OpenProcess failed: %lu\n", GetLastError());
    if (hProcess)
    {
        Size = ARRAYSIZE(Buffer);
        hr = pGetApplicationRestartSettings(hProcess, Buffer, &Size, NULL);
        ok(hr == E_ACCESSDENIED, "Own process, PROCESS_QUERY_INFORMATION only: hr 0x%lx\n", hr);
        CloseHandle(hProcess);
    }

    hProcess = OpenProcess(PROCESS_VM_READ, FALSE, GetCurrentProcessId());
    ok(hProcess != NULL, "OpenProcess failed: %lu\n", GetLastError());
    if (hProcess)
    {
        Size = ARRAYSIZE(Buffer);
        hr = pGetApplicationRestartSettings(hProcess, Buffer, &Size, NULL);
        /* Windows x64 gives ERROR_INVALID_HANDLE, WOW64 the NT status as an HRESULT */
        ok(FAILED(hr), "Own process, PROCESS_VM_READ only: hr 0x%lx\n", hr);
        CloseHandle(hProcess);
    }

    Size = ARRAYSIZE(Buffer);
    hr = pGetApplicationRestartSettings(NULL, Buffer, &Size, NULL);
    ok(hr == E_INVALIDARG, "NULL handle: hr 0x%lx\n", hr);

    hr = pUnregisterApplicationRestart();
    ok(hr == S_OK, "hr 0x%lx\n", hr);
}

static void TestRestartOtherProcess(const char *Self)
{
    WCHAR Path[MAX_PATH], CommandLine[MAX_PATH * 2], Buffer[64];
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    SECURITY_ATTRIBUTES Inherit = { sizeof(Inherit), NULL, TRUE };
    HANDLE hReady, hDone;
    DWORD Size, Flags;
    HRESULT hr;

    hReady = CreateEventW(&Inherit, TRUE, FALSE, NULL);
    hDone = CreateEventW(&Inherit, TRUE, FALSE, NULL);
    ok(hReady && hDone, "CreateEvent failed: %lu\n", GetLastError());
    if (!hReady || !hDone)
        return;

    GetModuleFileNameW(NULL, Path, ARRAYSIZE(Path));
    StringCchPrintfW(CommandLine, ARRAYSIZE(CommandLine), L"\"%s\" ApplicationRestart child %Iu %Iu",
                     Path, (ULONG_PTR)hReady, (ULONG_PTR)hDone);
    if (!CreateProcessW(Path, CommandLine, NULL, NULL, TRUE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcess failed: %lu\n", GetLastError());
        CloseHandle(hReady);
        CloseHandle(hDone);
        return;
    }

    ok(WaitForSingleObject(hReady, 30000) == WAIT_OBJECT_0, "Child did not register\n");
    Size = ARRAYSIZE(Buffer);
    Flags = 0xdeadbeef;
    Buffer[0] = UNICODE_NULL;
    hr = pGetApplicationRestartSettings(ProcessInfo.hProcess, Buffer, &Size, &Flags);
    trace("Other process: hr 0x%lx Size %lu Flags 0x%lx Buffer '%S'\n", hr, Size, Flags, Buffer);
    if (is_reactos())
    {
        /* The registration of another process cannot be read here */
        ok(hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Other process: hr 0x%lx\n", hr);
    }
    else
    {
        ok(hr == S_OK, "Other process: hr 0x%lx\n", hr);
        ok(!wcscmp(Buffer, CHILD_COMMAND_LINE), "Other process: Buffer '%S'\n", Buffer);
        ok(Flags == RESTART_NO_PATCH, "Other process: Flags 0x%lx\n", Flags);
    }

    /* Our own registration is not the child's */
    Size = ARRAYSIZE(Buffer);
    hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, NULL);
    ok(hr == HR_NOT_FOUND, "Own process while the child is registered: hr 0x%lx\n", hr);

    SetEvent(hDone);
    ok(WaitForSingleObject(ProcessInfo.hProcess, 30000) == WAIT_OBJECT_0, "Child did not exit\n");
    CloseHandle(ProcessInfo.hProcess);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(hReady);
    CloseHandle(hDone);
}

static void TestRecovery(void)
{
    RECOVERY_CALLBACK Callback;
    PVOID Parameter;
    DWORD PingInterval, Flags;
    BOOL Cancelled;
    HRESULT hr;

    /* Nothing registered */
    hr = pUnregisterApplicationRecoveryCallback();
    ok(hr == S_OK, "Unregister, nothing registered: hr 0x%lx\n", hr);
    Callback = (RECOVERY_CALLBACK)(ULONG_PTR)0xdeadbeef;
    Parameter = (PVOID)(ULONG_PTR)0xdeadbeef;
    PingInterval = Flags = 0xdeadbeef;
    hr = pGetApplicationRecoveryCallback(GetCurrentProcess(), &Callback, &Parameter, &PingInterval, &Flags);
    ok(hr == S_OK, "Nothing registered: hr 0x%lx\n", hr);
    ok(Callback == NULL && Parameter == NULL && PingInterval == 0 && Flags == 0,
       "Nothing registered: Callback %p Parameter %p PingInterval %lu Flags 0x%lx\n", Callback, Parameter, PingInterval, Flags);

    /* Arguments */
    hr = pRegisterApplicationRecoveryCallback(NULL, NULL, 0, 0);
    ok(hr == E_INVALIDARG, "NULL callback: hr 0x%lx\n", hr);
    hr = pRegisterApplicationRecoveryCallback(RecoveryCallback, NULL, RECOVERY_MAX_PING_INTERVAL + 1, 0);
    ok(hr == E_INVALIDARG, "Ping interval above the maximum: hr 0x%lx\n", hr);
    /* Flags are not checked */
    hr = pRegisterApplicationRecoveryCallback(RecoveryCallback, NULL, 0, 1);
    ok(hr == S_OK, "Flags 1: hr 0x%lx\n", hr);
    hr = pRegisterApplicationRecoveryCallback(RecoveryCallback, NULL, 0, 0x80000000);
    ok(hr == S_OK, "Flags 0x80000000: hr 0x%lx\n", hr);
    Flags = 0;
    hr = pGetApplicationRecoveryCallback(GetCurrentProcess(), &Callback, NULL, NULL, &Flags);
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    ok(Flags == 0x80000000, "Flags 0x%lx\n", Flags);

    /* Ping interval 0: the default */
    hr = pRegisterApplicationRecoveryCallback(RecoveryCallback, (PVOID)(ULONG_PTR)0x1234, 0, 0);
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    Callback = NULL;
    Parameter = NULL;
    PingInterval = Flags = 0xdeadbeef;
    hr = pGetApplicationRecoveryCallback(GetCurrentProcess(), &Callback, &Parameter, &PingInterval, &Flags);
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    ok(Callback == RecoveryCallback, "Callback %p, expected %p\n", Callback, RecoveryCallback);
    ok(Parameter == (PVOID)(ULONG_PTR)0x1234, "Parameter %p\n", Parameter);
    ok(PingInterval == RECOVERY_DEFAULT_PING_INTERVAL, "PingInterval %lu\n", PingInterval);
    ok(Flags == 0, "Flags 0x%lx\n", Flags);

    hr = pRegisterApplicationRecoveryCallback(RecoveryCallback, NULL, RECOVERY_MAX_PING_INTERVAL, 0);
    ok(hr == S_OK, "Maximum ping interval: hr 0x%lx\n", hr);
    hr = pGetApplicationRecoveryCallback(GetCurrentProcess(), &Callback, &Parameter, &PingInterval, &Flags);
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    ok(Parameter == NULL, "Parameter %p\n", Parameter);
    ok(PingInterval == RECOVERY_MAX_PING_INTERVAL, "PingInterval %lu\n", PingInterval);

    /* Optional outputs */
    Callback = NULL;
    hr = pGetApplicationRecoveryCallback(GetCurrentProcess(), &Callback, NULL, NULL, NULL);
    ok(hr == S_OK, "Callback only: hr 0x%lx\n", hr);
    ok(Callback == RecoveryCallback, "Callback %p\n", Callback);
    hr = pGetApplicationRecoveryCallback(GetCurrentProcess(), NULL, NULL, NULL, NULL);
    ok(hr == E_INVALIDARG, "No callback output: hr 0x%lx\n", hr);
    hr = pGetApplicationRecoveryCallback(NULL, &Callback, NULL, NULL, NULL);
    ok(hr == E_INVALIDARG, "NULL handle: hr 0x%lx\n", hr);

    /* Outside a recovery */
    Cancelled = 0xdead;
    hr = pApplicationRecoveryInProgress(&Cancelled);
    ok(hr == E_FAIL, "ApplicationRecoveryInProgress outside a recovery: hr 0x%lx\n", hr);
    ok(Cancelled == FALSE, "ApplicationRecoveryInProgress outside a recovery: Cancelled %d\n", Cancelled);
    hr = pApplicationRecoveryInProgress(NULL);
    ok(hr == E_INVALIDARG, "ApplicationRecoveryInProgress(NULL): hr 0x%lx\n", hr);

    /* Unregistered */
    hr = pUnregisterApplicationRecoveryCallback();
    ok(hr == S_OK, "hr 0x%lx\n", hr);
    Callback = (RECOVERY_CALLBACK)(ULONG_PTR)0xdeadbeef;
    Parameter = (PVOID)(ULONG_PTR)0xdeadbeef;
    PingInterval = Flags = 0xdeadbeef;
    hr = pGetApplicationRecoveryCallback(GetCurrentProcess(), &Callback, &Parameter, &PingInterval, &Flags);
    ok(hr == S_OK, "After unregister: hr 0x%lx\n", hr);
    ok(Callback == NULL && Parameter == NULL && PingInterval == 0 && Flags == 0,
       "After unregister: Callback %p Parameter %p PingInterval %lu Flags 0x%lx\n", Callback, Parameter, PingInterval, Flags);
    hr = pUnregisterApplicationRecoveryCallback();
    ok(hr == S_OK, "Second unregister: hr 0x%lx\n", hr);

    Cancelled = 0xdead;
    hr = pApplicationRecoveryInProgress(&Cancelled);
    ok(hr == E_FAIL, "ApplicationRecoveryInProgress, not registered: hr 0x%lx\n", hr);
    ok(Cancelled == FALSE, "ApplicationRecoveryInProgress, not registered: Cancelled %d\n", Cancelled);
}

/* ReactOS's kernel32_vista.dll may export the functions too: a process has one registration */
static void TestKernel32Vista(void)
{
    FN_GetApplicationRestartSettings pVistaGet;
    FN_UnregisterApplicationRestart pVistaUnregister;
    FN_RegisterApplicationRestart pVistaRegister;
    WCHAR Buffer[64];
    HMODULE hVista;
    DWORD Size;
    HRESULT hr;

    hVista = LoadLibraryW(L"kernel32_vista.dll");
    if (!hVista)
    {
        skip("No kernel32_vista.dll\n");
        return;
    }
    pVistaRegister = (FN_RegisterApplicationRestart)GetProcAddress(hVista, "RegisterApplicationRestart");
    pVistaUnregister = (FN_UnregisterApplicationRestart)GetProcAddress(hVista, "UnregisterApplicationRestart");
    pVistaGet = (FN_GetApplicationRestartSettings)GetProcAddress(hVista, "GetApplicationRestartSettings");
    /* It exports them only where kernel32 does not (below export version 0x600) */
    if (!pVistaRegister || !pVistaUnregister || !pVistaGet)
        skip("kernel32_vista.dll leaves these functions to kernel32\n");
    else
    {
        hr = pRegisterApplicationRestart(L"through kernel32", 0);
        ok(hr == S_OK, "hr 0x%lx\n", hr);
        Size = ARRAYSIZE(Buffer);
        hr = pVistaGet(GetCurrentProcess(), Buffer, &Size, NULL);
        ok(hr == S_OK, "kernel32_vista.dll does not see the registration: hr 0x%lx\n", hr);
        ok(hr == S_OK && !wcscmp(Buffer, L"through kernel32"), "Buffer '%S'\n", Buffer);
        hr = pVistaUnregister();
        ok(hr == S_OK, "hr 0x%lx\n", hr);
        Size = ARRAYSIZE(Buffer);
        hr = pGetApplicationRestartSettings(GetCurrentProcess(), Buffer, &Size, NULL);
        ok(hr == HR_NOT_FOUND, "kernel32 still has the registration: hr 0x%lx\n", hr);
    }
    FreeLibrary(hVista);
}

static void Child(char **argv)
{
    HANDLE hReady = (HANDLE)(ULONG_PTR)_strtoui64(argv[3], NULL, 10);
    HANDLE hDone = (HANDLE)(ULONG_PTR)_strtoui64(argv[4], NULL, 10);

    pRegisterApplicationRestart(CHILD_COMMAND_LINE, RESTART_NO_PATCH);
    SetEvent(hReady);
    WaitForSingleObject(hDone, 60000);
}

START_TEST(ApplicationRestart)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    char **argv;
    int argc;

    pRegisterApplicationRestart = (FN_RegisterApplicationRestart)GetProcAddress(hKernel32, "RegisterApplicationRestart");
    pUnregisterApplicationRestart = (FN_UnregisterApplicationRestart)GetProcAddress(hKernel32, "UnregisterApplicationRestart");
    pGetApplicationRestartSettings = (FN_GetApplicationRestartSettings)GetProcAddress(hKernel32, "GetApplicationRestartSettings");
    pRegisterApplicationRecoveryCallback = (FN_RegisterApplicationRecoveryCallback)GetProcAddress(hKernel32, "RegisterApplicationRecoveryCallback");
    pUnregisterApplicationRecoveryCallback = (FN_UnregisterApplicationRecoveryCallback)GetProcAddress(hKernel32, "UnregisterApplicationRecoveryCallback");
    pGetApplicationRecoveryCallback = (FN_GetApplicationRecoveryCallback)GetProcAddress(hKernel32, "GetApplicationRecoveryCallback");
    pApplicationRecoveryInProgress = (FN_ApplicationRecoveryInProgress)GetProcAddress(hKernel32, "ApplicationRecoveryInProgress");

    if (!pRegisterApplicationRestart && is_reactos() && !GetProcAddress(hKernel32, "GetTickCount64"))
    {
        skip("No NT 6 exports\n");
        return;
    }
    if (!pRegisterApplicationRestart || !pUnregisterApplicationRestart || !pGetApplicationRestartSettings ||
        !pRegisterApplicationRecoveryCallback || !pUnregisterApplicationRecoveryCallback ||
        !pGetApplicationRecoveryCallback || !pApplicationRecoveryInProgress)
    {
        ok(0, "Missing exports: %p %p %p %p %p %p %p\n", pRegisterApplicationRestart, pUnregisterApplicationRestart,
           pGetApplicationRestartSettings, pRegisterApplicationRecoveryCallback, pUnregisterApplicationRecoveryCallback,
           pGetApplicationRecoveryCallback, pApplicationRecoveryInProgress);
        return;
    }

    argc = winetest_get_mainargs(&argv);
    if (argc >= 5 && !strcmp(argv[2], "child"))
    {
        Child(argv);
        return;
    }

    TestRestart();
    TestRestartHandles();
    TestRestartOtherProcess(argv[0]);
    TestRecovery();
    TestKernel32Vista();
}
