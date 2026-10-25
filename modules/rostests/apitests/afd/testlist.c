#define STANDALONE
#include <apitest.h>

extern void func_select(void);
extern void func_send(void);
extern void func_sendtounbound(void);
extern void func_windowsize(void);
extern void func_defensive(void);
extern void func_dgramstub(void);
extern void func_dgramhot(void);
extern void func_dgramclose(void);
extern void func_dgramfail(void);
extern void func_dgramlate(void);
extern void func_tcpnonadmin(void);
extern void func_tcpsetinfoex(void);
extern void func_tcpaccept(void);

const struct test winetest_testlist[] =
{
    { "select", func_select },
    { "send", func_send },
    { "sendtounbound", func_sendtounbound },
    { "windowsize", func_windowsize },
    { "defensive", func_defensive },
    { "dgramstub", func_dgramstub },
    { "dgramhot", func_dgramhot },
    { "dgramclose", func_dgramclose },
    { "dgramfail", func_dgramfail },
    { "dgramlate", func_dgramlate },
    { "tcpnonadmin", func_tcpnonadmin },
    { "tcpsetinfoex", func_tcpsetinfoex },
    { "tcpaccept", func_tcpaccept },
    { 0, 0 }
};
