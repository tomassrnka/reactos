/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     System-wide and per-process commit charge accounting
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define MODULE_INVOLVED_IN_ARM3
#include <mm/ARM3/miarm.h>

/* GLOBALS ********************************************************************/

/*
 * Pages below the commit limit that only kernel-tier charges (pool, kernel
 * stack growth) may use, so that user requests fail before the kernel does.
 */
SIZE_T MiCommitReserve;

/* PRIVATE FUNCTIONS **********************************************************/

static
VOID
MiUpdatePeakCommitment(
    _In_ SIZE_T TotalCommittedPages)
{
    SIZE_T PeakCommitment = MmPeakCommitment;

    while ((TotalCommittedPages > PeakCommitment) &&
           (InterlockedCompareExchangeSizeT(&MmPeakCommitment,
                                            TotalCommittedPages,
                                            PeakCommitment) != PeakCommitment))
    {
        PeakCommitment = MmPeakCommitment;
    }
}

CODE_SEG("INIT")
VOID
NTAPI
MiInitializeCommitLimit(VOID)
{
    SIZE_T RamPages;

    /*
     * The RAM part of the limit is what Mm can still hand out now, less a
     * small margin, plus whatever was charged already: those pages came out
     * of the same RAM.
     */
    RamPages = MmAvailablePages;
    if (RamPages > 1024) RamPages -= 1024;
    RamPages += MmTotalCommittedPages;

    MmTotalCommitLimit = RamPages;
    MmTotalCommitLimitMaximum = RamPages;

    MiCommitReserve = RamPages / 64;
    if (MiCommitReserve < 1024) MiCommitReserve = 1024;
    if (MiCommitReserve > 8192) MiCommitReserve = 8192;

    DPRINT1("Commit limit %Iu pages (RAM part), reserve %Iu pages\n",
            MmTotalCommitLimit, MiCommitReserve);
}

BOOLEAN
NTAPI
MiChargeCommit(
    _In_ SIZE_T Pages,
    _In_ MI_COMMIT_TIER Tier)
{
    SIZE_T OldCommitted, NewCommitted, Ceiling;

    if (Pages == 0)
        return TRUE;

    for (;;)
    {
        OldCommitted = *(volatile SIZE_T *)&MmTotalCommittedPages;
        NewCommitted = OldCommitted + Pages;
        if (NewCommitted < OldCommitted)
            return FALSE;

        Ceiling = *(volatile SIZE_T *)&MmTotalCommitLimit;
        if (Tier == MiCommitUser)
            Ceiling = (Ceiling > MiCommitReserve) ? (Ceiling - MiCommitReserve) : 0;

        if (NewCommitted > Ceiling)
            return FALSE;

        if (InterlockedCompareExchangeSizeT(&MmTotalCommittedPages,
                                            NewCommitted,
                                            OldCommitted) == OldCommitted)
        {
            break;
        }
    }

    MiUpdatePeakCommitment(NewCommitted);
    return TRUE;
}

VOID
NTAPI
MiReturnCommit(
    _In_ SIZE_T Pages)
{
    SIZE_T OldCommitted;

    if (Pages == 0)
        return;

    OldCommitted = InterlockedExchangeAddSizeT(&MmTotalCommittedPages, -(LONG_PTR)Pages);
    if (OldCommitted < Pages)
    {
        /* More was returned than charged: put the difference back */
        DPRINT1("MiReturnCommit: returning %Iu pages, only %Iu charged\n", Pages, OldCommitted);
        ASSERT(FALSE);
        InterlockedExchangeAddSizeT(&MmTotalCommittedPages, Pages - OldCommitted);
    }
}

BOOLEAN
NTAPI
MiWaitForCommitGrowth(
    _In_ SIZE_T Pages)
{
    SIZE_T Ceiling, Committed;

    /* Grow by what a user charge of this size lacks now, not by its size */
    Ceiling = MmTotalCommitLimit;
    Ceiling = (Ceiling > MiCommitReserve) ? (Ceiling - MiCommitReserve) : 0;
    Committed = MmTotalCommittedPages;
    if (Committed + Pages <= Ceiling)
        return TRUE;

    return MiWaitForPagingFileGrowth(Committed + Pages - Ceiling);
}

BOOLEAN
NTAPI
MiChargeCommitWithGrowth(
    _In_ SIZE_T Pages)
{
    ULONG Round;

    for (Round = 0; Round < 4; Round++)
    {
        if (MiChargeCommit(Pages, MiCommitUser))
            return TRUE;

        /*
         * Growth means waiting for file I/O in another thread. A caller that
         * holds a guarded or fast mutex may hold a lock that I/O needs.
         */
        if (KeAreAllApcsDisabled() || !MiWaitForCommitGrowth(Pages))
            break;
    }

    return FALSE;
}

VOID
NTAPI
MiAddProcessCommit(
    _Inout_ PEPROCESS Process,
    _In_ SIZE_T Pages)
{
    SIZE_T NewCharge, Peak;

    if (Pages == 0)
        return;

    NewCharge = InterlockedExchangeAddSizeT(&Process->CommitCharge, Pages) + Pages;

    Peak = Process->CommitChargePeak;
    while ((NewCharge > Peak) &&
           (InterlockedCompareExchangeSizeT(&Process->CommitChargePeak, NewCharge, Peak) != Peak))
    {
        Peak = Process->CommitChargePeak;
    }
}

VOID
NTAPI
MiSubtractProcessCommit(
    _Inout_ PEPROCESS Process,
    _In_ SIZE_T Pages)
{
    SIZE_T OldCharge;

    if (Pages == 0)
        return;

    OldCharge = InterlockedExchangeAddSizeT(&Process->CommitCharge, -(LONG_PTR)Pages);
    if (OldCharge < Pages)
    {
        DPRINT1("MiSubtractProcessCommit: process %p returns %Iu pages, only %Iu charged\n",
                Process, Pages, OldCharge);
        ASSERT(FALSE);
        InterlockedExchangeAddSizeT(&Process->CommitCharge, Pages - OldCharge);
    }
}

VOID
NTAPI
MiReturnProcessCommit(
    _Inout_ PEPROCESS Process,
    _In_ SIZE_T Pages)
{
    MiSubtractProcessCommit(Process, Pages);
    MiReturnCommit(Pages);
}

/* EOF */
