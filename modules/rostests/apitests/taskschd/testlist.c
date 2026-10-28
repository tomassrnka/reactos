#define STANDALONE
#include <apitest.h>

extern void func_TaskScheduler(void);

const struct test winetest_testlist[] =
{
    { "TaskScheduler", func_TaskScheduler },
    { 0, 0 }
};
