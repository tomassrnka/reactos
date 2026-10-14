/*
 * PROJECT:     ReactOS propsys
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     VariantCompare
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Every rule below was measured on Windows Server 2008 R2 and Windows 10 (x86 and x64) by running
 * VariantCompare on grids of variants; none of it comes from Windows code.
 * The digit-run key of the string comparison follows the layout of the SORT_DIGITSASNUMBERS sort key
 * that Wine builds in dlls/kernelbase/locale.c (LGPL-2.1-or-later, Copyright Alexandre Julliard):
 * runs of at most 19 digits, each with a count, the digits and a leading-zero tie break.
 */

#include "precomp.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "winreg.h"
#include "winuser.h"
#include "shlobj.h"
#include "propvarutil.h"

WINE_DEFAULT_DEBUG_CHANNEL(propsys);

enum cmp_kind
{
    CMP_EMPTY,      /* VT_EMPTY, VT_NULL */
    CMP_OTHER,      /* every type that is "equal" to everything but empty */
    CMP_INT,
    CMP_REAL,
    CMP_CY,
    CMP_DEC,
    CMP_STRING,
    CMP_ARRAY
};

struct cmp_value
{
    enum cmp_kind kind;
    VARTYPE vt;
    BOOL negative;          /* CMP_INT: sign and magnitude */
    ULONGLONG magnitude;
    double real;            /* CMP_REAL: also VT_DATE */
    LONGLONG cy;
    DECIMAL dec;
    const WCHAR *str;
    SAFEARRAY *array;       /* CMP_ARRAY: NULL is an empty array */
};

static int cmp_values(const struct cmp_value *a, const struct cmp_value *b);

static int cmp_u64(ULONGLONG a, ULONGLONG b)
{
    return a < b ? -1 : a > b;
}

static void cmp_set_signed(struct cmp_value *val, LONGLONG n)
{
    val->kind = CMP_INT;
    val->negative = n < 0;
    val->magnitude = n < 0 ? 0 - (ULONGLONG)n : (ULONGLONG)n;
}

static void cmp_set_unsigned(struct cmp_value *val, ULONGLONG n)
{
    val->kind = CMP_INT;
    val->negative = FALSE;
    val->magnitude = n;
}

static void cmp_set_real(struct cmp_value *val, double d)
{
    val->kind = CMP_REAL;
    val->real = d;
}

/* An array of one of these element types is compared, an array of any other type is "equal" */
static UINT cmp_element_size(VARTYPE vt)
{
    switch (vt)
    {
    case VT_I1:
    case VT_UI1:
        return 1;
    case VT_I2:
    case VT_UI2:
    case VT_BOOL:
        return 2;
    case VT_I4:
    case VT_UI4:
    case VT_R4:
        return 4;
    case VT_I8:
    case VT_UI8:
    case VT_R8:
    case VT_DATE:
        return 8;
    case VT_BSTR:
        return sizeof(BSTR);
    default:
        return 0;
    }
}

/* VT_INT is a 32 bit integer as the left operand and not compared as the right one */
static void cmp_classify(const VARIANT *var, struct cmp_value *val, BOOL right)
{
    VARTYPE vt = V_VT(var);

    memset(val, 0, sizeof(*val));
    val->vt = vt;
    val->kind = CMP_OTHER;

    /* A reference to an array behaves as an empty array */
    if ((vt & ~(VT_BYREF | VT_TYPEMASK)) == VT_ARRAY)
    {
        val->kind = CMP_ARRAY;
        if (!(vt & VT_BYREF))
            val->array = V_ARRAY(var);
        return;
    }
    if (vt & ~VT_TYPEMASK)
        return;

    switch (vt)
    {
    case VT_EMPTY:
    case VT_NULL:
        val->kind = CMP_EMPTY;
        break;
    case VT_I1:
        cmp_set_signed(val, V_I1(var));
        break;
    case VT_ERROR:
        cmp_set_signed(val, V_ERROR(var));
        break;
    case VT_UI1:
        cmp_set_unsigned(val, V_UI1(var));
        break;
    case VT_I2:
        cmp_set_signed(val, V_I2(var));
        break;
    case VT_BOOL:
        cmp_set_signed(val, V_BOOL(var));
        break;
    case VT_UI2:
        cmp_set_unsigned(val, V_UI2(var));
        break;
    case VT_I4:
        cmp_set_signed(val, V_I4(var));
        break;
    case VT_INT:
        if (!right)
            cmp_set_signed(val, V_INT(var));
        break;
    case VT_UI4:
        cmp_set_unsigned(val, V_UI4(var));
        break;
    case VT_I8:
        cmp_set_signed(val, V_I8(var));
        break;
    case VT_UI8:
        cmp_set_unsigned(val, V_UI8(var));
        break;
    case VT_R4:
        cmp_set_real(val, V_R4(var));
        break;
    case VT_R8:
        cmp_set_real(val, V_R8(var));
        break;
    case VT_DATE:
        cmp_set_real(val, V_DATE(var));
        break;
    case VT_CY:
        val->kind = CMP_CY;
        val->cy = V_CY(var).int64;
        break;
    case VT_DECIMAL:
        val->kind = CMP_DEC;
        val->dec = V_DECIMAL(var);
        break;
    case VT_BSTR:
        val->kind = CMP_STRING;
        val->str = V_BSTR(var) ? V_BSTR(var) : L"";
        break;
    }
}

/* ---- integers and reals ---- */

static int cmp_ints(const struct cmp_value *a, const struct cmp_value *b)
{
    if (a->negative != b->negative)
        return a->negative ? -1 : 1;
    if (a->negative)
        return cmp_u64(b->magnitude, a->magnitude);
    return cmp_u64(a->magnitude, b->magnitude);
}

/* A boolean against a byte counts as the unsigned value of its low byte */
static void cmp_bool_as_byte(struct cmp_value *val)
{
    cmp_set_unsigned(val, (BYTE)(val->negative ? 0 - val->magnitude : val->magnitude));
}

/* The double is narrowed to a single, whatever its range, when the other operand is a single */
static int cmp_reals(const struct cmp_value *a, const struct cmp_value *b)
{
    double d1 = a->real, d2 = b->real;

    if ((a->vt == VT_R4) != (b->vt == VT_R4))
    {
        float f1 = (float)d1, f2 = (float)d2;

        d1 = f1;
        d2 = f2;
    }
    if (d1 < d2)
        return -1;
    return d1 > d2;
}

#ifdef _M_IX86
/* Windows Server 2008 R2 on x86 compares the integer with the exact value of the real */
static int cmp_int_real_exact(const struct cmp_value *val, double real)
{
    double magnitude = real < 0 ? -real : real;
    ULONGLONG whole;
    int ret;

    if (real == 0)
        return val->magnitude ? (val->negative ? -1 : 1) : 0;
    if (real < 0 && !val->negative)
        return 1;
    if (real > 0 && val->negative)
        return -1;

    if (magnitude >= 18446744073709551616.0)
        ret = -1;
    else
    {
        whole = (ULONGLONG)magnitude;
        if (val->magnitude != whole)
            ret = val->magnitude > whole ? 1 : -1;
        else
            ret = magnitude > (double)whole ? -1 : 0;
    }
    return val->negative ? -ret : ret;
}
#endif

/* An integer against a real (a double, a single or a date) */
static int cmp_int_real(const struct cmp_value *val, const struct cmp_value *real)
{
    double d = real->real;

    /* An unsigned 64 bit integer is compared with the raw bits of the real, a negative real is below it */
    if (val->vt == VT_UI8)
    {
        ULONGLONG bits;

        if (d < 0)
            return 1;
        memcpy(&bits, &d, sizeof(bits));
        return cmp_u64(val->magnitude, bits);
    }

    if (d != d)
        return 0;
#ifdef _M_IX86
    return cmp_int_real_exact(val, d);
#else
    {
        double i = val->negative ? -(double)val->magnitude : (double)val->magnitude;

        if (i < d)
            return -1;
        return i > d;
    }
#endif
}

/* ---- currency and decimal ---- */

/* value = mant * 10^-scale; mant is a 192 bit number, least significant word first */
struct cmp_fixed
{
    BOOL negative;
    ULONG mant[6];
    int scale;
};

static BOOL fixed_mul10(struct cmp_fixed *f)
{
    ULONGLONG carry = 0, t;
    int i;

    for (i = 0; i < 6; i++)
    {
        t = (ULONGLONG)f->mant[i] * 10 + carry;
        f->mant[i] = (ULONG)t;
        carry = t >> 32;
    }
    return carry != 0;
}

static BOOL fixed_is_zero(const struct cmp_fixed *f)
{
    int i;

    for (i = 0; i < 6; i++)
        if (f->mant[i])
            return FALSE;
    return TRUE;
}

static void fixed_set(struct cmp_fixed *f, BOOL negative, ULONGLONG lo, ULONG hi, int scale)
{
    memset(f, 0, sizeof(*f));
    f->negative = negative;
    f->mant[0] = (ULONG)lo;
    f->mant[1] = (ULONG)(lo >> 32);
    f->mant[2] = hi;
    f->scale = scale;
}

/*
 * A real is a decimal of its 15 significant digits. A real that does not fit in a decimal (2^96 and more), an
 * infinity and a NaN are above every decimal, or below it when negative; the largest number here stands for them.
 */
static void fixed_from_real(struct cmp_fixed *f, double real)
{
    char text[48];
    ULONGLONG digits;
    int i, exponent;

    fixed_set(f, real < 0, 0, 0, 0);
    if (real != real || real >= 79228162514264337593543950336.0 || real <= -79228162514264337593543950336.0)
    {
        memset(f->mant, 0xff, sizeof(f->mant));
        return;
    }
    if (real == 0)
        return;

    /* d.dddddddddddddde+xxx */
    sprintf(text, "%.14e", real < 0 ? -real : real);
    digits = text[0] - '0';
    for (i = 2; i <= 15; i++)
        digits = digits * 10 + (text[i] - '0');
    exponent = atoi(text + 17) - 14;

    /* A decimal has at most 28 digits after the point; the rest is cut off */
    for (; exponent < -28 && digits; exponent++)
        digits /= 10;
    fixed_set(f, real < 0, digits, 0, exponent < 0 ? -exponent : 0);
    for (; exponent > 0; exponent--)
        fixed_mul10(f);
}

/* An integer, a currency, a decimal or a real as a number of the form above */
static void cmp_to_fixed(const struct cmp_value *val, struct cmp_fixed *f)
{
    switch (val->kind)
    {
    case CMP_INT:
        fixed_set(f, val->negative, val->magnitude, 0, 0);
        break;
    case CMP_CY:
        fixed_set(f, val->cy < 0, val->cy < 0 ? 0 - (ULONGLONG)val->cy : (ULONGLONG)val->cy, 0, 4);
        break;
    case CMP_DEC:
        fixed_set(f, (val->dec.sign & DECIMAL_NEG) != 0, ((ULONGLONG)val->dec.Mid32 << 32) | val->dec.Lo32,
                  val->dec.Hi32, val->dec.scale);
        break;
    default:
        fixed_from_real(f, val->real);
        break;
    }
}

static int cmp_fixed_values(struct cmp_fixed a, struct cmp_fixed b)
{
    BOOL zero1 = fixed_is_zero(&a), zero2 = fixed_is_zero(&b);
    int i, ret;

    if (zero1 || zero2)
    {
        if (zero1 && zero2)
            return 0;
        if (zero1)
            return b.negative ? 1 : -1;
        return a.negative ? -1 : 1;
    }
    if (a.negative != b.negative)
        return a.negative ? -1 : 1;

    /* Same scale: a number that does not fit in the mantissa any more is the larger one */
    ret = 0;
    while (!ret && a.scale < b.scale)
    {
        ret = fixed_mul10(&a) ? 1 : 0;
        a.scale++;
    }
    while (!ret && b.scale < a.scale)
    {
        ret = fixed_mul10(&b) ? -1 : 0;
        b.scale++;
    }
    for (i = 5; i >= 0 && !ret; i--)
        ret = cmp_u64(a.mant[i], b.mant[i]);
    return a.negative ? -ret : ret;
}

/*
 * A real as a currency: the exact binary value times 10000, rounded half to even (the oleaut32 conversion rounds
 * differently on some compilers). FALSE when the result does not fit in a currency.
 */
static BOOL cmp_real_to_cy(double real, LONGLONG *cy)
{
    ULONGLONG mant, a, b, lo, hi, limit;
    BOOL half = FALSE, sticky = FALSE;
    int exponent, shift, i;
    double frac;

    if (real - real != 0)
        return FALSE;
    frac = frexp(real < 0 ? -real : real, &exponent);
    if (frac == 0)
    {
        *cy = 0;
        return TRUE;
    }

    /* The value is mant * 2^exponent with a 53 bit integer mant; from 2^52 on it is beyond the range of a currency */
    exponent -= 53;
    if (exponent >= 0)
        return FALSE;
    shift = -exponent;
    if (shift > 68)
    {
        *cy = 0;
        return TRUE;
    }

    /* mant * 10000 as the 128 bit number hi:lo, shifted right by shift bits */
    mant = (ULONGLONG)ldexp(frac, 53);
    a = (mant & 0xffffffff) * 10000;
    b = (mant >> 32) * 10000;
    lo = a + (b << 32);
    hi = (b >> 32) + (lo < a);
    for (i = 0; i < shift; i++)
    {
        sticky |= half;
        half = (lo & 1) != 0;
        lo = (lo >> 1) | (hi << 63);
        hi >>= 1;
    }
    if (hi)
        return FALSE;
    if (half && (sticky || (lo & 1)))
    {
        if (lo == ~(ULONGLONG)0)
            return FALSE;
        lo++;
    }

    limit = 0x7fffffffffffffffULL + (real < 0 ? 1 : 0);
    if (lo > limit)
        return FALSE;
    *cy = real < 0 ? (LONGLONG)(0 - lo) : (LONGLONG)lo;
    return TRUE;
}

/* A currency against a real: the real is rounded to the four digits of a currency */
static int cmp_cy_real(const struct cmp_value *cy, const struct cmp_value *real)
{
    LONGLONG rounded;

    if (real->real != real->real)
    {
        /* Windows x86 calls a NaN equal to a currency, Windows x64 puts it below */
#ifdef _M_IX86
        return 0;
#else
        return 1;
#endif
    }
    if (!cmp_real_to_cy(real->real, &rounded))
        return real->real > 0 ? -1 : 1;
    return cy->cy < rounded ? -1 : cy->cy > rounded;
}

static int cmp_numbers(struct cmp_value *a, struct cmp_value *b)
{
    struct cmp_fixed f1, f2;

    if (a->vt == VT_BOOL && b->vt == VT_UI1)
        cmp_bool_as_byte(a);
    else if (a->vt == VT_UI1 && b->vt == VT_BOOL)
        cmp_bool_as_byte(b);

    if (a->kind == CMP_INT && b->kind == CMP_INT)
        return cmp_ints(a, b);
    if (a->kind == CMP_REAL && b->kind == CMP_REAL)
        return cmp_reals(a, b);
    if (a->kind == CMP_REAL && b->kind == CMP_INT)
        return -cmp_int_real(b, a);
    if (a->kind == CMP_INT && b->kind == CMP_REAL)
        return cmp_int_real(a, b);
    if (a->kind == CMP_CY && b->kind == CMP_REAL)
        return cmp_cy_real(a, b);
    if (a->kind == CMP_REAL && b->kind == CMP_CY)
        return -cmp_cy_real(b, a);

    /* What is left is an integer, a currency or a real against a decimal, or an integer against a currency */
    cmp_to_fixed(a, &f1);
    cmp_to_fixed(b, &f2);
    return cmp_fixed_values(f1, f2);
}

/* ---- strings ---- */

/* The zero of the decimal digit block that holds the character, else 0 */
static WCHAR cmp_digit_zero(WCHAR ch)
{
    static const WCHAR zeroes[] =
    {
        0x0030, 0x0660, 0x06f0, 0x0966, 0x09e6, 0x0a66, 0x0ae6, 0x0b66, 0x0be6, 0x0c66,
        0x0ce6, 0x0d66, 0x0e50, 0x0ed0, 0x0f20, 0x1040, 0x1090, 0x17e0, 0x1810, 0x1946,
        0x1bb0, 0x1c40, 0x1c50, 0xa620, 0xa8d0, 0xa900, 0xaa50, 0xff10
    };
    UINT i;

    if (ch < 0x0030)
        return 0;
    if (ch <= 0x0039)
        return 0x0030;
    if (ch < 0x0660)
        return 0;
    for (i = 1; i < sizeof(zeroes) / sizeof(zeroes[0]); i++)
        if (ch >= zeroes[i] && ch <= zeroes[i] + 9)
            return zeroes[i];
    return 0;
}

static WCHAR *cmp_put_two(WCHAR *out, UINT n)
{
    *out++ = L'0' + n / 10;
    *out++ = L'0' + n % 10;
    return out;
}

/*
 * The text with every run of digits (of the same script, 19 at most) replaced by: its count of digits after the
 * leading zeros as two digits, those digits, and 99 minus its leading zeros as two digits. The ordinary string
 * comparison then orders numbers by value and, for equal values, the run with more leading zeros first,
 * at the place of the run and not at the end of the string.
 */
static WCHAR *cmp_make_key(const WCHAR *str, UINT len, UINT *key_len)
{
    WCHAR *key, *out;
    UINT i = 0, count, zeros, k;
    WCHAR zero;

    /* Five characters at most for a character of the text */
    if (len > (UINT)(INT_MAX / 5))
        return NULL;
    out = key = HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)len * 5 + 1) * sizeof(WCHAR));
    if (!key)
        return NULL;

    while (i < len)
    {
        zero = cmp_digit_zero(str[i]);
        if (!zero)
        {
            *out++ = str[i++];
            continue;
        }
        for (count = 1; count < 19 && i + count < len && str[i + count] >= zero && str[i + count] <= zero + 9; count++)
            ;
        for (zeros = 0; zeros < count && str[i + zeros] == zero; zeros++)
            ;
        out = cmp_put_two(out, count - zeros);
        for (k = zeros; k < count; k++)
            *out++ = L'0' + (str[i + k] - zero);
        out = cmp_put_two(out, 99 - zeros);
        i += count;
    }

    *key_len = out - key;
    return key;
}

/* Digits that are equal as numbers but written in different scripts: the first differing character decides */
static int cmp_digit_scripts(const WCHAR *s1, const WCHAR *s2)
{
    for (;;)
    {
        while (*s1 && !cmp_digit_zero(*s1)) s1++;
        while (*s2 && !cmp_digit_zero(*s2)) s2++;
        if (!*s1 || !*s2)
            return 0;
        if (*s1 != *s2)
            return *s1 < *s2 ? -1 : 1;
        s1++;
        s2++;
    }
}

/* Windows compares the text up to the first NUL, without regard to case, with digit runs as numbers */
static int cmp_strings(const struct cmp_value *a, const struct cmp_value *b)
{
    UINT len1 = lstrlenW(a->str), len2 = lstrlenW(b->str), key_len1 = 0, key_len2 = 0;
    WCHAR *key1 = cmp_make_key(a->str, len1, &key_len1);
    WCHAR *key2 = cmp_make_key(b->str, len2, &key_len2);
    int ret;

    /* Without memory for the keys the digits are compared as text */
    if (key1 && key2)
        ret = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, key1, key_len1, key2, key_len2);
    else
        ret = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a->str, len1, b->str, len2);
    HeapFree(GetProcessHeap(), 0, key1);
    HeapFree(GetProcessHeap(), 0, key2);

    if (ret && ret != CSTR_EQUAL)
        return ret - CSTR_EQUAL;
    return cmp_digit_scripts(a->str, b->str);
}

/* ---- arrays ---- */

static void cmp_element(VARTYPE vt, const void *data, struct cmp_value *val)
{
    VARIANT var;

    memset(&var, 0, sizeof(var));
    V_VT(&var) = vt;
    memcpy(&V_UI8(&var), data, cmp_element_size(vt));
    cmp_classify(&var, val, FALSE);
}

/* Elements are compared from the first one on only when the index 0 is inside the bounds of both arrays */
static BOOL cmp_array_has_index_zero(const SAFEARRAY *array)
{
    return array->rgsabound[0].lLbound <= 0 &&
           (LONGLONG)array->rgsabound[0].lLbound + array->rgsabound[0].cElements > 0;
}

/*
 * Arrays of the element types in cmp_element_size are compared element by element, each by the rules for single
 * values, and then by length. A multidimensional array sorts below a one-dimensional one, two of them are equal.
 */
static int cmp_arrays(const struct cmp_value *a, const struct cmp_value *b)
{
    VARTYPE vt1 = a->vt & VT_TYPEMASK, vt2 = b->vt & VT_TYPEMASK;
    UINT size1 = cmp_element_size(vt1), size2 = cmp_element_size(vt2), dims1 = 1, dims2 = 1;
    ULONG count1 = 0, count2 = 0, common, i;

    if (!size1 || !size2)
        return 0;

    if (a->array)
    {
        dims1 = a->array->cDims;
        count1 = a->array->rgsabound[0].cElements;
    }
    if (b->array)
    {
        dims2 = b->array->cDims;
        count2 = b->array->rgsabound[0].cElements;
    }
    if (dims1 != 1 || dims2 != 1)
    {
        if (dims1 != 1 && dims2 != 1)
            return 0;
        return dims1 != 1 ? -1 : 1;
    }

    common = count1 < count2 ? count1 : count2;
    if (common && cmp_array_has_index_zero(a->array) && cmp_array_has_index_zero(b->array))
    {
        if (a->array->cbElements != size1 || b->array->cbElements != size2 || !a->array->pvData || !b->array->pvData)
            return 0;
        for (i = 0; i < common; i++)
        {
            struct cmp_value e1, e2;
            int ret;

            cmp_element(vt1, (const BYTE *)a->array->pvData + (SIZE_T)i * size1, &e1);
            cmp_element(vt2, (const BYTE *)b->array->pvData + (SIZE_T)i * size2, &e2);
            ret = cmp_values(&e1, &e2);
            if (ret)
                return ret;
        }
    }

    return cmp_u64(count1, count2);
}

/* ---- all kinds ---- */

static BOOL cmp_is_wide_unsigned(const struct cmp_value *val)
{
    return val->vt == VT_UI2 || val->vt == VT_UI4 || val->vt == VT_UI8;
}

/*
 * An unsigned 16, 32 or 64 bit integer is "equal" to a string, a boolean, VT_INT, a currency and a decimal.
 * It is also "equal" to a single; the 16 and 32 bit ones are compared with a double or a date, the 64 bit one
 * with the raw bits of it.
 */
static BOOL cmp_is_unsigned_exception(const struct cmp_value *u, const struct cmp_value *other)
{
    if (!cmp_is_wide_unsigned(u))
        return FALSE;
    return other->kind == CMP_STRING || other->kind == CMP_CY || other->kind == CMP_DEC ||
           other->vt == VT_BOOL || other->vt == VT_INT || other->vt == VT_R4;
}

static int cmp_values(const struct cmp_value *a_in, const struct cmp_value *b_in)
{
    struct cmp_value a = *a_in, b = *b_in;

    if (a.kind == CMP_EMPTY)
        return b.kind == CMP_EMPTY ? 0 : -1;
    if (b.kind == CMP_EMPTY)
        return 1;
    if (a.kind == CMP_OTHER || b.kind == CMP_OTHER)
        return 0;

    /* VT_I1 and VT_ERROR are compared only with their own type */
    if ((a.vt == VT_I1 || b.vt == VT_I1 || a.vt == VT_ERROR || b.vt == VT_ERROR) && a.vt != b.vt)
        return 0;

    if (a.kind == CMP_ARRAY || b.kind == CMP_ARRAY)
        return a.kind == b.kind ? cmp_arrays(&a, &b) : 0;

    if (a.kind == CMP_STRING && b.kind == CMP_STRING)
        return cmp_strings(&a, &b);

    if (cmp_is_unsigned_exception(&a, &b) || cmp_is_unsigned_exception(&b, &a))
        return 0;

    /* Numbers sort before strings */
    if (a.kind == CMP_STRING)
        return 1;
    if (b.kind == CMP_STRING)
        return -1;

    return cmp_numbers(&a, &b);
}

INT WINAPI VariantCompare(REFVARIANT var1, REFVARIANT var2)
{
    struct cmp_value a, b;

    TRACE("%p, %p\n", var1, var2);

    cmp_classify(var1, &a, FALSE);
    cmp_classify(var2, &b, TRUE);

    return cmp_values(&a, &b);
}
