
#define STANDALONE
#include <wine/test.h>

extern void func_d2d1_present(void);

const struct test winetest_testlist[] =
{
    { "d2d1_present", func_d2d1_present },

    { 0, 0 }
};
