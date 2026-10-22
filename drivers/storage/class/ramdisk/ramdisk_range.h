/*
 * PROJECT:     ReactOS RAM Disk Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Overflow-safe I/O range validation, shared with the host test
 */

#ifndef _RAMDISK_RANGE_H_
#define _RAMDISK_RANGE_H_

/*
 * Returns nonzero if [ByteOffset, ByteOffset + Length) lies entirely within a
 * disk of DiskLength bytes. Never computes ByteOffset + Length, so it cannot
 * overflow. A zero-length request at exactly the end of the disk is in range.
 */
static __inline int
RamdiskIsIoRangeValid(long long DiskLength,
                      long long ByteOffset,
                      unsigned long Length)
{
    if (DiskLength < 0) return 0;
    if (ByteOffset < 0) return 0;
    if (ByteOffset > DiskLength) return 0;
    return (unsigned long long)Length <=
           (unsigned long long)(DiskLength - ByteOffset);
}

#endif /* _RAMDISK_RANGE_H_ */
