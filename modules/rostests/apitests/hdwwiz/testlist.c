/*
 * PROJECT:     ReactOS hdwwiz.cpl API Tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Test list
 */

#define STANDALONE
#include <apitest.h>

extern void func_AddHardwareWizard(void);

const struct test winetest_testlist[] =
{
    { "AddHardwareWizard", func_AddHardwareWizard },

    { 0, 0 }
};
