#define STANDALONE
#include <apitest.h>

extern void func_Surrogate(void);

const struct test winetest_testlist[] =
{
    { "Surrogate", func_Surrogate },
    { 0, 0 }
};
