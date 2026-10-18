/* ReactOS WoW64 USER raw-input and SendInput translation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64win.h"

ULONG_PTR Wow64WinUserInputService(ULONG Number, ULONG *a)
{
    switch (Number)
    {
    default:
        return Wow64WinUserImeService(Number, a);
    }
}
