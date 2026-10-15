
#define STANDALONE
#include <apitest.h>

extern void func_WinHttpOpen(void);
extern void func_WinHttpRequest(void);

const struct test winetest_testlist[] =
{
    { "WinHttpOpen", func_WinHttpOpen },
    { "WinHttpRequest", func_WinHttpRequest },

    { 0, 0 }
};
