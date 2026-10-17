/* ReactOS WoW64 USER display, desktop and window-station translation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64win.h"

ULONG_PTR Wow64WinUserDisplayService(ULONG Number, ULONG *a)
{
    switch (Number)
    {
    default:
        return Wow64WinUserMiscService(Number, a);
    }
}
