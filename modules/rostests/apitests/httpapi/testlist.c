#define STANDALONE
#include <apitest.h>

extern void func_HttpServerApi(void);

const struct test winetest_testlist[] =
{
    { "HttpServerApi", func_HttpServerApi },

    { 0, 0 }
};
