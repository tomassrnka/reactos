/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY in an attribute list
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Expectations were measured on Windows Server 2008 R2 (6.1.7601) and Windows 10 22H2:
 * a list entry is a DWORD on Windows 7 and may also be a DWORD64 from Windows 8 on and
 * two of them from Windows 10 1703 on; any other size is ERROR_BAD_LENGTH. Update accepts
 * any value, and CreateProcess refuses a DWORD with a bit that is not a defined
 * mitigation. ReactOS refuses a request for a mitigation its child cannot get
 * (see BASEP_HONOURED_MITIGATIONS in kernel32), measured with the no-execute probe in
 * the lane notes: DEP holds on amd64 only, ATL thunk emulation and SEHOP never.
 */

#include "precomp.h"
#include <ndk/rtlfuncs.h>

#ifndef EXTENDED_STARTUPINFO_PRESENT
#define EXTENDED_STARTUPINFO_PRESENT 0x00080000
#endif
#ifndef PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY
#define PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY 0x00020007
#endif
#ifndef PROC_THREAD_ATTRIBUTE_HANDLE_LIST
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x00020002
#endif

typedef BOOL (WINAPI *INITIALIZE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD, PSIZE_T);
typedef BOOL (WINAPI *UPDATE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR,
                                 PVOID, SIZE_T, PVOID, PSIZE_T);
typedef VOID (WINAPI *DELETE_LIST)(LPPROC_THREAD_ATTRIBUTE_LIST);

static INITIALIZE_LIST pInitialize;
static UPDATE_LIST pUpdate;
static DELETE_LIST pDelete;
static WCHAR Image[MAX_PATH];
static BOOL IsReactOS, IsWin8, IsWin10_1703;
static HANDLE Inheritable;

/* DEP holds for a child wherever the processor enforces no-execute: on an amd64 kernel for a
 * 64-bit process, and for a 32-bit one under WOW64, and nowhere on an i386 kernel. */
static DWORD HonouredMitigations;

static DWORD DetectHonouredMitigations(void)
{
#ifdef _WIN64
    return 0x1;
#else
    /* A 32-bit process honours DEP exactly when it runs on an amd64 kernel (under WOW64).
     * Read the kernel's own architecture from the shared data at offset 0x2c
     * (KUSER_SHARED_DATA.ImageNumberLow) - the same signal the kernel32 validation uses,
     * so the test and the code can never disagree. */
    USHORT KernelMachine = *(volatile USHORT *)(ULONG_PTR)(KUSER_SHARED_DATA_UMPTR + 0x2C);
    return (KernelMachine == IMAGE_FILE_MACHINE_AMD64) ? 0x1 : 0x0;
#endif
}

/* Returns the error of the failing step, or ERROR_SUCCESS when the child ran and exited with 0.
 * With WithHandleList the same list also holds a handle-list entry (the inheritable event). */
static DWORD RunWithPolicy(const void *Value, SIZE_T Size, BOOL WithHandleList, BOOL *UpdateFailed)
{
    SIZE_T ListSize = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST List;
    STARTUPINFOEXW Startup;
    PROCESS_INFORMATION Info;
    WCHAR CommandLine[MAX_PATH + 64];
    DWORD Error, ExitCode = 0xdead, Count = WithHandleList ? 2 : 1;
    HANDLE Handles[1];
    BOOL Ret;

    pInitialize(NULL, Count, 0, &ListSize);
    List = HeapAlloc(GetProcessHeap(), 0, ListSize);
    if (!List) return ERROR_OUTOFMEMORY;
    Ret = pInitialize(List, Count, 0, &ListSize);
    ok(Ret, "InitializeProcThreadAttributeList: %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    Ret = pUpdate(List, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, (PVOID)Value, Size, NULL, NULL);
    *UpdateFailed = !Ret;
    if (Ret && WithHandleList)
    {
        Handles[0] = Inheritable;
        Ret = pUpdate(List, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, Handles, sizeof(Handles), NULL, NULL);
        ok(Ret, "handle list Update: %lu\n", GetLastError());
    }
    if (!Ret)
    {
        Error = GetLastError();
        pDelete(List);
        HeapFree(GetProcessHeap(), 0, List);
        return Error;
    }

    ZeroMemory(&Startup, sizeof(Startup));
    ZeroMemory(&Info, sizeof(Info));
    Startup.StartupInfo.cb = sizeof(Startup);
    Startup.lpAttributeList = List;
    StringCchPrintfW(CommandLine, _countof(CommandLine), L"\"%ls\" ProcThreadMitigation child", Image);
    SetLastError(0xdeadbeef);
    Ret = CreateProcessW(NULL, CommandLine, NULL, NULL, WithHandleList, EXTENDED_STARTUPINFO_PRESENT,
                         NULL, NULL, &Startup.StartupInfo, &Info);
    Error = Ret ? ERROR_SUCCESS : GetLastError();
    if (Ret)
    {
        ok(WaitForSingleObject(Info.hProcess, 60000) == WAIT_OBJECT_0, "the child did not exit\n");
        GetExitCodeProcess(Info.hProcess, &ExitCode);
        ok(ExitCode == 0, "child exit code %lu\n", ExitCode);
        CloseHandle(Info.hThread);
        CloseHandle(Info.hProcess);
    }
    pDelete(List);
    HeapFree(GetProcessHeap(), 0, List);
    return Error;
}

/* What CreateProcess says to a DWORD policy */
static DWORD DwordError(DWORD Value)
{
    if (IsReactOS)
        return (Value & ~HonouredMitigations) ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
    return (Value & ~7UL) ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
}

static VOID ExpectDword(DWORD Value, BOOL WithHandleList)
{
    BOOL UpdateFailed;
    DWORD Expected = DwordError(Value);
    DWORD Error = RunWithPolicy(&Value, sizeof(Value), WithHandleList, &UpdateFailed);

    ok(!UpdateFailed, "0x%lx: Update failed with %lu\n", Value, Error);
    ok(Error == Expected, "0x%lx (handle list %d): CreateProcess error %lu, expected %lu\n",
       Value, WithHandleList, Error, Expected);
}

static VOID ExpectSize(SIZE_T Size, DWORD ExpectedError)
{
    ULONGLONG Value[2] = {0, 0};
    BOOL UpdateFailed;
    DWORD Error = RunWithPolicy(Value, Size, FALSE, &UpdateFailed);

    ok(Error == ExpectedError, "size %lu: error %lu, expected %lu\n", (ULONG)Size, Error, ExpectedError);
    ok(UpdateFailed == (ExpectedError != ERROR_SUCCESS),
       "size %lu: Update failed %d for expected error %lu\n", (ULONG)Size, UpdateFailed, ExpectedError);
}

/* ReactOS reads the whole declared width and refuses a request it cannot honour */
static VOID ExpectWide(SIZE_T Size, ULONGLONG First, ULONGLONG Second, DWORD ExpectedError)
{
    ULONGLONG Value[2];
    BOOL UpdateFailed;
    DWORD Error;

    Value[0] = First;
    Value[1] = Second;
    Error = RunWithPolicy(Value, Size, FALSE, &UpdateFailed);
    ok(!UpdateFailed, "size %lu: Update failed with %lu\n", (ULONG)Size, Error);
    ok(Error == ExpectedError, "size %lu, 0x%I64x 0x%I64x: error %lu, expected %lu\n",
       (ULONG)Size, First, Second, Error, ExpectedError);
}

static VOID ExpectBadPointer(SIZE_T Size)
{
    BOOL UpdateFailed;
    DWORD Error = RunWithPolicy((PVOID)(ULONG_PTR)0x10, Size, FALSE, &UpdateFailed);

    ok(!UpdateFailed, "size %lu: Update failed with %lu\n", (ULONG)Size, Error);
    ok(Error == ERROR_NOACCESS, "size %lu, bad pointer: error %lu\n", (ULONG)Size, Error);
}

START_TEST(ProcThreadMitigation)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    RTL_OSVERSIONINFOW Version = {sizeof(Version)};
    SECURITY_ATTRIBUTES Sa = {sizeof(Sa), NULL, TRUE};
    char **Argv;
    int Argc = winetest_get_mainargs(&Argv);
    DWORD EightByteError, SixteenByteError;

    if (Argc == 3 && !strcmp(Argv[2], "child")) ExitProcess(0);
    pInitialize = (INITIALIZE_LIST)GetProcAddress(Kernel32, "InitializeProcThreadAttributeList");
    pUpdate = (UPDATE_LIST)GetProcAddress(Kernel32, "UpdateProcThreadAttribute");
    pDelete = (DELETE_LIST)GetProcAddress(Kernel32, "DeleteProcThreadAttributeList");
    if (!pInitialize || !pUpdate || !pDelete)
    {
        skip("Process attributes unavailable\n");
        return;
    }
    /* GetVersionEx tells a program without a manifest that it runs on Windows 8 */
    RtlGetVersion(&Version);
    IsReactOS = is_reactos();
    HonouredMitigations = DetectHonouredMitigations();
    IsWin8 = Version.dwMajorVersion > 6 || (Version.dwMajorVersion == 6 && Version.dwMinorVersion >= 2);
    IsWin10_1703 = Version.dwMajorVersion > 10 || (Version.dwMajorVersion == 10 && Version.dwBuildNumber >= 15063);
    GetModuleFileNameW(NULL, Image, _countof(Image));
    Inheritable = CreateEventW(&Sa, TRUE, FALSE, NULL);

    /* Defined DWORD mitigations: DEP, DEP-ATL thunk emulation, SEHOP. Update takes any
     * value; CreateProcess refuses bits that no mitigation uses (and, on ReactOS, bits
     * the child cannot get). */
    ExpectDword(0, FALSE);
    ExpectDword(1, FALSE);
    ExpectDword(2, FALSE);
    ExpectDword(3, FALSE);
    ExpectDword(4, FALSE);
    ExpectDword(7, FALSE);
    ExpectDword(0x8, FALSE);
    ExpectDword(0x10, FALSE);
    ExpectDword(0x40101067, FALSE);
    /* A mitigation entry and a handle-list entry in one list */
    ExpectDword(0, TRUE);
    ExpectDword(1, TRUE);
    ExpectDword(7, TRUE);
    ExpectDword(0x10, TRUE);

    EightByteError = (IsReactOS || IsWin8) ? ERROR_SUCCESS : ERROR_BAD_LENGTH;
    SixteenByteError = (IsReactOS || IsWin10_1703) ? ERROR_SUCCESS : ERROR_BAD_LENGTH;
    ExpectSize(sizeof(DWORD), ERROR_SUCCESS);
    ExpectSize(sizeof(DWORD64), EightByteError);
    ExpectSize(2 * sizeof(DWORD64), SixteenByteError);
    ExpectSize(3, ERROR_BAD_LENGTH);
    ExpectSize(12, ERROR_BAD_LENGTH);

    if (IsReactOS)
    {
        /* The wide forms are read in full; any bit but an honoured one is refused */
        ExpectWide(8, HonouredMitigations, 0, ERROR_SUCCESS);
        ExpectWide(8, 0x100, 0, ERROR_INVALID_PARAMETER);
        ExpectWide(8, 0x8000000000000000ULL, 0, ERROR_INVALID_PARAMETER);
        ExpectWide(16, 0, 0, ERROR_SUCCESS);
        ExpectWide(16, 0, 1, ERROR_INVALID_PARAMETER);
        ExpectWide(16, 0x100, 0, ERROR_INVALID_PARAMETER);
        ExpectBadPointer(4);
        ExpectBadPointer(8);
        ExpectBadPointer(16);
    }
    CloseHandle(Inheritable);
}
