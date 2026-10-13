#define STANDALONE
#include <apitest.h>

extern void func_attrib(void);
extern void func_cd(void);
extern void func_echo(void);
extern void func_exit(void);
extern void func_fc(void);
extern void func_pipe(void);
extern void func_pushd(void);
extern void func_start(void);

const struct test winetest_testlist[] =
{
    { "attrib", func_attrib },
    { "cd", func_cd },
    { "echo", func_echo },
    { "exit", func_exit },
    { "fc", func_fc },
    { "pipe", func_pipe },
    { "pushd", func_pushd },
    { "start", func_start },
    { 0, 0 }
};
