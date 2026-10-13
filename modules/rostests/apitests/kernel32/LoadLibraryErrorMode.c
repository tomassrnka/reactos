/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for SetThreadErrorMode and GetThreadErrorMode around LoadLibraryExW
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef BOOL (WINAPI *PFN_SET_THREAD_ERROR_MODE)(DWORD, LPDWORD);
typedef DWORD (WINAPI *PFN_GET_THREAD_ERROR_MODE)(VOID);
typedef ULONG (NTAPI *PFN_RTL_GET_THREAD_ERROR_MODE)(VOID);

static PFN_SET_THREAD_ERROR_MODE pSetThreadErrorMode;
static PFN_GET_THREAD_ERROR_MODE pGetThreadErrorMode;
static PFN_RTL_GET_THREAD_ERROR_MODE pRtlGetThreadErrorMode;

static
VOID
TestModes(VOID)
{
    static const struct
    {
        DWORD Mode;
        ULONG NativeMode;
    } Modes[] =
    {
        { 0, 0 },
        { SEM_FAILCRITICALERRORS, 0x10 },
        { SEM_NOGPFAULTERRORBOX, 0x20 },
        { SEM_NOOPENFILEERRORBOX, 0x40 },
        { SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX, 0x70 },
    };
    DWORD Mode, OldMode, Bit, Error;
    ULONG i;
    BOOL Ret;

    for (i = 0; i < _countof(Modes); i++)
    {
        Mode = pGetThreadErrorMode();
        OldMode = 0xdeadbeef;
        SetLastError(0xdeadbeef);
        Ret = pSetThreadErrorMode(Modes[i].Mode, &OldMode);
        Error = GetLastError();
        ok(Ret == TRUE, "[%lu] SetThreadErrorMode returned %d, error %lu\n", i, Ret, Error);
        ok(Error == 0xdeadbeef, "[%lu] Last error %lu\n", i, Error);
        ok(OldMode == Mode, "[%lu] Old mode 0x%lx, expected 0x%lx\n", i, OldMode, Mode);
        ok(pGetThreadErrorMode() == Modes[i].Mode, "[%lu] Mode 0x%lx\n", i, pGetThreadErrorMode());
        if (pRtlGetThreadErrorMode)
            ok(pRtlGetThreadErrorMode() == Modes[i].NativeMode,
               "[%lu] Native mode 0x%lx, expected 0x%lx\n", i, pRtlGetThreadErrorMode(), Modes[i].NativeMode);
    }

    /* The old mode is optional */
    Ret = pSetThreadErrorMode(SEM_NOGPFAULTERRORBOX, NULL);
    ok(Ret == TRUE, "SetThreadErrorMode returned %d\n", Ret);
    ok(pGetThreadErrorMode() == SEM_NOGPFAULTERRORBOX, "Mode 0x%lx\n", pGetThreadErrorMode());

    /* Every other bit is rejected and nothing changes */
    for (Bit = 1; Bit; Bit <<= 1)
    {
        if (Bit & (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX))
            continue;
        OldMode = 0xdeadbeef;
        SetLastError(0xdeadbeef);
        Ret = pSetThreadErrorMode(Bit | SEM_FAILCRITICALERRORS, &OldMode);
        Error = GetLastError();
        ok(Ret == FALSE, "Mode 0x%lx: SetThreadErrorMode returned %d\n", Bit, Ret);
        ok(Error == ERROR_INVALID_PARAMETER, "Mode 0x%lx: error %lu\n", Bit, Error);
        ok(OldMode == 0xdeadbeef, "Mode 0x%lx: old mode 0x%lx\n", Bit, OldMode);
        ok(pGetThreadErrorMode() == SEM_NOGPFAULTERRORBOX, "Mode 0x%lx: mode 0x%lx\n", Bit, pGetThreadErrorMode());
    }
}

static DWORD ThreadInitialMode, ThreadModeAfterSet;

static
DWORD
WINAPI
ModeThread(LPVOID Param)
{
    ThreadInitialMode = pGetThreadErrorMode();
    pSetThreadErrorMode(SEM_NOOPENFILEERRORBOX, NULL);
    ThreadModeAfterSet = pGetThreadErrorMode();
    return 0;
}

static
VOID
TestPerThread(VOID)
{
    HANDLE Thread;
    UINT ProcessMode;

    ok(pSetThreadErrorMode(SEM_FAILCRITICALERRORS, NULL), "SetThreadErrorMode failed\n");

    /* The mode belongs to the thread: a new thread starts without it and cannot change ours */
    ThreadInitialMode = ThreadModeAfterSet = 0xdeadbeef;
    Thread = CreateThread(NULL, 0, ModeThread, NULL, 0, NULL);
    ok(Thread != NULL, "CreateThread failed, error %lu\n", GetLastError());
    if (Thread)
    {
        ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "Thread did not end\n");
        CloseHandle(Thread);
        ok(ThreadInitialMode == 0, "New thread mode 0x%lx\n", ThreadInitialMode);
        ok(ThreadModeAfterSet == SEM_NOOPENFILEERRORBOX, "Thread mode after set 0x%lx\n", ThreadModeAfterSet);
    }
    ok(pGetThreadErrorMode() == SEM_FAILCRITICALERRORS, "Mode 0x%lx\n", pGetThreadErrorMode());

    /* The process mode is separate */
    ProcessMode = SetErrorMode(SEM_NOGPFAULTERRORBOX);
    ok(pGetThreadErrorMode() == SEM_FAILCRITICALERRORS, "Mode after SetErrorMode 0x%lx\n", pGetThreadErrorMode());
    ok(pSetThreadErrorMode(SEM_NOOPENFILEERRORBOX, NULL), "SetThreadErrorMode failed\n");
    /* SEM_NOALIGNMENTFAULTEXCEPT cannot be cleared once inherited */
    ok(SetErrorMode(ProcessMode) == (SEM_NOGPFAULTERRORBOX | (ProcessMode & SEM_NOALIGNMENTFAULTEXCEPT)),
       "Process mode changed\n");
}

/* VLC 3.0 loads every plugin like this and skips the plugin when SetThreadErrorMode fails */
static
VOID
TestLoadSequence(VOID)
{
    WCHAR Path[MAX_PATH], Dir[MAX_PATH], Missing[MAX_PATH];
    DWORD OldMode, Error;
    HMODULE Module, Other;
    UINT Length;
    BOOL Ret, HaveDir;

    ok(pSetThreadErrorMode(SEM_NOGPFAULTERRORBOX, NULL), "SetThreadErrorMode failed\n");

    Length = GetSystemDirectoryW(Path, _countof(Path));
    ok(Length != 0 && Length < _countof(Path) - 16, "GetSystemDirectoryW returned %u\n", Length);
    if (Length == 0 || Length >= _countof(Path) - 16)
        return;
    StringCchCatW(Path, _countof(Path), L"\\version.dll");

    OldMode = 0xdeadbeef;
    Ret = pSetThreadErrorMode(SEM_FAILCRITICALERRORS, &OldMode);
    ok(Ret != FALSE, "SetThreadErrorMode returned %d, error %lu\n", Ret, GetLastError());
    ok(OldMode == SEM_NOGPFAULTERRORBOX, "Old mode 0x%lx\n", OldMode);
    if (!Ret)
        return;

    Module = LoadLibraryExW(Path, NULL, 0);
    ok(Module != NULL, "LoadLibraryExW(%ls) failed, error %lu\n", Path, GetLastError());
    ok(pGetThreadErrorMode() == SEM_FAILCRITICALERRORS, "Mode during the load 0x%lx\n", pGetThreadErrorMode());

    /* A module that is not there fails quietly */
    Length = GetTempPathW(_countof(Dir), Dir);
    HaveDir = Length != 0 && Length < _countof(Dir) &&
              SUCCEEDED(StringCchPrintfW(Dir + Length, _countof(Dir) - Length,
                                         L"LoadLibraryErrorMode-%lu", GetCurrentProcessId())) &&
              (CreateDirectoryW(Dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) &&
              SUCCEEDED(StringCchPrintfW(Missing, _countof(Missing), L"%ls\\missing.dll", Dir));
    ok(HaveDir, "Could not create a directory for the missing module, error %lu\n", GetLastError());
    if (HaveDir)
    {
        SetLastError(0xdeadbeef);
        Other = LoadLibraryExW(Missing, NULL, 0);
        Error = GetLastError();
        ok(Other == NULL, "LoadLibraryExW(%ls) succeeded\n", Missing);
        ok(Error == ERROR_MOD_NOT_FOUND, "Error %lu\n", Error);
        if (Other)
            FreeLibrary(Other);
        RemoveDirectoryW(Dir);
    }

    ok(pSetThreadErrorMode(OldMode, NULL), "SetThreadErrorMode failed\n");
    ok(pGetThreadErrorMode() == SEM_NOGPFAULTERRORBOX, "Restored mode 0x%lx\n", pGetThreadErrorMode());

    if (Module)
        FreeLibrary(Module);
}

START_TEST(LoadLibraryErrorMode)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    DWORD SavedMode;

    pSetThreadErrorMode = (PFN_SET_THREAD_ERROR_MODE)GetProcAddress(Kernel32, "SetThreadErrorMode");
    pGetThreadErrorMode = (PFN_GET_THREAD_ERROR_MODE)GetProcAddress(Kernel32, "GetThreadErrorMode");
    pRtlGetThreadErrorMode = (PFN_RTL_GET_THREAD_ERROR_MODE)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                                           "RtlGetThreadErrorMode");
    if (!pSetThreadErrorMode || !pGetThreadErrorMode)
    {
        skip("SetThreadErrorMode or GetThreadErrorMode is not exported\n");
        return;
    }

    SavedMode = pGetThreadErrorMode();
    TestModes();
    TestPerThread();
    TestLoadSequence();
    pSetThreadErrorMode(SavedMode, NULL);
}
