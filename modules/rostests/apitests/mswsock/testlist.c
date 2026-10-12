#define STANDALONE
#include <apitest.h>

extern void func_AcceptEx(void);

const struct test winetest_testlist[] =
{
    { "AcceptEx", func_AcceptEx },
    { 0, 0 }
};
