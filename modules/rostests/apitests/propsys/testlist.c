#define STANDALONE
#include <apitest.h>

extern void func_VariantCompare(void);

const struct test winetest_testlist[] =
{
    { "VariantCompare", func_VariantCompare },
    { 0, 0 }
};
