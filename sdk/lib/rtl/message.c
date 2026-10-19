/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            lib/rtl/message.c
 * PURPOSE:         Message table functions
 * PROGRAMMERS:     Eric Kohl
 */

/* INCLUDES *****************************************************************/

#include <rtl.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlFindMessage(
    IN PVOID BaseAddress,
    IN ULONG Type,
    IN ULONG Language,
    IN ULONG MessageId,
    OUT PMESSAGE_RESOURCE_ENTRY* MessageResourceEntry)
{
    LDR_RESOURCE_INFO ResourceInfo;
    PIMAGE_RESOURCE_DATA_ENTRY ResourceDataEntry;
    PMESSAGE_RESOURCE_DATA MessageTable;
    NTSTATUS Status;
    ULONG EntryOffset = 0, IdOffset = 0;
    PMESSAGE_RESOURCE_ENTRY MessageEntry;
    ULONG i;

    DPRINT("RtlFindMessage()\n");

    ResourceInfo.Type = Type;
    ResourceInfo.Name = 1;
    ResourceInfo.Language = Language;

    Status = LdrFindResource_U(BaseAddress,
                               &ResourceInfo,
                               RESOURCE_DATA_LEVEL,
                               &ResourceDataEntry);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    DPRINT("ResourceDataEntry: %p\n", ResourceDataEntry);

    Status = LdrAccessResource(BaseAddress,
                               ResourceDataEntry,
                               (PVOID*)&MessageTable,
                               NULL);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    DPRINT("MessageTable: %p\n", MessageTable);

    DPRINT("NumberOfBlocks %lu\n", MessageTable->NumberOfBlocks);
    for (i = 0; i < MessageTable->NumberOfBlocks; i++)
    {
        DPRINT("LoId 0x%08lx  HiId 0x%08lx  Offset 0x%08lx\n",
               MessageTable->Blocks[i].LowId,
               MessageTable->Blocks[i].HighId,
               MessageTable->Blocks[i].OffsetToEntries);
    }

    for (i = 0; i < MessageTable->NumberOfBlocks; i++)
    {
        if ((MessageId >= MessageTable->Blocks[i].LowId) &&
            (MessageId <= MessageTable->Blocks[i].HighId))
        {
            EntryOffset = MessageTable->Blocks[i].OffsetToEntries;
            IdOffset = MessageId - MessageTable->Blocks[i].LowId;
            break;
        }

        if (MessageId < MessageTable->Blocks[i].LowId)
        {
            return STATUS_MESSAGE_NOT_FOUND;
        }
    }

    if (MessageTable->NumberOfBlocks <= i)
    {
        return STATUS_MESSAGE_NOT_FOUND;
    }

    MessageEntry = (PMESSAGE_RESOURCE_ENTRY)
        ((PUCHAR)MessageTable + MessageTable->Blocks[i].OffsetToEntries);

    DPRINT("EntryOffset 0x%08lx\n", EntryOffset);
    DPRINT("IdOffset 0x%08lx\n", IdOffset);

    DPRINT("MessageEntry: %p\n", MessageEntry);
    for (i = 0; i < IdOffset; i++)
    {
        MessageEntry = (PMESSAGE_RESOURCE_ENTRY)
            ((PUCHAR)MessageEntry + (ULONG)MessageEntry->Length);
    }

    if (MessageEntry->Flags == 0)
    {
        DPRINT("AnsiText: %s\n", MessageEntry->Text);
    }
    else
    {
        DPRINT("UnicodeText: %S\n", (PWSTR)MessageEntry->Text);
    }

    if (MessageResourceEntry != NULL)
    {
        *MessageResourceEntry = MessageEntry;
    }

    return STATUS_SUCCESS;
}

/*
 * The message formatter below is taken from Wine's dlls/ntdll/rtlstr.c (LGPL),
 * Copyright 1996 Marcus Meissner, 2009 and 2020 Alexandre Julliard.
 */

struct format_message_args
{
    int           last;         /* last used arg */
    ULONG_PTR    *array;        /* args array */
    va_list      *list;         /* args va_list */
    UINT64        arglist[102]; /* arguments fetched from va_list */
    BOOL          overflow;     /* more arguments were asked for than arglist holds */
};

#define TAG_MSGARG 'gsMR'

static NTSTATUS add_chars(WCHAR **buffer, WCHAR *end, const WCHAR *str, ULONG len)
{
    if (len > end - *buffer) return STATUS_BUFFER_OVERFLOW;
    RtlCopyMemory(*buffer, str, len * sizeof(WCHAR));
    *buffer += len;
    return STATUS_SUCCESS;
}

static UINT64 get_arg(int nr, struct format_message_args *args_data, BOOL is64)
{
    if (nr == -1) nr = args_data->last + 1;
    if (nr < 1) return 0;
    if (nr > ARRAYSIZE(args_data->arglist))
    {
        args_data->overflow = TRUE;
        return 0;
    }
    while (nr > args_data->last)
        args_data->arglist[args_data->last++] = is64 ? va_arg(*args_data->list, UINT64)
                                                     : va_arg(*args_data->list, ULONG_PTR);
    return args_data->arglist[nr - 1];
}

static NTSTATUS add_format(WCHAR **buffer, WCHAR *end, const WCHAR **src, int insert, BOOLEAN ansi,
                           struct format_message_args *args_data)
{
    const WCHAR *format = *src;
    WCHAR *p, fmt[32];
    ULONG_PTR args[5] = { 0 };
    BOOL is_64 = FALSE;
    UINT64 val;
    WCHAR *wide = NULL;
    int len, stars = 0, nb_args = 0;
    NTSTATUS status = STATUS_SUCCESS;

    p = fmt;
    *p++ = '%';

    if (*format++ == '!')
    {
        const WCHAR *fmtend = wcschr(format, '!');

        if (!fmtend || fmtend - format > ARRAYSIZE(fmt) - 2) return STATUS_INVALID_PARAMETER;
        *src = fmtend + 1;

        while (*format && wcschr(L"0123456789 +-*#.", *format))
        {
            if (*format == '*') stars++;
            *p++ = *format++;
        }
        if (stars > 2) return STATUS_INVALID_PARAMETER;

        switch (*format)
        {
        case 'c': case 'C':
        case 's': case 'S':
            if (ansi) *p++ = *format++ ^ ('s' - 'S');
            break;
        case 'I':
            if (sizeof(void *) == sizeof(int) && format[1] == '6' && format[2] == '4') is_64 = TRUE;
            break;
        }
        while (format != fmtend) *p++ = *format++;
    }
    else *p++ = ansi ? 'S' : 's'; /* simple string */

    *p = 0;
    if (args_data->list)
    {
        get_arg(insert - 1, args_data, is_64);  /* make sure previous args have been fetched */
        while (stars--)
        {
            args[nb_args++] = get_arg(insert, args_data, FALSE);
            insert = -1;
        }
        /* replicate MS bug: drop an argument when using va_list with width/precision */
        if (insert == -1) args_data->last--;
        val = get_arg(insert, args_data, is_64);
        args[nb_args++] = val;
        args[nb_args] = val >> 32;
    }
    else if (args_data->array)
    {
        args[nb_args++] = args_data->array[insert - 1];
        if (args_data->last < insert) args_data->last = insert;
        /* replicate MS bug: first arg is considered 64-bit, even if it's actually width or precision */
        if (is_64) nb_args++;
        while (stars--) args[nb_args++] = args_data->array[args_data->last++];
    }
    else return STATUS_INVALID_PARAMETER;

    if (args_data->overflow) return STATUS_INVALID_PARAMETER;

    /* The CRT converts an ANSI string argument with one count per character,
     * which runs past the end of a double-byte string; convert it here. */
    {
        WCHAR *f = fmt + 1, *type = NULL;
        int value_index = 0;
        ULONG limit = MAXULONG;

        while (*f && wcschr(L"-+0 #", *f)) f++;
        if (*f == '*') { value_index++; f++; }
        else while (*f >= '0' && *f <= '9') f++;
        if (*f == '.')
        {
            f++;
            if (*f == '*')
            {
                if ((int)args[value_index] >= 0) limit = (int)args[value_index];
                value_index++;
                f++;
            }
            else
            {
                limit = 0;
                while (*f >= '0' && *f <= '9')
                    limit = min(limit, MAXLONG / 10) * 10 + (*f++ - '0');
            }
        }
        if (*f == 'h' && (f[1] == 's' || f[1] == 'S')) type = f + 1;
        else if (*f == 'S') type = f;

        if (type)
        {
            const char *str = (const char *)args[value_index];

            if (str)
            {
                ULONG bytes, size = 0;

                while (size < limit && str[size]) size++;
                if (size > (MAXULONG - sizeof(WCHAR)) / sizeof(WCHAR)) return STATUS_NO_MEMORY;
                RtlMultiByteToUnicodeSize(&bytes, str, size);
                wide = RtlpAllocateMemory(bytes + sizeof(WCHAR), TAG_MSGARG);
                if (!wide) return STATUS_NO_MEMORY;
                RtlZeroMemory(wide, bytes + sizeof(WCHAR));
                RtlMultiByteToUnicodeN(wide, bytes, &bytes, str, size);
                args[value_index] = (ULONG_PTR)wide;
            }
            if (type != f)
                memmove(f, type, (wcslen(type) + 1) * sizeof(WCHAR));
            *f = 's';
        }
    }

    if (end - *buffer < 1)
        status = STATUS_BUFFER_OVERFLOW;
    else
    {
        len = _snwprintf(*buffer, end - *buffer - 1, fmt, args[0], args[1], args[2], args[3], args[4]);
        if (len == -1)
        {
            /* _snwprintf does not terminate a truncated string */
            (*buffer)[end - *buffer - 1] = 0;
            status = STATUS_BUFFER_OVERFLOW;
        }
        else *buffer += len;
    }

    if (wide) RtlpFreeMemory(wide, TAG_MSGARG);
    return status;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlFormatMessageEx(
    IN PWSTR Message,
    IN ULONG MaxWidth OPTIONAL,
    IN BOOLEAN IgnoreInserts,
    IN BOOLEAN ArgumentsAreAnsi,
    IN BOOLEAN ArgumentsAreAnArray,
    IN va_list* Arguments,
    OUT PWSTR Buffer,
    IN ULONG BufferSize,
    OUT PULONG ReturnLength OPTIONAL,
    IN ULONG Flags)
{
    static const WCHAR spaceW = ' ';
    static const WCHAR crW    = '\r';
    static const WCHAR tabW   = '\t';
    static const WCHAR crlfW[] = {'\r','\n'};

    struct format_message_args args_data;
    NTSTATUS status = STATUS_SUCCESS;
    const WCHAR *src = Message;
    ULONG width = MaxWidth;

    WCHAR *buffer = Buffer;                         /* current position */
    WCHAR *end   = buffer + BufferSize / sizeof(WCHAR); /* end of buffer */
    WCHAR *line  = buffer;                         /* start of last line */
    WCHAR *space = NULL;                           /* last space */

    if (Flags) DPRINT1("%S unknown flags %lx\n", Message, Flags);

    args_data.last  = 0;
    args_data.overflow = FALSE;
    args_data.array = ArgumentsAreAnArray ? (ULONG_PTR *)Arguments : NULL;
    args_data.list  = ArgumentsAreAnArray ? NULL : Arguments;

    for ( ; *src; src++)
    {
        switch (*src)
        {
        case '\r':
            if (src[1] == '\n') src++;
            /* fall through */
        case '\n':
            if (!width)
            {
                status = add_chars(&buffer, end, crlfW, 2);
                line = buffer;
                space = NULL;
                break;
            }
            /* fall through */
        case ' ':
            space = buffer;
            status = add_chars(&buffer, end, &spaceW, 1);
            break;
        case '\t':
            if (space == buffer - 1) space = buffer;
            status = add_chars(&buffer, end, &tabW, 1);
            break;
        case '%':
            src++;
            switch (*src)
            {
            case 0:
                return STATUS_INVALID_PARAMETER;
            case 't':
                if (!width)
                {
                    status = add_chars(&buffer, end, &tabW, 1);
                    break;
                }
                /* fall through */
            case 'n':
                status = add_chars(&buffer, end, crlfW, 2);
                line = buffer;
                space = NULL;
                break;
            case 'r':
                status = add_chars(&buffer, end, &crW, 1);
                line = buffer;
                space = NULL;
                break;
            case '0':
                while (src[1]) src++;
                break;
            case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                if (!IgnoreInserts)
                {
                    int nr = *src++ - '0';
                    if (*src >= '0' && *src <= '9') nr = nr * 10 + *src++ - '0';
                    status = add_format(&buffer, end, &src, nr, ArgumentsAreAnsi, &args_data);
                    src--;
                    break;
                }
                /* fall through */
            default:
                if (IgnoreInserts) status = add_chars(&buffer, end, src - 1, 2);
                else status = add_chars(&buffer, end, src, 1);
                break;
            }
            break;
        default:
            status = add_chars(&buffer, end, src, 1);
            break;
        }

        if (!NT_SUCCESS(status)) return status;

        if (width && buffer - line >= width)
        {
            LONG_PTR diff = 2;
            WCHAR *next;

            if (space)  /* split line at the last space */
            {
                next = space + 1;
                while (space > line && (space[-1] == ' ' || space[-1] == '\t')) space--;
                diff -= next - space;
            }
            else space = next = buffer;  /* split at the end of the buffer */

            if (diff > 0 && end - buffer < diff) return STATUS_BUFFER_OVERFLOW;
            RtlMoveMemory(space + 2, next, (buffer - next) * sizeof(WCHAR));
            buffer += diff;
            RtlCopyMemory(space, crlfW, sizeof(crlfW));
            line = space + 2;
            space = NULL;
        }
    }

    status = add_chars(&buffer, end, L"", 1);
    if (!NT_SUCCESS(status)) return status;

    if (ReturnLength) *ReturnLength = (buffer - Buffer) * sizeof(WCHAR);
    return STATUS_SUCCESS;
}

/**********************************************************************
 *  RtlFormatMessage  (NTDLL.@)
 *
 * Formats a message (similar to sprintf).
 *
 * PARAMS
 *   Message             [I] Message to format.
 *   MaxWidth            [I] Maximum width in characters of each output line (optional).
 *   IgnoreInserts       [I] Whether to copy the message without processing inserts.
 *   ArgumentsAreAnsi    [I] Whether Arguments may have ANSI strings.
 *   ArgumentsAreAnArray [I] Whether Arguments is actually an array rather than a va_list *.
 *   Arguments           [I]
 *   Buffer              [O] Buffer to store processed message in.
 *   BufferSize          [I] Size of Buffer (in bytes).
 *   ReturnLength        [O] Size of the formatted message (in bytes; optional).
 *
 * RETURNS
 *      NTSTATUS code.
 *
 * @implemented
 */
NTSTATUS
NTAPI
RtlFormatMessage(
    IN PWSTR Message,
    IN ULONG MaxWidth OPTIONAL,
    IN BOOLEAN IgnoreInserts,
    IN BOOLEAN ArgumentsAreAnsi,
    IN BOOLEAN ArgumentsAreAnArray,
    IN va_list* Arguments,
    OUT PWSTR Buffer,
    IN ULONG BufferSize,
    OUT PULONG ReturnLength OPTIONAL)
{
    /* Call the extended API */
    return RtlFormatMessageEx(Message,
                              MaxWidth,
                              IgnoreInserts,
                              ArgumentsAreAnsi,
                              ArgumentsAreAnArray,
                              Arguments,
                              Buffer,
                              BufferSize,
                              ReturnLength,
                              0);
}

/* EOF */
