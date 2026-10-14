#define STANDALONE
#include <apitest.h>

extern void func_DiskModel(void);
extern void func_DiskLayout(void);
extern void func_DiskOps(void);

const struct test winetest_testlist[] =
{
    { "DiskModel", func_DiskModel },
    { "DiskLayout", func_DiskLayout },
    { "DiskOps", func_DiskOps },
    { 0, 0 }
};
