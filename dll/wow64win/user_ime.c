/* ReactOS WoW64 USER IME and input-context translation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64win.h"

ULONG_PTR Wow64WinUserImeService(ULONG Number, ULONG *a)
{
    switch (Number)
    {
    default:
        return Wow64WinUserDisplayService(Number, a);
    }
}
