/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Stand-in for the apitest framework, to run LocaleTable.c on Windows (see build-standalone.sh)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#ifndef LOCALE_TABLE_SHIM_H
#define LOCALE_TABLE_SHIM_H

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef UNICODE_NULL
#define UNICODE_NULL ((WCHAR)0)
#endif

static int g_Total, g_Failed, g_Skipped;

#define ok(cond, ...) \
    do { \
        g_Total++; \
        if (!(cond)) \
        { \
            g_Failed++; \
            if (g_Failed <= 50) { printf("%s:%d: Test failed: ", __FILE__, __LINE__); printf(__VA_ARGS__); } \
        } \
    } while (0)
#define skip(...) do { g_Skipped++; printf("skipped: "); printf(__VA_ARGS__); } while (0)
#define START_TEST(name) static void func_##name(void); \
    int main(void) { func_##name(); printf("%s: %d tests executed, %d failed, %d skipped\n", #name, g_Total, g_Failed, g_Skipped); return g_Failed != 0; } \
    static void func_##name(void)

#endif
