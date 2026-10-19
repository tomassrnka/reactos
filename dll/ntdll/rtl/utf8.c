/*
 * PROJECT:     ReactOS NT User Mode Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     RtlUTF8ToUnicodeN as the ntdll of Windows 7 and later answers it
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

/*
 * The decoder follows Wine's ntdll (dlls/ntdll/locale_private.h), Copyright 2000 Alexandre Julliard and others,
 * LGPL-2.1+: it takes the bytes of a sequence one by one, so that a sequence that stops early costs one
 * replacement character and the next character is read from where the sequence stopped. This is what the
 * tests of Wine's rtlstr measure on Windows 10 for every kind of invalid input.
 */
static
ULONG
DecodeUtf8Char(
    _In_ UCHAR Lead,
    _Inout_ const CHAR **Source,
    _In_ const CHAR *SourceEnd)
{
    /* the number of bytes that follow, by lead byte value from 0x80 on */
    static const CHAR Utf8Length[128] =
    {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x80-0x8f */
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x90-0x9f */
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xa0-0xaf */
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0xb0-0xbf */
        0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1, /* 0xc0-0xcf */
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, /* 0xd0-0xdf */
        2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, /* 0xe0-0xef */
        3,3,3,3,3,0,0,0,0,0,0,0,0,0,0,0  /* 0xf0-0xff */
    };
    static const UCHAR Utf8Mask[4] = { 0x7f, 0x1f, 0x0f, 0x07 };

    ULONG Length = Utf8Length[Lead - 0x80];
    ULONG Result = Lead & Utf8Mask[Length];
    const CHAR *End = *Source + Length;

    /* a sequence that the end of the string cuts short takes the continuation bytes that are there, and no others (measured) */
    if (End > SourceEnd)
    {
        while (*Source < SourceEnd && ((UCHAR)**Source ^ 0x80) < 0x40)
            (*Source)++;
        return ~0;
    }

    switch (Length)
    {
        case 3:
            if ((Lead = End[-3] ^ 0x80) >= 0x40) break;
            Result = (Result << 6) | Lead;
            (*Source)++;
            if (Result < 0x10) break;
            /* fall through */
        case 2:
            if ((Lead = End[-2] ^ 0x80) >= 0x40) break;
            Result = (Result << 6) | Lead;
            if (Result >= 0x110000 >> 6) break;
            (*Source)++;
            if (Result < 0x20) break;
            if (Result >= 0xd800 >> 6 && Result <= 0xdfff >> 6) break;
            /* fall through */
        case 1:
            if ((Lead = End[-1] ^ 0x80) >= 0x40) break;
            Result = (Result << 6) | Lead;
            (*Source)++;
            if (Result < 0x80) break;
            return Result;
    }
    return ~0;
}

static
NTSTATUS
Utf8ToWideSize(
    _In_ const CHAR *Source,
    _In_ ULONG SourceLength,
    _Out_ PULONG Length)
{
    NTSTATUS Status = STATUS_SUCCESS;
    const CHAR *SourceEnd = Source + SourceLength;
    ULONG Result, Count;

    for (Count = 0; Source < SourceEnd; Count++)
    {
        UCHAR Lead = (UCHAR)*Source++;

        if (Lead < 0x80)
            continue;
        if ((Result = DecodeUtf8Char(Lead, &Source, SourceEnd)) > 0x10ffff)
            Status = STATUS_SOME_NOT_MAPPED;
        else if (Result > 0xffff)
            Count++;
    }

    *Length = Count;
    return Status;
}

static
NTSTATUS
Utf8ToWide(
    _Out_writes_(DestinationLength) WCHAR *Destination,
    _In_ ULONG DestinationLength,
    _Out_ PULONG Length,
    _In_ const CHAR *Source,
    _In_ ULONG SourceLength)
{
    NTSTATUS Status = STATUS_SUCCESS;
    const CHAR *SourceEnd = Source + SourceLength;
    WCHAR *DestinationEnd = Destination + DestinationLength;
    ULONG Result;

    while (Destination < DestinationEnd && Source < SourceEnd)
    {
        UCHAR Lead = (UCHAR)*Source++;

        if (Lead < 0x80)
        {
            *Destination++ = Lead;
            continue;
        }

        if ((Result = DecodeUtf8Char(Lead, &Source, SourceEnd)) <= 0xffff)
        {
            *Destination++ = (WCHAR)Result;
        }
        else if (Result <= 0x10ffff)
        {
            /* a surrogate pair */
            Result -= 0x10000;
            *Destination++ = 0xd800 | (Result >> 10);
            if (Destination == DestinationEnd)
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            *Destination++ = 0xdc00 | (Result & 0x3ff);
        }
        else
        {
            *Destination++ = 0xfffd;
            Status = STATUS_SOME_NOT_MAPPED;
        }
    }

    if (Source < SourceEnd)
        Status = STATUS_BUFFER_TOO_SMALL;

    *Length = DestinationLength - (ULONG)(DestinationEnd - Destination);
    return Status;
}

/******************************************************************************
 * RtlUTF8ToUnicodeN [NTDLL.@]
 *
 * The ntdll export of Windows 7 and later. The function of the same name in the shared runtime library (the
 * ntdll_vista export, the kernel and the callers inside ntdll) keeps its older answers for invalid input.
 */
NTSTATUS NTAPI RtlpUTF8ToUnicodeNWindows(WCHAR *uni_dest, ULONG uni_bytes_max,
                                 ULONG *uni_bytes_written,
                                 const CHAR *utf8_src, ULONG utf8_bytes)
{
    NTSTATUS Status;
    ULONG Count;

    if (!utf8_src)
        return STATUS_INVALID_PARAMETER_4;
    if (!uni_bytes_written)
        return STATUS_INVALID_PARAMETER;

    if (!uni_dest)
        Status = Utf8ToWideSize(utf8_src, utf8_bytes, &Count);
    else
        Status = Utf8ToWide(uni_dest, uni_bytes_max / sizeof(WCHAR), &Count, utf8_src, utf8_bytes);

    *uni_bytes_written = Count * sizeof(WCHAR);
    return Status;
}
