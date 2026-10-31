#define STANDALONE
#include <apitest.h>

extern void func_args(void);
extern void func_wsf(void);

const struct test winetest_testlist[] =
{
    { "args", func_args },
    { "wsf", func_wsf },
    { 0, 0 }
};
