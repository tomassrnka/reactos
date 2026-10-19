/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     GPL - See COPYING in the top level directory
 * PURPOSE:     Absolute-axis scaling for mouhid, kept free of driver state
 *              so it can be unit-tested on its own
 */

#pragma once

//
// Map an absolute axis value onto 0..VirtualSize-1, the range win32k expects:
// LogicalMin gives 0 and LogicalMax gives VirtualSize-1, with rounding, in
// 64-bit arithmetic: (Position * (VirtualSize - 1) + Range / 2) / Range, where
// Position = value - LogicalMin and Range = LogicalMax - LogicalMin. This is the
// mapping of fix/abs-pointer-mapping (no +1 compensation).
//
// Signedness follows the HID spec: a negative LogicalMin makes the field
// signed, so the raw value is sign-extended from BitSize bits (all-negative
// ranges are valid). A non-negative LogicalMin with a negative LogicalMax means
// the maximum was encoded with its top bit set and is read as unsigned. The
// value is clamped to both endpoints.
//
// Returns FALSE (and leaves *Scaled untouched) on a failed extraction or an
// empty/inverted range; the caller keeps the axis' last valid coordinate
// instead of jumping to an edge.
//
static __inline
BOOLEAN
MouHid_ScaleAbsolute(
    IN NTSTATUS ExtractStatus,
    IN ULONG RawValue,
    IN USHORT BitSize,
    IN LONG LogicalMin,
    IN LONG LogicalMax,
    IN ULONG VirtualSize,
    OUT PLONG Scaled)
{
    LONGLONG Low, High, Value, Range;
    ULONG Mask;

    if (ExtractStatus != HIDP_STATUS_SUCCESS)
        return FALSE;

    Mask = (BitSize > 0 && BitSize < 32) ? ((1UL << BitSize) - 1) : 0xFFFFFFFFUL;

    Low = LogicalMin;
    if (LogicalMin >= 0 && LogicalMax < 0)
        High = (LONGLONG)((ULONG)LogicalMax & Mask);    /* unsigned maximum */
    else
        High = LogicalMax;

    if (LogicalMin < 0)
    {
        /* signed field: sign-extend the raw value from BitSize bits */
        RawValue &= Mask;
        if (BitSize > 0 && BitSize < 32 && (RawValue & (1UL << (BitSize - 1))))
            RawValue |= ~Mask;
        Value = (LONG)RawValue;
    }
    else
    {
        Value = (LONGLONG)(RawValue & Mask);
    }

    Range = High - Low;
    if (Range <= 0 || VirtualSize == 0)
        return FALSE;

    if (Value < Low)
        Value = Low;
    if (Value > High)
        Value = High;

    *Scaled = (LONG)(((Value - Low) * (LONGLONG)(VirtualSize - 1) + Range / 2) / Range);
    return TRUE;
}
