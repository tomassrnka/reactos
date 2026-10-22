#define STANDALONE
#include <apitest.h>

extern void func_StretchBlt(void);

const struct test winetest_testlist[] =
{
    { "StretchBlt", func_StretchBlt },
    { 0, 0 }
};
