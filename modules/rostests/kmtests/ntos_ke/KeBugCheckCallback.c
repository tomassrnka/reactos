/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-Mode Test Suite test for a bug check callback
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

/*
 * This test does not come back: it registers a bug check callback and then bug
 * checks. The kernel calls the callback during the bug check only if
 * KeRegisterBugCheckCallback set the record checksum. The callback writes
 * KeBugCheckCallback:CALLED to COM1, and the test passes if that line is in the
 * serial log (a debug build and a release build alike).
 * The test is hidden from the test list, so it runs only when named.
 */

#define COM1_PORT ((PUCHAR)(ULONG_PTR)0x3F8)

static KBUGCHECK_CALLBACK_RECORD CallbackRecord;
static UCHAR CallbackBuffer[16];

static
VOID
NTAPI
BugCheckCallback(
    _In_ PVOID Buffer,
    _In_ ULONG Length)
{
    static const CHAR Marker[] = "\nKeBugCheckCallback:CALLED\n";
    ULONG i;

    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Length);

    /* Write straight to the UART, the callback runs at HIGH_LEVEL during the bug check */
    for (i = 0; i < sizeof(Marker) - 1; i++)
        WRITE_PORT_UCHAR(COM1_PORT, Marker[i]);
}

START_TEST(KeBugCheckCallback)
{
    BOOLEAN Registered;

    Registered = KeRegisterBugCheckCallback(&CallbackRecord,
                                            BugCheckCallback,
                                            CallbackBuffer,
                                            sizeof(CallbackBuffer),
                                            (PUCHAR)"KmtTest");
    ok(Registered, "KeRegisterBugCheckCallback failed\n");
    if (!Registered)
        return;

    DbgPrint("KeBugCheckCallback: bug checking, expecting the callback line in the serial log\n");
    KeBugCheckEx(MANUALLY_INITIATED_CRASH, 0, 0, 0, 0);
}
