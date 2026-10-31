#define STANDALONE
#include <apitest.h>

extern void func_pwrshplugin(void);

const struct test winetest_testlist[] =
{
    { "pwrshplugin", func_pwrshplugin },
    { 0, 0 }
};
