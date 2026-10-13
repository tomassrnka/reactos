/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the export ordinals of comctl32 version 6
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "wine/test.h"

#include <windows.h>

static const struct
{
    const char *name;
    WORD ordinal;
} g_exports[] =
{
    { "InitCommonControls", 17 },
    { "TaskDialog", 344 },
    { "TaskDialogIndirect", 345 },
    { "LoadIconMetric", 380 },
    { "LoadIconWithScaleDown", 381 },
    { "SetWindowSubclass", 410 },
    { "GetWindowSubclass", 411 },
    { "RemoveWindowSubclass", 412 },
    { "DefSubclassProc", 413 },
};

START_TEST(exports)
{
    HMODULE hComCtl32 = GetModuleHandleW(L"comctl32.dll");
    HRESULT (WINAPI *pTaskDialogIndirect)(const void *, int *, int *, BOOL *);
    HRESULT hr;
    UINT i;

    if (!hComCtl32)
    {
        skip("comctl32.dll is not loaded\n");
        return;
    }

    /* Version 5 does not have these exports */
    if (!GetProcAddress(hComCtl32, "LoadIconMetric"))
    {
        skip("comctl32 version 5 is loaded\n");
        return;
    }

    for (i = 0; i < ARRAYSIZE(g_exports); i++)
    {
        FARPROC byName = GetProcAddress(hComCtl32, g_exports[i].name);
        FARPROC byOrdinal = GetProcAddress(hComCtl32, MAKEINTRESOURCEA(g_exports[i].ordinal));

        ok(byName != NULL, "%s is not exported by name\n", g_exports[i].name);
        ok(byOrdinal != NULL, "Ordinal %u (%s) is not exported\n", g_exports[i].ordinal, g_exports[i].name);
        ok(byName == byOrdinal, "%s: ordinal %u gives %p, the name gives %p\n",
           g_exports[i].name, g_exports[i].ordinal, byOrdinal, byName);
    }

    pTaskDialogIndirect = (void *)GetProcAddress(hComCtl32, MAKEINTRESOURCEA(345));
    if (pTaskDialogIndirect)
    {
        /* No configuration: invalid argument, no dialog */
        hr = pTaskDialogIndirect(NULL, NULL, NULL, NULL);
        ok(hr == E_INVALIDARG, "TaskDialogIndirect(NULL) returned 0x%08lx\n", hr);
    }
}
