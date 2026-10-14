#define STANDALONE
#include <apitest.h>

extern void func_GptVolumes(void);
extern void func_StorDeviceNumber(void);

const struct test winetest_testlist[] =
{
    { "GptVolumes", func_GptVolumes },
    { "StorDeviceNumber", func_StorDeviceNumber },
    { 0, 0 }
};
