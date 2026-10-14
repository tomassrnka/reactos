/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
/* Fixed-width transport between the PE32 runner and the native probe. */
typedef struct _SHARED_VIEW_RESULT
{
    ULONG Mode;
    ULONG Wow64;
    LONG Status;
    ULONG Callbacks;
    ULONG FillCount;
    ULONG Failure;
    ULONGLONG Filled;
    ULONGLONG Handles[4];
    ULONGLONG Address[8];
    ULONGLONG End[8];
} SHARED_VIEW_RESULT;
