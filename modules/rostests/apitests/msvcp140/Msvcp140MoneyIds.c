/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the money facet ids exported by msvcp140.dll
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The Microsoft msvcp140.dll (14.44) exports all of them (export table of the redistributable that
 * the LibreOffice installer carries); LibreOffice imports eight and does not load without them. The
 * Windows 10 reference machine has no msvcp140.dll, so the exports were not measured there.
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>

static const char *g_Ids[] =
{
    "?id@?$money_get@DV?$istreambuf_iterator@DU?$char_traits@D@std@@@std@@@std@@2V0locale@2@A",
    "?id@?$money_get@GV?$istreambuf_iterator@GU?$char_traits@G@std@@@std@@@std@@2V0locale@2@A",
    "?id@?$money_get@_WV?$istreambuf_iterator@_WU?$char_traits@_W@std@@@std@@@std@@2V0locale@2@A",
    "?id@?$money_put@DV?$ostreambuf_iterator@DU?$char_traits@D@std@@@std@@@std@@2V0locale@2@A",
    "?id@?$money_put@GV?$ostreambuf_iterator@GU?$char_traits@G@std@@@std@@@std@@2V0locale@2@A",
    "?id@?$money_put@_WV?$ostreambuf_iterator@_WU?$char_traits@_W@std@@@std@@@std@@2V0locale@2@A",
    "?id@?$moneypunct@D$00@std@@2V0locale@2@A",
    "?id@?$moneypunct@D$0A@@std@@2V0locale@2@A",
    "?id@?$moneypunct@G$00@std@@2V0locale@2@A",
    "?id@?$moneypunct@G$0A@@std@@2V0locale@2@A",
    "?id@?$moneypunct@_W$00@std@@2V0locale@2@A",
    "?id@?$moneypunct@_W$0A@@std@@2V0locale@2@A",
};

START_TEST(Msvcp140MoneyIds)
{
    HMODULE Msvcp140 = LoadLibraryW(L"msvcp140.dll");
    UINT i;

    if (!Msvcp140)
    {
        ok(!is_reactos(), "msvcp140.dll does not load, error %lu\n", GetLastError());
        skip("msvcp140.dll is not installed\n");
        return;
    }

    for (i = 0; i < _countof(g_Ids); i++)
        ok(GetProcAddress(Msvcp140, g_Ids[i]) != NULL, "%s is not exported\n", g_Ids[i]);
}
