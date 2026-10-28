#define STANDALONE
#include <apitest.h>

extern void func_FakeMiniport(void);

const struct test winetest_testlist[] =
{
    { "FakeMiniport", func_FakeMiniport },

    { 0, 0 }
};
