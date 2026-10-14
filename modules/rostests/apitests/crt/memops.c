/*
 * PROJECT:         ReactOS API tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Exhaustive tests for memcpy, memmove and memset (msvcrt and ntdll exports)
 */

#include <apitest.h>

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define MEMOPS_LOG printf
#include "memops_core.h"

/* A source or destination that ends exactly at an inaccessible page: an access past the end faults */
static void Test_Guard(const char *name, PFN_COPY copy, PFN_COPY move, PFN_SET set)
{
    SYSTEM_INFO si;
    DWORD old;
    size_t page, len, i;
    unsigned char *p, *end, ref[400], dst[400];

    GetSystemInfo(&si);
    page = si.dwPageSize;
    p = VirtualAlloc(NULL, 2 * page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ok(p != NULL, "%s: VirtualAlloc failed\n", name);
    if (!p) return;
    ok(VirtualProtect(p + page, page, PAGE_NOACCESS, &old), "%s: VirtualProtect failed\n", name);
    end = p + page;
    g_fails = 0;
    g_seed = 5;
    for (len = 0; len <= 300; len++)
    {
        Randomize(end - 400, 400);
        RefCopy(ref, end - 400, 400);
        RefCopy(dst, ref, 400);
        /* source ends at the guard page */
        copy(dst, end - len, len);
        for (i = 0; i < 400; i++)
            if (dst[i] != (i < len ? ref[400 - len + i] : ref[i])) { MEMOPS_FAIL("%s: copy from the end, len %u\n", name, (unsigned)len); break; }
        /* destination ends at the guard page */
        RefCopy(end - 400, ref, 400);
        copy(end - len, ref, len);
        for (i = 0; i < 400; i++)
            if (end[i - 400] != (i >= 400 - len ? ref[i - (400 - len)] : ref[i])) { MEMOPS_FAIL("%s: copy to the end, len %u\n", name, (unsigned)len); break; }
        RefCopy(end - 400, ref, 400);
        set(end - len, 0x5A, len);
        for (i = 0; i < 400; i++)
            if (end[i - 400] != (i >= 400 - len ? 0x5A : ref[i])) { MEMOPS_FAIL("%s: set to the end, len %u\n", name, (unsigned)len); break; }
        /* overlapping moves that end at the guard page, both directions */
        RefCopy(end - 400, ref, 400);
        move(end - len, end - len - 7, len);
        for (i = 0; i < 400; i++)
            if (end[i - 400] != (i >= 400 - len ? ref[i - 7] : ref[i])) { MEMOPS_FAIL("%s: move up to the end, len %u\n", name, (unsigned)len); break; }
        RefCopy(end - 400, ref, 400);
        move(end - len - 7, end - len, len);
        for (i = 0; i < 400; i++)
            if (end[i - 400] != (i >= 400 - len - 7 && i < 400 - 7 ? ref[i + 7] : ref[i])) { MEMOPS_FAIL("%s: move down from the end, len %u\n", name, (unsigned)len); break; }
    }
    ok(g_fails == 0, "%s (guard page): %d failure(s)\n", name, g_fails);
    VirtualFree(p, 0, MEM_RELEASE);
}

START_TEST(memops)
{
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    PFN_COPY pnt_memcpy, pnt_memmove;
    PFN_SET pnt_memset;
    PFN_RTLMOVE pRtlMoveMemory;
    PFN_RTLFILL pRtlFillMemory;

    /* The exports of the C runtime that this program imports (built with -fno-builtin) */
    Test_Copy("msvcrt memcpy", memcpy);
    Test_Copy("msvcrt memmove", memmove);
    Test_Move("msvcrt memmove", memmove, NULL);
    Test_MoveBig("msvcrt memmove", memmove, NULL);
    Test_Set("msvcrt memset", memset, NULL);
    Test_Guard("msvcrt", memcpy, memmove, memset);

    pnt_memcpy = (PFN_COPY)GetProcAddress(hNtdll, "memcpy");
    pnt_memmove = (PFN_COPY)GetProcAddress(hNtdll, "memmove");
    pnt_memset = (PFN_SET)GetProcAddress(hNtdll, "memset");
    pRtlMoveMemory = (PFN_RTLMOVE)GetProcAddress(hNtdll, "RtlMoveMemory");
    pRtlFillMemory = (PFN_RTLFILL)GetProcAddress(hNtdll, "RtlFillMemory");

    if (pnt_memcpy) Test_Copy("ntdll memcpy", pnt_memcpy);
    if (pnt_memmove)
    {
        Test_Copy("ntdll memmove", pnt_memmove);
        Test_Move("ntdll memmove", pnt_memmove, NULL);
        Test_MoveBig("ntdll memmove", pnt_memmove, NULL);
    }
    if (pnt_memset) Test_Set("ntdll memset", pnt_memset, NULL);
    if (pnt_memcpy && pnt_memmove && pnt_memset) Test_Guard("ntdll", pnt_memcpy, pnt_memmove, pnt_memset);
    if (pRtlMoveMemory)
    {
        Test_Move("ntdll RtlMoveMemory", NULL, pRtlMoveMemory);
        Test_MoveBig("ntdll RtlMoveMemory", NULL, pRtlMoveMemory);
    }
    if (pRtlFillMemory) Test_Set("ntdll RtlFillMemory", NULL, pRtlFillMemory);
}
