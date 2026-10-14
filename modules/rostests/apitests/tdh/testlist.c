#define STANDALONE
#include <apitest.h>

extern void func_tdh(void);

const struct test winetest_testlist[] =
{
    { "tdh", func_tdh },
    { 0, 0 }
};
