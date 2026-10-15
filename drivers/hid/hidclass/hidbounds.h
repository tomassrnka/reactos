/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     GPL - See COPYING in the top level directory
 * PURPOSE:     Report-buffer bounds helpers for hidclass, kept free of driver
 *              state so they can be unit-tested on their own
 */

#pragma once

//
// A feature request is serviceable only when the selected report really has a
// feature part (FeatureLength != 0), the caller's buffer holds all of it, and
// (for GET) an output MDL exists. A zero output length gives no MDL, and
// mapping a NULL MDL dereferences it.
//
static __inline
BOOLEAN
HidClass_IsFeatureRequestValid(
    IN USHORT FeatureLength,
    IN ULONG BufferLength,
    IN PVOID Mdl,
    IN BOOLEAN NeedMdl)
{
    if (FeatureLength == 0)
        return FALSE;

    if (BufferLength < FeatureLength)
        return FALSE;

    if (NeedMdl && Mdl == NULL)
        return FALSE;

    return TRUE;
}

//
// Place one input report into the caller's buffer. Offset is the number of
// synthetic report-ID bytes hidclass prepends for a report without an ID; that
// prefix is zeroed. A short transfer is zero-filled to the full report length
// so no stale bytes from an earlier report are decoded, and the completion
// count is the normalized total (prefix + full report). Fails when the report
// does not fit the destination.
//
static __inline
BOOLEAN
HidClass_PlaceInputReport(
    OUT PUCHAR Destination,
    IN ULONG DestinationLength,
    IN ULONG Offset,
    IN const UCHAR *Source,
    IN ULONG Transferred,
    IN ULONG ReportLength,
    OUT PULONG Information)
{
    ULONG CopyLength;

    if (Offset > DestinationLength || ReportLength > DestinationLength - Offset)
        return FALSE;

    CopyLength = min(Transferred, ReportLength);

    RtlZeroMemory(Destination, Offset);
    RtlCopyMemory(Destination + Offset, Source, CopyLength);
    if (CopyLength < ReportLength)
        RtlZeroMemory(Destination + Offset + CopyLength, ReportLength - CopyLength);

    *Information = Offset + ReportLength;
    return TRUE;
}
