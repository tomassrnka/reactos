/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for RegGetValueA/W buffer bounds, termination and expansion
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <ndk/cmfuncs.h>
#include <pseh/pseh2.h>

#define TEST_KEY    L"Software\\ReactOS RegGetValue apitest"
#define FILL_BYTE   0xAB
#define GUARD_BYTE  0xCC
#define GUARD_LEN   2048
#define LONG_CHARS  300

typedef struct _CALL_RESULT
{
    LONG Ret;
    DWORD Size;
    DWORD Type;
    BOOL Faulted;
    BOOL GuardOk;
    BYTE Data[1024];
} CALL_RESULT;

static HKEY hTestKey;

static void
DoCall(BOOL bUnicode, PCSTR pszValue, DWORD dwFlags, BOOL bBuffer, DWORD cbIn, CALL_RESULT *Result)
{
    BYTE *Buffer = NULL;
    DWORD i;
    WCHAR szValueW[64];

    MultiByteToWideChar(CP_ACP, 0, pszValue, -1, szValueW, _countof(szValueW));
    ZeroMemory(Result, sizeof(*Result));

    if (bBuffer)
    {
        Buffer = HeapAlloc(GetProcessHeap(), 0, cbIn + GUARD_LEN);
        if (!Buffer)
        {
            skip("No memory\n");
            Result->Ret = -2;
            return;
        }
        FillMemory(Buffer, cbIn, FILL_BYTE);
        FillMemory(Buffer + cbIn, GUARD_LEN, GUARD_BYTE);
    }

    Result->Size = cbIn;
    Result->Type = 0xdeadbeef;
    _SEH2_TRY
    {
        if (bUnicode)
            Result->Ret = RegGetValueW(hTestKey, NULL, szValueW, dwFlags, &Result->Type, Buffer, &Result->Size);
        else
            Result->Ret = RegGetValueA(hTestKey, NULL, pszValue, dwFlags, &Result->Type, Buffer, &Result->Size);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Result->Faulted = TRUE;
        Result->Ret = -1;
    }
    _SEH2_END;

    Result->GuardOk = TRUE;
    if (Buffer)
    {
        for (i = 0; i < GUARD_LEN; i++)
        {
            if (Buffer[cbIn + i] != GUARD_BYTE)
            {
                Result->GuardOk = FALSE;
                break;
            }
        }
        CopyMemory(Result->Data, Buffer, min(cbIn, sizeof(Result->Data)));
        HeapFree(GetProcessHeap(), 0, Buffer);
    }

    if (winetest_debug > 1)
    {
        trace("%c %-12s flags %08lx buf %d in %4lu -> ret %ld size %lu type %ld guard %d fault %d\n",
              bUnicode ? 'W' : 'A', pszValue, dwFlags, bBuffer, cbIn,
              Result->Ret, Result->Size, Result->Type, Result->GuardOk, Result->Faulted);
    }
}

static void
DoCallSubKey(PCSTR pszValue, DWORD cbIn, CALL_RESULT *Result)
{
    BYTE *Buffer;
    DWORD i;
    WCHAR szValueW[64];

    MultiByteToWideChar(CP_ACP, 0, pszValue, -1, szValueW, _countof(szValueW));
    ZeroMemory(Result, sizeof(*Result));
    Buffer = HeapAlloc(GetProcessHeap(), 0, cbIn + GUARD_LEN);
    if (!Buffer)
    {
        skip("No memory\n");
        Result->Ret = -2;
        return;
    }
    FillMemory(Buffer, cbIn, FILL_BYTE);
    FillMemory(Buffer + cbIn, GUARD_LEN, GUARD_BYTE);

    Result->Size = cbIn;
    _SEH2_TRY
    {
        Result->Ret = RegGetValueW(HKEY_CURRENT_USER, TEST_KEY, szValueW, RRF_RT_REG_SZ,
                                   &Result->Type, Buffer, &Result->Size);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Result->Faulted = TRUE;
        Result->Ret = -1;
    }
    _SEH2_END;

    Result->GuardOk = TRUE;
    for (i = 0; i < GUARD_LEN; i++)
    {
        if (Buffer[cbIn + i] != GUARD_BYTE)
        {
            Result->GuardOk = FALSE;
            break;
        }
    }
    CopyMemory(Result->Data, Buffer, min(cbIn, sizeof(Result->Data)));
    HeapFree(GetProcessHeap(), 0, Buffer);
}

/* The call must stay inside the buffer it was given and must not fault */
#define ok_safe(r) \
    do { \
        ok(!(r)->Faulted, "call faulted\n"); \
        ok((r)->GuardOk, "write past the end of the buffer\n"); \
    } while (0)

static BOOL
IsAllByte(const BYTE *p, DWORD cb, BYTE b)
{
    DWORD i;
    for (i = 0; i < cb; i++)
    {
        if (p[i] != b)
            return FALSE;
    }
    return TRUE;
}

/* Stores the bytes as given: RegSetValueExW can add a terminator */
static void
SetRaw(PCWSTR pszName, DWORD dwType, const void *pData, DWORD cbData)
{
    UNICODE_STRING Name;
    NTSTATUS Status;
    DWORD dwStoredType = 0, cbStored = 0;
    LONG ret;

    RtlInitUnicodeString(&Name, pszName);
    Status = NtSetValueKey(hTestKey, &Name, 0, dwType, (PVOID)pData, cbData);
    ok(Status == STATUS_SUCCESS, "NtSetValueKey(%ls) failed with 0x%lx\n", pszName, Status);

    ret = RegQueryValueExW(hTestKey, pszName, NULL, &dwStoredType, NULL, &cbStored);
    ok(ret == ERROR_SUCCESS, "RegQueryValueExW(%ls) failed with %ld\n", pszName, ret);
    ok(dwStoredType == dwType && cbStored == cbData, "%ls stored as type %lu size %lu\n",
       pszName, dwStoredType, cbStored);
}

static void
TestExpandLong(void)
{
    CALL_RESULT r;
    WCHAR szExpected[LONG_CHARS + 1];
    CHAR szExpectedA[LONG_CHARS + 1];
    const DWORD cbExp = (LONG_CHARS + 1) * sizeof(WCHAR);
    const DWORD cbRaw = sizeof(L"%REGGET_LONG%");
    DWORD i;

    for (i = 0; i < LONG_CHARS; i++)
    {
        szExpected[i] = L'x';
        szExpectedA[i] = 'x';
    }
    szExpected[LONG_CHARS] = UNICODE_NULL;
    szExpectedA[LONG_CHARS] = ANSI_NULL;

    /* Size query */
    DoCall(TRUE, "ExpLong", RRF_RT_REG_SZ, FALSE, 0, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == cbExp, "size %lu\n", r.Size);
    ok(r.Type == REG_SZ, "type %lu\n", r.Type);

    /* Size query with a nonzero size and no buffer */
    DoCall(TRUE, "ExpLong", RRF_RT_REG_SZ, FALSE, 1000, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == cbExp, "size %lu\n", r.Size);

    /* The raw value fits, the expanded one does not */
    DoCall(TRUE, "ExpLong", RRF_RT_REG_SZ, TRUE, 520, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == cbExp, "size %lu\n", r.Size);

    /* One character short */
    DoCall(TRUE, "ExpLong", RRF_RT_REG_SZ, TRUE, cbExp - sizeof(WCHAR), &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == cbExp, "size %lu\n", r.Size);

    /* One byte short */
    DoCall(TRUE, "ExpLong", RRF_RT_REG_SZ, TRUE, cbExp - 1, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == cbExp, "size %lu\n", r.Size);

    /* Exact size */
    DoCall(TRUE, "ExpLong", RRF_RT_REG_SZ, TRUE, cbExp, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == cbExp, "size %lu\n", r.Size);
    ok(r.Type == REG_SZ, "type %lu\n", r.Type);
    ok(!memcmp(r.Data, szExpected, cbExp), "wrong data\n");

    /* One byte more (odd size) */
    DoCall(TRUE, "ExpLong", RRF_RT_REG_SZ, TRUE, cbExp + 1, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == cbExp, "size %lu\n", r.Size);
    ok(!memcmp(r.Data, szExpected, cbExp), "wrong data\n");

    /* A small buffer is zeroed on failure, and only that buffer */
    DoCall(TRUE, "ExpLong", RRF_RT_REG_SZ | RRF_ZEROONFAILURE, TRUE, 520, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(IsAllByte(r.Data, 520, 0), "buffer not zeroed\n");

    /* No expansion */
    DoCall(TRUE, "ExpLong", RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, TRUE, cbRaw, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == cbRaw, "size %lu\n", r.Size);
    ok(r.Type == REG_EXPAND_SZ, "type %lu\n", r.Type);
    ok(!memcmp(r.Data, L"%REGGET_LONG%", cbRaw), "wrong data\n");

    /* Windows reports one character more than needed here */
    DoCall(TRUE, "ExpLong", RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, TRUE, cbRaw - sizeof(WCHAR), &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size >= cbRaw && r.Size <= cbRaw + sizeof(WCHAR), "size %lu\n", r.Size);
    ok(r.Type == REG_EXPAND_SZ, "type %lu\n", r.Type);

    /* Only the type is asked for: it is the type after expansion */
    r.Type = 0xdeadbeef;
    r.Ret = RegGetValueW(hTestKey, NULL, L"ExpLong", RRF_RT_REG_SZ, &r.Type, NULL, NULL);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Type == REG_SZ, "type %lu\n", r.Type);

    /* Through a subkey name */
    DoCallSubKey("ExpLong", 520, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == cbExp, "size %lu\n", r.Size);

    DoCallSubKey("ExpLong", cbExp, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == cbExp, "size %lu\n", r.Size);
    ok(!memcmp(r.Data, szExpected, cbExp), "wrong data\n");

    /* RRF_RT_ANY expands too */
    DoCall(TRUE, "ExpLong", RRF_RT_ANY, TRUE, 520, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == cbExp, "size %lu\n", r.Size);

    DoCall(TRUE, "ExpLong", RRF_RT_ANY, TRUE, cbExp, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Type == REG_SZ, "type %lu\n", r.Type);
    ok(!memcmp(r.Data, szExpected, cbExp), "wrong data\n");

    /* ANSI. The size query and ERROR_MORE_DATA ask one byte more than the
     * expanded string, as ExpandEnvironmentStringsA does. Windows also fails
     * a buffer of the exact size; ReactOS fills it. */
    DoCall(FALSE, "ExpLong", RRF_RT_REG_SZ, FALSE, 0, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == LONG_CHARS + 2, "size %lu\n", r.Size);

    DoCall(FALSE, "ExpLong", RRF_RT_REG_SZ, FALSE, 1000, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == LONG_CHARS + 2, "size %lu\n", r.Size);

    /* A zero-size buffer */
    DoCall(FALSE, "ExpLong", RRF_RT_REG_SZ, TRUE, 0, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == LONG_CHARS + 2, "size %lu\n", r.Size);

    DoCall(FALSE, "ExpLong", RRF_RT_REG_SZ, TRUE, 260, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == LONG_CHARS + 2, "size %lu\n", r.Size);

    DoCall(FALSE, "ExpLong", RRF_RT_REG_SZ, TRUE, LONG_CHARS, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == LONG_CHARS + 2, "size %lu\n", r.Size);

    DoCall(FALSE, "ExpLong", RRF_RT_REG_SZ, TRUE, LONG_CHARS + 1, &r);
    ok_safe(&r);
    ok((r.Ret == ERROR_SUCCESS && r.Size == LONG_CHARS + 1 && !memcmp(r.Data, szExpectedA, LONG_CHARS + 1)) ||
       (r.Ret == ERROR_MORE_DATA && r.Size == LONG_CHARS + 2),
       "ret %ld size %lu\n", r.Ret, r.Size);

    DoCall(FALSE, "ExpLong", RRF_RT_REG_SZ, TRUE, LONG_CHARS + 2, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == LONG_CHARS + 1 || r.Size == LONG_CHARS + 2, "size %lu\n", r.Size);
    ok(!memcmp(r.Data, szExpectedA, LONG_CHARS + 1), "wrong data\n");

    /* RRF_NOEXPAND keeps the raw string */
    DoCall(FALSE, "ExpLong", RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, TRUE, sizeof("%REGGET_LONG%"), &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == sizeof("%REGGET_LONG%"), "size %lu\n", r.Size);
    ok(r.Type == REG_EXPAND_SZ, "type %lu\n", r.Type);
    ok(!memcmp(r.Data, "%REGGET_LONG%", sizeof("%REGGET_LONG%")), "wrong data\n");
}


static void
TestExpandShrink(void)
{
    CALL_RESULT r;
    const DWORD cbRaw = sizeof(L"%REGGET_ONE%abc");
    const DWORD cbExp = sizeof(L"Qabc");

    /* Windows reports the larger of the raw and the expanded size and needs a
     * buffer for the raw string; ReactOS reports the expanded size */
    DoCall(TRUE, "ExpShrink", RRF_RT_REG_SZ, FALSE, 0, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == cbExp || r.Size == cbRaw, "size %lu\n", r.Size);

    DoCall(TRUE, "ExpShrink", RRF_RT_REG_SZ, TRUE, cbExp - 1, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == cbExp || r.Size == cbRaw, "size %lu\n", r.Size);

    DoCall(TRUE, "ExpShrink", RRF_RT_REG_SZ, TRUE, cbExp, &r);
    ok_safe(&r);
    ok((r.Ret == ERROR_SUCCESS && r.Size == cbExp && !memcmp(r.Data, L"Qabc", cbExp)) ||
       (r.Ret == ERROR_MORE_DATA && r.Size == cbRaw),
       "ret %ld size %lu\n", r.Ret, r.Size);

    DoCall(TRUE, "ExpShrink", RRF_RT_REG_SZ, TRUE, cbRaw, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == cbExp || r.Size == cbRaw, "size %lu\n", r.Size);
    ok(!memcmp(r.Data, L"Qabc", cbExp), "wrong data\n");

    DoCall(FALSE, "ExpShrink", RRF_RT_REG_SZ, TRUE, sizeof("Qabc"), &r);
    ok_safe(&r);
    ok((r.Ret == ERROR_SUCCESS && r.Size == sizeof("Qabc") && !memcmp(r.Data, "Qabc", sizeof("Qabc"))) ||
       (r.Ret == ERROR_MORE_DATA && r.Size == sizeof("%REGGET_ONE%abc")),
       "ret %ld size %lu\n", r.Ret, r.Size);

    DoCall(FALSE, "ExpShrink", RRF_RT_REG_SZ, TRUE, 64, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == sizeof("Qabc") || r.Size == sizeof("%REGGET_ONE%abc"), "size %lu\n", r.Size);
    ok(!memcmp(r.Data, "Qabc", sizeof("Qabc")), "wrong data\n");
}


static void
TestNoTerminator(void)
{
    CALL_RESULT r;

    /* REG_SZ "ABCDEFGH" stored without a terminator: 16 bytes */
    DoCall(TRUE, "SzNoTerm", RRF_RT_REG_SZ, FALSE, 0, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == 18, "size %lu\n", r.Size);

    DoCall(TRUE, "SzNoTerm", RRF_RT_REG_SZ, TRUE, 16, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == 18, "size %lu\n", r.Size);

    DoCall(TRUE, "SzNoTerm", RRF_RT_REG_SZ, TRUE, 17, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == 18, "size %lu\n", r.Size);

    DoCall(TRUE, "SzNoTerm", RRF_RT_REG_SZ, TRUE, 18, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == 18, "size %lu\n", r.Size);
    ok(!memcmp(r.Data, L"ABCDEFGH", 18), "not terminated\n");

    DoCall(TRUE, "SzNoTerm", RRF_RT_ANY, TRUE, 16, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == 18, "size %lu\n", r.Size);

    DoCall(TRUE, "SzNoTerm", RRF_RT_REG_SZ | RRF_ZEROONFAILURE, TRUE, 16, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(IsAllByte(r.Data, 16, 0), "buffer not zeroed\n");

    DoCall(FALSE, "SzNoTerm", RRF_RT_REG_SZ, FALSE, 0, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == 9, "size %lu\n", r.Size);

    DoCall(FALSE, "SzNoTerm", RRF_RT_REG_SZ, TRUE, 8, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == 9, "size %lu\n", r.Size);

    DoCall(FALSE, "SzNoTerm", RRF_RT_REG_SZ, TRUE, 9, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == 9, "size %lu\n", r.Size);
    ok(!memcmp(r.Data, "ABCDEFGH", 9), "not terminated\n");

    /* REG_EXPAND_SZ "%REGGET_ONE%XY" without a terminator, expands to "QXY".
     * Windows reports the terminated raw size (30), ReactOS the expanded one. */
    DoCall(TRUE, "ExpNoTerm", RRF_RT_REG_SZ, FALSE, 0, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == sizeof(L"QXY") || r.Size == 30, "size %lu\n", r.Size);

    DoCall(TRUE, "ExpNoTerm", RRF_RT_REG_SZ, TRUE, 64, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == sizeof(L"QXY") || r.Size == 30, "size %lu\n", r.Size);
    ok(r.Type == REG_SZ, "type %lu\n", r.Type);
    ok(!memcmp(r.Data, L"QXY", sizeof(L"QXY")), "wrong data\n");

    /* Windows reports one character more than needed here */
    DoCall(TRUE, "ExpNoTerm", RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, TRUE, 28, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == 30 || r.Size == 32, "size %lu\n", r.Size);

    DoCall(TRUE, "ExpNoTerm", RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, TRUE, 30, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == 30, "size %lu\n", r.Size);
    ok(r.Type == REG_EXPAND_SZ, "type %lu\n", r.Type);
    ok(!memcmp(r.Data, L"%REGGET_ONE%XY", 30), "not terminated\n");

    DoCall(FALSE, "ExpNoTerm", RRF_RT_REG_SZ, TRUE, 64, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == sizeof("QXY") || r.Size == 15, "size %lu\n", r.Size);
    ok(!memcmp(r.Data, "QXY", sizeof("QXY")), "wrong data\n");

    /* REG_SZ with an odd byte count, "ABC" and one byte of 'D': the partial
     * character is dropped. Windows reports 9 bytes in the size query. */
    DoCall(TRUE, "SzOdd", RRF_RT_REG_SZ, FALSE, 0, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == 8 || r.Size == 9, "size %lu\n", r.Size);

    DoCall(TRUE, "SzOdd", RRF_RT_REG_SZ, TRUE, 7, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == 8, "size %lu\n", r.Size);

    DoCall(TRUE, "SzOdd", RRF_RT_REG_SZ, TRUE, 8, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == 8, "size %lu\n", r.Size);
    ok(!memcmp(r.Data, L"ABC", 8), "wrong data\n");

    DoCall(TRUE, "SzOdd", RRF_RT_REG_SZ, TRUE, 9, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == 8, "size %lu\n", r.Size);
    ok(!memcmp(r.Data, L"ABC", 8), "wrong data\n");

    /* REG_MULTI_SZ "A\0B" without its terminators: 6 bytes. Windows adds one
     * terminator, ReactOS the two a REG_MULTI_SZ needs. */
    DoCall(TRUE, "MultiNoTerm", RRF_RT_REG_MULTI_SZ, FALSE, 0, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == 10, "size %lu\n", r.Size);

    DoCall(TRUE, "MultiNoTerm", RRF_RT_REG_MULTI_SZ, TRUE, 6, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == 8 || r.Size == 10, "size %lu\n", r.Size);

    DoCall(TRUE, "MultiNoTerm", RRF_RT_REG_MULTI_SZ, TRUE, 8, &r);
    ok_safe(&r);
    ok((r.Ret == ERROR_SUCCESS && r.Size == 8 && !memcmp(r.Data, L"A\0B", 8)) ||
       (r.Ret == ERROR_MORE_DATA && r.Size == 10),
       "ret %ld size %lu\n", r.Ret, r.Size);

    DoCall(TRUE, "MultiNoTerm", RRF_RT_REG_MULTI_SZ, TRUE, 10, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == 8 || r.Size == 10, "size %lu\n", r.Size);
    ok(!memcmp(r.Data, L"A\0B", 8), "wrong data\n");
    ok(r.Size != 10 || (r.Data[8] == 0 && r.Data[9] == 0), "second terminator missing\n");

    /* Zero-byte REG_SZ */
    DoCall(TRUE, "SzEmpty", RRF_RT_REG_SZ, FALSE, 0, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == sizeof(WCHAR), "size %lu\n", r.Size);

    DoCall(TRUE, "SzEmpty", RRF_RT_REG_SZ, TRUE, 0, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == sizeof(WCHAR), "size %lu\n", r.Size);

    DoCall(TRUE, "SzEmpty", RRF_RT_REG_SZ, TRUE, 2, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == sizeof(WCHAR), "size %lu\n", r.Size);
    ok(r.Data[0] == 0 && r.Data[1] == 0, "not terminated\n");

    /* Terminated REG_SZ: size query, exact size, one byte short. Windows
     * reports one character more than needed in the size query and on
     * ERROR_MORE_DATA. */
    DoCall(TRUE, "SzTerm", RRF_RT_REG_SZ, FALSE, 0, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == sizeof(L"abc") || r.Size == sizeof(L"abc") + sizeof(WCHAR), "size %lu\n", r.Size);

    DoCall(TRUE, "SzTerm", RRF_RT_REG_SZ, TRUE, sizeof(L"abc"), &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == sizeof(L"abc"), "size %lu\n", r.Size);
    ok(!memcmp(r.Data, L"abc", sizeof(L"abc")), "wrong data\n");

    DoCall(TRUE, "SzTerm", RRF_RT_REG_SZ, TRUE, sizeof(L"abc") - 1, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == sizeof(L"abc") || r.Size == sizeof(L"abc") + sizeof(WCHAR), "size %lu\n", r.Size);
}

static void
TestOneByte(void)
{
    CALL_RESULT r;
    BYTE Buffer[1 + GUARD_LEN];
    DWORD dwType, cbData, i;
    LONG ret;

    /* RegQueryValueExW itself must not terminate past a one-byte buffer */
    FillMemory(Buffer, sizeof(Buffer), GUARD_BYTE);
    Buffer[0] = FILL_BYTE;
    cbData = 1;
    ret = RegQueryValueExW(hTestKey, L"SzOne", NULL, &dwType, Buffer, &cbData);
    ok(ret == ERROR_SUCCESS, "ret %ld\n", ret);
    ok(cbData == 1, "size %lu\n", cbData);
    for (i = 1; i < sizeof(Buffer); i++)
    {
        if (Buffer[i] != GUARD_BYTE)
            break;
    }
    ok(i == sizeof(Buffer), "RegQueryValueExW wrote past the buffer at %lu\n", i);

    /* The partial character is dropped: an empty string remains */
    DoCall(TRUE, "SzOne", RRF_RT_REG_SZ, FALSE, 0, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == 2 || r.Size == 3, "size %lu\n", r.Size);

    DoCall(TRUE, "SzOne", RRF_RT_REG_SZ, TRUE, 1, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_MORE_DATA, "ret %ld\n", r.Ret);
    ok(r.Size == 2 || r.Size == 3, "size %lu\n", r.Size);

    DoCall(TRUE, "SzOne", RRF_RT_REG_SZ, TRUE, 2, &r);
    ok_safe(&r);
    ok(r.Ret == ERROR_SUCCESS, "ret %ld\n", r.Ret);
    ok(r.Size == 2, "size %lu\n", r.Size);
    ok(r.Data[0] == 0 && r.Data[1] == 0, "not terminated\n");
}

static void
TestAliasedSubKey(void)
{
    WCHAR szName[64];
    DWORD dwBefore = 0, dwAfter = 0, cbData, i;
    HKEY hSubKey;
    LONG ret;

    /* A caller can pass the same buffer as subkey name and as output; the
     * subkey must still be closed. The default value is an empty string. */
    ret = RegCreateKeyExW(hTestKey, L"Alias", 0, NULL, REG_OPTION_VOLATILE,
                          KEY_SET_VALUE, NULL, &hSubKey, NULL);
    ok(ret == ERROR_SUCCESS, "RegCreateKeyExW failed with %ld\n", ret);
    if (ret != ERROR_SUCCESS)
        return;
    ret = RegSetValueExW(hSubKey, NULL, 0, REG_SZ, (const BYTE *)L"", sizeof(L""));
    ok(ret == ERROR_SUCCESS, "RegSetValueExW failed with %ld\n", ret);
    RegCloseKey(hSubKey);

    ok(GetProcessHandleCount(GetCurrentProcess(), &dwBefore), "GetProcessHandleCount failed\n");
    for (i = 0; i < 20; i++)
    {
        StringCbCopyW(szName, sizeof(szName), L"Alias");
        cbData = sizeof(szName);
        ret = RegGetValueW(hTestKey, szName, NULL, RRF_RT_REG_SZ, NULL, szName, &cbData);
        if (ret != ERROR_SUCCESS || szName[0])
            break;
    }
    ok(ret == ERROR_SUCCESS, "ret %ld\n", ret);
    ok(szName[0] == UNICODE_NULL, "wrong data\n");
    ok(GetProcessHandleCount(GetCurrentProcess(), &dwAfter), "GetProcessHandleCount failed\n");
    ok(dwAfter < dwBefore + 10, "handles before %lu, after %lu\n", dwBefore, dwAfter);

    RegDeleteKeyW(hTestKey, L"Alias");
}

static void
TestAliasedValueName(void)
{
    SYSTEM_INFO si;
    BYTE *pPages;
    WCHAR *pszBuf;
    DWORD dwOld = 0, dwType = 0, cbData = 8;
    LONG ret = -1;
    BOOL bFaulted = FALSE;

    /* The value name is passed in the output buffer, which ends at an
     * inaccessible page. The value "v" is 8 bytes without a terminator, so
     * it fills the buffer and the name is gone after the first read. */
    GetSystemInfo(&si);
    pPages = VirtualAlloc(NULL, 2 * si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ok(pPages != NULL, "VirtualAlloc failed with %lu\n", GetLastError());
    if (!pPages)
        return;
    ok(VirtualProtect(pPages + si.dwPageSize, si.dwPageSize, PAGE_NOACCESS, &dwOld),
       "VirtualProtect failed with %lu\n", GetLastError());

    pszBuf = (WCHAR *)(pPages + si.dwPageSize - 8);
    pszBuf[0] = L'v';
    pszBuf[1] = pszBuf[2] = pszBuf[3] = UNICODE_NULL;
    _SEH2_TRY
    {
        ret = RegGetValueW(hTestKey, NULL, pszBuf, RRF_RT_REG_SZ, &dwType, pszBuf, &cbData);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        bFaulted = TRUE;
    }
    _SEH2_END;
    ok(!bFaulted, "call faulted\n");
    ok(ret == ERROR_MORE_DATA, "ret %ld\n", ret);
    ok(cbData == 10, "size %lu\n", cbData);

    VirtualFree(pPages, 0, MEM_RELEASE);
}

static void
TestTypeOnly(void)
{
    DWORD dwType;
    LONG ret;

    /* RRF_RT_DWORD and RRF_RT_QWORD accept REG_BINARY of the right size, also
     * when nothing but the type is asked for */
    dwType = 0xdeadbeef;
    ret = RegGetValueW(hTestKey, NULL, L"Bin4", RRF_RT_DWORD, &dwType, NULL, NULL);
    ok(ret == ERROR_SUCCESS, "ret %ld\n", ret);
    ok(dwType == REG_BINARY, "type %lu\n", dwType);

    dwType = 0xdeadbeef;
    ret = RegGetValueA(hTestKey, NULL, "Bin4", RRF_RT_DWORD, &dwType, NULL, NULL);
    ok(ret == ERROR_SUCCESS, "ret %ld\n", ret);
    ok(dwType == REG_BINARY, "type %lu\n", dwType);

    /* Windows does not check the size when no size is asked for */
    ret = RegGetValueW(hTestKey, NULL, L"Bin4", RRF_RT_QWORD, NULL, NULL, NULL);
    ok(ret == ERROR_DATATYPE_MISMATCH || ret == ERROR_SUCCESS, "ret %ld\n", ret);

    ret = RegGetValueA(hTestKey, NULL, "Bin4", RRF_RT_QWORD, NULL, NULL, NULL);
    ok(ret == ERROR_DATATYPE_MISMATCH || ret == ERROR_SUCCESS, "ret %ld\n", ret);

    /* Only the type of an expandable value: REG_SZ after expansion */
    dwType = 0xdeadbeef;
    ret = RegGetValueA(hTestKey, NULL, "ExpLong", RRF_RT_REG_SZ, &dwType, NULL, NULL);
    ok(ret == ERROR_SUCCESS, "ret %ld\n", ret);
    ok(dwType == REG_SZ, "type %lu\n", dwType);
}

START_TEST(RegGetValue)
{
    static const WCHAR NoTerm[8] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H' };
    static const WCHAR ExpNoTerm[14] = { '%', 'R', 'E', 'G', 'G', 'E', 'T', '_', 'O', 'N', 'E', '%', 'X', 'Y' };
    static const BYTE Odd[7] = { 'A', 0, 'B', 0, 'C', 0, 'D' };
    static const WCHAR MultiNoTerm[3] = { 'A', 0, 'B' };
    static const BYTE One[1] = { 'A' };
    static const BYTE Bin4[4] = { 1, 2, 3, 4 };
    static const WCHAR V[4] = { 'A', 'B', 'C', 'D' };
    WCHAR szLong[LONG_CHARS + 1];
    LONG ret;
    DWORD i;

    for (i = 0; i < LONG_CHARS; i++)
        szLong[i] = L'x';
    szLong[LONG_CHARS] = UNICODE_NULL;
    ok(SetEnvironmentVariableW(L"REGGET_LONG", szLong), "SetEnvironmentVariableW failed\n");
    ok(SetEnvironmentVariableW(L"REGGET_ONE", L"Q"), "SetEnvironmentVariableW failed\n");

    RegDeleteKeyW(HKEY_CURRENT_USER, TEST_KEY);
    ret = RegCreateKeyExW(HKEY_CURRENT_USER, TEST_KEY, 0, NULL, REG_OPTION_VOLATILE,
                          KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE,
                          NULL, &hTestKey, NULL);
    if (ret != ERROR_SUCCESS)
    {
        skip("RegCreateKeyExW failed with %ld\n", ret);
        return;
    }

    SetRaw(L"ExpLong", REG_EXPAND_SZ, L"%REGGET_LONG%", sizeof(L"%REGGET_LONG%"));
    SetRaw(L"ExpShrink", REG_EXPAND_SZ, L"%REGGET_ONE%abc", sizeof(L"%REGGET_ONE%abc"));
    SetRaw(L"SzNoTerm", REG_SZ, NoTerm, sizeof(NoTerm));
    SetRaw(L"ExpNoTerm", REG_EXPAND_SZ, ExpNoTerm, sizeof(ExpNoTerm));
    SetRaw(L"SzOdd", REG_SZ, Odd, sizeof(Odd));
    SetRaw(L"MultiNoTerm", REG_MULTI_SZ, MultiNoTerm, sizeof(MultiNoTerm));
    SetRaw(L"SzEmpty", REG_SZ, NULL, 0);
    SetRaw(L"SzTerm", REG_SZ, L"abc", sizeof(L"abc"));
    SetRaw(L"SzOne", REG_SZ, One, sizeof(One));
    SetRaw(L"Bin4", REG_BINARY, Bin4, sizeof(Bin4));
    SetRaw(L"v", REG_SZ, V, sizeof(V));

    TestExpandLong();
    TestExpandShrink();
    TestNoTerminator();
    TestOneByte();
    TestAliasedSubKey();
    TestAliasedValueName();
    TestTypeOnly();

    RegCloseKey(hTestKey);
    RegDeleteKeyW(HKEY_CURRENT_USER, TEST_KEY);
    SetEnvironmentVariableW(L"REGGET_LONG", NULL);
    SetEnvironmentVariableW(L"REGGET_ONE", NULL);
}
