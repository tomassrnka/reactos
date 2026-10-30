/*
 * Subject interface package for PowerShell scripts
 *
 * Signs and verifies PowerShell scripts and the other files PowerShell signs
 * (.ps1, .psm1, .psd1, .ps1xml, .psc1, .cdxml, .mof): the signature is a PKCS #7 message
 * kept as base64 in a comment block at the end of the file. The digest covers the file as
 * text, without the block.
 *
 * Copyright 2026 reactos-nitro contributors
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winnls.h"
#include "wincrypt.h"
#include "mssip.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(pwrshsip);

#ifndef TRUST_E_BAD_DIGEST
#define TRUST_E_BAD_DIGEST _HRESULT_TYPEDEF_(0x80096010L)
#endif
#ifndef TRUST_E_NOSIGNATURE
#define TRUST_E_NOSIGNATURE _HRESULT_TYPEDEF_(0x800B0100L)
#endif

#define SPC_SIPINFO_OBJID "1.3.6.1.4.1.311.2.1.30"

static GUID pwsh_subject = {0x603bcc1f, 0x4b59, 0x4e08, {0xb7, 0x24, 0xd2, 0xc6, 0x29, 0x7e, 0xf3, 0x51}};

enum comment_style { STYLE_HASH, STYLE_XML, STYLE_C };

static const struct
{
    const WCHAR *ext;
    enum comment_style style;
} extensions[] =
{
    {L".ps1", STYLE_HASH}, {L".psm1", STYLE_HASH}, {L".psd1", STYLE_HASH}, {L".ps1xml", STYLE_XML},
    {L".psc1", STYLE_XML}, {L".cdxml", STYLE_XML}, {L".mof", STYLE_C},
};

static const char begin_text[] = "SIG # Begin signature block";
static const char end_text[] = "SIG # End signature block";

static BOOL style_of_name(const WCHAR *name, enum comment_style *style)
{
    const WCHAR *ext;
    unsigned int i;

    if (!name || !(ext = wcsrchr(name, '.'))) return FALSE;
    for (i = 0; i < ARRAYSIZE(extensions); i++)
    {
        if (!wcsicmp(ext, extensions[i].ext))
        {
            if (style) *style = extensions[i].style;
            return TRUE;
        }
    }
    return FALSE;
}

/***********************************************************************
 *              PwrShSIPIsFileSupportedName (PWRSHSIP.@)
 */
BOOL WINAPI PwrShSIPIsFileSupportedName(WCHAR *name, GUID *subject)
{
    TRACE("%s, %p\n", debugstr_w(name), subject);

    if (!style_of_name(name, NULL)) return FALSE;
    *subject = pwsh_subject;
    return TRUE;
}

struct script
{
    BYTE *raw;
    DWORD raw_len;
    WCHAR *text;     /* the text, a byte order mark is kept as U+FEFF */
    DWORD text_len;  /* in characters */
    enum { ENC_ANSI, ENC_UTF8, ENC_UTF16 } encoding;
    enum comment_style style;
};

static void free_script(struct script *s)
{
    free(s->raw);
    free(s->text);
    memset(s, 0, sizeof(*s));
}

static BOOL read_handle(HANDLE file, BYTE **data, DWORD *len)
{
    LARGE_INTEGER size;
    DWORD read;
    BYTE *buf;

    if (!GetFileSizeEx(file, &size) || size.QuadPart > 0x10000000) return FALSE;
    if (!(buf = malloc(size.u.LowPart ? size.u.LowPart : 1)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    if (!ReadFile(file, buf, size.u.LowPart, &read, NULL) || read != size.u.LowPart)
    {
        free(buf);
        return FALSE;
    }
    *data = buf;
    *len = read;
    return TRUE;
}

static BOOL load_script(SIP_SUBJECTINFO *info, struct script *s)
{
    HANDLE file = info->hFile;
    BOOL opened = FALSE, ret;

    memset(s, 0, sizeof(*s));
    if (!style_of_name(info->pwsFileName, &s->style) && !style_of_name(info->pwsDisplayName, &s->style))
        s->style = STYLE_HASH;

    if (info->dwUnionChoice == MSSIP_ADDINFO_BLOB && info->psBlob && info->psBlob->pbMemObject)
    {
        s->raw_len = info->psBlob->cbMemObject;
        if (s->raw_len > 0x10000000)
        {
            SetLastError(ERROR_INVALID_DATA);
            return FALSE;
        }
        if (!(s->raw = malloc(s->raw_len ? s->raw_len : 1)))
        {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        memcpy(s->raw, info->psBlob->pbMemObject, s->raw_len);
    }
    else
    {
        if (!file || file == INVALID_HANDLE_VALUE)
        {
            if (!info->pwsFileName)
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            file = CreateFileW(info->pwsFileName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
            if (file == INVALID_HANDLE_VALUE) return FALSE;
            opened = TRUE;
        }
        ret = read_handle(file, &s->raw, &s->raw_len);
        if (opened) CloseHandle(file);
        if (!ret) return FALSE;
    }

    if (s->raw_len >= 2 && s->raw[0] == 0xff && s->raw[1] == 0xfe)
    {
        s->encoding = ENC_UTF16;
        s->text_len = s->raw_len / sizeof(WCHAR);
        s->text = malloc((s->text_len + 1) * sizeof(WCHAR));
        if (s->text) memcpy(s->text, s->raw, s->text_len * sizeof(WCHAR));
    }
    else if (s->raw_len >= 2 && s->raw[0] == 0xfe && s->raw[1] == 0xff)
    {
        /* big endian text is not supported */
        free_script(s);
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    else if (s->raw_len >= 3 && s->raw[0] == 0xef && s->raw[1] == 0xbb && s->raw[2] == 0xbf)
    {
        int len = MultiByteToWideChar(CP_UTF8, 0, (char *)s->raw, s->raw_len, NULL, 0);

        s->encoding = ENC_UTF8;
        s->text_len = len;
        s->text = malloc((len + 1) * sizeof(WCHAR));
        if (s->text && len) MultiByteToWideChar(CP_UTF8, 0, (char *)s->raw, s->raw_len, s->text, len);
    }
    else
    {
        DWORD i;

        s->encoding = ENC_ANSI;
        s->text_len = s->raw_len;
        s->text = malloc((s->text_len + 1) * sizeof(WCHAR));
        if (s->text) for (i = 0; i < s->raw_len; i++) s->text[i] = s->raw[i];
    }
    if (!s->text)
    {
        free_script(s);
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    s->text[s->text_len] = 0;
    return TRUE;
}

static void comment_wrappers(enum comment_style style, const char **open, const char **close)
{
    switch (style)
    {
    case STYLE_XML: *open = "<!-- "; *close = " -->"; break;
    case STYLE_C:   *open = "/* ";   *close = " */";  break;
    default:        *open = "# ";    *close = "";     break;
    }
}

/* A line (without its terminator) is a comment of the style whose body is exactly "text", or, with text NULL,
 * only base64 characters. */
static BOOL line_is(const WCHAR *line, DWORD len, enum comment_style style, const char *text, DWORD *payload,
                    DWORD *payload_len)
{
    const char *open, *close;
    DWORD olen, clen, i;

    comment_wrappers(style, &open, &close);
    olen = strlen(open);
    clen = strlen(close);
    if (len < olen + clen) return FALSE;
    for (i = 0; i < olen; i++) if (line[i] != (WCHAR)open[i]) return FALSE;
    for (i = 0; i < clen; i++) if (line[len - clen + i] != (WCHAR)close[i]) return FALSE;
    len -= olen + clen;
    if (text)
    {
        DWORD tlen = strlen(text);

        if (len != tlen) return FALSE;
        for (i = 0; i < tlen; i++) if (line[olen + i] != (WCHAR)text[i]) return FALSE;
    }
    else
    {
        for (i = 0; i < len; i++)
        {
            WCHAR c = line[olen + i];

            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '+' || c == '/' || c == '=')) return FALSE;
        }
    }
    if (payload) *payload = olen;
    if (payload_len) *payload_len = len;
    return TRUE;
}

/* Gets the next line: its start, its length without the terminator, and whether it ends in CR LF. A line
 * without a terminator can only be the last one. */
static BOOL next_line(const struct script *s, DWORD *pos, DWORD *start, DWORD *len, BOOL *crlf, BOOL *terminated)
{
    DWORD p = *pos;

    if (p >= s->text_len) return FALSE;
    *start = p;
    while (p < s->text_len && s->text[p] != '\n') p++;
    *terminated = p < s->text_len;
    *len = p - *start;
    *crlf = FALSE;
    if (*terminated)
    {
        if (*len && s->text[p - 1] == '\r')
        {
            *crlf = TRUE;
            (*len)--;
        }
        p++;
    }
    *pos = p;
    return TRUE;
}

/* The signature block is the last begin line, followed by lines of base64 comments and an end line that is
 * the end of the file; every line break in it is CR LF and the begin line follows a line break. Anything
 * else (text after the block, a second block, stray text in it) is not a signature. */
static BOOL find_block(const struct script *s, DWORD *begin, DWORD *end)
{
    DWORD pos = 0, start, len, last_begin = 0;
    BOOL crlf, term, found = FALSE;

    while (next_line(s, &pos, &start, &len, &crlf, &term))
    {
        if (line_is(s->text + start, len, s->style, begin_text, NULL, NULL))
        {
            found = TRUE;
            last_begin = start;
        }
    }
    if (!found || last_begin < 2 || s->text[last_begin - 2] != '\r' || s->text[last_begin - 1] != '\n')
        return FALSE;

    /* no end line in front of the block */
    pos = 0;
    while (pos < last_begin && next_line(s, &pos, &start, &len, &crlf, &term))
        if (line_is(s->text + start, len, s->style, end_text, NULL, NULL)) return FALSE;

    pos = last_begin;
    next_line(s, &pos, &start, &len, &crlf, &term);
    if (!crlf) return FALSE;
    while (next_line(s, &pos, &start, &len, &crlf, &term))
    {
        if (line_is(s->text + start, len, s->style, end_text, NULL, NULL))
        {
            if (pos != s->text_len || (term && !crlf)) return FALSE;
            *begin = last_begin;
            *end = pos;
            return TRUE;
        }
        if (!crlf || !line_is(s->text + start, len, s->style, NULL, NULL, NULL)) return FALSE;
    }
    return FALSE;
}

/* the part of the text a digest covers: everything in front of the signature block, without the line
 * break that signing adds in front of it */
static DWORD content_length(const struct script *s)
{
    DWORD begin, end, len;

    if (!find_block(s, &begin, &end)) return s->text_len;
    len = begin;
    if (len >= 2 && s->text[len - 2] == '\r' && s->text[len - 1] == '\n') len -= 2;
    return len;
}

static ALG_ID alg_from_oid(const char *oid)
{
    if (!oid) return 0;
    return CertOIDToAlgId(oid);
}

static BOOL hash_content(const struct script *s, ALG_ID alg, BYTE *digest, DWORD *len)
{
    HCRYPTPROV prov;
    HCRYPTHASH hash;
    BOOL ret = FALSE;

    if (!alg)
    {
        SetLastError(NTE_BAD_ALGID);
        return FALSE;
    }
    if (!CryptAcquireContextW(&prov, NULL, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return FALSE;
    if (CryptCreateHash(prov, alg, 0, 0, &hash))
    {
        ret = CryptHashData(hash, (BYTE *)s->text, content_length(s) * sizeof(WCHAR), 0) &&
              CryptGetHashParam(hash, HP_HASHVAL, digest, len, 0);
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
    return ret;
}

static int base64_value(WCHAR c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* returns the decoded message in a new buffer */
static BYTE *extract_signature(const struct script *s, DWORD *len)
{
    DWORD begin, end, pos, start, line_len, i, n = 0, bits = 0, acc = 0, payload, payload_len;
    BOOL crlf, term;
    BYTE *msg;

    if (!find_block(s, &begin, &end)) return NULL;
    if (!(msg = malloc((end - begin) / 4 * 3 + 4))) return NULL;

    pos = begin;
    next_line(s, &pos, &start, &line_len, &crlf, &term); /* the begin line */
    while (next_line(s, &pos, &start, &line_len, &crlf, &term))
    {
        if (line_is(s->text + start, line_len, s->style, end_text, NULL, NULL)) break;
        if (!line_is(s->text + start, line_len, s->style, NULL, &payload, &payload_len)) continue;
        for (i = 0; i < payload_len; i++)
        {
            int v = base64_value(s->text[start + payload + i]);

            if (v < 0) continue;
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                msg[n++] = (acc >> bits) & 0xff;
            }
        }
    }
    *len = n;
    return msg;
}

/***********************************************************************
 *              PwrShSIPGetSignedDataMsg (PWRSHSIP.@)
 */
BOOL WINAPI PwrShSIPGetSignedDataMsg(SIP_SUBJECTINFO *info, DWORD *encoding, DWORD index, DWORD *size, BYTE *buf)
{
    struct script s;
    BYTE *msg;
    DWORD len;
    BOOL ret = FALSE;

    TRACE("%p, %p, %lu, %p, %p\n", info, encoding, index, size, buf);

    if (!info || info->cbSize != sizeof(*info) || !size)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (index)
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    if (!load_script(info, &s)) return FALSE;
    msg = extract_signature(&s, &len);
    free_script(&s);
    if (!msg || !len)
    {
        /* an unsigned file has no message, which is not an error */
        free(msg);
        *size = 0;
        if (encoding) *encoding = 0;
        return TRUE;
    }

    if (encoding) *encoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    if (!buf) ret = TRUE;
    else if (*size < len) SetLastError(ERROR_INSUFFICIENT_BUFFER);
    else
    {
        memcpy(buf, msg, len);
        ret = TRUE;
    }
    *size = len;
    free(msg);
    return ret;
}

/* an ASCII/UTF-16 string appended to a byte buffer in the encoding of the script */
struct out
{
    BYTE *data;
    DWORD len, cap;
};

static BOOL out_add(struct out *o, const BYTE *data, DWORD len)
{
    if (o->len + len > o->cap)
    {
        DWORD cap = max(o->cap * 2, o->len + len + 256);
        BYTE *p = realloc(o->data, cap);

        if (!p) return FALSE;
        o->data = p;
        o->cap = cap;
    }
    memcpy(o->data + o->len, data, len);
    o->len += len;
    return TRUE;
}

static BOOL out_text(struct out *o, const struct script *s, const WCHAR *text, DWORD len)
{
    if (s->encoding == ENC_UTF16) return out_add(o, (const BYTE *)text, len * sizeof(WCHAR));
    if (s->encoding == ENC_UTF8)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, text, len, NULL, 0, NULL, NULL);
        char *tmp = malloc(n ? n : 1);
        BOOL ret;

        if (!tmp) return FALSE;
        WideCharToMultiByte(CP_UTF8, 0, text, len, tmp, n, NULL, NULL);
        ret = out_add(o, (BYTE *)tmp, n);
        free(tmp);
        return ret;
    }
    {
        DWORD i;

        for (i = 0; i < len; i++)
        {
            BYTE c = (BYTE)text[i];

            if (!out_add(o, &c, 1)) return FALSE;
        }
    }
    return TRUE;
}

static BOOL out_ascii(struct out *o, const struct script *s, const char *text)
{
    WCHAR tmp[128];
    DWORD len = strlen(text), i;

    if (len > ARRAYSIZE(tmp)) len = ARRAYSIZE(tmp);
    for (i = 0; i < len; i++) tmp[i] = (BYTE)text[i];
    return out_text(o, s, tmp, len);
}

static BOOL write_script(SIP_SUBJECTINFO *info, const struct out *o)
{
    HANDLE file = info->hFile;
    BOOL opened = FALSE, ret;
    DWORD written;

    if (!file || file == INVALID_HANDLE_VALUE)
    {
        if (!info->pwsFileName)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        file = CreateFileW(info->pwsFileName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (file == INVALID_HANDLE_VALUE) return FALSE;
        opened = TRUE;
    }
    ret = SetFilePointer(file, 0, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
          WriteFile(file, o->data, o->len, &written, NULL) && written == o->len && SetEndOfFile(file);
    if (opened) CloseHandle(file);
    return ret;
}

static BOOL write_block(struct out *o, const struct script *s, const BYTE *msg, DWORD len)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *open, *close;
    char line[96];
    DWORD i = 0;

    comment_wrappers(s->style, &open, &close);
    sprintf(line, "%s%s%s\r\n", open, begin_text, close);
    if (!out_ascii(o, s, line)) return FALSE;
    while (i < len)
    {
        char b64[65];
        DWORD n = 0;

        while (i < len && n < 64)
        {
            DWORD chunk = min(3, len - i), v = 0, k;

            for (k = 0; k < 3; k++) v = (v << 8) | (k < chunk ? msg[i + k] : 0);
            b64[n++] = tbl[(v >> 18) & 63];
            b64[n++] = tbl[(v >> 12) & 63];
            b64[n++] = chunk > 1 ? tbl[(v >> 6) & 63] : '=';
            b64[n++] = chunk > 2 ? tbl[v & 63] : '=';
            i += chunk;
        }
        b64[n] = 0;
        sprintf(line, "%s%s%s\r\n", open, b64, close);
        if (!out_ascii(o, s, line)) return FALSE;
    }
    sprintf(line, "%s%s%s\r\n", open, end_text, close);
    return out_ascii(o, s, line);
}

/***********************************************************************
 *              PwrShSIPPutSignedDataMsg (PWRSHSIP.@)
 */
BOOL WINAPI PwrShSIPPutSignedDataMsg(SIP_SUBJECTINFO *info, DWORD encoding, DWORD *index, DWORD size, BYTE *buf)
{
    static const BYTE crlf16[] = {'\r', 0, '\n', 0}, crlf[] = {'\r', '\n'};
    struct script s;
    struct out o = {0};
    BOOL ret = FALSE;
    DWORD len;

    TRACE("%p, %#lx, %p, %lu, %p\n", info, encoding, index, size, buf);

    if (!info || info->cbSize != sizeof(*info) || !buf || !size)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!load_script(info, &s)) return FALSE;

    /* replace a block that is already there */
    len = content_length(&s);
    if (out_text(&o, &s, s.text, len) &&
        out_add(&o, s.encoding == ENC_UTF16 ? crlf16 : crlf, s.encoding == ENC_UTF16 ? sizeof(crlf16) : sizeof(crlf)) &&
        write_block(&o, &s, buf, size))
        ret = write_script(info, &o);
    else if (GetLastError() == ERROR_SUCCESS)
        SetLastError(ERROR_OUTOFMEMORY);
    if (ret && index) *index = 0;
    free(o.data);
    free_script(&s);
    return ret;
}

/***********************************************************************
 *              PwrShSIPRemoveSignedDataMsg (PWRSHSIP.@)
 */
BOOL WINAPI PwrShSIPRemoveSignedDataMsg(SIP_SUBJECTINFO *info, DWORD index)
{
    struct script s;
    struct out o = {0};
    DWORD begin, end;
    BOOL ret = FALSE;

    TRACE("%p, %lu\n", info, index);

    if (!info || info->cbSize != sizeof(*info))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (index)
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    if (!load_script(info, &s)) return FALSE;
    if (!find_block(&s, &begin, &end))
    {
        free_script(&s);
        SetLastError(TRUST_E_NOSIGNATURE);
        return FALSE;
    }
    if (out_text(&o, &s, s.text, content_length(&s)))
        ret = write_script(info, &o);
    free(o.data);
    free_script(&s);
    return ret;
}

/***********************************************************************
 *              PwrShSIPCreateIndirectData (PWRSHSIP.@)
 */
BOOL WINAPI PwrShSIPCreateIndirectData(SIP_SUBJECTINFO *info, DWORD *size, SIP_INDIRECT_DATA *data)
{
    /* SpcSipInfo: version 0x010000, the subject GUID and five reserved integers */
    static const BYTE sip_info_head[] = {0x30, 0x26, 0x02, 0x03, 0x01, 0x00, 0x00, 0x04, 0x10};
    static const BYTE sip_info_tail[] = {0x02, 0x01, 0x00, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00,
                                         0x02, 0x01, 0x00};
    static const BYTE null_params[] = {0x05, 0x00};
    static const char data_oid[] = SPC_SIPINFO_OBJID;
    const char *digest_oid = info ? info->DigestAlgorithm.pszObjId : NULL;
    BYTE digest[64];
    DWORD digest_len = sizeof(digest), needed, oid_len;
    struct script s;
    ALG_ID alg;
    BYTE *p;

    TRACE("%p, %p, %p\n", info, size, data);

    if (!info || info->cbSize != sizeof(*info) || !size)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!digest_oid || !(alg = alg_from_oid(digest_oid)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    oid_len = strlen(digest_oid) + 1;

    needed = sizeof(*data) + sizeof(data_oid) + sizeof(sip_info_head) + sizeof(GUID) + sizeof(sip_info_tail) +
             oid_len + sizeof(null_params);
    if (!load_script(info, &s)) return FALSE;
    if (!hash_content(&s, alg, digest, &digest_len))
    {
        free_script(&s);
        return FALSE;
    }
    free_script(&s);
    needed += digest_len;

    if (!data)
    {
        *size = needed;
        return TRUE;
    }
    if (*size < needed)
    {
        *size = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    memset(data, 0, sizeof(*data));
    p = (BYTE *)(data + 1);
    memcpy(p, data_oid, sizeof(data_oid));
    data->Data.pszObjId = (char *)p;
    p += sizeof(data_oid);
    data->Data.Value.pbData = p;
    memcpy(p, sip_info_head, sizeof(sip_info_head));
    p += sizeof(sip_info_head);
    memcpy(p, &pwsh_subject, sizeof(GUID));
    p += sizeof(GUID);
    memcpy(p, sip_info_tail, sizeof(sip_info_tail));
    p += sizeof(sip_info_tail);
    data->Data.Value.cbData = p - data->Data.Value.pbData;
    memcpy(p, digest_oid, oid_len);
    data->DigestAlgorithm.pszObjId = (char *)p;
    p += oid_len;
    memcpy(p, null_params, sizeof(null_params));
    data->DigestAlgorithm.Parameters.pbData = p;
    data->DigestAlgorithm.Parameters.cbData = sizeof(null_params);
    p += sizeof(null_params);
    memcpy(p, digest, digest_len);
    data->Digest.pbData = p;
    data->Digest.cbData = digest_len;
    *size = needed;
    return TRUE;
}

/***********************************************************************
 *              PwrShSIPVerifyIndirectData (PWRSHSIP.@)
 */
BOOL WINAPI PwrShSIPVerifyIndirectData(SIP_SUBJECTINFO *info, SIP_INDIRECT_DATA *data)
{
    BYTE digest[64];
    DWORD len = sizeof(digest);
    struct script s;
    BOOL ret;

    TRACE("%p, %p\n", info, data);

    if (!info || info->cbSize != sizeof(*info) || !data || !data->DigestAlgorithm.pszObjId)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!load_script(info, &s)) return FALSE;
    ret = hash_content(&s, alg_from_oid(data->DigestAlgorithm.pszObjId), digest, &len);
    free_script(&s);
    if (!ret) return FALSE;
    if (len != data->Digest.cbData || memcmp(digest, data->Digest.pbData, len))
    {
        SetLastError(TRUST_E_BAD_DIGEST);
        return FALSE;
    }
    return TRUE;
}

/***********************************************************************
 *              DllRegisterServer (PWRSHSIP.@)
 */
HRESULT WINAPI DllRegisterServer(void)
{
    static WCHAR dll[] = L"PWRSHSIP.DLL";
    static WCHAR get[] = L"PwrShSIPGetSignedDataMsg";
    static WCHAR put[] = L"PwrShSIPPutSignedDataMsg";
    static WCHAR create[] = L"PwrShSIPCreateIndirectData";
    static WCHAR verify[] = L"PwrShSIPVerifyIndirectData";
    static WCHAR remove[] = L"PwrShSIPRemoveSignedDataMsg";
    static WCHAR is_my_type[] = L"PwrShSIPIsFileSupportedName";
    SIP_ADD_NEWPROVIDER prov;

    memset(&prov, 0, sizeof(prov));
    prov.cbStruct = sizeof(prov);
    prov.pwszDLLFileName = dll;
    prov.pgSubject = &pwsh_subject;
    prov.pwszGetFuncName = get;
    prov.pwszPutFuncName = put;
    prov.pwszCreateFuncName = create;
    prov.pwszVerifyFuncName = verify;
    prov.pwszRemoveFuncName = remove;
    prov.pwszIsFunctionNameFmt2 = is_my_type;
    return CryptSIPAddProvider(&prov) ? S_OK : S_FALSE;
}

/***********************************************************************
 *              DllUnregisterServer (PWRSHSIP.@)
 */
HRESULT WINAPI DllUnregisterServer(void)
{
    CryptSIPRemoveProvider(&pwsh_subject);
    return S_OK;
}
