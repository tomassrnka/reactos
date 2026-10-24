/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Loader contract for a DLL whose DllMain has a parent-presence
 *              precondition (the mscoreei.dll / err 1114 shape, generalized).
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 */

#include "precomp.h"

/*
 * Background (T116): mscoreei.dll is a private child component of mscoree.dll.
 * Its DllMain's first required step is GetModuleHandleW(L"mscoree.dll"); if that
 * returns NULL the DllMain returns FALSE and LoadLibrary fails with
 * ERROR_DLL_INIT_FAILED (1114). A direct LoadLibrary("mscoreei.dll") without
 * mscoree.dll loaded therefore fails by design -- on Windows and on ReactOS
 * alike. This test asserts the two loader behaviors that make the real hosting
 * chain (mscoree -> clr -> mscoreei) work, using neutral helper DLLs:
 *   1. When a DLL's DllMain returns FALSE, LoadLibrary fails with
 *      ERROR_DLL_INIT_FAILED and the module is not left loaded.
 *   2. A module loaded earlier is visible to a later-loaded module's DllMain via
 *      GetModuleHandleW, so a child with a parent-presence gate initializes.
 */

static VOID test_child_without_parent(VOID)
{
    HMODULE hChild;
    DWORD err;

    /* Parent absent: the child's DllMain returns FALSE. */
    SetLastError(0xdeadbeef);
    hChild = LoadLibraryExW(L"t116_dllinit_child.dll", NULL, 0);
    err = GetLastError();
    ok(hChild == NULL, "child loaded without parent present: %p\n", hChild);
    ok(err == ERROR_DLL_INIT_FAILED,
       "expected ERROR_DLL_INIT_FAILED (%u), got %lu\n",
       ERROR_DLL_INIT_FAILED, err);

    /* A failed init must not leave the module loaded. */
    ok(GetModuleHandleW(L"t116_dllinit_child.dll") == NULL,
       "child left loaded after DllMain returned FALSE\n");

    if (hChild)
        FreeLibrary(hChild);
}

static VOID test_child_with_parent(VOID)
{
    HMODULE hParent, hChild, hGet;
    DWORD (WINAPI *pGetMarker)(VOID);

    hParent = LoadLibraryExW(L"t116_dllinit_parent.dll", NULL, 0);
    ok(hParent != NULL, "parent failed to load: %lu\n", GetLastError());

    /* The loader must make the parent visible to the child's DllMain. */
    hGet = GetModuleHandleW(L"t116_dllinit_parent.dll");
    ok(hGet == hParent, "GetModuleHandleW(parent) = %p, expected %p\n",
       hGet, hParent);

    SetLastError(0xdeadbeef);
    hChild = LoadLibraryExW(L"t116_dllinit_child.dll", NULL, 0);
    ok(hChild != NULL, "child failed to load with parent present: %lu\n",
       GetLastError());

    if (hChild)
    {
        pGetMarker = (DWORD (WINAPI *)(VOID))GetProcAddress(hChild, "GetMarker");
        ok(pGetMarker != NULL, "GetMarker export not found: %lu\n", GetLastError());
        if (pGetMarker)
            ok(pGetMarker() == 42, "GetMarker returned %lu, expected 42\n",
               pGetMarker());
        FreeLibrary(hChild);
    }

    if (hParent)
        FreeLibrary(hParent);
}

START_TEST(DllInitParentPrecondition)
{
    test_child_without_parent();
    test_child_with_parent();
}
