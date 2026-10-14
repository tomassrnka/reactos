#define STANDALONE
#include <apitest.h>

extern void func_tbs(void);

const struct test winetest_testlist[] =
{
    { "tbs", func_tbs },
    { 0, 0 }
};
