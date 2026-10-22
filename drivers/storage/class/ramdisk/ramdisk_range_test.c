/*
 * Host-side contract test for RamdiskIsIoRangeValid (not part of the ROS build).
 *   gcc -Wall -o ramdisk_range_test ramdisk_range_test.c && ./ramdisk_range_test
 */
#include <stdio.h>
#include "ramdisk_range.h"

static int Failures;

#define CHECK(Expect, Disk, Off, Len) do { \
    int Got_ = RamdiskIsIoRangeValid((Disk), (Off), (Len)); \
    if (Got_ != (Expect)) { printf("FAIL line %d: disk=%lld off=%lld len=%lu -> %d\n", \
        __LINE__, (long long)(Disk), (long long)(Off), (unsigned long)(Len), Got_); Failures++; } \
} while (0)

int main(void)
{
    const long long D = 0x100000; /* 1 MiB */

    /* In range */
    CHECK(1, D, 0, 512);
    CHECK(1, D, 0, D);
    CHECK(1, D, D - 512, 512);
    CHECK(1, D, D, 0);
    /* One byte / sector past the end */
    CHECK(0, D, 0, D + 1);
    CHECK(0, D, D - 512, 513);
    CHECK(0, D, D, 1);
    CHECK(0, D, D + 1, 0);
    CHECK(0, D, D + 512, 512);
    /* Far outside the disk (the physical-memory OOB case) */
    CHECK(0, D, 0x7FFFFFFF00000000LL, 512);
    CHECK(0, D, 0x100000000LL, 512);
    /* Negative offsets */
    CHECK(0, D, -1, 512);
    CHECK(0, D, -512, 1024);
    CHECK(0, D, (-0x7FFFFFFFFFFFFFFFLL - 1), 1);
    /* Overflow attempts: Offset + Length would wrap */
    CHECK(0, 0x7FFFFFFFFFFFFFFFLL, 0x7FFFFFFFFFFFFFFFLL, 0xFFFFFFFFUL);
    CHECK(0, D, 0x7FFFFFFFFFFFFFFFLL, 0xFFFFFFFFUL);
    CHECK(0, D, D - 1, 0xFFFFFFFFUL);
    /* Large disk, 32-bit length cannot exceed it incorrectly */
    CHECK(1, 0x200000000LL, 0x1FFFFFF00LL, 0x100);
    CHECK(0, 0x200000000LL, 0x1FFFFFF00LL, 0x101);
    /* Bogus disk length */
    CHECK(0, -1, 0, 0);

    printf(Failures ? "FAILED: %d\n" : "ALL PASSED\n", Failures);
    return Failures != 0;
}
