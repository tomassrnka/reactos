/*
 * PROJECT:     ReactOS gdi32
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The same layout pin for the 32-bit WoW64 guest build (W32K_WIDE), which must match amd64
 */

#ifndef W32K_WIDE
#define W32K_WIDE
#endif
#include "w32klayout.c"

#ifndef W32K_WIDE_ACTIVE
#error The wide layout pin must be compiled with W32K_WIDE
#endif
