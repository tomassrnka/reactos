#define STANDALONE
#include <apitest.h>

extern void func_DOMDocument(void);

const struct test winetest_testlist[] =
{
    { "DOMDocument", func_DOMDocument },
    { 0, 0 }
};
