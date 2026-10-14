#define STANDALONE
#include <apitest.h>

extern void func_wevtapi(void);

const struct test winetest_testlist[] =
{
    { "wevtapi", func_wevtapi },
    { 0, 0 }
};
