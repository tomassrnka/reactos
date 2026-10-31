/*
 * A reader of the .evtx files of Windows: the file header, the chunks, the records, and the binary XML
 * of the records (templates, substitutions) written out as the XML of an event.
 *
 * Copyright 2026 the reactos-nitro authors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "wevtapi_private.h"
#include "sddl.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wevtapi);

/*
 * The layout is the one that the public description of the format gives (libevtx, "Windows XML Event
 * Log (EVTX) format"): a header of 4096 bytes, then chunks of 64 KiB. A chunk has a header of 512 bytes
 * and records that follow it one after the other up to the free space. A record has a signature, its
 * size, its number, the time it was written at, a fragment of binary XML and the size again. The XML is
 * a stream of tokens; a template instance names a definition (the XML with holes) somewhere in the
 * chunk and brings the values that fill the holes.
 *
 * The file is untrusted. Every offset and every length is checked against the chunk, which is what
 * is in memory, before it is used; recursion (elements, templates and fragments in values) stops at
 * MAX_DEPTH; a record has a budget of tokens and of output; the number of values of a template and
 * their total size are capped. Nothing is cached between records: a name or a template is read from
 * the chunk by its offset each time, so there is no cache that a file could grow.
 */

#define FILE_HEADER_SIZE    4096
#define CHUNK_SIZE          65536
#define CHUNK_HEADER_SIZE   512
#define RECORD_MIN_SIZE     28
#define MAX_RECORDS         (CHUNK_SIZE / RECORD_MIN_SIZE)
#define MAX_DEPTH           64
#define MAX_TOKENS          200000
#define MAX_OUTPUT          (4 * 1024 * 1024)       /* characters of the XML of one event */
#define MAX_TEMPLATE_VALUES 4096
#define MAX_NAME            256
#define MAX_BLOB            (1024 * 1024)
#define MAX_USER_VALUES     4096

enum
{
    TOK_EOF = 0x00, TOK_OPEN = 0x01, TOK_CLOSE = 0x02, TOK_EMPTY = 0x03, TOK_END = 0x04, TOK_VALUE = 0x05,
    TOK_ATTR = 0x06, TOK_CDATA = 0x07, TOK_CHARREF = 0x08, TOK_ENTITY = 0x09, TOK_PITARGET = 0x0a,
    TOK_PIDATA = 0x0b, TOK_TEMPLATE = 0x0c, TOK_SUBST = 0x0d, TOK_OPTSUBST = 0x0e, TOK_FRAGMENT = 0x0f
};

/* the types of the values: those of EVT_VARIANT_TYPE, plus the XML of a fragment */
#define TYPE_BINXML 0x21
#define TYPE_ARRAY  0x80

static DWORD crc_table[256];
static BOOL crc_ready;

static DWORD crc32(const BYTE *data, SIZE_T len, DWORD crc)
{
    if (!crc_ready)
    {
        DWORD i, j, c;

        for (i = 0; i < 256; i++)
        {
            for (c = i, j = 0; j < 8; j++) c = (c & 1) ? 0xedb88320 ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = TRUE;
    }
    crc = ~crc;
    while (len--) crc = crc_table[(crc ^ *data++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static WORD get16(const BYTE *p) { return p[0] | (p[1] << 8); }
static DWORD get32(const BYTE *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((DWORD)p[3] << 24); }
static ULONGLONG get64(const BYTE *p) { return get32(p) | ((ULONGLONG)get32(p + 4) << 32); }

/* The file */

struct evtx_file
{
    HANDLE handle;
    WCHAR *path;
    DWORD chunk_count;
    LONGLONG chunk_base;            /* the offset of the first chunk in the file */
    BYTE *chunk;                    /* the chunk that is loaded */
    BOOL loaded;
    LONG chunk_index;               /* the chunk that is loaded, or the next one to load */
    DWORD offsets[MAX_RECORDS];     /* of the records of the loaded chunk, in the order of the file */
    DWORD record_count;
    LONG record_pos;                /* the next one to read, in the order of the file */
    BOOL drop_first;                /* a chunk with a wrong checksum loses the first record that is read */
    BOOL damaged_after;             /* what follows the records that were found is not records */
    BOOL reverse;
    BOOL checked;                   /* the checksums were looked at once; a seek back to the start does not report them again */
};

static BOOL read_at(HANDLE file, LONGLONG offset, BYTE *buf, DWORD len)
{
    LARGE_INTEGER pos;
    DWORD read;

    pos.QuadPart = offset;
    if (!SetFilePointerEx(file, pos, NULL, FILE_BEGIN)) return FALSE;
    return ReadFile(file, buf, len, &read, NULL) && read == len;
}

DWORD evtx_open(const WCHAR *path, BOOL reverse, struct evtx_file **ret)
{
    struct evtx_file *file;
    BYTE header[512];
    LARGE_INTEGER size;
    DWORD header_size, count, error = ERROR_SUCCESS;
    WORD block_size;

    *ret = NULL;
    if (!(file = calloc(1, sizeof(*file)))) return ERROR_OUTOFMEMORY;
    file->handle = INVALID_HANDLE_VALUE;
    if (!(file->path = _wcsdup(path)) || !(file->chunk = malloc(CHUNK_SIZE))) { error = ERROR_OUTOFMEMORY; goto fail; }
    file->handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file->handle == INVALID_HANDLE_VALUE) { error = GetLastError(); goto fail; }
    if (!GetFileSizeEx(file->handle, &size)) { error = GetLastError(); goto fail; }
    if (size.QuadPart < sizeof(header) || !read_at(file->handle, 0, header, sizeof(header))) { error = ERROR_EVENTLOG_FILE_CORRUPT; goto fail; }
    if (memcmp(header, "ElfFile\0", 8)) { error = ERROR_EVENTLOG_FILE_CORRUPT; goto fail; }
    header_size = get32(header + 32);
    block_size = get16(header + 40);
    count = get16(header + 42);
    if (header_size < 128 || header_size > block_size || block_size < 128 || block_size % 4096 || block_size > 65536)
    {
        error = ERROR_EVENTLOG_FILE_CORRUPT;
        goto fail;
    }
    file->chunk_base = block_size;
    /* no more chunks than the file has room for, the last of them perhaps cut off (which is reported when it is read) */
    if (size.QuadPart <= block_size) count = 0;
    else if ((size.QuadPart - block_size + CHUNK_SIZE - 1) / CHUNK_SIZE < count)
        count = (DWORD)((size.QuadPart - block_size + CHUNK_SIZE - 1) / CHUNK_SIZE);
    file->chunk_count = count;
    file->reverse = reverse;
    evtx_rewind(file);
    file->checked = FALSE;
    *ret = file;
    return ERROR_SUCCESS;

fail:
    evtx_close(file);
    return error;
}

void evtx_close(struct evtx_file *file)
{
    if (!file) return;
    if (file->handle != INVALID_HANDLE_VALUE) CloseHandle(file->handle);
    free(file->chunk);
    free(file->path);
    free(file);
}

void evtx_rewind(struct evtx_file *file)
{
    /* Windows reads a file again from its start with no complaint about the checksums that it has seen */
    file->checked = TRUE;
    file->loaded = FALSE;
    file->chunk_index = file->reverse ? (LONG)file->chunk_count : -1;
    file->record_count = file->record_pos = 0;
    file->drop_first = FALSE;
}

/* Binary XML into XML */

struct sb
{
    WCHAR *buf;
    SIZE_T len, cap;
    BOOL fail;
};

static void sb_add_n(struct sb *b, const WCHAR *text, SIZE_T len)
{
    if (b->fail) return;
    if (b->len + len + 1 > b->cap)
    {
        SIZE_T cap = b->cap ? b->cap : 1024;
        WCHAR *p;

        if (b->len + len > MAX_OUTPUT) { b->fail = TRUE; return; }
        while (cap < b->len + len + 1) cap *= 2;
        if (!(p = realloc(b->buf, cap * sizeof(WCHAR)))) { b->fail = TRUE; return; }
        b->buf = p;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, text, len * sizeof(WCHAR));
    b->len += len;
    b->buf[b->len] = 0;
}

static void sb_add(struct sb *b, const WCHAR *text) { sb_add_n(b, text, wcslen(text)); }

static void sb_char(struct sb *b, WCHAR c) { sb_add_n(b, &c, 1); }

static void sb_escape_n(struct sb *b, const WCHAR *text, SIZE_T len, BOOL attribute)
{
    for (; len; len--, text++)
    {
        switch (*text)
        {
        case '&': sb_add(b, L"&amp;"); break;
        case '<': sb_add(b, L"&lt;"); break;
        case '>': sb_add(b, L"&gt;"); break;
        case '\'': if (attribute) sb_add(b, L"&apos;"); else sb_char(b, *text); break;
        case '"': if (attribute) sb_add(b, L"&quot;"); else sb_char(b, *text); break;
        default:
            /* what XML has no character for is left out */
            if (*text >= ' ' || *text == '\t' || *text == '\n' || *text == '\r') sb_char(b, *text);
            break;
        }
    }
}

static void sb_printf(struct sb *b, const WCHAR *format, ...)
{
    WCHAR text[128];
    va_list args;

    va_start(args, format);
    _vsnwprintf(text, ARRAY_SIZE(text), format, args);
    va_end(args);
    text[ARRAY_SIZE(text) - 1] = 0;
    sb_add(b, text);
}

struct sub
{
    DWORD type;
    DWORD size;
    DWORD offset;                   /* in the chunk */
};

struct subs
{
    struct sub *v;
    DWORD count;
};

struct render
{
    const BYTE *c;                  /* the chunk */
    struct sb out;
    DWORD tokens;
    DWORD data_depth;               /* inside EventData or UserData */
    struct evtx_value *values;
    DWORD value_count, value_cap;
    BYTE *blob;
    DWORD blob_len, blob_cap;
    DWORD error;
};

static BOOL fail_render(struct render *r)
{
    r->error = ERROR_EVENTLOG_FILE_CORRUPT;
    return FALSE;
}

/* the sum is made wide: a length of the file must not wrap around on a 32-bit build */
#define NEED(r, pos, n) ((ULONGLONG)(pos) + (ULONGLONG)(n) <= CHUNK_SIZE)

static BOOL tick(struct render *r)
{
    if (++r->tokens > MAX_TOKENS || r->out.fail) return fail_render(r);
    return TRUE;
}

/* The name that the offset (which is read at pos) names. Inline names follow the offset. */
static BOOL read_name(struct render *r, DWORD *pos, WCHAR *name)
{
    const BYTE *c = r->c;
    DWORD off, n, i;

    if (!NEED(r, *pos, 4)) return fail_render(r);
    off = get32(c + *pos);
    *pos += 4;
    if (off == *pos)
    {
        if (!NEED(r, off, 8)) return fail_render(r);
        n = get16(c + off + 6);
        if (!NEED(r, off + 8, (n + 1) * 2)) return fail_render(r);
        *pos = off + 8 + (n + 1) * 2;
    }
    else
    {
        if (off < CHUNK_HEADER_SIZE || !NEED(r, off, 8)) return fail_render(r);
        n = get16(c + off + 6);
        if (!NEED(r, off + 8, (n + 1) * 2)) return fail_render(r);
    }
    if (!n || n >= MAX_NAME) return fail_render(r);
    for (i = 0; i < n; i++)
    {
        WCHAR ch = get16(c + off + 8 + i * 2);

        /* a name of an element or an attribute: letters, digits and a few marks */
        if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9') && ch != '_' &&
            ch != '-' && ch != '.' && ch != ':' && ch < 0x80)
            return fail_render(r);
        if (ch < 0x80 && ch >= '0' && ch <= '9' && !i) return fail_render(r);
        name[i] = ch;
    }
    name[n] = 0;
    return TRUE;
}

static DWORD fixed_size(DWORD type)
{
    switch (type & 0x7f)
    {
    case 0x03: case 0x04: return 1;
    case 0x05: case 0x06: return 2;
    case 0x07: case 0x08: case 0x0b: case 0x0d: case 0x14: return 4;
    case 0x09: case 0x0a: case 0x0c: case 0x11: case 0x15: return 8;
    case 0x0f: case 0x12: return 16;
    default: return 0;
    }
}

/* The text of one value of a type */
static void value_item_text(struct sb *b, DWORD type, const BYTE *p, DWORD size)
{
    switch (type & 0x7f)
    {
    case 0x03: sb_printf(b, L"%d", (int)(signed char)p[0]); break;
    case 0x04: sb_printf(b, L"%u", p[0]); break;
    case 0x05: sb_printf(b, L"%d", (int)(short)get16(p)); break;
    case 0x06: sb_printf(b, L"%u", get16(p)); break;
    case 0x07: sb_printf(b, L"%d", (int)get32(p)); break;
    case 0x08: sb_printf(b, L"%u", get32(p)); break;
    case 0x09: sb_printf(b, L"%I64d", (LONGLONG)get64(p)); break;
    case 0x0a: sb_printf(b, L"%I64u", get64(p)); break;
    case 0x0b:
    {
        DWORD v = get32(p);
        float f;

        memcpy(&f, &v, 4);
        sb_printf(b, L"%g", (double)f);
        break;
    }
    case 0x0c:
    {
        ULONGLONG v = get64(p);
        double d;

        memcpy(&d, &v, 8);
        sb_printf(b, L"%g", d);
        break;
    }
    case 0x0d: sb_add(b, get32(p) ? L"true" : L"false"); break;
    case 0x0f:
        sb_printf(b, L"{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", get32(p), get16(p + 4), get16(p + 6), p[8], p[9],
                  p[10], p[11], p[12], p[13], p[14], p[15]);
        break;
    case 0x11:
    {
        ULONGLONG ft = get64(p);
        FILETIME f;
        SYSTEMTIME st;

        f.dwLowDateTime = (DWORD)ft;
        f.dwHighDateTime = (DWORD)(ft >> 32);
        if (!FileTimeToSystemTime(&f, &st)) memset(&st, 0, sizeof(st));
        sb_printf(b, L"%04u-%02u-%02uT%02u:%02u:%02u.%07uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  (unsigned)(ft % 10000000));
        break;
    }
    case 0x12:
        sb_printf(b, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", get16(p), get16(p + 2), get16(p + 6), get16(p + 8), get16(p + 10),
                  get16(p + 12), get16(p + 14));
        break;
    case 0x14: sb_printf(b, L"0x%x", get32(p)); break;
    case 0x15: sb_printf(b, L"0x%I64x", get64(p)); break;
    }
}

static BOOL sid_valid(const BYTE *p, DWORD size, DWORD *len)
{
    DWORD n;

    if (size < 8 || p[0] != 1) return FALSE;
    n = p[1];
    if (n > 15 || 8 + n * 4 > size) return FALSE;
    *len = 8 + n * 4;
    return TRUE;
}

/* The text of a value, as the XML has it. The value is in range. */
static void value_text(struct sb *b, DWORD type, const BYTE *p, DWORD size, BOOL attribute)
{
    DWORD base = type & 0x7f, i, len;

    if (type & TYPE_ARRAY)
    {
        if (base == 1 || base == 2)
        {
            DWORD start = 0, unit = base == 1 ? 2 : 1, n = size / unit, first = 1;

            for (i = 0; i <= n; i++)
            {
                BOOL end = i == n || (unit == 2 ? get16(p + i * 2) == 0 : p[i] == 0);

                if (!end) continue;
                if (i > start || i < n)
                {
                    if (!first) sb_char(b, ',');
                    first = 0;
                    if (unit == 2)
                    {
                        WCHAR *s = malloc((i - start + 1) * sizeof(WCHAR));
                        DWORD k;

                        if (s)
                        {
                            for (k = start; k < i; k++) s[k - start] = get16(p + k * 2);
                            sb_escape_n(b, s, i - start, attribute);
                            free(s);
                        }
                    }
                    else
                    {
                        WCHAR *s = malloc((i - start + 1) * sizeof(WCHAR));
                        DWORD k;

                        if (s)
                        {
                            for (k = start; k < i; k++) s[k - start] = p[k];
                            sb_escape_n(b, s, i - start, attribute);
                            free(s);
                        }
                    }
                }
                start = i + 1;
            }
            return;
        }
        len = fixed_size(base);
        if (!len) return;
        for (i = 0; i + len <= size; i += len)
        {
            if (i) sb_char(b, ',');
            value_item_text(b, base, p + i, len);
        }
        return;
    }

    switch (base)
    {
    case 0x00: break;
    case 0x01:
    {
        DWORD n = size / 2, k;
        WCHAR *s;

        /* a string ends where its first NUL is */
        for (k = 0; k < n && get16(p + k * 2); k++) {}
        n = k;
        if (!(s = malloc((n + 1) * sizeof(WCHAR)))) { b->fail = TRUE; break; }
        for (k = 0; k < n; k++) s[k] = get16(p + k * 2);
        sb_escape_n(b, s, n, attribute);
        free(s);
        break;
    }
    case 0x02:
    {
        WCHAR *s;
        DWORD n = 0;
        int got;

        while (n < size && p[n]) n++;
        if (!(s = malloc((n + 1) * sizeof(WCHAR)))) { b->fail = TRUE; break; }
        got = n ? MultiByteToWideChar(CP_ACP, 0, (const char *)p, n, s, n) : 0;
        sb_escape_n(b, s, got, attribute);
        free(s);
        break;
    }
    case 0x0e:
    {
        static const WCHAR hex[] = L"0123456789ABCDEF";

        for (i = 0; i < size; i++)
        {
            sb_char(b, hex[p[i] >> 4]);
            sb_char(b, hex[p[i] & 15]);
        }
        break;
    }
    case 0x10:
        if (size == 4) sb_printf(b, L"0x%x", get32(p));
        else if (size == 8) sb_printf(b, L"0x%I64x", get64(p));
        break;
    case 0x13:
    {
        DWORD k;

        if (!sid_valid(p, size, &len)) break;
        {
            ULONGLONG authority = 0;

            for (k = 2; k < 8; k++) authority = (authority << 8) | p[k];
            sb_printf(b, L"S-%u-%I64u", p[0], authority);
        }
        for (k = 0; k < p[1]; k++) sb_printf(b, L"-%u", get32(p + 8 + k * 4));
        break;
    }
    default:
        len = fixed_size(base);
        if (len && size >= len) value_item_text(b, base, p, len);
        break;
    }
}

static BOOL add_user_value(struct render *r, DWORD type, const BYTE *p, DWORD size)
{
    struct evtx_value *v;

    if (r->value_count >= MAX_USER_VALUES) return TRUE;
    /* a value that is shorter than its type is no value; the readers of the variants rely on the size */
    if (!(type & TYPE_ARRAY) && ((fixed_size(type) && size < fixed_size(type)) || (type == 0x10 && size != 4 && size != 8)))
    {
        type = 0;
        size = 0;
    }
    if (r->value_count == r->value_cap)
    {
        DWORD cap = r->value_cap ? r->value_cap * 2 : 16;
        struct evtx_value *n = realloc(r->values, cap * sizeof(*n));

        if (!n) { r->error = ERROR_OUTOFMEMORY; return FALSE; }
        r->values = n;
        r->value_cap = cap;
    }
    if (r->blob_len + size > MAX_BLOB) return TRUE;
    if (r->blob_len + size > r->blob_cap)
    {
        DWORD cap = r->blob_cap ? r->blob_cap : 1024;
        BYTE *n;

        while (cap < r->blob_len + size) cap *= 2;
        if (!(n = realloc(r->blob, cap))) { r->error = ERROR_OUTOFMEMORY; return FALSE; }
        r->blob = n;
        r->blob_cap = cap;
    }
    v = &r->values[r->value_count++];
    v->type = type;
    v->size = size;
    v->offset = r->blob_len;
    memcpy(r->blob + r->blob_len, p, size);
    r->blob_len += size;
    return TRUE;
}

static BOOL render_fragment(struct render *r, DWORD pos, const struct subs *subs, DWORD depth, DWORD *end);

/* A substitution in the content of an element or of an attribute */
static BOOL render_subst(struct render *r, DWORD pos, const struct subs *subs, BOOL attribute, DWORD depth)
{
    const BYTE *c = r->c;
    WORD index;
    const struct sub *s;

    if (!NEED(r, pos, 4)) return fail_render(r);
    index = get16(c + pos + 1);
    if (!subs || index >= subs->count) return fail_render(r);
    s = &subs->v[index];
    if (s->type == TYPE_BINXML)
    {
        /* a fragment inside the value: only in content, and the data of the event may be in it */
        if (attribute) return fail_render(r);
        if (!s->size) return TRUE;
        return render_fragment(r, s->offset, NULL, depth + 1, NULL);
    }
    if (r->data_depth && !add_user_value(r, s->type, c + s->offset, s->size)) return FALSE;
    value_text(&r->out, s->type, c + s->offset, s->size, attribute);
    return !r->out.fail || fail_render(r);
}

/* A value that the token holds itself. Returns its size in bytes, with the token. */
static BOOL render_inline_value(struct render *r, DWORD *pos, BOOL attribute)
{
    const BYTE *c = r->c;
    DWORD type, size;

    if (!NEED(r, *pos, 2)) return fail_render(r);
    type = c[*pos + 1];
    *pos += 2;
    if (type == 1)
    {
        if (!NEED(r, *pos, 2)) return fail_render(r);
        size = get16(c + *pos) * 2;
        *pos += 2;
        if (!NEED(r, *pos, size)) return fail_render(r);
        value_text(&r->out, 1, c + *pos, size, attribute);
        *pos += size;
        return TRUE;
    }
    size = fixed_size(type);
    if (!size || !NEED(r, *pos, size)) return fail_render(r);
    value_text(&r->out, type, c + *pos, size, attribute);
    *pos += size;
    return TRUE;
}

static BOOL render_element(struct render *r, DWORD *pos, const struct subs *subs, DWORD depth);
static BOOL render_template(struct render *r, DWORD *pos, DWORD depth);

/* One token of the content of an element, or of the value of an attribute */
static BOOL render_value_token(struct render *r, DWORD *pos, const struct subs *subs, BOOL attribute, DWORD depth)
{
    const BYTE *c = r->c;
    DWORD tok = c[*pos] & 0x3f;
    WCHAR name[MAX_NAME];

    switch (tok)
    {
    case TOK_VALUE:
        return render_inline_value(r, pos, attribute);
    case TOK_SUBST:
    case TOK_OPTSUBST:
        if (!render_subst(r, *pos, subs, attribute, depth)) return FALSE;
        *pos += 4;
        return TRUE;
    case TOK_CHARREF:
    {
        WCHAR ch;

        if (!NEED(r, *pos, 3)) return fail_render(r);
        ch = get16(c + *pos + 1);
        *pos += 3;
        sb_escape_n(&r->out, &ch, 1, attribute);
        return TRUE;
    }
    case TOK_ENTITY:
        (*pos)++;
        if (!read_name(r, pos, name)) return FALSE;
        sb_char(&r->out, '&');
        sb_add(&r->out, name);
        sb_char(&r->out, ';');
        return TRUE;
    case TOK_CDATA:
    {
        DWORD size;
        WCHAR *s;
        DWORD i;

        if (!NEED(r, *pos, 3)) return fail_render(r);
        size = get16(c + *pos + 1);
        *pos += 3;
        if (!NEED(r, *pos, size * 2) || !(s = malloc((size + 1) * sizeof(WCHAR)))) return fail_render(r);
        for (i = 0; i < size; i++) s[i] = get16(c + *pos + i * 2);
        sb_escape_n(&r->out, s, size, attribute);
        free(s);
        *pos += size * 2;
        return TRUE;
    }
    }
    return fail_render(r);
}

static BOOL is_data_name(const WCHAR *name)
{
    return !wcscmp(name, L"EventData") || !wcscmp(name, L"UserData");
}

/* An element, at the token that opens it. pos is left behind its end. */
static BOOL render_element(struct render *r, DWORD *pos, const struct subs *subs, DWORD depth)
{
    const BYTE *c = r->c;
    WCHAR name[MAX_NAME], attr_name[MAX_NAME];
    DWORD tok, data_size, attr_end;
    BOOL has_attrs, data_element;

    if (depth > MAX_DEPTH || !tick(r)) return fail_render(r);
    if (!NEED(r, *pos, 7)) return fail_render(r);
    tok = c[*pos];
    has_attrs = (tok & 0x40) != 0;
    data_size = get32(c + *pos + 3);
    (void)data_size;
    *pos += 7;
    if (!read_name(r, pos, name)) return FALSE;

    sb_char(&r->out, '<');
    sb_add(&r->out, name);
    if (has_attrs)
    {
        if (!NEED(r, *pos, 4)) return fail_render(r);
        attr_end = get32(c + *pos);
        *pos += 4;
        if (attr_end > CHUNK_SIZE || *pos > CHUNK_SIZE - attr_end) return fail_render(r);
        attr_end += *pos;
        while (*pos < attr_end)
        {
            SIZE_T mark;
            DWORD t;

            if (!tick(r) || !NEED(r, *pos, 1) || (c[*pos] & 0x3f) != TOK_ATTR) return fail_render(r);
            (*pos)++;
            if (!read_name(r, pos, attr_name)) return FALSE;
            mark = r->out.len;
            sb_char(&r->out, ' ');
            sb_add(&r->out, attr_name);
            sb_add(&r->out, L"='");
            t = (DWORD)r->out.len;
            do
            {
                if (!NEED(r, *pos, 1) || !tick(r)) return fail_render(r);
                if (!render_value_token(r, pos, subs, TRUE, depth)) return FALSE;
            }
            while (*pos < attr_end && (c[*pos] & 0x3f) != TOK_ATTR);
            if (r->out.len == t)
            {
                /* an attribute with no value is not there */
                r->out.len = mark;
                if (r->out.buf) r->out.buf[mark] = 0;
            }
            else sb_char(&r->out, '\'');
        }
        if (*pos != attr_end) return fail_render(r);
    }
    if (!NEED(r, *pos, 1)) return fail_render(r);
    tok = c[(*pos)++];
    if (tok == TOK_EMPTY)
    {
        sb_add(&r->out, L"/>");
        return !r->out.fail || fail_render(r);
    }
    if (tok != TOK_CLOSE) return fail_render(r);
    sb_char(&r->out, '>');
    data_element = is_data_name(name);
    if (data_element) r->data_depth++;
    for (;;)
    {
        if (!tick(r) || !NEED(r, *pos, 1)) return fail_render(r);
        tok = c[*pos] & 0x3f;
        if (tok == TOK_END)
        {
            (*pos)++;
            break;
        }
        if (tok == TOK_OPEN)
        {
            if (!render_element(r, pos, subs, depth + 1)) return FALSE;
        }
        else if (tok == TOK_TEMPLATE)
        {
            if (!render_template(r, pos, depth + 1)) return FALSE;
        }
        else if (!render_value_token(r, pos, subs, FALSE, depth)) return FALSE;
    }
    if (data_element) r->data_depth--;
    sb_add(&r->out, L"</");
    sb_add(&r->out, name);
    sb_char(&r->out, '>');
    return !r->out.fail || fail_render(r);
}

/* A template instance: the definition (inline, or elsewhere in the chunk) and the values. pos is left behind the instance. */
static BOOL render_template(struct render *r, DWORD *pos, DWORD depth)
{
    const BYTE *c = r->c;
    DWORD p, def, count, i, total = 0, body, end;
    struct subs subs = { NULL, 0 };
    BOOL ok;

    if (depth > MAX_DEPTH || !tick(r)) return fail_render(r);
    if (!NEED(r, *pos, 10)) return fail_render(r);
    def = get32(c + *pos + 6);
    p = *pos + 10;
    if (def == p)
    {
        DWORD size;

        if (!NEED(r, p, 24)) return fail_render(r);
        size = get32(c + p + 20);
        if (!NEED(r, p + 24, size)) return fail_render(r);
        body = p + 24;
        p = body + size;
    }
    else
    {
        if (def < CHUNK_HEADER_SIZE || !NEED(r, def, 24)) return fail_render(r);
        body = def + 24;
    }
    if (!NEED(r, p, 4)) return fail_render(r);
    count = get32(c + p);
    p += 4;
    if (count > MAX_TEMPLATE_VALUES || !NEED(r, p, count * 4)) return fail_render(r);
    if (count && !(subs.v = calloc(count, sizeof(*subs.v)))) { r->error = ERROR_OUTOFMEMORY; return FALSE; }
    subs.count = count;
    for (i = 0; i < count; i++)
    {
        subs.v[i].size = get16(c + p + i * 4);
        subs.v[i].type = c[p + i * 4 + 2];
    }
    p += count * 4;
    for (i = 0; i < count; i++)
    {
        subs.v[i].offset = p;
        total += subs.v[i].size;
        if (!NEED(r, p, subs.v[i].size)) { free(subs.v); return fail_render(r); }
        p += subs.v[i].size;
    }
    (void)total;

    ok = render_fragment(r, body, &subs, depth + 1, &end);
    free(subs.v);
    if (!ok) return FALSE;
    *pos = p;
    return TRUE;
}

/* The tokens of a fragment of binary XML: a fragment header, then an element or a template instance, up to the end. */
static BOOL render_fragment(struct render *r, DWORD pos, const struct subs *subs, DWORD depth, DWORD *end)
{
    const BYTE *c = r->c;

    if (depth > MAX_DEPTH) return fail_render(r);
    for (;;)
    {
        DWORD tok;

        if (!tick(r) || !NEED(r, pos, 1)) return fail_render(r);
        tok = c[pos] & 0x3f;
        if (tok == TOK_EOF) { pos++; break; }
        if (tok == TOK_FRAGMENT)
        {
            if (!NEED(r, pos, 4)) return fail_render(r);
            pos += 4;
        }
        else if (tok == TOK_OPEN)
        {
            if (!render_element(r, &pos, subs, depth + 1)) return FALSE;
        }
        else if (tok == TOK_TEMPLATE)
        {
            if (!render_template(r, &pos, depth + 1)) return FALSE;
        }
        else return fail_render(r);
    }
    if (end) *end = pos;
    return TRUE;
}

/* Events */

void evtx_event_free(struct evtx_event *event)
{
    DWORD i;

    if (!event) return;
    free(event->xml);
    free(event->path);
    free(event->provider);
    free(event->channel);
    free(event->computer);
    if (event->strings)
        for (i = 0; i < event->value_count; i++) free(event->strings[i]);
    free(event->strings);
    free(event->sid);
    free(event->values);
    free(event->blob);
    free(event);
}

static WCHAR *dup_n(const WCHAR *s)
{
    return _wcsdup(s ? s : L"");
}

static int hexval(WCHAR c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* {747ef6fd-e535-4d16-b510-42c90f6873a1} */
static BOOL parse_guid(const WCHAR *s, GUID *guid)
{
    BYTE bytes[16];
    int i, k = 0;

    if (!s || wcslen(s) != 38 || s[0] != '{' || s[37] != '}') return FALSE;
    for (i = 1; i < 37; i++)
    {
        int hi, lo;

        if (i == 9 || i == 14 || i == 19 || i == 24)
        {
            if (s[i] != '-') return FALSE;
            continue;
        }
        if ((hi = hexval(s[i])) < 0 || (lo = hexval(s[i + 1])) < 0) return FALSE;
        bytes[k++] = hi * 16 + lo;
        i++;
    }
    guid->Data1 = (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
    guid->Data2 = (bytes[4] << 8) | bytes[5];
    guid->Data3 = (bytes[6] << 8) | bytes[7];
    memcpy(guid->Data4, bytes + 8, 8);
    return TRUE;
}

static ULONGLONG parse_u64(const WCHAR *s, BOOL *ok)
{
    ULONGLONG v = 0;
    int base = 10;
    SIZE_T n = 0;

    *ok = FALSE;
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    for (; *s; s++, n++)
    {
        int d;

        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return 0;
        if (v > ((~0ULL) - d) / base) return 0;
        v = v * base + d;
    }
    *ok = n != 0;
    return v;
}

/* 2016-09-21T10:22:57.9104312Z */
static BOOL parse_time(const WCHAR *s, ULONGLONG *ft)
{
    SYSTEMTIME st;
    FILETIME f;
    unsigned y, mo, d, h, mi, sec;
    unsigned long long frac = 0;
    int n, digits = 0;

    if (!s || swscanf(s, L"%4u-%2u-%2uT%2u:%2u:%2u%n", &y, &mo, &d, &h, &mi, &sec, &n) < 6) return FALSE;
    memset(&st, 0, sizeof(st));
    st.wYear = y; st.wMonth = mo; st.wDay = d; st.wHour = h; st.wMinute = mi; st.wSecond = sec;
    if (!SystemTimeToFileTime(&st, &f)) return FALSE;
    *ft = f.dwLowDateTime | ((ULONGLONG)f.dwHighDateTime << 32);
    s += n;
    if (*s == '.')
    {
        for (s++; *s >= '0' && *s <= '9' && digits < 7; s++, digits++) frac = frac * 10 + (*s - '0');
        while (digits++ < 7) frac *= 10;
        *ft += frac;
    }
    return TRUE;
}

static BOOL fill_event(struct evtx_event *event, const WCHAR *path, ULONGLONG header_id, ULONGLONG header_time, struct render *r)
{
    struct xnode *doc;
    const WCHAR *v;
    BOOL ok;
    ULONGLONG n;
    DWORD i;

    /* the XML is rendered by this file, so it is the document that the query layer reads as well */
    if (!(doc = xml_parse_event(event->xml))) return FALSE;
    if (!(v = xml_value(doc, L"Event/System/Provider/@Name")) && !xml_value(doc, L"Event/System")) { xml_free(doc); return FALSE; }
    event->provider = dup_n(v);
    event->channel = dup_n(xml_value(doc, L"Event/System/Channel"));
    event->computer = dup_n(xml_value(doc, L"Event/System/Computer"));
    event->path = _wcsdup(path);
    event->has_provider_guid = parse_guid(xml_value(doc, L"Event/System/Provider/@Guid"), &event->provider_guid);
    event->has_activity = parse_guid(xml_value(doc, L"Event/System/Correlation/@ActivityID"), &event->activity);
    event->has_related_activity = parse_guid(xml_value(doc, L"Event/System/Correlation/@RelatedActivityID"), &event->related_activity);
    n = parse_u64(xml_value(doc, L"Event/System/EventID"), &ok);
    event->id = ok ? (WORD)n : 0;
    n = parse_u64(xml_value(doc, L"Event/System/EventID/@Qualifiers"), &ok);
    event->has_qualifiers = ok;
    event->qualifiers = ok ? (WORD)n : 0;
    n = parse_u64(xml_value(doc, L"Event/System/Version"), &ok);
    event->version = ok ? (BYTE)n : 0;
    n = parse_u64(xml_value(doc, L"Event/System/Level"), &ok);
    event->level = ok ? (BYTE)n : 0;
    n = parse_u64(xml_value(doc, L"Event/System/Task"), &ok);
    event->task = ok ? (WORD)n : 0;
    n = parse_u64(xml_value(doc, L"Event/System/Opcode"), &ok);
    event->opcode = ok ? (BYTE)n : 0;
    event->keywords = parse_u64(xml_value(doc, L"Event/System/Keywords"), &ok);
    event->record_id = parse_u64(xml_value(doc, L"Event/System/EventRecordID"), &ok);
    if (!ok) event->record_id = header_id;
    if (!parse_time(xml_value(doc, L"Event/System/TimeCreated/@SystemTime"), &event->time)) event->time = header_time;
    event->process_id = (DWORD)parse_u64(xml_value(doc, L"Event/System/Execution/@ProcessID"), &ok);
    event->thread_id = (DWORD)parse_u64(xml_value(doc, L"Event/System/Execution/@ThreadID"), &ok);
    if ((v = xml_value(doc, L"Event/System/Security/@UserID")) && v[0])
    {
        PSID sid = NULL;

        if (ConvertStringSidToSidW(v, &sid))
        {
            DWORD len = GetLengthSid(sid);

            if ((event->sid = malloc(len))) memcpy(event->sid, sid, len);
            LocalFree(sid);
        }
    }
    xml_free(doc);
    if (!event->provider || !event->channel || !event->computer || !event->path) return FALSE;

    /* the data of the event, with the text of each value for the messages that insert them */
    event->values = r->values;
    event->value_count = r->value_count;
    event->blob = r->blob;
    event->blob_len = r->blob_len;
    r->values = NULL;
    r->blob = NULL;
    r->value_count = r->blob_len = 0;
    if (event->value_count)
    {
        if (!(event->strings = calloc(event->value_count, sizeof(*event->strings)))) return FALSE;
        for (i = 0; i < event->value_count; i++)
        {
            struct sb b = { NULL, 0, 0, FALSE };
            const struct evtx_value *val = &event->values[i];

            value_text(&b, val->type, event->blob + val->offset, val->size, FALSE);
            if (b.fail) { free(b.buf); return FALSE; }
            /* the text is the value, not the XML of it */
            event->strings[i] = b.buf ? b.buf : _wcsdup(L"");
            if (!event->strings[i]) return FALSE;
        }
    }
    return TRUE;
}

/* Of the record at an offset of the loaded chunk. Returns ERROR_SUCCESS with the event, or an error. */
static DWORD read_record(struct evtx_file *file, DWORD offset, struct evtx_event **ret)
{
    const BYTE *c = file->chunk;
    DWORD size = get32(c + offset + 4);
    struct render r;
    struct evtx_event *event;
    DWORD pos, end;
    BOOL ok;

    memset(&r, 0, sizeof(r));
    r.c = c;
    pos = offset + 24;
    /* the binary XML of the record starts with a fragment header, and ends where the record does */
    ok = render_fragment(&r, pos, NULL, 0, &end) && end <= offset + size - 4;
    if (!ok || !r.out.buf)
    {
        DWORD error = r.error ? r.error : ERROR_EVENTLOG_FILE_CORRUPT;

        free(r.out.buf);
        free(r.values);
        free(r.blob);
        return error == ERROR_OUTOFMEMORY ? error : ERROR_EVENTLOG_FILE_CORRUPT;
    }
    if (!(event = calloc(1, sizeof(*event)))) ok = FALSE;
    else
    {
        event->xml = r.out.buf;
        r.out.buf = NULL;
        ok = fill_event(event, file->path, get64(c + offset + 8), get64(c + offset + 16), &r);
    }
    free(r.out.buf);
    free(r.values);
    free(r.blob);
    if (!ok)
    {
        evtx_event_free(event);
        return ERROR_EVENTLOG_FILE_CORRUPT;
    }
    *ret = event;
    return ERROR_SUCCESS;
}

/* Reads chunk number index into memory and finds its records. */
static DWORD load_chunk(struct evtx_file *file, DWORD index)
{
    const BYTE *c = file->chunk;
    DWORD free_offset, o, i;
    BOOL all_zero = TRUE;

    file->loaded = TRUE;
    file->record_count = 0;
    file->drop_first = FALSE;
    file->damaged_after = FALSE;
    if (!read_at(file->handle, file->chunk_base + (LONGLONG)index * CHUNK_SIZE, file->chunk, CHUNK_SIZE))
        return ERROR_EVENTLOG_FILE_CORRUPT;
    for (i = 0; i < CHUNK_HEADER_SIZE && all_zero; i++) all_zero = c[i] == 0;
    if (all_zero) return ERROR_SUCCESS;                 /* a chunk that was never used */
    if (memcmp(c, "ElfChnk\0", 8) || get32(c + 40) != 128) return ERROR_EVENTLOG_FILE_CORRUPT;
    free_offset = get32(c + 48);
    if (free_offset < CHUNK_HEADER_SIZE || free_offset > CHUNK_SIZE) return ERROR_EVENTLOG_FILE_CORRUPT;

    for (o = CHUNK_HEADER_SIZE; o + RECORD_MIN_SIZE <= free_offset && file->record_count < MAX_RECORDS;)
    {
        DWORD size;

        if (get32(c + o) != 0x2a2a) { file->damaged_after = TRUE; break; }
        size = get32(c + o + 4);
        if (size < RECORD_MIN_SIZE || size > free_offset - o || get32(c + o + size - 4) != size)
        {
            file->damaged_after = TRUE;
            break;
        }
        file->offsets[file->record_count++] = o;
        o += size;
    }

    /* the checksums of the chunk: what does not match is an edited or a damaged chunk */
    {
        DWORD header_crc = crc32(c, 120, 0);

        header_crc = crc32(c + 128, CHUNK_HEADER_SIZE - 128, header_crc);
        if (!file->checked && (get32(c + 124) != header_crc || get32(c + 52) != crc32(c + CHUNK_HEADER_SIZE, free_offset - CHUNK_HEADER_SIZE, 0)))
        {
            file->drop_first = TRUE;
            return ERROR_EVENTLOG_FILE_CORRUPT;
        }
    }
    return ERROR_SUCCESS;
}

DWORD evtx_next(struct evtx_file *file, struct evtx_event **ret)
{
    for (;;)
    {
        if (!file->loaded)
        {
            LONG index = file->chunk_index + (file->reverse ? -1 : 1);

            if (index < 0 || index >= (LONG)file->chunk_count) return ERROR_NO_MORE_ITEMS;
            file->chunk_index = index;
            file->record_pos = 0;
            /* a chunk that cannot be used is reported, and the call that follows goes on with the next one */
            if (load_chunk(file, index)) return ERROR_EVENTLOG_FILE_CORRUPT;
        }
        if ((DWORD)file->record_pos >= file->record_count)
        {
            BOOL damaged = file->damaged_after;

            file->loaded = FALSE;
            file->damaged_after = FALSE;
            if (damaged) return ERROR_EVENTLOG_FILE_CORRUPT;
            continue;
        }
        {
            DWORD i = file->record_pos++, error;

            if (file->drop_first)
            {
                file->drop_first = FALSE;
                continue;
            }
            if (file->reverse) i = file->record_count - 1 - i;
            error = read_record(file, file->offsets[i], ret);
            return error ? ERROR_EVENTLOG_FILE_CORRUPT : ERROR_SUCCESS;
        }
    }
}
