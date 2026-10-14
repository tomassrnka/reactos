/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for GetProcessMitigationPolicy and SetProcessMitigationPolicy
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef BOOL (WINAPI *FN_GetProcessMitigationPolicy)(HANDLE, int, PVOID, SIZE_T);
typedef BOOL (WINAPI *FN_SetProcessMitigationPolicy)(int, PVOID, SIZE_T);

static FN_GetProcessMitigationPolicy pGetProcessMitigationPolicy;
static FN_SetProcessMitigationPolicy pSetProcessMitigationPolicy;

#define PolicyDEP               0
#define PolicyMitigationOptions 5
#define PolicyCount             17

/* The flags that exist in the structure of each policy */
static const ULONG ValidFlags[PolicyCount] =
{
    0x3, 0xF, 0xF, 0x3, 0x3, 0, 0x1, 0x1F, 0x1F, 0x3, 0x1F, 0xF, 0xFFF, 0x7, 0x1F, 0x3FF, 0x3
};

/*
 * What Windows 10 answers to the first request of a process (every row was run in a process of its own): the error,
 * or 0 if the request is accepted. The system keeps what it accepted, so a later request can give another answer.
 */
static const struct
{
    int Policy;
    ULONG Flags;
    DWORD Error;
} SetResults[] =
{
{ 1, 0x0, 5 },
    { 1, 0x1, 0 },
    { 1, 0x2, 5 },
    { 1, 0x4, 5 },
    { 1, 0x8, 5 },
    { 1, 0x10, 87 },
    { 1, 0x100, 87 },
    { 1, 0x400, 87 },
    { 1, 0x3, 0 },
    { 1, 0x5, 0 },
    { 1, 0xf, 0 },
    { 1, 0x1f, 87 },
    { 1, 0xffffffff, 87 },
    { 2, 0x0, 0 },
    { 2, 0x1, 0 },
    { 2, 0x2, 87 },
    { 2, 0x4, 87 },
    { 2, 0x8, 0 },
    { 2, 0x10, 87 },
    { 2, 0x100, 87 },
    { 2, 0x400, 87 },
    { 2, 0x3, 0 },
    { 2, 0x5, 0 },
    { 2, 0xf, 0 },
    { 2, 0x1f, 87 },
    { 2, 0xffffffff, 87 },
    { 3, 0x0, 0 },
    { 3, 0x1, 87 },
    { 3, 0x2, 87 },
    { 3, 0x4, 87 },
    { 3, 0x8, 87 },
    { 3, 0x10, 87 },
    { 3, 0x100, 87 },
    { 3, 0x400, 87 },
    { 3, 0x3, 0 },
    { 3, 0x5, 87 },
    { 3, 0xf, 87 },
    { 3, 0x1f, 87 },
    { 3, 0xffffffff, 87 },
    { 4, 0x0, 0 },
    { 4, 0x1, 0 },
    { 4, 0x2, 0 },
    { 4, 0x4, 87 },
    { 4, 0x8, 87 },
    { 4, 0x10, 87 },
    { 4, 0x100, 87 },
    { 4, 0x400, 87 },
    { 4, 0x3, 0 },
    { 4, 0x5, 87 },
    { 4, 0xf, 87 },
    { 4, 0x1f, 87 },
    { 4, 0xffffffff, 87 },
    { 5, 0x0, 87 },
    { 5, 0x1, 87 },
    { 5, 0x2, 87 },
    { 5, 0x4, 87 },
    { 5, 0x8, 87 },
    { 5, 0x10, 87 },
    { 5, 0x100, 87 },
    { 5, 0x400, 87 },
    { 5, 0x3, 87 },
    { 5, 0x5, 87 },
    { 5, 0xf, 87 },
    { 5, 0x1f, 87 },
    { 5, 0xffffffff, 87 },
    { 6, 0x0, 0 },
    { 6, 0x1, 0 },
    { 6, 0x2, 87 },
    { 6, 0x4, 87 },
    { 6, 0x8, 87 },
    { 6, 0x10, 87 },
    { 6, 0x100, 87 },
    { 6, 0x400, 87 },
    { 6, 0x3, 87 },
    { 6, 0x5, 87 },
    { 6, 0xf, 87 },
    { 6, 0x1f, 87 },
    { 6, 0xffffffff, 87 },
    { 7, 0x0, 5 },
    { 7, 0x1, 5 },
    { 7, 0x2, 5 },
    { 7, 0x4, 5 },
    { 7, 0x8, 87 },
    { 7, 0x10, 87 },
    { 7, 0x100, 87 },
    { 7, 0x400, 87 },
    { 7, 0x3, 5 },
    { 7, 0x5, 5 },
    { 7, 0xf, 87 },
    { 7, 0x1f, 87 },
    { 7, 0xffffffff, 87 },
    { 8, 0x0, 0 },
    { 8, 0x1, 0 },
    { 8, 0x2, 5 },
    { 8, 0x4, 0 },
    { 8, 0x8, 0 },
    { 8, 0x10, 0 },
    { 8, 0x100, 87 },
    { 8, 0x400, 87 },
    { 8, 0x3, 87 },
    { 8, 0x5, 0 },
    { 8, 0xf, 87 },
    { 8, 0x1f, 87 },
    { 8, 0xffffffff, 87 },
    { 9, 0x0, 0 },
    { 9, 0x1, 0 },
    { 9, 0x2, 0 },
    { 9, 0x4, 87 },
    { 9, 0x8, 87 },
    { 9, 0x10, 87 },
    { 9, 0x100, 87 },
    { 9, 0x400, 87 },
    { 9, 0x3, 0 },
    { 9, 0x5, 87 },
    { 9, 0xf, 87 },
    { 9, 0x1f, 87 },
    { 9, 0xffffffff, 87 },
    { 10, 0x0, 0 },
    { 10, 0x1, 0 },
    { 10, 0x2, 0 },
    { 10, 0x4, 0 },
    { 10, 0x8, 0 },
    { 10, 0x10, 0 },
    { 10, 0x100, 87 },
    { 10, 0x400, 87 },
    { 10, 0x3, 0 },
    { 10, 0x5, 0 },
    { 10, 0xf, 0 },
    { 10, 0x1f, 0 },
    { 10, 0xffffffff, 87 },
    { 11, 0x0, 87 },
    { 11, 0x1, 87 },
    { 11, 0x2, 87 },
    { 11, 0x4, 87 },
    { 11, 0x8, 87 },
    { 11, 0x10, 87 },
    { 11, 0x100, 87 },
    { 11, 0x400, 87 },
    { 11, 0x3, 87 },
    { 11, 0x5, 87 },
    { 11, 0xf, 87 },
    { 11, 0x1f, 87 },
    { 11, 0xffffffff, 87 },
    { 12, 0x0, 87 },
    { 12, 0x1, 87 },
    { 12, 0x2, 87 },
    { 12, 0x4, 87 },
    { 12, 0x8, 87 },
    { 12, 0x10, 87 },
    { 12, 0x100, 87 },
    { 12, 0x400, 87 },
    { 12, 0x3, 87 },
    { 12, 0x5, 87 },
    { 12, 0xf, 87 },
    { 12, 0x1f, 87 },
    { 12, 0xffffffff, 87 },
    { 13, 0x0, 0 },
    { 13, 0x1, 0 },
    { 13, 0x2, 0 },
    { 13, 0x4, 87 },
    { 13, 0x8, 87 },
    { 13, 0x10, 87 },
    { 13, 0x100, 87 },
    { 13, 0x400, 87 },
    { 13, 0x3, 0 },
    { 13, 0x5, 0 },
    { 13, 0xf, 87 },
    { 13, 0x1f, 87 },
    { 13, 0xffffffff, 87 },
    { 14, 0x0, 0 },
    { 14, 0x1, 0 },
    { 14, 0x2, 0 },
    { 14, 0x4, 0 },
    { 14, 0x8, 0 },
    { 14, 0x10, 87 },
    { 14, 0x100, 87 },
    { 14, 0x400, 87 },
    { 14, 0x3, 0 },
    { 14, 0x5, 0 },
    { 14, 0xf, 0 },
    { 14, 0x1f, 87 },
    { 14, 0xffffffff, 87 },
    { 15, 0x0, 5 },
    { 15, 0x1, 5 },
    { 15, 0x2, 5 },
    { 15, 0x4, 5 },
    { 15, 0x8, 5 },
    { 15, 0x10, 5 },
    { 15, 0x100, 0 },
    { 15, 0x400, 87 },
    { 15, 0x3, 5 },
    { 15, 0x5, 5 },
    { 15, 0xf, 5 },
    { 15, 0x1f, 5 },
    { 15, 0xffffffff, 87 },
    { 16, 0x0, 0 },
    { 16, 0x1, 0 },
    { 16, 0x2, 0 },
    { 16, 0x4, 87 },
    { 16, 0x8, 87 },
    { 16, 0x10, 87 },
    { 16, 0x100, 87 },
    { 16, 0x400, 87 },
    { 16, 0x3, 0 },
    { 16, 0x5, 87 },
    { 16, 0xf, 87 },
    { 16, 0x1f, 87 },
    { 16, 0xffffffff, 87 },
};

static DWORD
RunChild(const WCHAR *Arguments)
{
    WCHAR FileName[MAX_PATH], CommandLine[MAX_PATH + 100];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    DWORD ExitCode = 0xdeadbeef;

    GetModuleFileNameW(NULL, FileName, _countof(FileName));
    StringCbPrintfW(CommandLine, sizeof(CommandLine), L"\"%ls\" ProcessMitigationPolicy child %ls", FileName, Arguments);

    RtlZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    if (!CreateProcessW(NULL, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcess failed with %lu\n", GetLastError());
        return ExitCode;
    }

    WaitForSingleObject(ProcessInfo.hProcess, INFINITE);
    GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
    return ExitCode;
}

/* Child: one request; the exit code is 0 for success, else the error */
static void
RunRequest(int Policy, ULONG Flags)
{
    ULONG Buffer[4] = { Flags, 0, 0, 0 };
    BOOL Ret;

    Ret = pSetProcessMitigationPolicy(Policy, Buffer, (Policy == PolicyDEP || Policy == PolicyMitigationOptions) ? 8 : 4);
    ExitProcess(Ret ? 0 : GetLastError());
}

static void
test_GetResults(void)
{
    ULONG Buffer[16];
    BYTE Bytes[64];
    DWORD Length, Expected;
    int Policy, Index;
    BOOL Ret;

    for (Policy = 0; Policy < PolicyCount; Policy++)
    {
        Expected = (Policy == PolicyDEP || Policy == PolicyMitigationOptions) ? 8 : 4;

        /* Only the right length is taken, except for the mask of the options that takes more */
        for (Length = 0; Length <= 12; Length++)
        {
            RtlFillMemory(Buffer, sizeof(Buffer), 0xcc);
            SetLastError(0xdeadbeef);
            Ret = pGetProcessMitigationPolicy(GetCurrentProcess(), Policy, Buffer, Length);
            if (Length == Expected || (Policy == PolicyMitigationOptions && Length > Expected))
            {
                ok(Ret == TRUE, "Policy %d, length %lu: returned %d, error %lu\n", Policy, Length, Ret, GetLastError());
                if (Ret && Policy != PolicyDEP && Policy != PolicyMitigationOptions)
                {
                    ok((Buffer[0] & ~ValidFlags[Policy]) == 0, "Policy %d: flags %08lx have undefined bits\n", Policy, Buffer[0]);
                    ok(Buffer[1] == 0xcccccccc, "Policy %d: wrote beyond its length\n", Policy);
                }
            }
            else
            {
                ok(Ret == FALSE, "Policy %d, length %lu: returned %d\n", Policy, Length, Ret);
                ok(GetLastError() == ERROR_INVALID_PARAMETER, "Policy %d, length %lu: error %lu\n", Policy, Length, GetLastError());
                ok(Buffer[0] == 0xcccccccc, "Policy %d, length %lu: wrote the buffer\n", Policy, Length);
            }
        }
    }

    /* The DEP policy: the flags and the permanent flag, the padding behind them is left alone */
    RtlFillMemory(Bytes, sizeof(Bytes), 0xcc);
    ok(pGetProcessMitigationPolicy(GetCurrentProcess(), PolicyDEP, Bytes, 8), "Get DEP failed with %lu\n", GetLastError());
    ok((*(ULONG *)Bytes & ~3) == 0, "DEP flags %08lx\n", *(ULONG *)Bytes);
    ok(Bytes[4] <= 1, "DEP permanent %02x\n", Bytes[4]);
    ok(Bytes[5] == 0xcc && Bytes[6] == 0xcc && Bytes[7] == 0xcc, "DEP padding was written\n");

    /* The options mask takes any length from 8 bytes on, and is filled up to it */
    for (Index = 0; Index < 3; Index++)
    {
        static const DWORD Lengths[] = { 8, 24, 64 };

        RtlFillMemory(Bytes, sizeof(Bytes), 0xcc);
        Ret = pGetProcessMitigationPolicy(GetCurrentProcess(), PolicyMitigationOptions, Bytes, Lengths[Index]);
        ok(Ret, "Get options mask with length %lu failed with %lu\n", Lengths[Index], GetLastError());
        if (Lengths[Index] == 64)
            ok(Bytes[63] == 0, "The buffer was not filled up to its length\n");
    }
}

static void
test_InvalidPolicies(void)
{
    static const int Policies[] = { -1, 17, 18, 19, 100, 0x7fffffff };
    ULONG Buffer[4] = { 0 };
    int i;

    for (i = 0; i < _countof(Policies); i++)
    {
        SetLastError(0xdeadbeef);
        ok(pGetProcessMitigationPolicy(GetCurrentProcess(), Policies[i], Buffer, 4) == FALSE, "Get %d succeeded\n", Policies[i]);
        ok(GetLastError() == ERROR_INVALID_PARAMETER, "Get %d: error %lu\n", Policies[i], GetLastError());
        SetLastError(0xdeadbeef);
        ok(pSetProcessMitigationPolicy(Policies[i], Buffer, 4) == FALSE, "Set %d succeeded\n", Policies[i]);
        ok(GetLastError() == ERROR_INVALID_PARAMETER, "Set %d: error %lu\n", Policies[i], GetLastError());
    }
}

static void
test_Handles(void)
{
    ULONG Buffer[4];
    HANDLE Handle;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = pGetProcessMitigationPolicy(NULL, 2, Buffer, 4);
    ok(Ret == FALSE && GetLastError() == ERROR_INVALID_HANDLE, "NULL: %d, error %lu\n", Ret, GetLastError());
    SetLastError(0xdeadbeef);
    Ret = pGetProcessMitigationPolicy((HANDLE)(ULONG_PTR)0xdeadbee0, 2, Buffer, 4);
    ok(Ret == FALSE && GetLastError() == ERROR_INVALID_HANDLE, "invalid: %d, error %lu\n", Ret, GetLastError());
    SetLastError(0xdeadbeef);
    Ret = pGetProcessMitigationPolicy(GetCurrentThread(), 2, Buffer, 4);
    ok(Ret == FALSE && GetLastError() == ERROR_INVALID_HANDLE, "thread: %d, error %lu\n", Ret, GetLastError());

    Handle = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    ok(Handle != NULL, "OpenProcess failed with %lu\n", GetLastError());
    if (Handle)
    {
        SetLastError(0xdeadbeef);
        Ret = pGetProcessMitigationPolicy(Handle, 2, Buffer, 4);
        ok(Ret == FALSE && GetLastError() == ERROR_ACCESS_DENIED, "SYNCHRONIZE: %d, error %lu\n", Ret, GetLastError());
        CloseHandle(Handle);
    }

    Handle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    ok(Handle != NULL, "OpenProcess failed with %lu\n", GetLastError());
    if (Handle)
    {
        BYTE Pseudo[8], Real[8];

        ok(pGetProcessMitigationPolicy(Handle, 2, Buffer, 4), "PROCESS_QUERY_INFORMATION: error %lu\n", GetLastError());

        /* A handle of this process says what the pseudo handle says */
        RtlFillMemory(Pseudo, sizeof(Pseudo), 0xcc);
        RtlFillMemory(Real, sizeof(Real), 0xcc);
        ok(pGetProcessMitigationPolicy(GetCurrentProcess(), PolicyDEP, Pseudo, 8), "DEP, pseudo handle: error %lu\n", GetLastError());
        ok(pGetProcessMitigationPolicy(Handle, PolicyDEP, Real, 8), "DEP with PROCESS_QUERY_INFORMATION: error %lu\n", GetLastError());
        ok(!memcmp(Pseudo, Real, 5), "DEP: %08lx %02x with the pseudo handle, %08lx %02x with the handle\n",
           *(ULONG *)Pseudo, Pseudo[4], *(ULONG *)Real, Real[4]);
        CloseHandle(Handle);
    }

    /* The right to query a little is enough for every policy but DEP */
    Handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    if (!Handle && is_reactos())
        skip("The right to query a process a little is not given by the kernel\n");
    else
        ok(Handle != NULL, "OpenProcess failed with %lu\n", GetLastError());
    if (Handle)
    {
        ok(pGetProcessMitigationPolicy(Handle, 2, Buffer, 4), "PROCESS_QUERY_LIMITED_INFORMATION: error %lu\n", GetLastError());
        SetLastError(0xdeadbeef);
        Ret = pGetProcessMitigationPolicy(Handle, PolicyDEP, Buffer, 8);
        ok(Ret == FALSE && GetLastError() == ERROR_ACCESS_DENIED, "DEP with PROCESS_QUERY_LIMITED_INFORMATION: %d, error %lu\n", Ret, GetLastError());
        CloseHandle(Handle);
    }
}

static void
test_SetLengths(void)
{
    ULONG Buffer[16] = { 0 };
    DWORD Length;
    int Policy;

    for (Policy = 1; Policy < PolicyCount; Policy++)
    {
        for (Length = 0; Length <= 12; Length++)
        {
            if (Length == 4 || Length == 8)
                continue;

            SetLastError(0xdeadbeef);
            ok(pSetProcessMitigationPolicy(Policy, Buffer, Length) == FALSE, "Policy %d, length %lu succeeded\n", Policy, Length);
            ok(GetLastError() == ERROR_INVALID_PARAMETER, "Policy %d, length %lu: error %lu\n", Policy, Length, GetLastError());
        }
    }

    /* DEP of the process cannot be changed with this function; the error depends on the kind of process */
    for (Length = 0; Length <= 12; Length += 4)
    {
        SetLastError(0xdeadbeef);
        ok(pSetProcessMitigationPolicy(PolicyDEP, Buffer, Length) == FALSE, "DEP, length %lu succeeded\n", Length);
        ok(GetLastError() == ERROR_NOT_SUPPORTED || GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_INVALID_PARAMETER,
           "DEP, length %lu: error %lu\n", Length, GetLastError());
    }
}

static void
test_SetResults(void)
{
    WCHAR Arguments[40];
    DWORD ExitCode;
    int i;

    for (i = 0; i < _countof(SetResults); i++)
    {
        StringCbPrintfW(Arguments, sizeof(Arguments), L"%d %lx", SetResults[i].Policy, SetResults[i].Flags);
        ExitCode = RunChild(Arguments);
        if (ExitCode == 0xdeadbeef)
            return;

        ok(ExitCode == SetResults[i].Error, "Policy %d, flags %08lx: error %lu, expected %lu\n",
           SetResults[i].Policy, SetResults[i].Flags, ExitCode, SetResults[i].Error);
    }
}

START_TEST(ProcessMitigationPolicy)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    char **argv;
    int argc;

    pGetProcessMitigationPolicy = (FN_GetProcessMitigationPolicy)GetProcAddress(hKernel32, "GetProcessMitigationPolicy");
    pSetProcessMitigationPolicy = (FN_SetProcessMitigationPolicy)GetProcAddress(hKernel32, "SetProcessMitigationPolicy");
    if ((!pGetProcessMitigationPolicy || !pSetProcessMitigationPolicy) && !is_reactos())
    {
        skip("GetProcessMitigationPolicy or SetProcessMitigationPolicy is not exported\n");
        return;
    }
    ok(pGetProcessMitigationPolicy != NULL, "GetProcessMitigationPolicy is not exported\n");
    ok(pSetProcessMitigationPolicy != NULL, "SetProcessMitigationPolicy is not exported\n");
    if (!pGetProcessMitigationPolicy || !pSetProcessMitigationPolicy)
        return;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 5 && !strcmp(argv[2], "child"))
    {
        RunRequest(atoi(argv[3]), strtoul(argv[4], NULL, 16));
        return;
    }

    test_GetResults();
    test_InvalidPolicies();
    test_Handles();
    test_SetLengths();
    test_SetResults();
}
