/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for the SRW lock try functions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

typedef VOID (NTAPI *PFN_SRW)(PRTL_SRWLOCK);
typedef BOOLEAN (NTAPI *PFN_TRY_SRW)(PRTL_SRWLOCK);

static PFN_SRW pRtlInitializeSRWLock;
static PFN_SRW pRtlAcquireSRWLockShared;
static PFN_SRW pRtlReleaseSRWLockShared;
static PFN_SRW pRtlAcquireSRWLockExclusive;
static PFN_SRW pRtlReleaseSRWLockExclusive;
static PFN_TRY_SRW pRtlTryAcquireSRWLockShared;
static PFN_TRY_SRW pRtlTryAcquireSRWLockExclusive;

START_TEST(RtlSRWLock)
{
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    RTL_SRWLOCK Lock;

    pRtlInitializeSRWLock = (PFN_SRW)GetProcAddress(hNtdll, "RtlInitializeSRWLock");
    pRtlAcquireSRWLockShared = (PFN_SRW)GetProcAddress(hNtdll, "RtlAcquireSRWLockShared");
    pRtlReleaseSRWLockShared = (PFN_SRW)GetProcAddress(hNtdll, "RtlReleaseSRWLockShared");
    pRtlAcquireSRWLockExclusive = (PFN_SRW)GetProcAddress(hNtdll, "RtlAcquireSRWLockExclusive");
    pRtlReleaseSRWLockExclusive = (PFN_SRW)GetProcAddress(hNtdll, "RtlReleaseSRWLockExclusive");
    pRtlTryAcquireSRWLockShared = (PFN_TRY_SRW)GetProcAddress(hNtdll, "RtlTryAcquireSRWLockShared");
    pRtlTryAcquireSRWLockExclusive = (PFN_TRY_SRW)GetProcAddress(hNtdll, "RtlTryAcquireSRWLockExclusive");
    if (!pRtlInitializeSRWLock || !pRtlAcquireSRWLockShared || !pRtlReleaseSRWLockShared ||
        !pRtlAcquireSRWLockExclusive || !pRtlReleaseSRWLockExclusive ||
        !pRtlTryAcquireSRWLockShared || !pRtlTryAcquireSRWLockExclusive)
    {
        skip("SRW lock functions are not available\n");
        return;
    }

    /* Try shared on a free lock, then release: the lock must be free again */
    pRtlInitializeSRWLock(&Lock);
    ok(pRtlTryAcquireSRWLockShared(&Lock), "Try shared on a free lock failed\n");
    ok(!pRtlTryAcquireSRWLockExclusive(&Lock), "Try exclusive on a shared lock succeeded\n");
    pRtlReleaseSRWLockShared(&Lock);
    ok(Lock.Ptr == NULL, "Lock is %p after the release\n", Lock.Ptr);
    ok(pRtlTryAcquireSRWLockExclusive(&Lock), "Try exclusive on a free lock failed\n");
    pRtlReleaseSRWLockExclusive(&Lock);
    ok(Lock.Ptr == NULL, "Lock is %p after the release\n", Lock.Ptr);

    /* Two shared owners through the try function */
    pRtlInitializeSRWLock(&Lock);
    ok(pRtlTryAcquireSRWLockShared(&Lock), "First try shared failed\n");
    ok(pRtlTryAcquireSRWLockShared(&Lock), "Second try shared failed\n");
    pRtlReleaseSRWLockShared(&Lock);
    ok(!pRtlTryAcquireSRWLockExclusive(&Lock), "Try exclusive with one shared owner left succeeded\n");
    pRtlReleaseSRWLockShared(&Lock);
    ok(Lock.Ptr == NULL, "Lock is %p after the releases\n", Lock.Ptr);

    /* Mixed with the blocking shared acquire */
    pRtlInitializeSRWLock(&Lock);
    pRtlAcquireSRWLockShared(&Lock);
    ok(pRtlTryAcquireSRWLockShared(&Lock), "Try shared on a shared lock failed\n");
    pRtlReleaseSRWLockShared(&Lock);
    pRtlReleaseSRWLockShared(&Lock);
    ok(Lock.Ptr == NULL, "Lock is %p after the releases\n", Lock.Ptr);

    /* Try shared must fail on an exclusively owned lock and leave it exclusive */
    pRtlInitializeSRWLock(&Lock);
    pRtlAcquireSRWLockExclusive(&Lock);
    ok(!pRtlTryAcquireSRWLockShared(&Lock), "Try shared on an exclusive lock succeeded\n");
    ok(!pRtlTryAcquireSRWLockExclusive(&Lock), "Try exclusive on an exclusive lock succeeded\n");
    StartSeh()
        pRtlReleaseSRWLockExclusive(&Lock);
    EndSeh(STATUS_SUCCESS);
    ok(Lock.Ptr == NULL, "Lock is %p after the release\n", Lock.Ptr);
}
