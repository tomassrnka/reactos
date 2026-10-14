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
    if (pRtlMoveMemory)
    {
        Test_Move("ntdll RtlMoveMemory", NULL, pRtlMoveMemory);
        Test_MoveBig("ntdll RtlMoveMemory", NULL, pRtlMoveMemory);
    }
    if (pRtlFillMemory) Test_Set("ntdll RtlFillMemory", NULL, pRtlFillMemory);
}
