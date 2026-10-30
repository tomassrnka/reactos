#define STANDALONE
#include <apitest.h>

extern void func_MfPlayer(void);

const struct test winetest_testlist[] =
{
    { "MfPlayer", func_MfPlayer },

    { 0, 0 }
};
