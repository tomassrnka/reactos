/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     GetNumaProcessorNodeEx (Windows 7 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows Server 2008 R2 and Windows 10: a processor of group 0 that exists gives its node; anything else
 * (another group, a nonzero reserved field, a number that is no processor) gives ERROR_INVALID_PARAMETER and the node
 * 0xFFFF. The last error is not touched on success. The masks of the nodes are 64 bits wide, so the processors 32 to 63
 * of an amd64 system are found too.
 */

#include "k32_vista.h"
#include <ndk/exfuncs.h>

#define NDEBUG
#include <debug.h>

BOOL
WINAPI
GetNumaProcessorNodeEx(
    _In_ PPROCESSOR_NUMBER Processor,
    _Out_ PUSHORT NodeNumber)
{
    SYSTEM_NUMA_INFORMATION NumaInformation;
    ULONG Length, Node;

    *NodeNumber = 0xFFFF;

    if (Processor->Group != 0 || Processor->Reserved != 0 || Processor->Number >= 64)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!NT_SUCCESS(NtQuerySystemInformation(SystemNumaProcessorMap, &NumaInformation, sizeof(NumaInformation), &Length)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    for (Node = 0; Node <= NumaInformation.HighestNodeNumber && Node < MAXIMUM_NUMA_NODES; Node++)
    {
        if (NumaInformation.ActiveProcessorsAffinityMask[Node] & (1ULL << Processor->Number))
        {
            *NodeNumber = (USHORT)Node;
            return TRUE;
        }
    }

    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}
