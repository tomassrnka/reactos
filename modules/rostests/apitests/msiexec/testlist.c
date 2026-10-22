#define STANDALONE
#include <apitest.h>

extern void func_OpenFailure(void);

const struct test winetest_testlist[] =
{
    { "OpenFailure", func_OpenFailure },
    { 0, 0 }
};
