#define STANDALONE
#include <apitest.h>

extern void func_craftdlls(void);

const struct test winetest_testlist[] =
{
    { "craftdlls", func_craftdlls },
    { 0, 0 }
};
