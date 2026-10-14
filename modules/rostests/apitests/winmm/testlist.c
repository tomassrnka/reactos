#define STANDALONE
#include <apitest.h>

extern void func_timeBeginPeriod(void);
extern void func_timeGetTime(void);

const struct test winetest_testlist[] =
{
    { "timeBeginPeriod", func_timeBeginPeriod },
    { "timeGetTime", func_timeGetTime },

    { 0, 0 }
};
