#define STANDALONE
#include <apitest.h>

extern void func_timeGetTime(void);

const struct test winetest_testlist[] =
{
    { "timeGetTime", func_timeGetTime },

    { 0, 0 }
};
