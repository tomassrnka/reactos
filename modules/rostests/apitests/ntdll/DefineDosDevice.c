/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for DefineDosDevice / QueryDosDevice, the base server request in a 32-bit process
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

static
void
TestDefinition(PCWSTR Name, DWORD Flags, PCWSTR Target, PCWSTR Expected)
{
    WCHAR Buffer[MAX_PATH];
    DWORD Length;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = DefineDosDeviceW(Flags, Name, Target);
    ok(Ret, "DefineDosDevice(%lx, %S, %S) failed, error %lu\n", Flags, Name, Target, GetLastError());
    if (!Ret)
        return;

    RtlFillMemory(Buffer, sizeof(Buffer), 0x55);
    Length = QueryDosDeviceW(Name, Buffer, RTL_NUMBER_OF(Buffer));
    ok(Length == wcslen(Expected) + 2, "QueryDosDevice length %lu, expected %lu\n", Length, (ULONG)wcslen(Expected) + 2);
    ok(!_wcsicmp(Buffer, Expected), "QueryDosDevice gave %S, expected %S\n", Buffer, Expected);

    SetLastError(0xdeadbeef);
    Ret = DefineDosDeviceW(DDD_REMOVE_DEFINITION, Name, NULL);
    ok(Ret, "remove failed, error %lu\n", GetLastError());
    SetLastError(0xdeadbeef);
    Length = QueryDosDeviceW(Name, Buffer, RTL_NUMBER_OF(Buffer));
    ok(Length == 0, "the device is still defined: %S\n", Buffer);
    ok_eq_ulong(GetLastError(), ERROR_FILE_NOT_FOUND);
}

START_TEST(DefineDosDevice)
{
    WCHAR Buffer[MAX_PATH];
    DWORD Length;
    BOOL Ret;

    /* The target is taken as a DOS path and converted ... */
    TestDefinition(L"WOW64PFDEV", 0, L"C:\\", L"\\??\\C:\\");
    /* ... or used as it is */
    TestDefinition(L"WOW64PFRAW", DDD_RAW_TARGET_PATH, L"\\Device\\Null", L"\\Device\\Null");
    /* A long target, as a name of 128 characters */
    {
        WCHAR Long[160];
        ULONG i;
        wcscpy(Long, L"\\Device\\");
        for (i = 8; i < 136; i++)
            Long[i] = L'A' + (i % 26);
        Long[136] = UNICODE_NULL;
        TestDefinition(L"WOW64PFLONG", DDD_RAW_TARGET_PATH, Long, Long);
    }

    /* A second definition is pushed on top and removed first */
    Ret = DefineDosDeviceW(DDD_RAW_TARGET_PATH, L"WOW64PFSTK", L"\\Device\\Null");
    ok(Ret, "first definition failed, error %lu\n", GetLastError());
    Ret = DefineDosDeviceW(DDD_RAW_TARGET_PATH, L"WOW64PFSTK", L"\\Device\\Beep");
    ok(Ret, "second definition failed, error %lu\n", GetLastError());
    Length = QueryDosDeviceW(L"WOW64PFSTK", Buffer, RTL_NUMBER_OF(Buffer));
    ok(Length != 0, "QueryDosDevice failed, error %lu\n", GetLastError());
    ok(!_wcsicmp(Buffer, L"\\Device\\Beep"), "top is %S\n", Buffer);
    Ret = DefineDosDeviceW(DDD_REMOVE_DEFINITION, L"WOW64PFSTK", NULL);
    ok(Ret, "remove failed, error %lu\n", GetLastError());
    Length = QueryDosDeviceW(L"WOW64PFSTK", Buffer, RTL_NUMBER_OF(Buffer));
    ok(Length != 0, "the first definition is gone, error %lu\n", GetLastError());
    ok(!_wcsicmp(Buffer, L"\\Device\\Null"), "second is %S\n", Buffer);
    Ret = DefineDosDeviceW(DDD_REMOVE_DEFINITION, L"WOW64PFSTK", NULL);
    ok(Ret, "remove failed, error %lu\n", GetLastError());

    /* Refused before the server is asked */
    SetLastError(0xdeadbeef);
    Ret = DefineDosDeviceW(0x80, L"WOW64PFBAD", L"C:\\");
    ok(!Ret, "bad flags accepted\n");
    ok_eq_ulong(GetLastError(), ERROR_INVALID_PARAMETER);
    SetLastError(0xdeadbeef);
    Ret = DefineDosDeviceW(0, L"WOW64PFBAD", NULL);
    ok(!Ret, "no target accepted\n");
    ok_eq_ulong(GetLastError(), ERROR_INVALID_PARAMETER);

    /* Removing what is not defined */
    SetLastError(0xdeadbeef);
    Ret = DefineDosDeviceW(DDD_REMOVE_DEFINITION, L"WOW64PFNONE", NULL);
    ok(Ret || GetLastError() == ERROR_FILE_NOT_FOUND, "remove of nothing: error %lu\n", GetLastError());
}
