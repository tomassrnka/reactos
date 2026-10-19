/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite test for a kernel mode fast fail
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

/*
 * This test does not come back: a fast fail in kernel mode bug checks with
 * KERNEL_SECURITY_CHECK_FAILURE (0x139), with the fast fail code as the first
 * parameter. Check that bug check in the debug log. The test is hidden from the
 * test list, so it runs only when named.
 */

static
VOID
NTAPI
DECLSPEC_NOINLINE
CorruptBackLink(
    _Inout_ PLIST_ENTRY ListHead)
{
    /* The head's back link no longer points to the last entry */
    ListHead->Blink = ListHead;
}

START_TEST(KeFastFail)
{
    LIST_ENTRY ListHead, Entry;

    InitializeListHead(&ListHead);
    InsertTailList(&ListHead, &Entry);
    CorruptBackLink(&ListHead);

    DbgPrint("KeFastFail: removing a corrupted list entry, expecting bug check 0x139 with code %u\n",
             FAST_FAIL_CORRUPT_LIST_ENTRY);
    RemoveEntryList(&Entry);

    /* Not reached. The fast fail is noreturn, so code after it says nothing about
       a kernel that returns from it; the bug check in the debug log is the result */
}
