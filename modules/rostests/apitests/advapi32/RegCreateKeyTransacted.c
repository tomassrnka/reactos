/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for ktmw32 transactions and the transacted registry functions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef HANDLE (WINAPI *FN_CreateTransaction)(LPSECURITY_ATTRIBUTES, LPGUID, DWORD, DWORD, DWORD, DWORD, LPWSTR);
typedef BOOL (WINAPI *FN_EndTransaction)(HANDLE);
typedef LSTATUS (WINAPI *FN_RegCreateKeyTransactedW)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM,
                                                     const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD, HANDLE, PVOID);

#define TEST_KEY L"Software\\ReactOS_apitest_RegCreateKeyTransacted"

START_TEST(RegCreateKeyTransacted)
{
    HMODULE hKtm, hAdvapi32 = GetModuleHandleW(L"advapi32.dll");
    FN_CreateTransaction pCreateTransaction;
    FN_EndTransaction pCommitTransaction, pRollbackTransaction;
    FN_RegCreateKeyTransactedW pRegCreateKeyTransactedW;
    HANDLE Transaction;
    HKEY hKey;
    DWORD Disposition;
    LSTATUS Error;

    pRegCreateKeyTransactedW = (FN_RegCreateKeyTransactedW)GetProcAddress(hAdvapi32, "RegCreateKeyTransactedW");
    ok(pRegCreateKeyTransactedW != NULL, "RegCreateKeyTransactedW is not exported\n");

    hKtm = LoadLibraryW(L"ktmw32.dll");
    ok(hKtm != NULL, "ktmw32.dll did not load: %lu\n", GetLastError());
    if (!hKtm || !pRegCreateKeyTransactedW)
        return;

    pCreateTransaction = (FN_CreateTransaction)GetProcAddress(hKtm, "CreateTransaction");
    pCommitTransaction = (FN_EndTransaction)GetProcAddress(hKtm, "CommitTransaction");
    pRollbackTransaction = (FN_EndTransaction)GetProcAddress(hKtm, "RollbackTransaction");
    ok(pCreateTransaction && pCommitTransaction && pRollbackTransaction, "ktmw32 lacks the transaction functions\n");
    if (!pCreateTransaction || !pCommitTransaction || !pRollbackTransaction)
        goto Cleanup;

    RegDeleteKeyW(HKEY_CURRENT_USER, TEST_KEY);

    SetLastError(0xdeadbeef);
    Transaction = pCreateTransaction(NULL, NULL, 0, 0, 0, 0, L"advapi32_apitest");
    if (Transaction == INVALID_HANDLE_VALUE)
    {
        /* No kernel transaction manager: it fails cleanly, and so does the transacted registry */
        ok(is_reactos(), "CreateTransaction failed with %lu\n", GetLastError());
        ok(GetLastError() != 0xdeadbeef && GetLastError() != ERROR_SUCCESS, "GetLastError() = %lu\n", GetLastError());

        ok(!pCommitTransaction(INVALID_HANDLE_VALUE), "CommitTransaction succeeded\n");
        ok(!pRollbackTransaction(INVALID_HANDLE_VALUE), "RollbackTransaction succeeded\n");

        hKey = (HKEY)(ULONG_PTR)0xdeadbeef;
        Error = pRegCreateKeyTransactedW(HKEY_CURRENT_USER, TEST_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL,
                                         &hKey, &Disposition, INVALID_HANDLE_VALUE, NULL);
        ok(Error == ERROR_CALL_NOT_IMPLEMENTED, "RegCreateKeyTransactedW returned %ld\n", Error);
        ok(RegOpenKeyExW(HKEY_CURRENT_USER, TEST_KEY, 0, KEY_READ, &hKey) == ERROR_FILE_NOT_FOUND,
           "The key was created\n");
        goto Cleanup;
    }

    /* A key created in a transaction that is rolled back does not exist */
    Error = pRegCreateKeyTransactedW(HKEY_CURRENT_USER, TEST_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL,
                                     &hKey, &Disposition, Transaction, NULL);
    ok(Error == ERROR_SUCCESS, "RegCreateKeyTransactedW returned %ld\n", Error);
    if (Error == ERROR_SUCCESS)
    {
        ok(Disposition == REG_CREATED_NEW_KEY, "Disposition = %lu\n", Disposition);
        RegCloseKey(hKey);
    }
    ok(pRollbackTransaction(Transaction), "RollbackTransaction failed with %lu\n", GetLastError());
    CloseHandle(Transaction);
    Error = RegOpenKeyExW(HKEY_CURRENT_USER, TEST_KEY, 0, KEY_READ, &hKey);
    ok(Error == ERROR_FILE_NOT_FOUND, "RegOpenKeyExW after the rollback returned %ld\n", Error);
    if (Error == ERROR_SUCCESS)
        RegCloseKey(hKey);

    /* Committed, it does */
    Transaction = pCreateTransaction(NULL, NULL, 0, 0, 0, 0, L"advapi32_apitest");
    ok(Transaction != INVALID_HANDLE_VALUE, "CreateTransaction failed with %lu\n", GetLastError());
    if (Transaction != INVALID_HANDLE_VALUE)
    {
        Error = pRegCreateKeyTransactedW(HKEY_CURRENT_USER, TEST_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL,
                                         &hKey, &Disposition, Transaction, NULL);
        ok(Error == ERROR_SUCCESS, "RegCreateKeyTransactedW returned %ld\n", Error);
        if (Error == ERROR_SUCCESS)
            RegCloseKey(hKey);
        ok(pCommitTransaction(Transaction), "CommitTransaction failed with %lu\n", GetLastError());
        CloseHandle(Transaction);
        Error = RegOpenKeyExW(HKEY_CURRENT_USER, TEST_KEY, 0, KEY_READ, &hKey);
        ok(Error == ERROR_SUCCESS, "RegOpenKeyExW after the commit returned %ld\n", Error);
        if (Error == ERROR_SUCCESS)
            RegCloseKey(hKey);
    }
    RegDeleteKeyW(HKEY_CURRENT_USER, TEST_KEY);

Cleanup:
    FreeLibrary(hKtm);
}
