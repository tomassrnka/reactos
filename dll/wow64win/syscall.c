/* ReactOS WoW64 win32k translation. SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64win.h"

ULONG_PTR Wow64WinFailure(ULONG Number, NTSTATUS Status)
{
    switch (Number)
    {
#include "failure.inc"
    case W64_NtUserGetMessage: return (ULONG_PTR)-1;
    default: return 0;
    }
}

static ULONG_PTR Dispatch(ULONG Number, ULONG *a)
{
    switch (Number)
    {
#include "generated.inc"
    default: return Wow64WinUserService(Number,a);
    }
}

NTSTATUS NTAPI Wow64WinSystemService(ULONG Number, ULONG *a)
{
    ULONG_PTR Result;
    DWORD LastError;
    /* win32k writes the native TEB, whereas guest SetLastError reads TEB32. */
    NtCurrentTeb()->LastErrorValue = WOW64_TEB32(NtCurrentTeb())->LastErrorValue;
    _SEH2_TRY
    {
        Result = Dispatch(Number,a);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        NtCurrentTeb()->LastErrorValue = RtlNtStatusToDosError(_SEH2_GetExceptionCode());
        Result = Wow64WinFailure(Number,_SEH2_GetExceptionCode());
    }
    _SEH2_END;
    LastError = NtCurrentTeb()->LastErrorValue;
    WOW64_TEB32(NtCurrentTeb())->LastErrorValue = LastError;
    return (ULONG)Result;
}
