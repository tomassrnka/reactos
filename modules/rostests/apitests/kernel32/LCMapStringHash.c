/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for LCMapStringEx(LCMAP_HASH) (Windows Vista and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The .NET runtime without ICU hashes every culture-aware string with LCMapStringEx(LCMAP_HASH | flags), 32-bit
 * output. The hash values are process-local; the contract tested here was taken from Windows 10 22H2: the
 * result is the hash and is stored in the 4-byte destination, strings that compare equal under the flags hash
 * equal, and the parameter and flag errors.
 *
 * .NET 8 (NLS mode) hashes culture-aware strings through LCMAP_SORTKEY instead: it asks for the size of the key
 * with a NULL destination, then fills a buffer of that size and throws unless both calls return the same length.
 * TestSortKeyLength covers that.
 */

#include "precomp.h"

#ifndef LCMAP_HASH
#define LCMAP_HASH 0x00040000
#endif
#ifndef LCMAP_SORTHANDLE
#define LCMAP_SORTHANDLE 0x20000000
#endif
#ifndef LINGUISTIC_IGNORECASE
#define LINGUISTIC_IGNORECASE 0x00000010
#endif
#ifndef LINGUISTIC_IGNOREDIACRITIC
#define LINGUISTIC_IGNOREDIACRITIC 0x00000020
#endif
#ifndef NORM_LINGUISTIC_CASING
#define NORM_LINGUISTIC_CASING 0x08000000
#endif
#ifndef SORT_DIGITSASNUMBERS
#define SORT_DIGITSASNUMBERS 0x00000008
#endif

#define PENDING 0xdeadbeefU

typedef INT (WINAPI *PFN_LCMapStringEx)(LPCWSTR, DWORD, LPCWSTR, INT, LPWSTR, INT, LPNLSVERSIONINFO, LPVOID,
                                        LPARAM);

static PFN_LCMapStringEx pLCMapStringEx;

/* Hashes the string; returns the function result, the stored value in *Stored and the last error in *Error */
static INT HashRaw(LPCWSTR Locale, DWORD Flags, LPCWSTR Str, INT Len, UINT *Stored, DWORD *Error)
{
    INT Result;

    *Stored = PENDING;
    SetLastError(0xdeadbeef);
    Result = pLCMapStringEx(Locale, LCMAP_HASH | Flags, Str, Len, (LPWSTR)Stored, 4, NULL, NULL, 0);
    *Error = GetLastError();
    return Result;
}

static UINT Hash(LPCWSTR Locale, DWORD Flags, LPCWSTR Str, int Line)
{
    UINT Stored;
    DWORD Error;
    INT Result = HashRaw(Locale, Flags, Str, -1, &Stored, &Error);

    ok(Result != 0, "line %d: hash of %s failed, error %lu\n", Line, wine_dbgstr_w(Str), Error);
    ok((UINT)Result == Stored, "line %d: result %08x, stored %08x\n", Line, (UINT)Result, Stored);
    return Stored;
}

static void CheckError(LPCWSTR Locale, DWORD Flags, LPCWSTR Str, INT Len, PVOID Dst, INT DstLen,
                       DWORD ExpectedError, int Line)
{
    UINT Buffer = PENDING;
    INT Result;
    DWORD Error;

    SetLastError(0xdeadbeef);
    Result = pLCMapStringEx(Locale, Flags, Str, Len, Dst ? (LPWSTR)&Buffer : NULL, DstLen, NULL, NULL, 0);
    Error = GetLastError();
    (void)Dst;
    ok(Result == 0, "line %d: flags %08lx cch %d: result %d, expected 0\n", Line, Flags, DstLen, Result);
    ok(Error == ExpectedError, "line %d: flags %08lx cch %d: error %lu, expected %lu\n", Line, Flags, DstLen,
       Error, ExpectedError);
    ok(Buffer == PENDING, "line %d: flags %08lx: destination written (%08x)\n", Line, Flags, Buffer);
}

#define ERR(loc, fl, str, len, dst, dl, e) CheckError(loc, fl, str, len, dst, dl, e, __LINE__)

static void TestErrors(void)
{
    static const DWORD InvalidFlags[] =
    {
        LCMAP_LOWERCASE, LCMAP_UPPERCASE, LCMAP_SORTKEY, LCMAP_BYTEREV, LCMAP_HIRAGANA, LCMAP_KATAKANA,
        LCMAP_HALFWIDTH, LCMAP_FULLWIDTH, LCMAP_SIMPLIFIED_CHINESE, LCMAP_TRADITIONAL_CHINESE,
        0x40, 0x80, 0x2000, 0x4000, 0x8000, 0x01000000, 0x80000000, NORM_IGNORECASE | LCMAP_LOWERCASE,
        LCMAP_SORTKEY | SORT_STRINGSORT
    };
    static const INT BadSizes[] = { -1, 0, 1, 2, 3, 5, 8, 16 };
    UINT Dummy[4], Stored;
    DWORD Error;
    INT Result;
    WCHAR InPlace[8] = L"Abc";
    unsigned i;

    /* the destination must be exactly 4 bytes */
    for (i = 0; i < _countof(BadSizes); i++)
        ERR(L"en-US", LCMAP_HASH, L"Abc", 3, Dummy, BadSizes[i], ERROR_INVALID_PARAMETER);
    ERR(L"en-US", LCMAP_HASH, L"Abc", 3, NULL, 4, ERROR_INVALID_PARAMETER);
    ERR(L"en-US", LCMAP_HASH, L"Abc", 3, NULL, 0, ERROR_INVALID_PARAMETER);

    /* source */
    ERR(L"en-US", LCMAP_HASH, NULL, 3, Dummy, 4, ERROR_INVALID_PARAMETER);
    ERR(L"en-US", LCMAP_HASH, L"Abc", 0, Dummy, 4, ERROR_INVALID_PARAMETER);

    /* locale, SORTHANDLE */
    ERR(L"zz-INVALID", LCMAP_HASH, L"Abc", 3, Dummy, 4, ERROR_INVALID_PARAMETER);
    ERR(L"en-US", LCMAP_HASH | LCMAP_SORTHANDLE, L"Abc", 3, Dummy, 4, ERROR_INVALID_PARAMETER);

    /* in place */
    SetLastError(0xdeadbeef);
    Result = pLCMapStringEx(L"en-US", LCMAP_HASH, InPlace, 3, InPlace, 4, NULL, NULL, 0);
    ok(Result == 0, "in place: result %d\n", Result);
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "in place: error %lu\n", GetLastError());

    /* flags that cannot be combined with the hash */
    for (i = 0; i < _countof(InvalidFlags); i++)
        ERR(L"en-US", LCMAP_HASH | InvalidFlags[i], L"Abc", 3, Dummy, 4, ERROR_INVALID_FLAGS);

    /* parameters are checked before the flags and the locale */
    ERR(L"en-US", LCMAP_HASH | LCMAP_LOWERCASE, L"Abc", 3, Dummy, 0, ERROR_INVALID_PARAMETER);
    ERR(L"en-US", LCMAP_HASH | LCMAP_LOWERCASE, L"Abc", 3, NULL, 4, ERROR_INVALID_PARAMETER);
    ERR(L"en-US", LCMAP_HASH | LCMAP_LOWERCASE, L"Abc", 0, Dummy, 4, ERROR_INVALID_PARAMETER);
    ERR(L"zz-INVALID", LCMAP_HASH | LCMAP_LOWERCASE, L"Abc", 3, Dummy, 4, ERROR_INVALID_PARAMETER);
    ERR(L"zz-INVALID", LCMAP_HASH, L"Abc", 3, Dummy, 0, ERROR_INVALID_PARAMETER);
    ERR(L"en-US", LCMAP_HASH | LCMAP_SORTHANDLE | LCMAP_LOWERCASE, L"Abc", 3, Dummy, 4, ERROR_INVALID_PARAMETER);

    /* every valid flag is accepted, alone and together */
    {
        static const DWORD ValidFlags[] =
        {
            0, NORM_IGNORECASE, NORM_IGNORENONSPACE, NORM_IGNORESYMBOLS, SORT_DIGITSASNUMBERS,
            LINGUISTIC_IGNORECASE, LINGUISTIC_IGNOREDIACRITIC, SORT_STRINGSORT, NORM_IGNOREKANATYPE,
            NORM_IGNOREWIDTH, NORM_LINGUISTIC_CASING, LOCALE_USE_CP_ACP,
            NORM_IGNORECASE | NORM_IGNORENONSPACE | NORM_IGNORESYMBOLS | NORM_IGNOREKANATYPE | NORM_IGNOREWIDTH |
                SORT_STRINGSORT | NORM_LINGUISTIC_CASING,
            LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC | NORM_LINGUISTIC_CASING
        };

        for (i = 0; i < _countof(ValidFlags); i++)
        {
            Result = HashRaw(L"en-US", ValidFlags[i], L"Abc", 3, &Stored, &Error);
            ok(Result != 0, "flags %08lx: failed, error %lu\n", ValidFlags[i], Error);
            ok((UINT)Result == Stored, "flags %08lx: result %08x, stored %08x\n", ValidFlags[i], (UINT)Result, Stored);
        }
    }
}

static void TestContract(void)
{
    static const WCHAR Embedded[] = L"Abc\0z";
    static const WCHAR Trailing[] = L"Abc\0";
    static const LPCWSTR Locales[] = { NULL, L"", L"en-US", L"tr-TR", L"ja-JP", L"de-DE" };
    UINT H1, H2, Stored;
    DWORD Error;
    INT Result;
    unsigned i;

    /* the result and the destination agree, for every way of naming the locale */
    for (i = 0; i < _countof(Locales); i++)
    {
        H1 = Hash(Locales[i], 0, L"Abc", __LINE__);
        H2 = Hash(Locales[i], 0, L"Abc", __LINE__);
        ok(H1 == H2, "locale %s: not deterministic, %08x and %08x\n", wine_dbgstr_w(Locales[i]), H1, H2);
        ok(H1 == Hash(L"en-US", 0, L"Abc", __LINE__), "locale %s: other hash %08x\n",
           wine_dbgstr_w(Locales[i]), H1);
        H1 = Hash(Locales[i], NORM_IGNORECASE, L"Abc", __LINE__);
        H2 = Hash(Locales[i], NORM_IGNORECASE, L"aBC", __LINE__);
        ok(H1 == H2, "locale %s: ignore case %08x vs %08x\n", wine_dbgstr_w(Locales[i]), H1, H2);
    }

    /* the flags change the hash of a string they change */
    ok(Hash(L"en-US", 0, L"Abc", __LINE__) != Hash(L"en-US", NORM_IGNORECASE, L"Abc", __LINE__),
       "ignore case did not change the hash\n");

    /* length: -1 is the length up to the terminator, a trailing NUL is ignored, an embedded one is not */
    Result = HashRaw(L"en-US", 0, L"Abc", 3, &H1, &Error);
    ok(Result != 0 && (UINT)Result == H1, "len 3: %08x %08x error %lu\n", (UINT)Result, H1, Error);
    Result = HashRaw(L"en-US", 0, L"Abc", -1, &H2, &Error);
    ok(Result != 0 && H1 == H2, "len -1: %08x vs %08x\n", H2, H1);
    Result = HashRaw(L"en-US", 0, L"Abc", -2, &H2, &Error);
    ok(Result != 0 && H1 == H2, "len -2: %08x vs %08x\n", H2, H1);
    Result = HashRaw(L"en-US", 0, Trailing, 4, &H2, &Error);
    ok(Result != 0 && H1 == H2, "trailing NUL: %08x vs %08x\n", H2, H1);
    Result = HashRaw(L"en-US", 0, Embedded, 5, &H2, &Error);
    ok(Result != 0 && H1 != H2, "embedded NUL: %08x vs %08x\n", H2, H1);
    Result = HashRaw(L"en-US", 0, L"Abc", 2, &H2, &Error);
    ok(Result != 0 && H1 != H2, "len 2: %08x vs %08x\n", H2, H1);

    /* the empty string has a hash (length -1), unlike length 0 */
    Result = HashRaw(L"en-US", 0, L"", -1, &Stored, &Error);
    ok(Result != 0 && (UINT)Result == Stored, "empty: %08x %08x error %lu\n", (UINT)Result, Stored, Error);

    /* only the first four bytes of a larger destination... are not accepted, see TestErrors; the neighbours of a
       4-byte destination stay untouched */
    {
        UINT Buffer[3] = { PENDING, PENDING, PENDING };

        Result = pLCMapStringEx(L"en-US", LCMAP_HASH, L"Abc", 3, (LPWSTR)&Buffer[1], 4, NULL, NULL, 0);
        ok(Result != 0 && (UINT)Result == Buffer[1], "got %08x, stored %08x\n", (UINT)Result, Buffer[1]);
        ok(Buffer[0] == PENDING && Buffer[2] == PENDING, "neighbours written: %08x %08x\n", Buffer[0], Buffer[2]);
    }
}

typedef struct
{
    LPCWSTR Locale;
    DWORD Flags;
    LPCWSTR A;
    LPCWSTR B;
} PAIR;

/* Compare equal on Windows under the flags */
static const PAIR EqualPairs[] =
{
    /* case */
    { L"en-US", NORM_IGNORECASE, L"Abc", L"aBC" },
    { L"en-US", NORM_IGNORECASE, L"\x00e9", L"\x00c9" },
    { L"en-US", NORM_IGNORECASE, L"\x03c3", L"\x03c2" },
    { L"en-US", NORM_IGNORECASE, L"\x03a3", L"\x03c3" },
    { L"en-US", NORM_IGNORECASE, L"i", L"I" },
    { L"en-US", LINGUISTIC_IGNORECASE, L"e", L"E" },
    { L"en-US", LINGUISTIC_IGNORECASE, L"The Quick Fox", L"tHE qUICK fOX" },
    { L"ja-JP", NORM_IGNORECASE, L"Abc", L"aBC" },
    /* width */
    { L"en-US", NORM_IGNOREWIDTH, L"A", L"\xff21" },
    { L"en-US", NORM_IGNOREWIDTH, L"abc", L"\xff41\xff42\xff43" },
    { L"en-US", NORM_IGNOREWIDTH, L"\xff01", L"!" },
    { L"en-US", NORM_IGNOREWIDTH, L"\x3000", L" " },
    { L"en-US", NORM_IGNOREWIDTH, L"\xff71", L"\x30a2" },
    { L"en-US", NORM_IGNOREWIDTH, L"\xffe0", L"\x00a2" },
    { L"en-US", NORM_IGNOREWIDTH | NORM_IGNORECASE, L"\xff21", L"a" },
    /* kana */
    { L"en-US", NORM_IGNOREKANATYPE, L"\x3042", L"\x30a2" },
    { L"en-US", NORM_IGNOREKANATYPE, L"\x3042\x3044", L"\x30a2\x30a4" },
    { L"en-US", NORM_IGNOREKANATYPE | NORM_IGNOREWIDTH, L"\xff71", L"\x3042" },
    { L"ja-JP", NORM_IGNOREKANATYPE | NORM_IGNOREWIDTH, L"\x30a2", L"\x3042" },
    /* nonspacing / diacritics */
    { L"en-US", NORM_IGNORENONSPACE, L"e", L"\x00e9" },
    { L"en-US", NORM_IGNORENONSPACE, L"resume", L"r\x00e9sum\x00e9" },
    { L"en-US", NORM_IGNORENONSPACE, L"e", L"e\x0301" },
    { L"en-US", NORM_IGNORENONSPACE, L"\x00e4", L"a" },
    { L"en-US", NORM_IGNORENONSPACE | NORM_IGNORECASE, L"\x00e4", L"A" },
    { L"en-US", LINGUISTIC_IGNOREDIACRITIC, L"e", L"\x00e9" },
    { L"en-US", LINGUISTIC_IGNOREDIACRITIC | NORM_IGNORECASE, L"\x00e4", L"A" },
    { L"en-US", LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC, L"e", L"\x00c9" },
    /* symbols */
    { L"en-US", NORM_IGNORESYMBOLS, L"a b", L"ab" },
    { L"en-US", NORM_IGNORESYMBOLS, L"a-b", L"ab" },
    { L"en-US", NORM_IGNORESYMBOLS, L"a,b.", L"ab" },
    { L"en-US", NORM_IGNORESYMBOLS, L"co-op", L"coop" },
    { L"en-US", NORM_IGNORESYMBOLS, L"a$b", L"ab" },
    { L"en-US", NORM_IGNORESYMBOLS, L"a+b", L"ab" },
    { L"en-US", NORM_IGNORESYMBOLS, L"a_b", L"ab" },
    { L"en-US", NORM_IGNORESYMBOLS, L"a#b", L"ab" },
    { L"en-US", NORM_IGNORESYMBOLS, L"a\tb", L"ab" },
    { L"en-US", NORM_IGNORESYMBOLS | NORM_IGNORECASE, L"A-B", L"ab" },
    { L"en-US", NORM_IGNORESYMBOLS | SORT_STRINGSORT, L"a-b", L"ab" },
    /* Turkish casing needs NORM_LINGUISTIC_CASING; without it tr-TR is the same as en-US */
    { L"tr-TR", NORM_IGNORECASE, L"i", L"I" },
    { L"tr-TR", LINGUISTIC_IGNORECASE, L"i", L"I" },
    { L"tr-TR", NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"i", L"\x0130" },
    { L"tr-TR", NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"\x0131", L"I" },
    { L"tr-TR", NORM_LINGUISTIC_CASING | LINGUISTIC_IGNORECASE, L"i", L"\x0130" },
    { L"tr-TR", NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"t\x0131tle", L"TITLE" },
    { L"tr-TR", NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"t\x0130tle", L"title" },
    { L"az-Latn-AZ", NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"i", L"\x0130" },
    { L"en-US", NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"i", L"I" },
    /* nothing to fold */
    { L"en-US", 0, L"ab", L"ab" },
    { L"tr-TR", 0, L"\x0131", L"\x0131" },
    { L"en-US", NORM_IGNORECASE | NORM_IGNORENONSPACE | NORM_IGNORESYMBOLS | NORM_IGNOREKANATYPE |
        NORM_IGNOREWIDTH, L"Caf\x00e9 - \xff21", L"CAFE a" },
};

/* Compare unequal on Windows, and the hashes differ there too */
static const PAIR UnequalPairs[] =
{
    { L"en-US", 0, L"Abc", L"aBC" },
    { L"en-US", 0, L"abc", L"abd" },
    { L"en-US", 0, L"abc", L"abcd" },
    { L"en-US", NORM_IGNORECASE, L"abc", L"abcd" },
    { L"en-US", NORM_IGNORECASE, L"abc", L"abd" },
    { L"en-US", 0, L"A", L"\xff21" },
    { L"en-US", 0, L"e", L"\x00e9" },
    { L"en-US", NORM_IGNORECASE, L"e", L"\x00e9" },
    { L"en-US", NORM_IGNORENONSPACE, L"e", L"E" },
    { L"en-US", LINGUISTIC_IGNOREDIACRITIC, L"e", L"E" },
    { L"en-US", LINGUISTIC_IGNORECASE, L"e", L"\x00c9" },
    { L"en-US", 0, L"co-op", L"coop" },
    { L"en-US", SORT_STRINGSORT, L"co-op", L"coop" },
    { L"en-US", NORM_IGNORECASE, L"co-op", L"COOP" },
    { L"en-US", 0, L"a b", L"ab" },
    { L"en-US", NORM_IGNORESYMBOLS, L"a1b", L"ab" },
    { L"en-US", NORM_IGNORESYMBOLS, L"a", L"b" },
    { L"en-US", 0, L"abc", L"abc\x200b" },
    { L"en-US", NORM_IGNOREWIDTH, L"a", L"b" },
    { L"en-US", NORM_IGNOREKANATYPE, L"\x3042", L"\x3044" },
    { L"en-US", NORM_IGNORECASE, L"a2", L"a02" },
    { L"en-US", SORT_DIGITSASNUMBERS, L"a2", L"a02" },
    /* Turkish dotted and dotless i */
    { L"tr-TR", NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"i", L"I" },
    { L"tr-TR", NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"\x0131", L"i" },
    { L"tr-TR", NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"\x0130", L"I" },
    { L"tr-TR", NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"TITLE", L"t\x0130tle" },
    { L"tr-TR", NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"TITLE", L"title" },
    { L"tr-TR", NORM_LINGUISTIC_CASING | LINGUISTIC_IGNORECASE, L"i", L"I" },
    { L"tr-TR", NORM_LINGUISTIC_CASING, L"i", L"\x0130" },
    { L"tr-TR", NORM_LINGUISTIC_CASING, L"\x0131", L"I" },
    { L"tr-TR", NORM_IGNORECASE, L"i", L"\x0130" },
    { L"tr-TR", NORM_IGNORECASE, L"\x0131", L"I" },
    { L"tr-TR", NORM_IGNORECASE, L"\x0131", L"i" },
    { L"tr-TR", 0, L"i", L"I" },
    { L"en-US", NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"i", L"\x0130" },
    { L"en-US", NORM_IGNORECASE, L"i", L"\x0130" },
};

static void TestPairs(void)
{
    unsigned i;
    UINT H1, H2;

    for (i = 0; i < _countof(EqualPairs); i++)
    {
        const PAIR *p = &EqualPairs[i];

        H1 = Hash(p->Locale, p->Flags, p->A, __LINE__);
        H2 = Hash(p->Locale, p->Flags, p->B, __LINE__);
        ok(H1 == H2, "equal pair %u (%s, flags %08lx, %s / %s): %08x vs %08x\n", i, wine_dbgstr_w(p->Locale),
           p->Flags, wine_dbgstr_w(p->A), wine_dbgstr_w(p->B), H1, H2);
    }
    for (i = 0; i < _countof(UnequalPairs); i++)
    {
        const PAIR *p = &UnequalPairs[i];

        H1 = Hash(p->Locale, p->Flags, p->A, __LINE__);
        H2 = Hash(p->Locale, p->Flags, p->B, __LINE__);
        ok(H1 != H2, "unequal pair %u (%s, flags %08lx, %s / %s): both %08x\n", i, wine_dbgstr_w(p->Locale),
           p->Flags, wine_dbgstr_w(p->A), wine_dbgstr_w(p->B), H1);
    }
}

/* Distinct strings under every flag set that keeps them distinct hash apart: no collisions in a sample */
static void TestSample(void)
{
    static const DWORD FlagSets[] = { 0, NORM_IGNORECASE, NORM_IGNORENONSPACE, SORT_STRINGSORT,
                                      NORM_IGNOREWIDTH | NORM_IGNOREKANATYPE };
    WCHAR Strings[64][8];
    UINT Hashes[64];
    unsigned f, i, j, n;

    for (f = 0; f < _countof(FlagSets); f++)
    {
        n = 0;
        for (i = 0; i < 26; i++)
        {
            Strings[n][0] = L'a' + i; Strings[n][1] = 0; n++;
        }
        for (i = 0; i < 10; i++)
        {
            Strings[n][0] = L'0' + i; Strings[n][1] = 0; n++;
        }
        for (i = 0; i < 12; i++)
        {
            /* words that share a prefix, a suffix, or the letters in another order */
            Strings[n][0] = L'a' + i; Strings[n][1] = L'x'; Strings[n][2] = L'y'; Strings[n][3] = 0; n++;
        }
        for (i = 0; i < 12; i++)
        {
            Strings[n][0] = L'x'; Strings[n][1] = L'y'; Strings[n][2] = L'a' + i; Strings[n][3] = 0; n++;
        }
        for (i = 0; i < n; i++)
            Hashes[i] = Hash(L"en-US", FlagSets[f], Strings[i], __LINE__);
        for (i = 0; i < n; i++)
            for (j = i + 1; j < n; j++)
                ok(Hashes[i] != Hashes[j], "flags %08lx: %s and %s both hash to %08x\n", FlagSets[f],
                   wine_dbgstr_w(Strings[i]), wine_dbgstr_w(Strings[j]), Hashes[i]);
    }

    /* long strings: heap path, last character, order */
    {
        WCHAR Long1[600], Long2[600];

        for (i = 0; i < 599; i++)
            Long1[i] = Long2[i] = L'a' + (i % 26);
        Long1[599] = Long2[599] = 0;
        Long2[598] = L'Z';
        ok(Hash(L"en-US", 0, Long1, __LINE__) == Hash(L"en-US", 0, Long1, __LINE__), "long string not deterministic\n");
        ok(Hash(L"en-US", 0, Long1, __LINE__) != Hash(L"en-US", 0, Long2, __LINE__), "long strings, last char differs\n");
        Long2[598] = Long1[598] ^ 0x20;
        ok(Hash(L"en-US", NORM_IGNORECASE, Long1, __LINE__) == Hash(L"en-US", NORM_IGNORECASE, Long2, __LINE__),
           "long strings, ignore case\n");
    }
    ok(Hash(L"en-US", 0, L"ab", __LINE__) != Hash(L"en-US", 0, L"ba", __LINE__), "order\n");
}

/* LCMAP_SORTKEY as .NET uses it: the size query and the fill return the same length, equal strings give equal keys */
static void TestSortKeyLength(void)
{
    static const DWORD FlagSets[] =
    {
        0, NORM_IGNORECASE, NORM_LINGUISTIC_CASING, NORM_LINGUISTIC_CASING | NORM_IGNORECASE,
        NORM_LINGUISTIC_CASING | NORM_IGNORECASE | SORT_STRINGSORT, NORM_IGNORENONSPACE, NORM_IGNORESYMBOLS,
        NORM_IGNOREKANATYPE | NORM_IGNOREWIDTH,
        NORM_LINGUISTIC_CASING | NORM_IGNORECASE | NORM_IGNORENONSPACE | NORM_IGNORESYMBOLS | NORM_IGNOREKANATYPE |
            NORM_IGNOREWIDTH | SORT_STRINGSORT
    };
    static const LPCWSTR Strings[] = { L"Abc", L"The quick brown fox", L"\x00e9t\x00e9", L"x" };
    BYTE Key[1024], Key2[1024];
    unsigned f, i;
    INT Query, Fill, Short;
    DWORD Error;

    for (f = 0; f < _countof(FlagSets); f++)
    {
        for (i = 0; i < _countof(Strings); i++)
        {
            SetLastError(0xdeadbeef);
            Query = pLCMapStringEx(L"en-US", LCMAP_SORTKEY | FlagSets[f], Strings[i], -1, NULL, 0, NULL, NULL, 0);
            ok(Query > 0, "flags %08lx %s: size query %d, error %lu\n", FlagSets[f], wine_dbgstr_w(Strings[i]),
               Query, GetLastError());
            if (Query <= 0 || Query > (INT)sizeof(Key)) continue;

            memset(Key, 0xcc, sizeof(Key));
            Fill = pLCMapStringEx(L"en-US", LCMAP_SORTKEY | FlagSets[f], Strings[i], -1, (LPWSTR)Key, sizeof(Key),
                                  NULL, NULL, 0);
            ok(Fill == Query, "flags %08lx %s: query %d, fill %d\n", FlagSets[f], wine_dbgstr_w(Strings[i]), Query,
               Fill);

            /* a buffer of exactly the queried size is enough, one byte less is not */
            memset(Key2, 0xcc, sizeof(Key2));
            Fill = pLCMapStringEx(L"en-US", LCMAP_SORTKEY | FlagSets[f], Strings[i], -1, (LPWSTR)Key2, Query,
                                  NULL, NULL, 0);
            ok(Fill == Query, "flags %08lx %s: exact buffer %d, expected %d\n", FlagSets[f],
               wine_dbgstr_w(Strings[i]), Fill, Query);
            ok(memcmp(Key, Key2, Query) == 0, "flags %08lx %s: keys differ\n", FlagSets[f],
               wine_dbgstr_w(Strings[i]));
            SetLastError(0xdeadbeef);
            Short = pLCMapStringEx(L"en-US", LCMAP_SORTKEY | FlagSets[f], Strings[i], -1, (LPWSTR)Key2, Query - 1,
                                   NULL, NULL, 0);
            Error = GetLastError();
            ok(Short == 0, "flags %08lx %s: short buffer returned %d\n", FlagSets[f], wine_dbgstr_w(Strings[i]),
               Short);
            ok(Error == ERROR_INSUFFICIENT_BUFFER, "flags %08lx %s: short buffer error %lu\n", FlagSets[f],
               wine_dbgstr_w(Strings[i]), Error);
        }
    }

    /* equal strings, equal keys: the property the hash of .NET relies on */
    {
        INT L1, L2;

        L1 = pLCMapStringEx(L"en-US", LCMAP_SORTKEY | NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"Abc", -1,
                            (LPWSTR)Key, sizeof(Key), NULL, NULL, 0);
        L2 = pLCMapStringEx(L"en-US", LCMAP_SORTKEY | NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"aBC", 3,
                            (LPWSTR)Key2, sizeof(Key2), NULL, NULL, 0);
        ok(L1 > 0 && L1 == L2 && memcmp(Key, Key2, L1) == 0, "ignore case keys: %d, %d\n", L1, L2);
        L2 = pLCMapStringEx(L"en-US", LCMAP_SORTKEY | NORM_LINGUISTIC_CASING | NORM_IGNORECASE, L"Abd", -1,
                            (LPWSTR)Key2, sizeof(Key2), NULL, NULL, 0);
        ok(L2 > 0 && (L1 != L2 || memcmp(Key, Key2, L1) != 0), "different strings, same key\n");
    }
}

START_TEST(LCMapStringHash)
{
    pLCMapStringEx = (PVOID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LCMapStringEx");
    if (!pLCMapStringEx)
    {
        skip("LCMapStringEx is not available\n");
        return;
    }

    TestErrors();
    TestContract();
    TestPairs();
    TestSample();
    TestSortKeyLength();
}
