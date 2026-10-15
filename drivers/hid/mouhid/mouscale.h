/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     GPL - See COPYING in the top level directory
 * PURPOSE:     Absolute-axis scaling for mouhid, kept free of driver state
 *              so it can be unit-tested on its own
 */

#pragma once

//
// Convert an absolute axis value into the 0..VirtualSize range. The value and
// the logical range come from an untrusted report descriptor: a failed
// extraction (e.g. a field wider than 32 bits) or a zero/negative range yields
// no movement instead of an uninitialized value or a division by zero.
//
static __inline
LONG
MouHid_ScaleAbsolute(
    IN NTSTATUS ExtractStatus,
    IN ULONG Value,
    IN LONG LogicalMin,
    IN LONG LogicalMax,
    IN ULONG VirtualSize)
{
    ULONGLONG Scaled;

    if (ExtractStatus != HIDP_STATUS_SUCCESS)
        return 0;

    if (LogicalMax <= 0 || LogicalMax <= LogicalMin)
        return 0;

    if (Value > (ULONG)LogicalMax)
        Value = (ULONG)LogicalMax;

    Scaled = ((ULONGLONG)Value * VirtualSize) / (ULONG)LogicalMax;
    return (LONG)Scaled;
}
