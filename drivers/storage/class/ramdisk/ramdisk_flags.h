/*
 * PROJECT:     ReactOS RAM Disk Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Device flag helper shared with the host test
 */

#ifndef _RAMDISK_FLAGS_H_
#define _RAMDISK_FLAGS_H_

/*
 * Returns Flags with only the given bit (DO_DEVICE_INITIALIZING) cleared.
 * All other device flags (DO_DIRECT_IO, DO_POWER_PAGABLE, ...) are preserved.
 */
static __inline unsigned long
RamdiskClearDeviceFlag(unsigned long Flags,
                       unsigned long Bit)
{
    return Flags & ~Bit;
}

#endif /* _RAMDISK_FLAGS_H_ */
