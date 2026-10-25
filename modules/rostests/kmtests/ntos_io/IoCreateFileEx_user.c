/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     IoCreateFileEx test, user-mode part: runs the driver, which does the creates
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>
#include "IoCreateFileEx.h"

START_TEST(IoCreateFileEx)
{
    DWORD Error;

    Error = KmtLoadAndOpenDriver(L"IoCreateFileEx", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    Error = KmtSendToDriver(IOCTL_RUN_ECP_TEST);
    ok(Error == ERROR_SUCCESS, "Expected ERROR_SUCCESS, got %lx\n", Error);

    KmtCloseDriver();
    KmtUnloadDriver();
}
