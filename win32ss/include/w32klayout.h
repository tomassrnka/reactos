/*
 * PROJECT:     ReactOS Win32 subsystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Compile-time pin of the layout of every structure win32k shares with user mode
 *
 * The tables in w32klayout/ record, for each structure, its size and alignment and the offset and size of
 * each leaf field and each named member on i386 and on amd64. A build fails when a layout drifts from them:
 *  - native i386 must match the i386 column;
 *  - native amd64 must match the amd64 column;
 *  - i386 with W32K_WIDE (the WoW64 guest build) must match the amd64 column for every wide structure,
 *    and the i386 column for the others.
 * A wide leaf that grows to 8 bytes keeps its 32-bit member and gains a <name>_K64 twin at the same offset.
 * A pointer to a thread, process or desktop (KPTRFIELD) has only the <name>_K64 member in the wide build.
 * Regenerate the tables with w32klayout/regen.sh after an intended change.
 *
 * Not pinned: the position of bit-fields (C has no constant for it; the offsets of the members around them,
 * the alignment and the size of the structure are), and structures that win32k and user32 each declare for
 * themselves outside the shared headers.
 */

#ifndef _W32KLAYOUT_H
#define _W32KLAYOUT_H

#include <stddef.h>
#include "w32kwide.h"

#if defined(_WIN64)
#define W32K_LAYOUT_PICK(a32, a64, aw, wide) (a64)
#elif defined(W32K_WIDE_ACTIVE)
#define W32K_LAYOUT_PICK(a32, a64, aw, wide) ((wide) ? (aw) : (a32))
#else
#define W32K_LAYOUT_PICK(a32, a64, aw, wide) (a32)
#endif

/* GCC and Clang only: the build compiles the pin with neither MSVC nor a non-x86 target */
#define W32K_LAYOUT_TYPE(T, s32, s64, a32, a64, wide) \
    _Static_assert(sizeof(T) == W32K_LAYOUT_PICK(s32, s64, s64, wide), "size of " #T " drifted"); \
    _Static_assert(__alignof__(T) == W32K_LAYOUT_PICK(a32, a64, a64, wide), "alignment of " #T " drifted");

#define W32K_LAYOUT_FIELD(T, f, o32, o64, z32, z64, zw, wide) \
    _Static_assert(offsetof(T, f) == W32K_LAYOUT_PICK(o32, o64, o64, wide), "offset of " #T "." #f " drifted"); \
    _Static_assert(sizeof(((T *)0)->f) == W32K_LAYOUT_PICK(z32, z64, zw, wide), "size of " #T "." #f " drifted");

#ifdef W32K_WIDE_ACTIVE
#define W32K_LAYOUT_K64(T, f, o64, z64) \
    _Static_assert(offsetof(T, f) == (o64), "offset of " #T "." #f " drifted"); \
    _Static_assert(sizeof(((T *)0)->f) == (z64), "size of " #T "." #f " drifted");
#else
#define W32K_LAYOUT_K64(T, f, o64, z64)
#endif

/* A KPTRFIELD: the native declaration, and in the wide build the 8-byte <name>_K64 member only */
#ifdef W32K_WIDE_ACTIVE
#define W32K_LAYOUT_FIELD_K64ONLY(T, f, o32, o64, z32, z64, twin) W32K_LAYOUT_K64(T, twin, o64, z64)
#else
#define W32K_LAYOUT_FIELD_K64ONLY(T, f, o32, o64, z32, z64, twin) W32K_LAYOUT_FIELD(T, f, o32, o64, z32, z64, z32, 1)
#endif

/* A leaf that only one architecture's declaration has (FLOATOBJ is a two-member structure on i386 only) */
#if defined(_WIN64)
#define W32K_LAYOUT_ONLY32(T, f, o, z)
#define W32K_LAYOUT_ONLY64(T, f, o, z) \
    _Static_assert(offsetof(T, f) == (o), "offset of " #T "." #f " drifted"); \
    _Static_assert(sizeof(((T *)0)->f) == (z), "size of " #T "." #f " drifted");
#elif defined(W32K_WIDE_ACTIVE)
#define W32K_LAYOUT_ONLY32(T, f, o, z)
#define W32K_LAYOUT_ONLY64(T, f, o, z)
#else
#define W32K_LAYOUT_ONLY64(T, f, o, z)
#define W32K_LAYOUT_ONLY32(T, f, o, z) \
    _Static_assert(offsetof(T, f) == (o), "offset of " #T "." #f " drifted"); \
    _Static_assert(sizeof(((T *)0)->f) == (z), "size of " #T "." #f " drifted");
#endif

/* The constants w32kwide.h uses to find the 64-bit TEB of a WoW64 thread */
#if defined(_WIN64) && defined(KTEB_PTI)
_Static_assert(offsetof(TEB, Win32ThreadInfo) == KTEB64_WIN32THREADINFO, "Win32ThreadInfo moved in the TEB");
_Static_assert(ROUND_TO_PAGES(sizeof(TEB)) == KTEB64_BIAS, "the 32-bit TEB no longer follows the 64-bit TEB at this distance");
#endif

#endif /* _W32KLAYOUT_H */
