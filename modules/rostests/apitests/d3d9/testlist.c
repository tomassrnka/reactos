#define STANDALONE
#include <apitest.h>

extern void func_NoticeThread(void);

const struct test winetest_testlist[] =
{
    { "NoticeThread", func_NoticeThread },
    { 0, 0 }
};
