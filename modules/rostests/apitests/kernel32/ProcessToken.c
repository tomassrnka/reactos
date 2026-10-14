/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     kernel32 exports the process and thread token functions from Windows 7 on
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef BOOL (WINAPI *FN_OpenProcessToken)(HANDLE, DWORD, PHANDLE);
typedef BOOL (WINAPI *FN_OpenThreadToken)(HANDLE, DWORD, BOOL, PHANDLE);
typedef BOOL (WINAPI *FN_SetThreadToken)(PHANDLE, HANDLE);
typedef BOOL (WINAPI *FN_CreateProcessAsUserW)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                                              BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);

static void
test_exports(HMODULE Module, PCSTR Name)
{
    FN_OpenProcessToken pOpenProcessToken;
    FN_OpenThreadToken pOpenThreadToken;
    FN_SetThreadToken pSetThreadToken;
    FN_CreateProcessAsUserW pCreateProcessAsUserW;
    HANDLE Token;
    BOOL Ret;

    pOpenProcessToken = (FN_OpenProcessToken)GetProcAddress(Module, "OpenProcessToken");
    pOpenThreadToken = (FN_OpenThreadToken)GetProcAddress(Module, "OpenThreadToken");
    pSetThreadToken = (FN_SetThreadToken)GetProcAddress(Module, "SetThreadToken");
    pCreateProcessAsUserW = (FN_CreateProcessAsUserW)GetProcAddress(Module, "CreateProcessAsUserW");

    ok(pOpenProcessToken != NULL, "%s does not export OpenProcessToken\n", Name);
    ok(pOpenThreadToken != NULL, "%s does not export OpenThreadToken\n", Name);
    ok(pSetThreadToken != NULL, "%s does not export SetThreadToken\n", Name);
    ok(pCreateProcessAsUserW != NULL, "%s does not export CreateProcessAsUserW\n", Name);

    if (pOpenProcessToken)
    {
        Token = NULL;
        Ret = pOpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token);
        ok(Ret, "%s: OpenProcessToken failed with %lu\n", Name, GetLastError());
        ok(Token != NULL, "%s: OpenProcessToken gave no token\n", Name);
        if (Ret)
            CloseHandle(Token);

        SetLastError(0xdeadbeef);
        Ret = pOpenProcessToken(NULL, TOKEN_QUERY, &Token);
        ok(!Ret, "%s: OpenProcessToken succeeded for a NULL process\n", Name);
        ok(GetLastError() == ERROR_INVALID_HANDLE, "%s: OpenProcessToken error %lu\n", Name, GetLastError());
    }

    if (pOpenThreadToken)
    {
        SetLastError(0xdeadbeef);
        Ret = pOpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &Token);
        ok(!Ret, "%s: OpenThreadToken succeeded for a thread that does not impersonate\n", Name);
        ok(GetLastError() == ERROR_NO_TOKEN, "%s: OpenThreadToken error %lu\n", Name, GetLastError());
    }

    if (pSetThreadToken)
    {
        Ret = pSetThreadToken(NULL, NULL);
        ok(Ret, "%s: SetThreadToken failed with %lu\n", Name, GetLastError());
    }
}

START_TEST(ProcessToken)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE Contract;

    /* ReactOS reports the version of Windows Server 2003 whatever its export level is, so look for a Windows 7 export */
    if (!GetProcAddress(Kernel32, "SetThreadGroupAffinity"))
    {
        skip("kernel32 exports the token functions from Windows 7 on\n");
        return;
    }

    test_exports(Kernel32, "kernel32");

    Contract = LoadLibraryW(L"api-ms-win-core-processthreads-l1-1-0.dll");
    ok(Contract != NULL, "The processthreads contract did not load: %lu\n", GetLastError());
    if (Contract)
    {
        test_exports(Contract, "api-ms-win-core-processthreads-l1-1-0");
        FreeLibrary(Contract);
    }
}
