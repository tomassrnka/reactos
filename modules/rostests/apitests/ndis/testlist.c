#define STANDALONE
#include <apitest.h>

extern void func_FakeMiniport(void);
extern void func_TcpipUnbind(void);

const struct test winetest_testlist[] =
{
    { "FakeMiniport", func_FakeMiniport },
    { "TcpipUnbind", func_TcpipUnbind },

    { 0, 0 }
};
