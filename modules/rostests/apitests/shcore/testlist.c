#define STANDALONE
#include <apitest.h>

extern void func_GetDpiForMonitor(void);
extern void func_ProcessDpiAwareness(void);
extern void func_FeatureStaging(void);
extern void func_GetScaleFactor(void);
extern void func_IStreamFunctions(void);
extern void func_SHCreateThreadWithHandle(void);
extern void func_SHRegGetValueFromHKCUHKLM(void);

const struct test winetest_testlist[] =
{
    { "GetDpiForMonitor", func_GetDpiForMonitor },
    { "ProcessDpiAwareness", func_ProcessDpiAwareness },
    { "FeatureStaging", func_FeatureStaging },
    { "GetScaleFactor", func_GetScaleFactor },
    { "IStreamFunctions", func_IStreamFunctions },
    { "SHCreateThreadWithHandle", func_SHCreateThreadWithHandle },
    { "SHRegGetValueFromHKCUHKLM", func_SHRegGetValueFromHKCUHKLM },
    { 0, 0 }
};
