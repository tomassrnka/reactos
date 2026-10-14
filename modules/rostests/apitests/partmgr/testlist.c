#define STANDALONE
#include <apitest.h>

extern void func_ExtendVolume(void);
extern void func_GrowPartition(void);
extern void func_StorDeviceNumber(void);

const struct test winetest_testlist[] =
{
    { "ExtendVolume", func_ExtendVolume },
    { "GrowPartition", func_GrowPartition },
    { "StorDeviceNumber", func_StorDeviceNumber },
    { 0, 0 }
};
