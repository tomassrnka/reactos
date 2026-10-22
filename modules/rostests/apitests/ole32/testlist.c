
#define STANDALONE
#include <wine/test.h>

extern void func_initializespy(void);
extern void func_ScmCreatedEvent(void);

const struct test winetest_testlist[] =
{
    { "initializespy", func_initializespy },
    { "ScmCreatedEvent", func_ScmCreatedEvent },

    { 0, 0 }
};
