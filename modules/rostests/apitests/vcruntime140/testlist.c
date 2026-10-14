/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the Visual C++ runtime DLLs
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define STANDALONE
#include <apitest.h>

extern void func_vcruntime140(void);

const struct test winetest_testlist[] =
{
    { "vcruntime140", func_vcruntime140 },

    { 0, 0 }
};
