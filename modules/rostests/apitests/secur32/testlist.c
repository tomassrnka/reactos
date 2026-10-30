/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test list for secur32
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define STANDALONE
#include <apitest.h>

extern void func_AcquireCredentialsHandleVersions(void);
extern void func_LsaLogonContext(void);
extern void func_SchannelHandshake(void);
extern void func_SchannelServerCert(void);

const struct test winetest_testlist[] =
{
    { "AcquireCredentialsHandleVersions", func_AcquireCredentialsHandleVersions },
    { "LsaLogonContext", func_LsaLogonContext },
    { "SchannelHandshake", func_SchannelHandshake },
    { "SchannelServerCert", func_SchannelServerCert },

    { 0, 0 }
};
