
#define STANDALONE
#include <apitest.h>

extern void func_WTHelperGetFileHash(void);

const struct test winetest_testlist[] =
{
    { "WTHelperGetFileHash", func_WTHelperGetFileHash },

    { 0, 0 }
};
