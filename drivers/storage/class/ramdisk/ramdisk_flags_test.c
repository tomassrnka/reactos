/*
 * Host-side contract test for RamdiskClearDeviceFlag (not part of the ROS build).
 *   gcc -Wall -o ramdisk_flags_test ramdisk_flags_test.c && ./ramdisk_flags_test
 */
#include <stdio.h>
#include "ramdisk_flags.h"

/* Values from the DDK (ntifs/wdm.h) */
#define DO_BUFFERED_IO         0x00000004
#define DO_EXCLUSIVE           0x00000008
#define DO_DIRECT_IO           0x00000010
#define DO_POWER_PAGABLE       0x00002000
#define DO_POWER_INRUSH        0x00004000
#define DO_DEVICE_INITIALIZING 0x00000080

static int Failures;

#define CHECK(Expect, In) do { \
    unsigned long Got_ = RamdiskClearDeviceFlag((In), DO_DEVICE_INITIALIZING); \
    if (Got_ != (unsigned long)(Expect)) { printf("FAIL line %d: in=%#lx got=%#lx want=%#lx\n", \
        __LINE__, (unsigned long)(In), Got_, (unsigned long)(Expect)); Failures++; } \
} while (0)

int main(void)
{
    /* The bus FDO state at the end of RamdiskAddDevice: IoCreateDevice set INITIALIZING,
       AddDevice set POWER_PAGABLE|DIRECT_IO. Result must keep both, drop INITIALIZING. */
    CHECK(DO_POWER_PAGABLE | DO_DIRECT_IO,
          DO_DEVICE_INITIALIZING | DO_POWER_PAGABLE | DO_DIRECT_IO);
    CHECK(0, DO_DEVICE_INITIALIZING);
    CHECK(DO_POWER_PAGABLE | DO_DIRECT_IO, DO_POWER_PAGABLE | DO_DIRECT_IO);
    CHECK(0, 0);
    CHECK(DO_BUFFERED_IO | DO_EXCLUSIVE | DO_POWER_INRUSH,
          DO_BUFFERED_IO | DO_EXCLUSIVE | DO_POWER_INRUSH | DO_DEVICE_INITIALIZING);
    CHECK(0xFFFFFF7FUL, 0xFFFFFFFFUL);

    /* Regression: the old expression (Flags &= DO_DEVICE_INITIALIZING) fails the contract */
    {
        unsigned long In = DO_DEVICE_INITIALIZING | DO_POWER_PAGABLE | DO_DIRECT_IO;
        unsigned long Old = In & DO_DEVICE_INITIALIZING;
        if (Old == (DO_POWER_PAGABLE | DO_DIRECT_IO)) { printf("FAIL: old expr unexpectedly ok\n"); Failures++; }
    }

    printf("%s (%d failures)\n", Failures ? "FAILED" : "PASSED", Failures);
    return Failures != 0;
}
