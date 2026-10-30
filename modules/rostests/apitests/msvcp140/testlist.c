#define STANDALONE
#include <apitest.h>

extern void func_Msvcp140MoneyIds(void);

const struct test winetest_testlist[] =
{
    { "Msvcp140MoneyIds", func_Msvcp140MoneyIds },

    { 0, 0 }
};
