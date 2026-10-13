
#define STANDALONE
#include <apitest.h>

extern void func_apisets(void);
extern void func_contracts(void);

const struct test winetest_testlist[] =
{
    { "apisets", func_apisets },
    { "contracts", func_contracts },
    { 0, 0 }
};
