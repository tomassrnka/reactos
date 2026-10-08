/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for RtlGetElementGenericTable
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define ELEMENT_COUNT 5

static
RTL_GENERIC_COMPARE_RESULTS
NTAPI
CompareRoutine(
    _In_ PRTL_GENERIC_TABLE Table,
    _In_ PVOID FirstStruct,
    _In_ PVOID SecondStruct)
{
    ULONG First = *(PULONG)FirstStruct;
    ULONG Second = *(PULONG)SecondStruct;

    if (First < Second)
        return GenericLessThan;
    if (First > Second)
        return GenericGreaterThan;
    return GenericEqual;
}

static
PVOID
NTAPI
AllocateRoutine(
    _In_ PRTL_GENERIC_TABLE Table,
    _In_ CLONG ByteSize)
{
    return RtlAllocateHeap(RtlGetProcessHeap(), 0, ByteSize);
}

static
VOID
NTAPI
FreeRoutine(
    _In_ PRTL_GENERIC_TABLE Table,
    _In_ PVOID Buffer)
{
    RtlFreeHeap(RtlGetProcessHeap(), 0, Buffer);
}

static
VOID
CheckElement(
    _In_ PRTL_GENERIC_TABLE Table,
    _In_ ULONG Index)
{
    PULONG Element;
    BOOLEAN InHeader;

    Element = RtlGetElementGenericTable(Table, Index);
    ok(Element != NULL, "Index %lu: got NULL\n", Index);
    if (Element == NULL)
        return;

    /* A walk that lands on the list head returns a pointer into the table */
    InHeader = ((PUCHAR)Element >= (PUCHAR)Table &&
                (PUCHAR)Element < (PUCHAR)(Table + 1));
    ok(!InHeader, "Index %lu: got %p, inside the table header at %p\n",
       Index, Element, Table);
    if (InHeader)
        return;

    ok(*Element == (Index + 1) * 10,
       "Index %lu: got %lu, expected %lu\n", Index, *Element, (Index + 1) * 10);
}

START_TEST(RtlGenericTable)
{
    static const ULONG OutOfOrder[] = { 4, 0, 2, 2, 1, 3, 0, 0, 4, 4, 3 };
    RTL_GENERIC_TABLE Table;
    BOOLEAN NewElement;
    PULONG Element;
    ULONG Value;
    ULONG Inserted;
    ULONG i;

    RtlInitializeGenericTable(&Table, CompareRoutine, AllocateRoutine,
                              FreeRoutine, NULL);

    /* Insert in ascending order, so insertion order and sorted order agree */
    for (Inserted = 0; Inserted < ELEMENT_COUNT; Inserted++)
    {
        Value = (Inserted + 1) * 10;
        NewElement = FALSE;
        Element = RtlInsertElementGenericTable(&Table, &Value, sizeof(Value),
                                               &NewElement);
        ok(Element != NULL, "Insert %lu failed\n", Value);
        if (Element == NULL)
            goto Cleanup;
        ok(NewElement == TRUE, "Insert %lu: NewElement is %u\n", Value, NewElement);
        ok_long(*Element, Value);
    }
    ok_long(RtlNumberGenericTableElements(&Table), ELEMENT_COUNT);

    /* In order */
    for (i = 0; i < ELEMENT_COUNT; i++)
        CheckElement(&Table, i);

    /* Out of order and repeated */
    for (i = 0; i < ARRAYSIZE(OutOfOrder); i++)
        CheckElement(&Table, OutOfOrder[i]);

    /* Past the end */
    ok(RtlGetElementGenericTable(&Table, ELEMENT_COUNT) == NULL,
       "Index %u: expected NULL\n", ELEMENT_COUNT);
    CheckElement(&Table, 2);

Cleanup:
    /* Delete everything that was inserted */
    for (i = 0; i < Inserted; i++)
    {
        Value = (i + 1) * 10;
        ok(RtlDeleteElementGenericTable(&Table, &Value),
           "Delete %lu failed\n", Value);
    }
    ok(RtlIsGenericTableEmpty(&Table), "Table is not empty\n");
    ok(RtlGetElementGenericTable(&Table, 0) == NULL,
       "Index 0 of an empty table: expected NULL\n");
}
