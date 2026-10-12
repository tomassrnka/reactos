#define STANDALONE
#include <apitest.h>

extern void func_VideoMemory(void);

const struct test winetest_testlist[] =
{
    { "VideoMemory", func_VideoMemory },
    { 0, 0 }
};
