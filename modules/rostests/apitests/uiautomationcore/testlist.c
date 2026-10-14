#define STANDALONE
#include <apitest.h>

extern void func_UiaDisconnectAllProviders(void);

const struct test winetest_testlist[] =
{
    { "UiaDisconnectAllProviders", func_UiaDisconnectAllProviders },
    { 0, 0 }
};
