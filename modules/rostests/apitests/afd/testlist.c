#define STANDALONE
#include <apitest.h>

extern void func_select(void);
extern void func_send(void);
extern void func_windowsize(void);
extern void func_defensive(void);
extern void func_tcpnonadmin(void);
extern void func_tcpaccept(void);

const struct test winetest_testlist[] =
{
    { "select", func_select },
    { "send", func_send },
    { "windowsize", func_windowsize },
    { "defensive", func_defensive },
    { "tcpnonadmin", func_tcpnonadmin },
    { "tcpaccept", func_tcpaccept },
    { 0, 0 }
};
