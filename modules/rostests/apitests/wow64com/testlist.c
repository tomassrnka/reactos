#define STANDALONE
#include <apitest.h>

extern void func_Registrations(void);

const struct test winetest_testlist[] =
{
    { "Registrations", func_Registrations },
    { 0, 0 }
};
