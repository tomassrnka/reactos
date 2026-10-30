/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for RtlVerifyVersionInfo and VerSetConditionMask
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected results were measured on Windows 10 (10.0.19045) with a probe
 * that called RtlVerifyVersionInfo for the same requests. Every request is
 * expressed relative to the running system's version, so the results do not
 * depend on which version that is.
 */

#include "precomp.h"

/* Request a major version of 3: lower than the version of any system that runs these tests */
#define MAJOR_LOW   (-100)

typedef struct _VERIFY_TEST
{
    LONG Delta[4];      /* Major, minor, SP major and SP minor, added to the running system's */
    ULONG TypeMask;
    UCHAR Condition[4]; /* Major, minor, SP major and SP minor; 0 = not set */
    NTSTATUS Status;
} VERIFY_TEST;

static const ULONG FieldType[4] =
{
    VER_MAJORVERSION, VER_MINORVERSION, VER_SERVICEPACKMAJOR, VER_SERVICEPACKMINOR
};

static const VERIFY_TEST VerifyTests[] =
{
    { { 0, 0, 0, 0 }, VER_SERVICEPACKMINOR, { 0, 0, 0, VER_EQUAL },
      STATUS_SUCCESS },
    { { 0, 0, 0, 1 }, VER_SERVICEPACKMINOR, { 0, 0, 0, VER_EQUAL },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 1, 0 }, VER_SERVICEPACKMAJOR, { 0, 0, VER_EQUAL, 0 },
      STATUS_REVISION_MISMATCH },
    { { 0, 1, 0, 0 }, VER_MINORVERSION, { 0, VER_EQUAL, 0, 0 },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 0 }, VER_MAJORVERSION, { VER_EQUAL, 0, 0, 0 },
      STATUS_SUCCESS },
    { { -1, 0, 0, 0 }, VER_MAJORVERSION, { VER_EQUAL, 0, 0, 0 },
      STATUS_REVISION_MISMATCH },
    { { 1, 0, 0, 0 }, VER_MAJORVERSION, { VER_EQUAL, 0, 0, 0 },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 0 }, VER_SERVICEPACKMINOR, { 0, 0, 0, VER_GREATER },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 1 }, VER_SERVICEPACKMINOR, { 0, 0, 0, VER_GREATER },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 1, 0 }, VER_SERVICEPACKMAJOR, { 0, 0, VER_GREATER, 0 },
      STATUS_REVISION_MISMATCH },
    { { 0, 1, 0, 0 }, VER_MINORVERSION, { 0, VER_GREATER, 0, 0 },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 0 }, VER_MAJORVERSION, { VER_GREATER, 0, 0, 0 },
      STATUS_SUCCESS },
    { { -1, 0, 0, 0 }, VER_MAJORVERSION, { VER_GREATER, 0, 0, 0 },
      STATUS_SUCCESS },
    { { 1, 0, 0, 0 }, VER_MAJORVERSION, { VER_GREATER, 0, 0, 0 },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 0 }, VER_SERVICEPACKMINOR, { 0, 0, 0, VER_GREATER_EQUAL },
      STATUS_SUCCESS },
    { { 0, 0, 0, 1 }, VER_SERVICEPACKMINOR, { 0, 0, 0, VER_GREATER_EQUAL },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 1, 0 }, VER_SERVICEPACKMAJOR, { 0, 0, VER_GREATER_EQUAL, 0 },
      STATUS_REVISION_MISMATCH },
    { { 0, 1, 0, 0 }, VER_MINORVERSION, { 0, VER_GREATER_EQUAL, 0, 0 },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 0 }, VER_MAJORVERSION, { VER_GREATER_EQUAL, 0, 0, 0 },
      STATUS_SUCCESS },
    { { -1, 0, 0, 0 }, VER_MAJORVERSION, { VER_GREATER_EQUAL, 0, 0, 0 },
      STATUS_SUCCESS },
    { { 1, 0, 0, 0 }, VER_MAJORVERSION, { VER_GREATER_EQUAL, 0, 0, 0 },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 0 }, VER_SERVICEPACKMINOR, { 0, 0, 0, VER_LESS },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 1 }, VER_SERVICEPACKMINOR, { 0, 0, 0, VER_LESS },
      STATUS_SUCCESS },
    { { 0, 0, 1, 0 }, VER_SERVICEPACKMAJOR, { 0, 0, VER_LESS, 0 },
      STATUS_SUCCESS },
    { { 0, 1, 0, 0 }, VER_MINORVERSION, { 0, VER_LESS, 0, 0 },
      STATUS_SUCCESS },
    { { 0, 0, 0, 0 }, VER_MAJORVERSION, { VER_LESS, 0, 0, 0 },
      STATUS_SUCCESS },
    { { -1, 0, 0, 0 }, VER_MAJORVERSION, { VER_LESS, 0, 0, 0 },
      STATUS_REVISION_MISMATCH },
    { { 1, 0, 0, 0 }, VER_MAJORVERSION, { VER_LESS, 0, 0, 0 },
      STATUS_SUCCESS },
    { { 0, 0, 0, 0 }, VER_SERVICEPACKMINOR, { 0, 0, 0, VER_LESS_EQUAL },
      STATUS_SUCCESS },
    { { 0, 0, 0, 1 }, VER_SERVICEPACKMINOR, { 0, 0, 0, VER_LESS_EQUAL },
      STATUS_SUCCESS },
    { { 0, 0, 1, 0 }, VER_SERVICEPACKMAJOR, { 0, 0, VER_LESS_EQUAL, 0 },
      STATUS_SUCCESS },
    { { 0, 1, 0, 0 }, VER_MINORVERSION, { 0, VER_LESS_EQUAL, 0, 0 },
      STATUS_SUCCESS },
    { { 0, 0, 0, 0 }, VER_MAJORVERSION, { VER_LESS_EQUAL, 0, 0, 0 },
      STATUS_SUCCESS },
    { { -1, 0, 0, 0 }, VER_MAJORVERSION, { VER_LESS_EQUAL, 0, 0, 0 },
      STATUS_REVISION_MISMATCH },
    { { 1, 0, 0, 0 }, VER_MAJORVERSION, { VER_LESS_EQUAL, 0, 0, 0 },
      STATUS_SUCCESS },
    { { 0, 0, 0, 0 }, VER_MINORVERSION | VER_SERVICEPACKMINOR, { 0, VER_EQUAL, 0, VER_GREATER },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 0 }, VER_MINORVERSION | VER_SERVICEPACKMINOR, { 0, VER_GREATER_EQUAL, 0, VER_GREATER },
      STATUS_SUCCESS },
    { { 0, 0, 0, 1 }, VER_MINORVERSION | VER_SERVICEPACKMINOR, { 0, VER_GREATER_EQUAL, 0, VER_GREATER_EQUAL },
      STATUS_REVISION_MISMATCH },
    { { -1, 0, 0, 0 }, VER_MINORVERSION | VER_SERVICEPACKMINOR, { 0, VER_EQUAL, 0, VER_GREATER },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 0 }, VER_MAJORVERSION | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, 0, 0, VER_GREATER },
      STATUS_SUCCESS },
    { { 0, 0, 0, 0 }, VER_MAJORVERSION | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, 0, 0, VER_GREATER_EQUAL },
      STATUS_SUCCESS },
    { { -1, 0, 0, 0 }, VER_MAJORVERSION | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, 0, 0, VER_GREATER },
      STATUS_SUCCESS },
    { { -1, 0, 0, 1 }, VER_MAJORVERSION | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, 0, 0, VER_GREATER },
      STATUS_SUCCESS },
    { { 0, 0, 0, 0 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMINOR, { VER_EQUAL, VER_EQUAL, 0, VER_GREATER },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 0 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMINOR, { VER_EQUAL, VER_EQUAL, 0, VER_GREATER_EQUAL },
      STATUS_SUCCESS },
    { { 0, 0, 0, 1 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, VER_GREATER_EQUAL, 0, VER_GREATER_EQUAL },
      STATUS_REVISION_MISMATCH },
    { { 0, 1, 0, 0 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMINOR, { VER_EQUAL, VER_EQUAL, 0, VER_GREATER },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 0 }, VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { 0, 0, VER_EQUAL, VER_GREATER },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 0 }, VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { 0, 0, VER_GREATER_EQUAL, VER_GREATER_EQUAL },
      STATUS_SUCCESS },
    { { 0, 0, 1, 0 }, VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { 0, 0, VER_GREATER_EQUAL, VER_GREATER },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 1 }, VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { 0, 0, VER_GREATER_EQUAL, VER_GREATER_EQUAL },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 0, 0 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, VER_GREATER_EQUAL, VER_GREATER_EQUAL, VER_GREATER_EQUAL },
      STATUS_SUCCESS },
    { { 0, 0, 0, 1 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, VER_GREATER_EQUAL, VER_GREATER_EQUAL, VER_GREATER_EQUAL },
      STATUS_REVISION_MISMATCH },
    { { 0, 0, 1, 0 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, VER_GREATER_EQUAL, VER_GREATER_EQUAL, VER_GREATER_EQUAL },
      STATUS_REVISION_MISMATCH },
    { { 0, 1, 0, 0 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, VER_GREATER_EQUAL, VER_GREATER_EQUAL, VER_GREATER_EQUAL },
      STATUS_REVISION_MISMATCH },
    { { -1, 0, 0, 1 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, 0, 0, 0 },
      STATUS_SUCCESS },
    { { 0, 0, 0, 0 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, VER_EQUAL, VER_GREATER, VER_GREATER },
      STATUS_SUCCESS },
    { { 0, 0, 0, 0 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, VER_EQUAL, VER_EQUAL, VER_GREATER },
      STATUS_SUCCESS },
    { { 0, 0, 0, 0 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_EQUAL, VER_EQUAL, VER_EQUAL, VER_GREATER },
      STATUS_REVISION_MISMATCH },
    { { MAJOR_LOW, 0, 0, 10 }, VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { 0, VER_GREATER_EQUAL, 0, 0 },
      STATUS_REVISION_MISMATCH },
    { { MAJOR_LOW, 0, 0, 10 }, VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, VER_GREATER_EQUAL, 0, 0 },
      STATUS_REVISION_MISMATCH },
    { { MAJOR_LOW, 0, 0, 10 }, VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_LESS, VER_GREATER_EQUAL, 0, 0 },
      STATUS_REVISION_MISMATCH },
    { { MAJOR_LOW, 0, 0, 10 }, VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, VER_LESS, 0, 0 },
      STATUS_SUCCESS },
    { { MAJOR_LOW, 0, 0, 0 }, VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, VER_GREATER_EQUAL, 0, 0 },
      STATUS_SUCCESS },
    { { MAJOR_LOW, 0, 10, 0 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, VER_GREATER_EQUAL, 0, 0 },
      STATUS_SUCCESS },
    { { MAJOR_LOW, 0, 0, 10 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, VER_GREATER_EQUAL, 0, 0 },
      STATUS_SUCCESS },
    { { MAJOR_LOW, 10, 0, 0 }, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, { VER_GREATER_EQUAL, VER_GREATER_EQUAL, 0, 0 },
      STATUS_SUCCESS },
};

#define TOUCHED 0x8000000000000000ULL

#define ok_hex64(expression, result) \
    do { \
        ULONGLONG Value_ = (expression); \
        ok(Value_ == (ULONGLONG)(result), "%s is 0x%I64x, expected 0x%I64x\n", \
           #expression, Value_, (ULONGLONG)(result)); \
    } while (0)

START_TEST(RtlVerifyVersionInfo)
{
    RTL_OSVERSIONINFOEXW Current, Request;
    ULONGLONG Mask;
    NTSTATUS Status;
    ULONG i, j;

    RtlZeroMemory(&Current, sizeof(Current));
    Current.dwOSVersionInfoSize = sizeof(Current);
    Status = RtlGetVersion((PRTL_OSVERSIONINFOW)&Current);
    ok_hex(Status, STATUS_SUCCESS);

    for (i = 0; i < RTL_NUMBER_OF(VerifyTests); i++)
    {
        const VERIFY_TEST *Test = &VerifyTests[i];

        Request = Current;
        if (Test->Delta[0] == MAJOR_LOW)
            Request.dwMajorVersion = 3;
        else
            Request.dwMajorVersion += Test->Delta[0];
        Request.dwMinorVersion += Test->Delta[1];
        Request.wServicePackMajor += (USHORT)Test->Delta[2];
        Request.wServicePackMinor += (USHORT)Test->Delta[3];

        Mask = 0;
        for (j = 0; j < 4; j++)
        {
            if (Test->Condition[j])
                Mask = VerSetConditionMask(Mask, FieldType[j], Test->Condition[j]);
        }

        Status = RtlVerifyVersionInfo(&Request, Test->TypeMask, Mask);
        ok(Status == Test->Status,
           "[%lu] RtlVerifyVersionInfo returned 0x%lx, expected 0x%lx\n",
           i, Status, Test->Status);
    }

    /* The type mask must not be empty, but the condition mask may be */
    Request = Current;
    Mask = VerSetConditionMask(0, VER_MAJORVERSION, VER_EQUAL);
    Status = RtlVerifyVersionInfo(&Request, 0, Mask);
    ok_hex(Status, STATUS_INVALID_PARAMETER);
    Status = RtlVerifyVersionInfo(&Request, 0, 0);
    ok_hex(Status, STATUS_INVALID_PARAMETER);
    Status = RtlVerifyVersionInfo(&Request, VER_MAJORVERSION, 0);
    ok_hex(Status, STATUS_SUCCESS);
    Status = RtlVerifyVersionInfo(&Request, VER_MAJORVERSION,
                                  VerSetConditionMask(0, VER_MINORVERSION, VER_EQUAL));
    ok_hex(Status, STATUS_SUCCESS);

    /* VerSetConditionMask flags the mask, whether or not a condition is set */
    ok_hex64(VerSetConditionMask(0, 0, VER_EQUAL), 0);
    ok_hex64(VerSetConditionMask(0x3f, 0, VER_GREATER), 0);
    ok_hex64(VerSetConditionMask(0, VER_MAJORVERSION, 0), TOUCHED);
    ok_hex64(VerSetConditionMask(0x38, VER_MAJORVERSION, 0), TOUCHED | 0x38);
    ok_hex64(VerSetConditionMask(0x38, VER_MAJORVERSION, VER_GREATER), TOUCHED | 0x38);
    ok_hex64(VerSetConditionMask(0, VER_MAJORVERSION, 0xf2), TOUCHED | (2 << 3));
    ok_hex64(VerSetConditionMask(TOUCHED, VER_MAJORVERSION, VER_GREATER_EQUAL), TOUCHED | (3 << 3));
    ok_hex64(VerSetConditionMask(0, VER_MINORVERSION, VER_LESS), TOUCHED | (4 << 0));
    ok_hex64(VerSetConditionMask(0, VER_BUILDNUMBER, VER_GREATER_EQUAL), TOUCHED | (3 << 6));
    ok_hex64(VerSetConditionMask(0, VER_PLATFORMID, VER_GREATER_EQUAL), TOUCHED | (3 << 9));
    ok_hex64(VerSetConditionMask(0, VER_SERVICEPACKMINOR, VER_GREATER_EQUAL), TOUCHED | (3 << 12));
    ok_hex64(VerSetConditionMask(0, VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL), TOUCHED | (3 << 15));
    ok_hex64(VerSetConditionMask(0, VER_SUITENAME, VER_GREATER_EQUAL), TOUCHED | (3 << 18));
    ok_hex64(VerSetConditionMask(0, VER_PRODUCT_TYPE, VER_GREATER_EQUAL), TOUCHED | (3 << 21));

    /* Only the type with the highest bit gets the condition */
    ok_hex64(VerSetConditionMask(0, VER_MAJORVERSION | VER_MINORVERSION, VER_GREATER_EQUAL), TOUCHED | (3 << 3));
    ok_hex64(VerSetConditionMask(0, VER_MINORVERSION | VER_SERVICEPACKMINOR, VER_GREATER_EQUAL), TOUCHED | (3 << 12));
    ok_hex64(VerSetConditionMask(0, VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR, VER_GREATER_EQUAL), TOUCHED | (3 << 15));
    ok_hex64(VerSetConditionMask(0, VER_BUILDNUMBER | VER_MAJORVERSION, VER_GREATER_EQUAL), TOUCHED | (3 << 6));
    ok_hex64(VerSetConditionMask(0, VER_SUITENAME | VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL), TOUCHED | (3 << 18));
    ok_hex64(VerSetConditionMask(0, VER_SUITENAME | VER_PRODUCT_TYPE, VER_GREATER_EQUAL), TOUCHED | (3 << 21));
}
