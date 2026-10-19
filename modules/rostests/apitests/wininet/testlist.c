
#define STANDALONE
#include <apitest.h>

extern void func_HttpGzip(void);
extern void func_InternetOpen(void);

const struct test winetest_testlist[] =
{
    { "HttpGzip", func_HttpGzip },
    { "InternetOpen", func_InternetOpen },

    { 0, 0 }
};
