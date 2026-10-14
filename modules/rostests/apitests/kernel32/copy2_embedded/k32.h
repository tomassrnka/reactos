/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     What copy2.c of kernel32 needs from k32.h, so that CopyFile2Identity.c can compile it into the test
 */

#ifndef _COPY2_EMBEDDED_K32_H
#define _COPY2_EMBEDDED_K32_H

#include "precomp.h"
#include <ndk/iofuncs.h>
#include <ndk/iotypes.h>
#include <ndk/mmfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/psfuncs.h>

#endif
