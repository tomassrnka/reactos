#define STANDALONE
#include <apitest.h>

extern void func_AdapterHandles(void);

const struct test winetest_testlist[] =
{
    { "AdapterHandles", func_AdapterHandles },
    { 0, 0 }
};
