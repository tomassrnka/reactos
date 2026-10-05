/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Inter-processor interrupt entry point
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <asm.inc>
#include <ks386.inc>
#include <internal/i386/asmmacro.S>

.code

TRAP_ENTRY HalpIpiInterrupt, KI_PUSH_FAKE_ERROR_CODE

END
