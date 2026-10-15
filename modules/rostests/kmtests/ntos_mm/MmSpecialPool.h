/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Kernel-Mode Test Suite special pool user-mode access test declarations
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _KMTEST_MMSPECIALPOOL_H_
#define _KMTEST_MMSPECIALPOOL_H_

#define IOCTL_GET_ADDRESSES 1

typedef struct _SPECIAL_POOL_ADDRESSES
{
    ULONGLONG Live;     /* a live special-pool allocation, held until the driver unloads */
    ULONGLONG Guard;    /* the first byte of its guard page */
    ULONGLONG Freed;    /* a special-pool allocation already freed */
    BOOLEAN Special;    /* FALSE when special pool did not serve the test tag */
} SPECIAL_POOL_ADDRESSES, *PSPECIAL_POOL_ADDRESSES;

#endif /* _KMTEST_MMSPECIALPOOL_H_ */
