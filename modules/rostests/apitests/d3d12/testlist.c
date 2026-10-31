#define STANDALONE
#include <apitest.h>

extern void func_d3d12(void);

const struct test winetest_testlist[] =
{
    { "d3d12", func_d3d12 },
    { 0, 0 }
};
