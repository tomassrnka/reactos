/* ReactOS WoW64 GDI drawing, font and device-context translation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64win.h"

ULONG_PTR Wow64WinGdiExtraService(ULONG Number, ULONG *a)
{
    switch (Number)
    {
    default:
        return Wow64WinUserInputService(Number, a);
    }
}
