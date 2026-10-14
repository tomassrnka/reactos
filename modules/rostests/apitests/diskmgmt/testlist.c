#define STANDALONE
#include <apitest.h>

extern void func_DiskModel(void);

const struct test winetest_testlist[] =
{
    { "DiskModel", func_DiskModel },
    { 0, 0 }
};
