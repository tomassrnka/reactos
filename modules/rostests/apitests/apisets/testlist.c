
#define STANDALONE
#include <apitest.h>

extern void func_apisets(void);
extern void func_contracts(void);
extern void func_ucrt(void);
extern void func_winrt(void);
extern void func_loader(void);

const struct test winetest_testlist[] =
{
    { "apisets", func_apisets },
    { "contracts", func_contracts },
    { "ucrt", func_ucrt },
    { "winrt", func_winrt },
    { "loader", func_loader },
    { 0, 0 }
};
