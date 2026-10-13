
#define STANDALONE
#include <apitest.h>

extern void func_apisets(void);
extern void func_winrt(void);

const struct test winetest_testlist[] =
{
    { "apisets", func_apisets },
    { "winrt", func_winrt },
    { 0, 0 }
};
