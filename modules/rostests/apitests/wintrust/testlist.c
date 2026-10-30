
#define STANDALONE
#include <apitest.h>

extern void func_SignatureTimeStamp(void);
extern void func_WTHelperGetFileHash(void);

const struct test winetest_testlist[] =
{
    { "SignatureTimeStamp", func_SignatureTimeStamp },
    { "WTHelperGetFileHash", func_WTHelperGetFileHash },

    { 0, 0 }
};
