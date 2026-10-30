#define STANDALONE
#include <apitest.h>

extern void func_BCryptKeyExport(void);

const struct test winetest_testlist[] =
{
    { "BCryptKeyExport", func_BCryptKeyExport },

    { 0, 0 }
};
