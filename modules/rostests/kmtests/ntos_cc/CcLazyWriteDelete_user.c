/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite for a lazy write that races with the deletion of another cache map, user-mode part
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

#define IOCTL_RUN_TEST 1

START_TEST(CcLazyWriteDelete)
{
    DWORD Ret;

    Ret = KmtLoadAndOpenDriver(L"CcLazyWriteDelete", FALSE);
    ok_eq_int(Ret, ERROR_SUCCESS);
    if (Ret)
        return;

    Ret = KmtSendToDriver(IOCTL_RUN_TEST);
    ok(Ret == ERROR_SUCCESS, "KmtSendToDriver failed: %lx\n", Ret);

    KmtCloseDriver();
    KmtUnloadDriver();
}
