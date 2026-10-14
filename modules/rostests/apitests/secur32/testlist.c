/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test list for secur32
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define STANDALONE
#include <apitest.h>

extern void func_LsaLogonContext(void);
extern void func_SchannelServerCert(void);

const struct test winetest_testlist[] =
{
    { "LsaLogonContext", func_LsaLogonContext },
    { "SchannelServerCert", func_SchannelServerCert },

    { 0, 0 }
};
