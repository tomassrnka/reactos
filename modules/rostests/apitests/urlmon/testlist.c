#define STANDALONE
#include <apitest.h>

extern void func_TaskQueue(void);

const struct test winetest_testlist[] =
{
    { "TaskQueue", func_TaskQueue },
    { 0, 0 }
};
