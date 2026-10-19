#define STANDALONE
#include <apitest.h>
extern void func_ClientPfn(void);
const struct test winetest_testlist[] =
{
    { "ClientPfn", func_ClientPfn },
    { 0, 0 }
};
