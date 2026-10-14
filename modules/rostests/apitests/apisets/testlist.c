
#define STANDALONE
#include <apitest.h>

extern void func_apisets(void);
extern void func_contracts(void);
extern void func_ucrt(void);
extern void func_winrt(void);

const struct test winetest_testlist[] =
{
    { "apisets", func_apisets },
    { "contracts", func_contracts },
    { "ucrt", func_ucrt },
    { "winrt", func_winrt },
    { 0, 0 }
};
