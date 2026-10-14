/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for GetNumaProcessorNodeEx (Windows 7 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows Server 2008 R2 and Windows 10 22H2 (x64 and WOW64), which have one node and one
 * processor group.
 */

#include "precomp.h"

typedef BOOL (WINAPI *PFN_GetNumaProcessorNodeEx)(PPROCESSOR_NUMBER, PUSHORT);
typedef BOOL (WINAPI *PFN_GetNumaProcessorNode)(UCHAR, PUCHAR);
typedef WORD (WINAPI *PFN_GetActiveProcessorGroupCount)(void);

#define PENDING_ERROR 0xdeadbeef

START_TEST(GetNumaProcessorNodeEx)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    PFN_GetNumaProcessorNodeEx pGetNumaProcessorNodeEx;
    PFN_GetNumaProcessorNode pGetNumaProcessorNode;
    PFN_GetActiveProcessorGroupCount pGetActiveProcessorGroupCount;
    SYSTEM_INFO Info;
    PROCESSOR_NUMBER Processor;
    USHORT Node;
    UCHAR Expected;
    DWORD Error;
    BOOL Result;
    ULONG i, Groups = 1;

    pGetNumaProcessorNodeEx = (PFN_GetNumaProcessorNodeEx)GetProcAddress(Kernel32, "GetNumaProcessorNodeEx");
    /* Windows Vista and older have none; ReactOS has it from export level 0x601, which a kernel32 with the Windows 7 functions shows */
    if (!pGetNumaProcessorNodeEx)
    {
        ok(!(is_reactos() && GetProcAddress(Kernel32, "K32EnumProcessModules")), "GetNumaProcessorNodeEx is not exported\n");
        skip("GetNumaProcessorNodeEx is not exported\n");
        return;
    }

    pGetNumaProcessorNode = (PFN_GetNumaProcessorNode)GetProcAddress(Kernel32, "GetNumaProcessorNode");
    pGetActiveProcessorGroupCount = (PFN_GetActiveProcessorGroupCount)GetProcAddress(Kernel32, "GetActiveProcessorGroupCount");
    if (pGetActiveProcessorGroupCount)
        Groups = pGetActiveProcessorGroupCount();
    GetSystemInfo(&Info);

    /* a processor of group 0 that is there gives its node and leaves the last error alone; the others fail */
    for (i = 0; i < 8 * sizeof(DWORD_PTR); i++)
    {
        BOOL Present = (Info.dwActiveProcessorMask >> i) & 1;

        Processor.Group = 0;
        Processor.Number = (BYTE)i;
        Processor.Reserved = 0;
        Node = 0x5555;
        SetLastError(PENDING_ERROR);
        Result = pGetNumaProcessorNodeEx(&Processor, &Node);
        Error = GetLastError();
        ok(Result == Present, "processor %lu: got %d\n", i, Result);
        if (Present)
        {
            /* the node of the processor, which the older function knows too; one node is the node 0 */
            Expected = 0;
            if (pGetNumaProcessorNode)
                ok(pGetNumaProcessorNode((UCHAR)i, &Expected), "processor %lu: no node\n", i);
            ok(Node == Expected, "processor %lu: node %u, expected %u\n", i, Node, Expected);
            ok(Error == PENDING_ERROR, "processor %lu: the last error became %lu\n", i, Error);
        }
        else
        {
            ok(Node == 0xFFFF, "processor %lu: node %#x\n", i, Node);
            ok(Error == ERROR_INVALID_PARAMETER, "processor %lu: last error %lu\n", i, Error);
        }
    }

    /* a processor number that no group has, a group that does not exist, a reserved field that is used */
    {
        static const PROCESSOR_NUMBER Bad[] =
        {
            { 0, 64, 0 }, { 0, 65, 0 }, { 0, 71, 0 }, { 0, 255, 0 },
            { 0, 0, 1 }, { 0, 0, 0xff }, { 0xffff, 0, 0 }, { 0xffff, 1, 0 },
        };

        for (i = 0; i < RTL_NUMBER_OF(Bad); i++)
        {
            Processor = Bad[i];
            Node = 0x5555;
            SetLastError(PENDING_ERROR);
            Result = pGetNumaProcessorNodeEx(&Processor, &Node);
            Error = GetLastError();
            ok(!Result, "case %lu: got %d\n", i, Result);
            ok(Node == 0xFFFF, "case %lu: node %#x\n", i, Node);
            ok(Error == ERROR_INVALID_PARAMETER, "case %lu: last error %lu\n", i, Error);
        }

        /* group 1 does not exist where there is one group */
        if (Groups <= 1)
        {
            Processor.Group = 1;
            Processor.Number = 0;
            Processor.Reserved = 0;
            Node = 0x5555;
            SetLastError(PENDING_ERROR);
            Result = pGetNumaProcessorNodeEx(&Processor, &Node);
            Error = GetLastError();
            ok(!Result, "group 1: got %d\n", Result);
            ok(Node == 0xFFFF, "group 1: node %#x\n", Node);
            ok(Error == ERROR_INVALID_PARAMETER, "group 1: last error %lu\n", Error);
        }
    }
}
