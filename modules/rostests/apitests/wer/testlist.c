#define STANDALONE
#include <apitest.h>

extern void func_wer(void);

const struct test winetest_testlist[] =
{
    { "wer", func_wer },
    { 0, 0 }
};
