/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for NtQuerySystemInformation
 * COPYRIGHT:   Copyright 2019 Thomas Faber (thomas.faber@reactos.org)
 *              Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

static
void
Test_FileCacheInformation(
    _In_ SYSTEM_INFORMATION_CLASS InfoClass,
    _Out_ PSYSTEM_FILECACHE_INFORMATION Result)
{
    SYSTEM_FILECACHE_INFORMATION Info;
    ULONG ReturnLength;
    NTSTATUS Status;

    ReturnLength = 0x55555555;
    Status = NtQuerySystemInformation(InfoClass, &Info, sizeof(Info.CurrentSize), &ReturnLength);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    RtlFillMemory(&Info, sizeof(Info), 0x55);
    ReturnLength = 0x55555555;
    Status = NtQuerySystemInformation(InfoClass, &Info, sizeof(Info), &ReturnLength);
    ok_hex(Status, STATUS_SUCCESS);
    ok_long(ReturnLength, sizeof(Info));
    if (!NT_SUCCESS(Status))
        return;

    trace("Class %d: CurrentSize 0x%Ix PeakSize 0x%Ix MinimumWorkingSet 0x%Ix MaximumWorkingSet 0x%Ix "
          "CurrentSizeIncludingTransitionInPages 0x%Ix PeakSizeIncludingTransitionInPages 0x%Ix Flags 0x%lx\n",
          InfoClass, Info.CurrentSize, Info.PeakSize, Info.MinimumWorkingSet, Info.MaximumWorkingSet,
          Info.CurrentSizeIncludingTransitionInPages, Info.PeakSizeIncludingTransitionInPages, Info.Flags);

    /* The sizes are in bytes, the IncludingTransition ones in pages. ReactOS
     * derives both from one page count, so the byte size must be larger;
     * on Windows the standby pages counted in pages can outweigh it */
    if (is_reactos())
    {
        ok(Info.CurrentSize > Info.CurrentSizeIncludingTransitionInPages,
           "CurrentSize 0x%Ix is not larger than CurrentSizeIncludingTransitionInPages 0x%Ix\n",
           Info.CurrentSize, Info.CurrentSizeIncludingTransitionInPages);
    }
    ok(Info.CurrentSize % PAGE_SIZE == 0, "CurrentSize 0x%Ix is not a multiple of the page size\n", Info.CurrentSize);
    ok(Info.PeakSize % PAGE_SIZE == 0, "PeakSize 0x%Ix is not a multiple of the page size\n", Info.PeakSize);
    ok(Info.PeakSize >= Info.CurrentSize, "PeakSize 0x%Ix < CurrentSize 0x%Ix\n", Info.PeakSize, Info.CurrentSize);
    ok(Info.CurrentSize != (SIZE_T)0x5555555555555555ULL, "CurrentSize not written\n");
    *Result = Info;
}

START_TEST(NtQuerySystemInformation)
{
    SYSTEM_FILECACHE_INFORMATION FileCache;
    NTSTATUS Status;

    Status = NtQuerySystemInformation(0, NULL, 0, NULL);
    ok_hex(Status, STATUS_INFO_LENGTH_MISMATCH);

    Status = NtQuerySystemInformation(0x80000000, NULL, 0, NULL);
    ok_hex(Status, STATUS_INVALID_INFO_CLASS);

    RtlZeroMemory(&FileCache, sizeof(FileCache));
    Test_FileCacheInformation(SystemFileCacheInformation, &FileCache);
}
