#define STANDALONE
#include <apitest.h>

extern void func_RefusedPackageLog(void);

const struct test winetest_testlist[] =
{
    { "RefusedPackageLog", func_RefusedPackageLog },
    { 0, 0 }
};
