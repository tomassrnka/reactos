/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Validate the apiset lookup in ApiSetResolveToHost
 * COPYRIGHT:   Copyright 2024 Mark Jansen <mark.jansen@reactos.org>
 */

#include <ndk/umtypes.h>
#include <ndk/rtlfuncs.h>
#include <apitest.h>
#include "apisetsp.h"

static void
resolve_single(PCUNICODE_STRING Apiset)
{
    UNICODE_STRING Tmp = {0};
    NTSTATUS Status = RtlDuplicateUnicodeString(RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE, Apiset, &Tmp);
    ok_ntstatus(Status, STATUS_SUCCESS);

    BOOLEAN Resolved = FALSE;
    UNICODE_STRING Result = {0};

    Status = ApiSetResolveToHost(~0u, &Tmp, &Resolved, &Result);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_eq_bool(Resolved, TRUE);
    ok(Result.Buffer != NULL, "Got NULL\n");
}

static void
resolve_fail_single(PCUNICODE_STRING Apiset)
{
    UNICODE_STRING Tmp = {0};
    NTSTATUS Status = RtlDuplicateUnicodeString(RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE, Apiset, &Tmp);
    ok_ntstatus(Status, STATUS_SUCCESS);

    BOOLEAN Resolved = FALSE;
    UNICODE_STRING Result = {0, 0, (PWSTR)0xbadbeef};

    Status = ApiSetResolveToHost(~0u, &Tmp, &Resolved, &Result);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_eq_bool(Resolved, FALSE);
    ok_ptr(Result.Buffer, (PWSTR)0xbadbeef);
}

static void
test_single(PCUNICODE_STRING Apiset)
{
    winetest_push_context("%S", Apiset->Buffer);
    resolve_single(Apiset);
    winetest_pop_context();
}

static void
fail_single(PCUNICODE_STRING Apiset)
{
    winetest_push_context("%S", Apiset->Buffer);
    resolve_fail_single(Apiset);
    winetest_pop_context();
}


START_TEST(apisets)
{
    // Ensure we can find some manually selected ones (both with and without extension):
    UNICODE_STRING Console1 = RTL_CONSTANT_STRING(L"api-MS-Win-Core-Console-L1-1-0.dll");
    UNICODE_STRING Console2 = RTL_CONSTANT_STRING(L"api-MS-Win-Core-Console-L1-1-0");
    UNICODE_STRING Handle1 = RTL_CONSTANT_STRING(L"api-MS-Win-Core-Handle-L1-1-0.dll");
    UNICODE_STRING Handle2 = RTL_CONSTANT_STRING(L"api-MS-Win-Core-Handle-L1-1-0");
    UNICODE_STRING Heap1 = RTL_CONSTANT_STRING(L"api-MS-Win-Core-Heap-L1-1-0.dll");
    UNICODE_STRING Heap2 = RTL_CONSTANT_STRING(L"api-MS-Win-Core-Heap-L1-1-0");

    test_single(&Console1);
    test_single(&Console2);
    test_single(&Handle1);
    test_single(&Handle2);
    test_single(&Heap1);
    test_single(&Heap2);

    // Walk the entire table, to ensure we can find each entry
    for (DWORD n = 0; n < g_ApisetsCount; ++n)
    {
        PCUNICODE_STRING Apiset = &g_Apisets[n].Name;
        winetest_push_context("%ld: %S", n, Apiset->Buffer);
        resolve_single(Apiset);
        winetest_pop_context();
    }

    // Any minor version of a contract resolves to the host of the contract
    UNICODE_STRING Minor1 = RTL_CONSTANT_STRING(L"api-ms-win-core-libraryloader-l1-2-1.dll");
    UNICODE_STRING Minor2 = RTL_CONSTANT_STRING(L"API-MS-Win-Core-ProcessThreads-L1-1-3");
    UNICODE_STRING Minor3 = RTL_CONSTANT_STRING(L"api-ms-win-core-synch-l1-2-99.dll");
    UNICODE_STRING Minor5 = RTL_CONSTANT_STRING(L"api-ms-win-core-synch-l1-2-x.dll");
    UNICODE_STRING Minor4 = RTL_CONSTANT_STRING(L"api-ms-win-crt-runtime-l1-1-7.dll");
    UNICODE_STRING Ucrt = RTL_CONSTANT_STRING(L"ucrtbase.dll");
    UNICODE_STRING Win8Only = RTL_CONSTANT_STRING(L"api-ms-win-core-file-l1-2-0");

    test_single(&Minor1);
    test_single(&Minor2);
    test_single(&Minor3);
    test_single(&Minor5);

    BOOLEAN Resolved = FALSE;
    UNICODE_STRING Result = {0};
    NTSTATUS Status = ApiSetResolveToHost(~0u, &Minor4, &Resolved, &Result);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_eq_bool(Resolved, TRUE);
    ok(Resolved && RtlEqualUnicodeString(&Result, &Ucrt, TRUE), "Got %.*ls\n", Result.Length / (int)sizeof(WCHAR), Result.Buffer);

    // A row of another version of the same contract counts
    Resolved = FALSE;
    Status = ApiSetResolveToHost(APISET_WIN10, &Win8Only, &Resolved, &Result);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_eq_bool(Resolved, TRUE);

    UNICODE_STRING Fail1 = RTL_CONSTANT_STRING(L"");
    UNICODE_STRING Fail2 = RTL_CONSTANT_STRING(L"api-");
    UNICODE_STRING Fail3 = RTL_CONSTANT_STRING(L"ext");

    fail_single(&Fail1);
    fail_single(&Fail2);
    fail_single(&Fail3);

    // Only the exact name when asked for (Windows 7)
    UNICODE_STRING Win7Name = RTL_CONSTANT_STRING(L"API-MS-Win-Core-Synch-L1-1-0.dll");
    UNICODE_STRING Win7Minor = RTL_CONSTANT_STRING(L"api-ms-win-core-synch-l1-1-5.dll");
    Resolved = FALSE;
    Status = ApiSetResolveToHostEx(APISET_WIN7, TRUE, &Win7Name, &Resolved, &Result);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_eq_bool(Resolved, TRUE);
    Resolved = TRUE;
    Status = ApiSetResolveToHostEx(APISET_WIN7, TRUE, &Win7Minor, &Resolved, &Result);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_eq_bool(Resolved, FALSE);
    Resolved = FALSE;
    Status = ApiSetResolveToHostEx(APISET_WIN7, FALSE, &Win7Minor, &Resolved, &Result);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_eq_bool(Resolved, TRUE);

    // Another level, or no minor version at all, is another contract; a path is not a contract name
    UNICODE_STRING Fail4 = RTL_CONSTANT_STRING(L"api-ms-win-core-synch-l1-9-0.dll");
    UNICODE_STRING Fail5 = RTL_CONSTANT_STRING(L"api-ms-win-core-synch-l1-2.dll");
    UNICODE_STRING Fail6 = RTL_CONSTANT_STRING(L"api-ms-win-core-synch-l1.dll");
    UNICODE_STRING Fail7 = RTL_CONSTANT_STRING(L"api-ms-win-core-nosuchcontract-l1-1-0.dll");
    UNICODE_STRING Fail8 = RTL_CONSTANT_STRING(L"api-ms-win-core-synch-l1-2-0-0.dll");
    UNICODE_STRING Fail9 = RTL_CONSTANT_STRING(L"api-ms-win-core-synch-l1-2-0\\..\\x.dll");
    UNICODE_STRING Fail10 = RTL_CONSTANT_STRING(L"api-ms-win-core-synch-l1-2-0/x.dll");

    fail_single(&Fail4);
    fail_single(&Fail5);
    fail_single(&Fail6);
    fail_single(&Fail7);
    fail_single(&Fail8);
    fail_single(&Fail9);
    fail_single(&Fail10);
}
