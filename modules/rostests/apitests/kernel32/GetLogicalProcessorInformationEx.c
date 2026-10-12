/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for GetLogicalProcessorInformationEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define MAX_ENTRIES 512

static BOOL (WINAPI *pGetLogicalProcessorInformation)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);
static BOOL (WINAPI *pGetLogicalProcessorInformationEx)(LOGICAL_PROCESSOR_RELATIONSHIP,
                                                        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                                        PDWORD);
static SYSTEM_LOGICAL_PROCESSOR_INFORMATION Info[MAX_ENTRIES];
static ULONG InfoCount;
static BYTE Buffer[MAX_ENTRIES * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)];

static
ULONG
ExpectedSize(
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP Relationship)
{
    switch (Relationship)
    {
        case RelationProcessorCore:
        case RelationProcessorPackage:
            return FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor.GroupMask[1]);
        case RelationNumaNode:
            return FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, NumaNode) +
                   sizeof(NUMA_NODE_RELATIONSHIP);
        case RelationCache:
            return FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Cache) +
                   sizeof(CACHE_RELATIONSHIP);
        case RelationGroup:
            return FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Group.GroupInfo[1]);
        default:
            return 0;
    }
}

static
ULONG
CountBits(
    _In_ ULONG_PTR Mask)
{
    ULONG Count = 0;

    for (; Mask; Mask &= Mask - 1)
        Count++;
    return Count;
}

/* Compare the entries of one relationship, or all, with GetLogicalProcessorInformation */
static
VOID
TestRelationship(
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP Relationship)
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Entry;
    SYSTEM_INFO SystemInfo;
    DWORD Length, Required, Offset, Error;
    ULONG Index, Groups;
    BOOL Success;

    GetSystemInfo(&SystemInfo);

    if (Relationship != RelationAll && Relationship != RelationGroup)
    {
        for (Index = 0; Index < InfoCount; Index++)
        {
            if (Info[Index].Relationship == Relationship)
                break;
        }
        if (Index == InfoCount)
        {
            skip("%d: GetLogicalProcessorInformation has no such entry\n", Relationship);
            return;
        }
    }

    /* Size query */
    Required = 0;
    SetLastError(0xdeadbeef);
    Success = pGetLogicalProcessorInformationEx(Relationship, NULL, &Required);
    Error = GetLastError();
    ok(!Success, "%d: Success = %d\n", Relationship, Success);
    ok(Error == ERROR_INSUFFICIENT_BUFFER, "%d: Error = %lu\n", Relationship, Error);
    ok(Required != 0, "%d: no length\n", Relationship);
    if (Required > sizeof(Buffer))
    {
        skip("%d: %lu bytes\n", Relationship, Required);
        return;
    }

    /* Too small for the first entry */
    Length = 8;
    memset(Buffer, 0x55, sizeof(Buffer));
    SetLastError(0xdeadbeef);
    Success = pGetLogicalProcessorInformationEx(Relationship,
                                                (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Buffer,
                                                &Length);
    Error = GetLastError();
    ok(!Success, "%d: Success = %d\n", Relationship, Success);
    ok(Error == ERROR_INSUFFICIENT_BUFFER, "%d: Error = %lu\n", Relationship, Error);
    ok(Length == Required, "%d: Length = %lu, expected %lu\n", Relationship, Length, Required);
    ok(Buffer[8] == 0x55, "%d: written past the buffer\n", Relationship);

    /* Exactly the size it needs: nothing is written after it */
    if (Required + 16 <= sizeof(Buffer))
    {
        Length = Required;
        memset(Buffer, 0x55, sizeof(Buffer));
        Success = pGetLogicalProcessorInformationEx(Relationship,
                                                    (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Buffer,
                                                    &Length);
        ok(Success, "%d: Success = %d, error %lu\n", Relationship, Success, GetLastError());
        ok(Length == Required, "%d: Length = %lu, expected %lu\n", Relationship, Length, Required);
        for (Offset = Required; Offset < Required + 16; Offset++)
        {
            if (Buffer[Offset] != 0x55)
                break;
        }
        ok(Offset == Required + 16, "%d: written past the buffer at %lu\n", Relationship, Offset);
    }

    Length = sizeof(Buffer);
    SetLastError(0xdeadbeef);
    Success = pGetLogicalProcessorInformationEx(Relationship,
                                                (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Buffer,
                                                &Length);
    Error = GetLastError();
    ok(Success, "%d: Success = %d, error %lu\n", Relationship, Success, Error);
    ok(Error == 0xdeadbeef, "%d: Error = %lu\n", Relationship, Error);
    ok(Length == Required, "%d: Length = %lu, expected %lu\n", Relationship, Length, Required);
    if (!Success)
        return;

    /*
     * The entries come in the order of GetLogicalProcessorInformation, then
     * the group. Newer versions add relationships the old function does not
     * have; those are not compared.
     */
    Index = 0;
    Groups = 0;
    for (Offset = 0; Offset < Length; Offset += Entry->Size)
    {
        Entry = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(Buffer + Offset);
        ok(Entry->Size != 0 && Offset + Entry->Size <= Length, "%d: entry at %lu: size %lu\n",
           Relationship, Offset, Entry->Size);
        if (Entry->Size == 0 || Offset + Entry->Size > Length)
            break;
        if (Relationship != RelationAll)
            ok(Entry->Relationship == Relationship, "%d: entry at %lu has relationship %d\n",
               Relationship, Offset, Entry->Relationship);
        if (Entry->Relationship > RelationGroup)
            continue;
        ok(Entry->Size == ExpectedSize(Entry->Relationship), "%d: entry at %lu: relationship %d, size %lu\n",
           Relationship, Offset, Entry->Relationship, Entry->Size);
        ok(Groups == 0, "%d: relationship %d after the group entry\n", Relationship, Entry->Relationship);

        if (Entry->Relationship == RelationGroup)
        {
            Groups++;
            ok(Entry->Group.MaximumGroupCount >= 1, "MaximumGroupCount = %u\n", Entry->Group.MaximumGroupCount);
            ok(Entry->Group.ActiveGroupCount == 1, "ActiveGroupCount = %u\n", Entry->Group.ActiveGroupCount);
            ok(Entry->Group.GroupInfo[0].ActiveProcessorMask == SystemInfo.dwActiveProcessorMask,
               "ActiveProcessorMask = 0x%Ix, expected 0x%Ix\n",
               Entry->Group.GroupInfo[0].ActiveProcessorMask, SystemInfo.dwActiveProcessorMask);
            ok(Entry->Group.GroupInfo[0].ActiveProcessorCount == SystemInfo.dwNumberOfProcessors,
               "ActiveProcessorCount = %u, expected %lu\n",
               Entry->Group.GroupInfo[0].ActiveProcessorCount, SystemInfo.dwNumberOfProcessors);
            ok(Entry->Group.GroupInfo[0].MaximumProcessorCount >= Entry->Group.GroupInfo[0].ActiveProcessorCount,
               "MaximumProcessorCount = %u\n", Entry->Group.GroupInfo[0].MaximumProcessorCount);
            ok(CountBits(Entry->Group.GroupInfo[0].ActiveProcessorMask) == Entry->Group.GroupInfo[0].ActiveProcessorCount,
               "Mask 0x%Ix for %u processors\n", Entry->Group.GroupInfo[0].ActiveProcessorMask,
               Entry->Group.GroupInfo[0].ActiveProcessorCount);
            continue;
        }

        /* Find the next entry of GetLogicalProcessorInformation that matches */
        while (Index < InfoCount && Relationship != RelationAll && Info[Index].Relationship != Relationship)
            Index++;
        ok(Index < InfoCount, "%d: more entries than GetLogicalProcessorInformation has\n", Relationship);
        if (Index >= InfoCount)
            break;
        ok(Entry->Relationship == Info[Index].Relationship, "%d: entry %lu: relationship %d, expected %d\n",
           Relationship, Index, Entry->Relationship, Info[Index].Relationship);

        switch (Entry->Relationship)
        {
            case RelationProcessorCore:
            case RelationProcessorPackage:
                ok(Entry->Processor.GroupCount == 1, "GroupCount = %u\n", Entry->Processor.GroupCount);
                ok(Entry->Processor.GroupMask[0].Group == 0, "Group = %u\n", Entry->Processor.GroupMask[0].Group);
                ok(Entry->Processor.GroupMask[0].Mask == Info[Index].ProcessorMask, "Mask = 0x%Ix, expected 0x%Ix\n",
                   Entry->Processor.GroupMask[0].Mask, Info[Index].ProcessorMask);
                if (Entry->Relationship == RelationProcessorCore)
                    ok(Entry->Processor.Flags == Info[Index].ProcessorCore.Flags, "Flags = %u, expected %u\n",
                       Entry->Processor.Flags, Info[Index].ProcessorCore.Flags);
                break;

            case RelationNumaNode:
                ok(Entry->NumaNode.NodeNumber == Info[Index].NumaNode.NodeNumber, "NodeNumber = %lu, expected %lu\n",
                   Entry->NumaNode.NodeNumber, Info[Index].NumaNode.NodeNumber);
                ok(Entry->NumaNode.GroupMask.Group == 0, "Group = %u\n", Entry->NumaNode.GroupMask.Group);
                ok(Entry->NumaNode.GroupMask.Mask == Info[Index].ProcessorMask, "Mask = 0x%Ix, expected 0x%Ix\n",
                   Entry->NumaNode.GroupMask.Mask, Info[Index].ProcessorMask);
                break;

            case RelationCache:
                ok(Entry->Cache.Level == Info[Index].Cache.Level, "Level = %u\n", Entry->Cache.Level);
                ok(Entry->Cache.Associativity == Info[Index].Cache.Associativity, "Associativity = %u\n",
                   Entry->Cache.Associativity);
                ok(Entry->Cache.LineSize == Info[Index].Cache.LineSize, "LineSize = %u\n", Entry->Cache.LineSize);
                ok(Entry->Cache.CacheSize == Info[Index].Cache.Size, "CacheSize = %lu\n", Entry->Cache.CacheSize);
                ok(Entry->Cache.Type == Info[Index].Cache.Type, "Type = %d\n", Entry->Cache.Type);
                ok(Entry->Cache.GroupMask.Mask == Info[Index].ProcessorMask, "Mask = 0x%Ix, expected 0x%Ix\n",
                   Entry->Cache.GroupMask.Mask, Info[Index].ProcessorMask);
                break;

            default:
                ok(0, "%d: unexpected relationship %d\n", Relationship, Entry->Relationship);
                break;
        }
        Index++;
    }
    ok(Offset == Length, "%d: entries end at %lu, length %lu\n", Relationship, Offset, Length);

    /* Every entry of GetLogicalProcessorInformation was returned */
    while (Index < InfoCount && Relationship != RelationAll && Info[Index].Relationship != Relationship)
        Index++;
    ok(Index == InfoCount, "%d: %lu of %lu entries returned\n", Relationship, Index, InfoCount);

    if (Relationship == RelationAll || Relationship == RelationGroup)
        ok(Groups == 1, "%d: %lu group entries\n", Relationship, Groups);
    else
        ok(Groups == 0, "%d: %lu group entries\n", Relationship, Groups);
}

START_TEST(GetLogicalProcessorInformationEx)
{
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    DWORD Length, Error;
    BOOL Success;

    pGetLogicalProcessorInformation = (PVOID)GetProcAddress(hKernel32, "GetLogicalProcessorInformation");
    pGetLogicalProcessorInformationEx = (PVOID)GetProcAddress(hKernel32, "GetLogicalProcessorInformationEx");
    if (!pGetLogicalProcessorInformation || !pGetLogicalProcessorInformationEx)
    {
        skip("GetLogicalProcessorInformationEx is not available\n");
        return;
    }

    Length = sizeof(Info);
    Success = pGetLogicalProcessorInformation(Info, &Length);
    ok(Success, "GetLogicalProcessorInformation failed with %lu\n", GetLastError());
    if (!Success)
        return;
    InfoCount = Length / sizeof(Info[0]);

    /* No length */
    SetLastError(0xdeadbeef);
    Success = pGetLogicalProcessorInformationEx(RelationAll,
                                                (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Buffer,
                                                NULL);
    Error = GetLastError();
    ok(!Success, "Success = %d\n", Success);
    ok(Error == ERROR_INVALID_PARAMETER, "Error = %lu\n", Error);

    /* With more than one processor group the old function sees only one */
    Length = sizeof(Buffer);
    Success = pGetLogicalProcessorInformationEx(RelationGroup,
                                                (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Buffer,
                                                &Length);
    ok(Success, "Success = %d, error %lu\n", Success, GetLastError());
    if (Success && Length >= ExpectedSize(RelationGroup) &&
        ((PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Buffer)->Group.ActiveGroupCount != 1)
    {
        skip("%u processor groups\n", ((PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Buffer)->Group.ActiveGroupCount);
        return;
    }

    TestRelationship(RelationProcessorCore);
    TestRelationship(RelationNumaNode);
    TestRelationship(RelationCache);
    TestRelationship(RelationProcessorPackage);
    TestRelationship(RelationGroup);
    TestRelationship(RelationAll);
}
