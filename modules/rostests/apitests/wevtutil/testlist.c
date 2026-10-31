#define STANDALONE
#include <apitest.h>

extern void func_wevtutil(void);

const struct test winetest_testlist[] =
{
    { "wevtutil", func_wevtutil },
    { 0, 0 }
};
