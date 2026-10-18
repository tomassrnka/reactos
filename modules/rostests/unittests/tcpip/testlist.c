#define STANDALONE
#include <apitest.h>

extern void func_Reassembly(void);

const struct test winetest_testlist[] =
{
    { "Reassembly", func_Reassembly },
    { 0, 0 }
};
