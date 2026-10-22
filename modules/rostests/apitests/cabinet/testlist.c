#define STANDALONE
#include <apitest.h>

extern void func_FdiNotify(void);

const struct test winetest_testlist[] =
{
    { "FdiNotify", func_FdiNotify },
    { 0, 0 }
};
