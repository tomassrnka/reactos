/*
 * PROJECT:     ReactOS sysdm.cpl API Tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Test list
 */

#define STANDALONE
#include <apitest.h>

extern void func_StartupRecovery(void);

const struct test winetest_testlist[] =
{
    { "StartupRecovery", func_StartupRecovery },

    { 0, 0 }
};
