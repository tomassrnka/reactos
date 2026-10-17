/* ReactOS WoW64 USER miscellaneous service translation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64win.h"

ULONG_PTR Wow64WinUserMiscService(ULONG Number, ULONG *a)
{
    switch (Number)
    {
    default:
        DbgPrint("WoW64win: unsupported service %lx\n", Number);
        RtlSetLastWin32Error(ERROR_CALL_NOT_IMPLEMENTED);
        return Wow64WinFailure(Number, STATUS_NOT_IMPLEMENTED);
    }
}
