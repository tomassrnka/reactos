#define STANDALONE
#include <apitest.h>

extern void func_NoDriverNotice(void);
extern void func_VideoMemory(void);

const struct test winetest_testlist[] =
{
    { "NoDriverNotice", func_NoDriverNotice },
    { "VideoMemory", func_VideoMemory },
    { 0, 0 }
};
