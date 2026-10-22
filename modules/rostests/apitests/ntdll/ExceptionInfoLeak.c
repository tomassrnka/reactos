/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that a hardware fault record does not leak kernel R11 to user mode
 */

#include "precomp.h"

/*
 * amd64 only: a user-mode hardware fault is delivered by trap.S, which fills
 * EXCEPTION_RECORD.ExceptionInformation[0..2] from r9/r10/r11 regardless of the
 * exception's NumberParameters, and KiDispatchExceptionToUser then copied the
 * whole record to the user stack. For an access violation (NumberParameters==2)
 * ExceptionInformation[2] is unused, so an unfixed kernel handed back the R11
 * register (a kernel pointer) to user mode. i386 copies only NumberParameters
 * parameters, so there is nothing to test there.
 */
#ifdef _M_AMD64

START_TEST(ExceptionInfoLeak)
{
    EXCEPTION_RECORD Seen;
    volatile ULONG_PTR *Bad = (volatile ULONG_PTR *)(ULONG_PTR)0x4;
    ULONG i;

    /* Poison the capture buffer so a short copy is visible. */
    RtlFillMemory(&Seen, sizeof(Seen), 0x5A);

    _SEH2_TRY
    {
        /* Access violation: the kernel reports NumberParameters == 2
           (access type + address); slot 2 is unused. */
        *Bad = 0;
    }
    _SEH2_EXCEPT((Seen = *_SEH2_GetExceptionInformation()->ExceptionRecord),
                 EXCEPTION_EXECUTE_HANDLER)
    {
    }
    _SEH2_END;

    ok(Seen.ExceptionCode == (DWORD)STATUS_ACCESS_VIOLATION,
       "ExceptionCode 0x%lx\n", Seen.ExceptionCode);
    ok(Seen.NumberParameters == 2, "NumberParameters %lu\n",
       (ULONG)Seen.NumberParameters);

    /* The unused parameter slots must be zero, not stale kernel register or
       stack contents. Before the fix, slot 2 carried the kernel R11 value. */
    ok(Seen.ExceptionInformation[2] == 0,
       "ExceptionInformation[2] leaked kernel data: 0x%p\n",
       (PVOID)Seen.ExceptionInformation[2]);
    for (i = 3; i < EXCEPTION_MAXIMUM_PARAMETERS; i++)
    {
        ok(Seen.ExceptionInformation[i] == 0,
           "ExceptionInformation[%lu] = 0x%p\n", i,
           (PVOID)Seen.ExceptionInformation[i]);
    }
}

#else /* _M_AMD64 */

START_TEST(ExceptionInfoLeak)
{
    skip("The R11 fault-record leak is amd64 only\n");
}

#endif /* _M_AMD64 */
