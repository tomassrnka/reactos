#define STANDALONE
#include <apitest.h>

extern void func_ndfapi(void);

const struct test winetest_testlist[] =
{
    { "ndfapi", func_ndfapi },
    { 0, 0 }
};
