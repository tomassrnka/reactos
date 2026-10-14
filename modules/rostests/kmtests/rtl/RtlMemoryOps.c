/*
 * PROJECT:         ReactOS kernel-mode tests
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Kernel-Mode Test Suite exhaustive memcpy, memmove and memset test
 */

#include <stddef.h>
__declspec(dllimport) void __stdcall RtlMoveMemory(void *, const void *, size_t);
__declspec(dllimport) void __stdcall RtlFillMemory(void *, size_t, unsigned char);

#define KMT_EMULATE_KERNEL
#include <kmt_test.h>

#define MEMOPS_LOG trace
#include "../../apitests/crt/memops_core.h"

START_TEST(RtlMemoryOps)
{
    Test_Copy("memcpy", (PFN_COPY)memcpy);
    Test_Copy("memmove", (PFN_COPY)memmove);
    Test_Move("memmove", (PFN_COPY)memmove, NULL);
    Test_MoveBig("memmove", (PFN_COPY)memmove, NULL);
    Test_Set("memset", (PFN_SET)memset, NULL);
    Test_Move("RtlMoveMemory", NULL, RtlMoveMemory);
    Test_MoveBig("RtlMoveMemory", NULL, RtlMoveMemory);
    Test_Set("RtlFillMemory", NULL, RtlFillMemory);
}
