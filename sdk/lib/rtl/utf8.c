/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Rtl functions of Vista+
 * COPYRIGHT:   2016 Thomas Faber <thomas.faber@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <rtl_vista.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

/******************************************************************************
 * RtlUnicodeToUTF8N [NTDLL.@]
 */
NTSTATUS NTAPI RtlUnicodeToUTF8N(CHAR *utf8_dest, ULONG utf8_bytes_max,
                                 ULONG *utf8_bytes_written,
                                 const WCHAR *uni_src, ULONG uni_bytes)
{
    NTSTATUS status;
    ULONG i;
    ULONG written;
    ULONG ch;
    BYTE utf8_ch[4];
    ULONG utf8_ch_len;

    if (!uni_src)
        return STATUS_INVALID_PARAMETER_4;
    if (!utf8_bytes_written)
        return STATUS_INVALID_PARAMETER;
    if (utf8_dest && uni_bytes % sizeof(WCHAR))
        return STATUS_INVALID_PARAMETER_5;

    written = 0;
    status = STATUS_SUCCESS;

    for (i = 0; i < uni_bytes / sizeof(WCHAR); i++)
    {
        /* decode UTF-16 into ch */
        ch = uni_src[i];
        if (ch >= 0xdc00 && ch <= 0xdfff)
        {
            ch = 0xfffd;
            status = STATUS_SOME_NOT_MAPPED;
        }
        else if (ch >= 0xd800 && ch <= 0xdbff)
        {
            if (i + 1 < uni_bytes / sizeof(WCHAR))
            {
                ch -= 0xd800;
                ch <<= 10;
                if (uni_src[i + 1] >= 0xdc00 && uni_src[i + 1] <= 0xdfff)
                {
                    ch |= uni_src[i + 1] - 0xdc00;
                    ch += 0x010000;
                    i++;
                }
                else
                {
                    ch = 0xfffd;
                    status = STATUS_SOME_NOT_MAPPED;
                }
            }
            else
            {
                ch = 0xfffd;
                status = STATUS_SOME_NOT_MAPPED;
            }
        }

        /* encode ch as UTF-8 */
        ASSERT(ch <= 0x10ffff);
        if (ch < 0x80)
        {
            utf8_ch[0] = ch & 0x7f;
            utf8_ch_len = 1;
        }
        else if (ch < 0x800)
        {
            utf8_ch[0] = 0xc0 | (ch >>  6 & 0x1f);
            utf8_ch[1] = 0x80 | (ch >>  0 & 0x3f);
            utf8_ch_len = 2;
        }
        else if (ch < 0x10000)
        {
            utf8_ch[0] = 0xe0 | (ch >> 12 & 0x0f);
            utf8_ch[1] = 0x80 | (ch >>  6 & 0x3f);
            utf8_ch[2] = 0x80 | (ch >>  0 & 0x3f);
            utf8_ch_len = 3;
        }
        else if (ch < 0x200000)
        {
            utf8_ch[0] = 0xf0 | (ch >> 18 & 0x07);
            utf8_ch[1] = 0x80 | (ch >> 12 & 0x3f);
            utf8_ch[2] = 0x80 | (ch >>  6 & 0x3f);
            utf8_ch[3] = 0x80 | (ch >>  0 & 0x3f);
            utf8_ch_len = 4;
        }

        if (!utf8_dest)
        {
            written += utf8_ch_len;
            continue;
        }

        if (utf8_bytes_max >= utf8_ch_len)
        {
            memcpy(utf8_dest, utf8_ch, utf8_ch_len);
            utf8_dest += utf8_ch_len;
            utf8_bytes_max -= utf8_ch_len;
            written += utf8_ch_len;
        }
        else
        {
            utf8_bytes_max = 0;
            status = STATUS_BUFFER_TOO_SMALL;
        }
    }

    *utf8_bytes_written = written;
    return status;
}


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
 */
NTSTATUS NTAPI RtlUTF8ToUnicodeN(WCHAR *uni_dest, ULONG uni_bytes_max,
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
