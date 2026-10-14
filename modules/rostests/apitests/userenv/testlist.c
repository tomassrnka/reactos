
#define STANDALONE
#include <apitest.h>

extern void func_DeriveAppContainerSid(void);
extern void func_GetProfileDirs(void);
extern void func_LoadUserProfile(void);

const struct test winetest_testlist[] =
{
    { "DeriveAppContainerSid", func_DeriveAppContainerSid },
    { "GetProfileDirs", func_GetProfileDirs },
    { "LoadUserProfile", func_LoadUserProfile },
    { 0, 0 }
};
