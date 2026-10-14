#define STANDALONE
#include <apitest.h>
extern void func_RtlDosApplyFileIsolationRedirection_LoadFrom(void);
const struct test winetest_testlist[] =
{
    { "LoadFrom", func_RtlDosApplyFileIsolationRedirection_LoadFrom },
    { NULL, NULL }
};
