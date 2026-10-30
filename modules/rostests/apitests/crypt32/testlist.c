
#define STANDALONE
#include <apitest.h>

extern void func_CertEnumSystemStore(void);
extern void func_CertEnumSystemStoreLocation(void);

const struct test winetest_testlist[] =
{
    { "CertEnumSystemStore", func_CertEnumSystemStore },
    { "CertEnumSystemStoreLocation", func_CertEnumSystemStoreLocation },

    { 0, 0 }
};
