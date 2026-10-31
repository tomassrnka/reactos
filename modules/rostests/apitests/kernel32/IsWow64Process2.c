/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for IsWow64Process2
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef BOOL (WINAPI *PFN_IsWow64Process2)(HANDLE, USHORT *, USHORT *);

#ifndef IMAGE_FILE_MACHINE_I386
#define IMAGE_FILE_MACHINE_I386 0x014c
#endif
#ifndef IMAGE_FILE_MACHINE_AMD64
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#endif

static USHORT GetNativeMachine(void)
{
    SYSTEM_INFO Info;

    GetNativeSystemInfo(&Info);
    switch (Info.wProcessorArchitecture)
    {
        case PROCESSOR_ARCHITECTURE_AMD64: return IMAGE_FILE_MACHINE_AMD64;
        case PROCESSOR_ARCHITECTURE_ARM64: return 0xAA64;
        case PROCESSOR_ARCHITECTURE_ARM: return 0x01c4;
        default: return IMAGE_FILE_MACHINE_I386;
    }
}

static void Check(PFN_IsWow64Process2 pIsWow64Process2, PCSTR Name, HANDLE Process, USHORT ExpectedProcess, USHORT ExpectedNative)
{
    USHORT ProcessMachine = 0xAAAA, NativeMachine = 0xBBBB;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = pIsWow64Process2(Process, &ProcessMachine, &NativeMachine);
    ok(Ret, "%s: failed with %lu\n", Name, GetLastError());
    ok(ProcessMachine == ExpectedProcess, "%s: process machine is %04x, expected %04x\n", Name, ProcessMachine, ExpectedProcess);
    ok(NativeMachine == ExpectedNative, "%s: native machine is %04x, expected %04x\n", Name, NativeMachine, ExpectedNative);
}

static void CheckFailure(PFN_IsWow64Process2 pIsWow64Process2, PCSTR Name, HANDLE Process, DWORD ExpectedError)
{
    USHORT ProcessMachine = 0xAAAA, NativeMachine = 0xBBBB;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = pIsWow64Process2(Process, &ProcessMachine, &NativeMachine);
    ok(!Ret && GetLastError() == ExpectedError, "%s: ret %d, error %lu, expected error %lu\n", Name, Ret, GetLastError(), ExpectedError);
    ok(ProcessMachine == 0xAAAA && NativeMachine == 0xBBBB, "%s: the results were written (%04x %04x)\n", Name, ProcessMachine, NativeMachine);
}

START_TEST(IsWow64Process2)
{
    PFN_IsWow64Process2 pIsWow64Process2;
    USHORT Native = GetNativeMachine(), ProcessMachine;
    BOOL Wow64 = FALSE;
    HANDLE Process;
    WCHAR Path[MAX_PATH];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    BOOL Ret;

    pIsWow64Process2 = (PFN_IsWow64Process2)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
    if (!pIsWow64Process2)
    {
        ok(!is_reactos(), "IsWow64Process2 is not exported by kernel32.dll\n");
        skip("IsWow64Process2 is not available\n");
        return;
    }

    IsWow64Process(GetCurrentProcess(), &Wow64);

    /* this process: the machine it emulates, or 0 for a native one */
    Check(pIsWow64Process2, "pseudo handle", GetCurrentProcess(), Wow64 ? IMAGE_FILE_MACHINE_I386 : 0, Native);
    Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    ok(Process != NULL, "OpenProcess failed with %lu\n", GetLastError());
    if (Process)
    {
        Check(pIsWow64Process2, "real handle", Process, Wow64 ? IMAGE_FILE_MACHINE_I386 : 0, Native);
        CloseHandle(Process);
    }
    Check(pIsWow64Process2, "(HANDLE)-1", (HANDLE)(LONG_PTR)-1, Wow64 ? IMAGE_FILE_MACHINE_I386 : 0, Native);

    /* the native machine is optional */
    ProcessMachine = 0xAAAA;
    SetLastError(0xdeadbeef);
    Ret = pIsWow64Process2(GetCurrentProcess(), &ProcessMachine, NULL);
    ok(Ret, "no native machine: failed with %lu\n", GetLastError());
    ok(ProcessMachine == (Wow64 ? IMAGE_FILE_MACHINE_I386 : 0), "no native machine: process machine is %04x\n", ProcessMachine);

    /* a NULL handle is not rejected, it reads as a native process */
    Check(pIsWow64Process2, "NULL handle", NULL, 0, Native);

    CheckFailure(pIsWow64Process2, "invalid handle", (HANDLE)(ULONG_PTR)0xdeadbeef, ERROR_INVALID_HANDLE);
    CheckFailure(pIsWow64Process2, "thread handle", GetCurrentThread(), ERROR_INVALID_HANDLE);

    /* the handle needs PROCESS_QUERY_LIMITED_INFORMATION */
    Process = OpenProcess(PROCESS_VM_READ, FALSE, GetCurrentProcessId());
    if (Process)
    {
        CheckFailure(pIsWow64Process2, "without query access", Process, ERROR_ACCESS_DENIED);
        CloseHandle(Process);
    }

    /* a 32-bit child on a 64-bit system */
    if (Native == IMAGE_FILE_MACHINE_AMD64)
    {
        UINT Length = GetSystemWow64DirectoryW(Path, ARRAYSIZE(Path));

        if (Length && Length < ARRAYSIZE(Path) - 12)
        {
            WCHAR CmdLine[MAX_PATH + 16];

            StringCchCatW(Path, ARRAYSIZE(Path), L"\\cmd.exe");
            if (GetFileAttributesW(Path) == INVALID_FILE_ATTRIBUTES)
            {
                skip("no 32-bit cmd.exe\n");
            }
            else
            {
                StringCchPrintfW(CmdLine, ARRAYSIZE(CmdLine), L"\"%ls\" /c exit", Path);
                Ret = CreateProcessW(NULL, CmdLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi);
                ok(Ret, "CreateProcess failed with %lu\n", GetLastError());
                if (Ret)
                {
                    Check(pIsWow64Process2, "32-bit child", pi.hProcess, IMAGE_FILE_MACHINE_I386, Native);
                    TerminateProcess(pi.hProcess, 0);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            }
        }
    }
}
