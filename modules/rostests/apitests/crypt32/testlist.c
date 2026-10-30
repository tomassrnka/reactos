
#define STANDALONE
#include <apitest.h>

extern void func_CertEnumSystemStore(void);
extern void func_CertEnumSystemStoreLocation(void);
extern void func_ChainTrust(void);

const struct test winetest_testlist[] =
{
    { "CertEnumSystemStore", func_CertEnumSystemStore },
    { "CertEnumSystemStoreLocation", func_CertEnumSystemStoreLocation },
    { "ChainTrust", func_ChainTrust },

    { 0, 0 }
};
