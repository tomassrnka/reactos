/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Test that gdi32 exports the Uniscribe API as forwarders to usp10
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#include "precomp.h"

static const char * const g_UniscribeNames[] =
{
    "ScriptApplyDigitSubstitution",
    "ScriptApplyLogicalWidth",
    "ScriptBreak",
    "ScriptCPtoX",
    "ScriptCacheGetHeight",
    "ScriptFreeCache",
    "ScriptGetCMap",
    "ScriptGetFontAlternateGlyphs",
    "ScriptGetFontFeatureTags",
    "ScriptGetFontLanguageTags",
    "ScriptGetFontProperties",
    "ScriptGetFontScriptTags",
    "ScriptGetGlyphABCWidth",
    "ScriptGetLogicalWidths",
    "ScriptGetProperties",
    "ScriptIsComplex",
    "ScriptItemize",
    "ScriptItemizeOpenType",
    "ScriptJustify",
    "ScriptLayout",
    "ScriptPlace",
    "ScriptPlaceOpenType",
    "ScriptPositionSingleGlyph",
    "ScriptRecordDigitSubstitution",
    "ScriptShape",
    "ScriptShapeOpenType",
    "ScriptStringAnalyse",
    "ScriptStringCPtoX",
    "ScriptStringFree",
    "ScriptStringGetLogicalWidths",
    "ScriptStringGetOrder",
    "ScriptStringOut",
    "ScriptStringValidate",
    "ScriptStringXtoCP",
    "ScriptString_pLogAttr",
    "ScriptString_pSize",
    "ScriptString_pcOutChars",
    "ScriptSubstituteSingleGlyph",
    "ScriptTextOut",
    "ScriptXtoCP",
};

START_TEST(UniscribeForwarders)
{
    HMODULE hGdi32, hUsp10;
    SIZE_T i;

    hGdi32 = GetModuleHandleW(L"gdi32.dll");
    ok(hGdi32 != NULL, "gdi32 is not loaded\n");
    if (!hGdi32)
        return;

    /* Loading usp10 explicitly must not change what the gdi32 forwarders resolve to */
    hUsp10 = LoadLibraryW(L"usp10.dll");
    ok(hUsp10 != NULL, "LoadLibrary(usp10) failed, error %lu\n", GetLastError());
    if (!hUsp10)
        return;

    for (i = 0; i < ARRAYSIZE(g_UniscribeNames); i++)
    {
        FARPROC pGdi32 = GetProcAddress(hGdi32, g_UniscribeNames[i]);
        FARPROC pUsp10 = GetProcAddress(hUsp10, g_UniscribeNames[i]);

        ok(pUsp10 != NULL, "%s: not exported by usp10\n", g_UniscribeNames[i]);
        ok(pGdi32 != NULL, "%s: not exported by gdi32\n", g_UniscribeNames[i]);
        ok(pGdi32 == pUsp10, "%s: gdi32 %p, usp10 %p\n", g_UniscribeNames[i], pGdi32, pUsp10);
    }

    FreeLibrary(hUsp10);
}
