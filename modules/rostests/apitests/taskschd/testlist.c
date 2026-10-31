#define STANDALONE
#include <apitest.h>

extern void func_Schtasks(void);
extern void func_TaskScheduler(void);
extern void func_TaskSchedulerApi(void);
extern void func_TaskSchedulerSecurity(void);

const struct test winetest_testlist[] =
{
    { "Schtasks", func_Schtasks },
    { "TaskScheduler", func_TaskScheduler },
    { "TaskSchedulerApi", func_TaskSchedulerApi },
    { "TaskSchedulerSecurity", func_TaskSchedulerSecurity },
    { 0, 0 }
};
