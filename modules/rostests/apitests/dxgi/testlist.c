#define STANDALONE
#include <apitest.h>

extern void func_EnumWarpAdapter(void);

const struct test winetest_testlist[] =
{
    { "EnumWarpAdapter", func_EnumWarpAdapter },

    { 0, 0 }
};
