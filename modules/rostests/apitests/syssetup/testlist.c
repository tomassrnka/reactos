#define STANDALONE
#include <apitest.h>
extern void func_ProgramFiles(void);
const struct test winetest_testlist[] = {
    { "ProgramFiles", func_ProgramFiles },
    { 0, 0 }
};
