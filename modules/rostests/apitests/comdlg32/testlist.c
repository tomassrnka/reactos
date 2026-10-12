#define STANDALONE
#include <apitest.h>

extern void func_SaveDialogFolder(void);

const struct test winetest_testlist[] =
{
    { "SaveDialogFolder", func_SaveDialogFolder },
    { 0, 0 }
};
