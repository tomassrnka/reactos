/* NtQuerySystemInformation(SystemRangeStartInformation): the start of system space as the
 * calling process sees it, a pointer-sized value. Under WoW64 (Windows Server 2008 R2 and
 * Windows 10 measured) it is the end of the 32-bit process's user range, the maximum user
 * address of SystemBasicInformation plus one. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "precomp.h"

START_TEST(Wow64SystemRangeStart)
{
    SYSTEM_BASIC_INFORMATION Basic;
    ULONG_PTR Start[2];
    ULONG Length;
    NTSTATUS Status;
    BOOL Wow64 = FALSE;

    IsWow64Process(GetCurrentProcess(), &Wow64);
    Status = NtQuerySystemInformation(SystemBasicInformation, &Basic, sizeof(Basic), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);

    Start[0] = Start[1] = 0x5a5a5a5a;
    Length = 0xdeadbeef;
    Status = NtQuerySystemInformation(SystemRangeStartInformation, Start, sizeof(Start[0]), &Length);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_long(Length, sizeof(ULONG_PTR));
    if (Wow64)
    {
        ok(Start[0] == Basic.MaximumUserModeAddress + 1, "Start %p, highest user address %p\n",
           (PVOID)Start[0], (PVOID)Basic.MaximumUserModeAddress);
        /* This test executable is not large address aware */
        ok(!(RtlImageNtHeader(GetModuleHandleW(NULL))->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE),
           "Large address aware\n");
        ok(Start[0] == 0x7fff0000, "Start %p\n", (PVOID)Start[0]);
    }
    else
        ok(Start[0] > Basic.MaximumUserModeAddress, "Start %p, highest user address %p\n",
           (PVOID)Start[0], (PVOID)Basic.MaximumUserModeAddress);
    ok(Start[1] == 0x5a5a5a5a, "Wrote past the value: %p\n", (PVOID)Start[1]);

    /* Without a return length */
    Start[0] = 0x5a5a5a5a;
    Status = NtQuerySystemInformation(SystemRangeStartInformation, Start, sizeof(Start[0]), NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok(Start[0] == Basic.MaximumUserModeAddress + 1 || !Wow64, "Start %p\n", (PVOID)Start[0]);

    /* The length must be exactly the size of a pointer. The return length after
     * this failure differs between Windows versions. */
    Start[0] = 0x5a5a5a5a;
    Status = NtQuerySystemInformation(SystemRangeStartInformation, Start, sizeof(Start), &Length);
    ok_ntstatus(Status, STATUS_INFO_LENGTH_MISMATCH);
    Status = NtQuerySystemInformation(SystemRangeStartInformation, Start, sizeof(Start[0]) - 1, &Length);
    ok_ntstatus(Status, STATUS_INFO_LENGTH_MISMATCH);
    ok(Start[0] == 0x5a5a5a5a, "Value written after a failure: %p\n", (PVOID)Start[0]);

    /* An invalid buffer; the length is checked first */
    Status = NtQuerySystemInformation(SystemRangeStartInformation, (PVOID)0x10, sizeof(Start[0]), NULL);
    ok_ntstatus(Status, STATUS_ACCESS_VIOLATION);
    Status = NtQuerySystemInformation(SystemRangeStartInformation, (PVOID)0x10, sizeof(Start), NULL);
    ok_ntstatus(Status, STATUS_INFO_LENGTH_MISMATCH);
    if (Wow64)
    {
        Status = NtQuerySystemInformation(SystemRangeStartInformation, (PVOID)(Basic.MaximumUserModeAddress + 1),
                                          sizeof(Start[0]), NULL);
        ok_ntstatus(Status, STATUS_ACCESS_VIOLATION);
    }
}
