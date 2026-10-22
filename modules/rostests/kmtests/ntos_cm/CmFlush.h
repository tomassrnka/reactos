/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite registry flush failure test declarations
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _KMTEST_CMFLUSH_H_
#define _KMTEST_CMFLUSH_H_

/* Status the test disk completes flushes with (input: ULONG) */
#define IOCTL_CMFLUSH_SET_FLUSH_STATUS  1
/* Number of flushes the test disk received (output: ULONG) */
#define IOCTL_CMFLUSH_GET_FLUSH_COUNT   2

#define CMFLUSH_DISK_NAME L"\\Device\\KmtCmFlushDisk"

#endif /* _KMTEST_CMFLUSH_H_ */
