#include "pch.h"

/* Headers that shouldn't be precompiled due to GCC bugs */
/* Probe and capture */
#include <reactos/probe.h>

/* The wide WoW64 clients read shared user addresses through their low halves. */
#if defined(_WIN64) && defined(BUILD_WOW64_ENABLED)
FORCEINLINE
ULONG_PTR
Win32kUserViewZeroBits(_In_ PEPROCESS Process)
{
    if (Process->Wow64Process != NULL)
        return 0x7FFFFFFF;
    return 0;
}
#else
#define Win32kUserViewZeroBits(Process) 0
#endif

#ifdef __cplusplus
#undef NULL
#define NULL nullptr
#endif
