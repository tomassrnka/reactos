/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ReactOS-private HAL query about the clock interrupt
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#pragma once

/* HalQuerySystemInformation class outside the documented ones; the buffer is a
   ULONG of HAL_ROS_CLOCK_* flags */
#define HalReactOSClockInformation ((HAL_QUERY_INFORMATION_CLASS)0x524F0001)

/* The clock interrupt counts the periods whose interrupt could not be delivered */
#define HAL_ROS_CLOCK_COUNTS_LOST_INTERRUPTS 0x00000001
