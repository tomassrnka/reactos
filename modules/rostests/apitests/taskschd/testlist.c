#define STANDALONE
#include <apitest.h>

extern void func_TaskScheduler(void);
extern void func_TaskSchedulerSecurity(void);

const struct test winetest_testlist[] =
{
    { "TaskScheduler", func_TaskScheduler },
    { "TaskSchedulerSecurity", func_TaskSchedulerSecurity },
    { 0, 0 }
};
