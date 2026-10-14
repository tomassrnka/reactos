/*
 * PROJECT:     ReactOS propsys
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     VariantCompare
 *
 * The rules below were measured on Windows Server 2008 R2 and Windows 10 (x86 and x64) by comparing
 * grids of variants of every type; they are not taken from any Windows code.
 */

#include "precomp.h"

#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include "winreg.h"
#include "winuser.h"
#include "shlobj.h"
#include "propvarutil.h"

WINE_DEFAULT_DEBUG_CHANNEL(propsys);

enum cmp_kind
{
    CMP_EMPTY,
    CMP_INT,
    CMP_REAL,
    CMP_CY,
    CMP_DEC,
    CMP_STRING,
    CMP_ARRAY,
    CMP_OTHER
};

struct cmp_value
{
    enum cmp_kind kind;
    VARTYPE vt;
    BOOL negative;
    ULONGLONG magnitude;
    double real;
    BOOL single;
    CY cy;
    DECIMAL dec;
    const WCHAR *str;
    UINT len;
    SAFEARRAY *array;
};

static int cmp_values(const struct cmp_value *a, const struct cmp_value *b);

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

/* The right operand of a VT_INT comparison is not supported on Windows, the left one is */
static void cmp_classify(const VARIANT *var, struct cmp_value *val, BOOL right)
{
    VARTYPE vt = V_VT(var);

    memset(val, 0, sizeof(*val));
    val->vt = vt;
    val->kind = CMP_OTHER;

    if ((vt & ~VT_TYPEMASK) == VT_ARRAY)
    {
        val->kind = CMP_ARRAY;
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
        val->single = TRUE;
        break;
    case VT_R8:
        cmp_set_real(val, V_R8(var));
        break;
    case VT_DATE:
        cmp_set_real(val, V_DATE(var));
        break;
    case VT_CY:
        val->kind = CMP_CY;
        val->cy = V_CY(var);
        break;
    case VT_DECIMAL:
        val->kind = CMP_DEC;
        val->dec = V_DECIMAL(var);
        break;
    case VT_BSTR:
        val->kind = CMP_STRING;
        val->str = V_BSTR(var) ? V_BSTR(var) : L"";
        val->len = V_BSTR(var) ? SysStringLen(V_BSTR(var)) : 0;
        break;
    }
}

static int cmp_ints(const struct cmp_value *a, const struct cmp_value *b)
{
    if (a->negative != b->negative)
        return a->negative ? -1 : 1;
    if (a->magnitude == b->magnitude)
        return 0;
    if (a->negative)
        return a->magnitude > b->magnitude ? -1 : 1;
    return a->magnitude > b->magnitude ? 1 : -1;
}

static int cmp_reals(const struct cmp_value *a, const struct cmp_value *b)
{
    double d1 = a->real, d2 = b->real;

    /*
     * A single and a double that came from the same decimal text are equal on Windows.
     * Values beyond the range of a single are compared as doubles.
     */
    if (a->single != b->single &&
        d1 >= -FLT_MAX && d1 <= FLT_MAX && d2 >= -FLT_MAX && d2 <= FLT_MAX)
    {
        float f1 = (float)d1, f2 = (float)d2;
        d1 = f1;
        d2 = f2;
    }
    if (d1 < d2)
        return -1;
    return d1 > d2 ? 1 : 0;
}

#ifdef _M_IX86
/* On x86 Windows an integer is compared with the exact value of the real, not with a rounded double */
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

    /* Same sign: compare the magnitudes, an integer is below 2^64 */
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

/* An integer against a real, in the order integer then real */
static int cmp_int_real(const struct cmp_value *val, const struct cmp_value *real)
{
    double d = real->real;

    /*
     * Windows compares an unsigned 64-bit integer with the raw bits of a double (or date) instead of its value:
     * every negative real is below it, any other is compared as the unsigned 64-bit pattern.
     */
    if (val->vt == VT_UI8)
    {
        ULONGLONG bits;

        memcpy(&bits, &d, sizeof(bits));
        if ((bits >> 63) && d != 0)
            return 1;
        if (val->magnitude == bits)
            return 0;
        return val->magnitude > bits ? 1 : -1;
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
        return i > d ? 1 : 0;
    }
#endif
}

static int cmp_from_varcmp(HRESULT hr)
{
    if (hr == VARCMP_LT)
        return -1;
    return hr == VARCMP_GT ? 1 : 0;
}

/* A real as a decimal of its 16 significant digits, which is how Windows compares it with a decimal */
static HRESULT cmp_dec_from_real(double real, DECIMAL *dec)
{
    char text[48];
    ULONG parts[3] = { 0, 0, 0 };
    ULONGLONG t, carry, digits = 0;
    int i, j, exponent, scale;

    memset(dec, 0, sizeof(*dec));
    if (real != real)
        return DISP_E_BADVARTYPE;
    if (real - real != 0)
        return DISP_E_OVERFLOW;
    if (real == 0)
        return S_OK;

    sprintf(text, "%.15e", real < 0 ? -real : real);
    digits = text[0] - '0';
    for (i = 2; i <= 16; i++)
        digits = digits * 10 + (text[i] - '0');
    exponent = atoi(text + 18) - 15;
    parts[0] = (ULONG)digits;
    parts[1] = (ULONG)(digits >> 32);

    scale = 0;
    for (; exponent > 0; exponent--)
    {
        for (i = 0, carry = 0; i < 3; i++)
        {
            t = (ULONGLONG)parts[i] * 10 + carry;
            parts[i] = (ULONG)t;
            carry = t >> 32;
        }
        if (carry)
            return DISP_E_OVERFLOW;
    }
    for (scale = -exponent; scale > 28; scale--)
    {
        for (j = 2, carry = 0; j >= 0; j--)
        {
            t = (carry << 32) | parts[j];
            parts[j] = (ULONG)(t / 10);
            carry = t % 10;
        }
    }

    dec->Lo32 = parts[0];
    dec->Mid32 = parts[1];
    dec->Hi32 = parts[2];
    dec->scale = (BYTE)scale;
    dec->sign = real < 0 ? DECIMAL_NEG : 0;
    return S_OK;
}

/* A value converted to a decimal */
static HRESULT cmp_to_dec(const struct cmp_value *val, DECIMAL *dec)
{
    switch (val->kind)
    {
    case CMP_INT:
        if (val->negative)
            return VarDecFromI8((LONGLONG)(0 - val->magnitude), dec);
        return VarDecFromUI8(val->magnitude, dec);
    case CMP_CY:
        return VarDecFromCy(val->cy, dec);
    case CMP_DEC:
        *dec = val->dec;
        return S_OK;
    default:
        return cmp_dec_from_real(val->real, dec);
    }
}

static int cmp_dec_numbers(const struct cmp_value *a, const struct cmp_value *b)
{
    DECIMAL d1, d2;
    HRESULT hr1 = cmp_to_dec(a, &d1), hr2 = cmp_to_dec(b, &d2);

    if (FAILED(hr1) || FAILED(hr2))
    {
        /* Only a real can fail, when it is beyond the range of a decimal */
        const struct cmp_value *real = FAILED(hr1) ? a : b;
        int ret;

        if (real->real != real->real)
            return 0;
        ret = real->real > 0 ? 1 : -1;
        return real == a ? ret : -ret;
    }
    hr1 = VarDecCmp(&d1, &d2);
    if (FAILED(hr1))
        return 0;
    return cmp_from_varcmp(hr1);
}

/* A currency against an integer, a real or a currency, in the order currency then other */
static int cmp_cy_number(const struct cmp_value *cy, const struct cmp_value *other)
{
    HRESULT hr;

    switch (other->kind)
    {
    case CMP_CY:
        if (cy->cy.int64 == other->cy.int64)
            return 0;
        return cy->cy.int64 < other->cy.int64 ? -1 : 1;
    case CMP_INT:
        /* Exact: scaled by 10000 and compared, an integer beyond the range of a currency is above or below it */
        if (other->magnitude > 922337203685477ULL)
            return other->negative ? 1 : -1;
        else
        {
            LONGLONG scaled = (LONGLONG)other->magnitude * 10000;

            if (other->negative)
                scaled = -scaled;
            if (cy->cy.int64 == scaled)
                return 0;
            return cy->cy.int64 < scaled ? -1 : 1;
        }
    default:
        if (other->real != other->real)
            return 0;
        /* The real is rounded to the four digits of a currency; one beyond its range is above or below it */
        hr = VarCyCmpR8(cy->cy, other->real);
        if (FAILED(hr))
            return other->real > 0 ? -1 : 1;
        return cmp_from_varcmp(hr);
    }
}

static int cmp_numbers(struct cmp_value *a, struct cmp_value *b)
{
    int ret;

    if (a->kind == CMP_DEC || b->kind == CMP_DEC)
        return cmp_dec_numbers(a, b);
    if (a->kind == CMP_CY)
        return cmp_cy_number(a, b);
    if (b->kind == CMP_CY)
        return -cmp_cy_number(b, a);
    if (a->kind == CMP_INT && b->kind == CMP_INT)
    {
        /* A boolean against a byte counts as the unsigned 16 bit value, so TRUE is the largest */
        if (a->vt == VT_UI1 && b->vt == VT_BOOL && b->negative)
        {
            b->magnitude = 0x10000 - b->magnitude;
            b->negative = FALSE;
        }
        else if (a->vt == VT_BOOL && b->vt == VT_UI1 && a->negative)
        {
            a->magnitude = 0x10000 - a->magnitude;
            a->negative = FALSE;
        }
        return cmp_ints(a, b);
    }
    if (a->kind == CMP_INT)
        return cmp_int_real(a, b);
    if (b->kind == CMP_INT)
    {
        ret = cmp_int_real(b, a);
        return -ret;
    }
    return cmp_reals(a, b);
}

/* A digit of any script (not the superscripts and the like), as the value 0 to 9, else -1 */
static int cmp_digit_value(WCHAR ch)
{
    WORD type;
    WCHAR folded;

    if (ch >= L'0' && ch <= L'9')
        return ch - L'0';
    if (ch < 0x660)
        return -1;
    if (!GetStringTypeW(CT_CTYPE1, &ch, 1, &type) || !(type & C1_DIGIT) || (type & C1_PUNCT))
        return -1;
    if (FoldStringW(MAP_FOLDDIGITS, &ch, 1, &folded, 1) != 1 || folded < L'0' || folded > L'9')
        return -1;
    return folded - L'0';
}

/* The code of the zero of the script of a digit; the digits of a number are all of one script */
static int cmp_digit_base(WCHAR ch)
{
    int val = cmp_digit_value(ch);

    return val < 0 ? -1 : (int)ch - val;
}

static WCHAR *cmp_put_number(WCHAR *out, UINT n)
{
    int i;

    for (i = 3; i >= 0; i--)
    {
        out[i] = L'0' + n % 10;
        n /= 10;
    }
    return out + 4;
}

/*
 * The text with every run of digits replaced by a run that the ordinary string comparison orders as Windows does:
 * the count of its significant digits, the digits, then the complement of its leading zeros, so that the longer
 * number is the larger and, with the same value, the run with more leading zeros comes first at once.
 * Windows compares strings like that (digits as numbers), also for the digits of other scripts.
 */
static WCHAR *cmp_make_key(const WCHAR *str, UINT len, UINT *key_len)
{
    WCHAR *key, *out;
    UINT i = 0, start, zeros, count;
    int base;

    out = key = HeapAlloc(GetProcessHeap(), 0, ((SIZE_T)len * 9 + 1) * sizeof(WCHAR));
    if (!key)
        return NULL;

    while (i < len)
    {
        int val = cmp_digit_value(str[i]);

        if (val < 0)
        {
            *out++ = str[i++];
            continue;
        }

        start = i;
        base = cmp_digit_base(str[i]);
        while (i < len && cmp_digit_base(str[i]) == base)
            i++;
        zeros = 0;
        while (start + zeros < i && cmp_digit_value(str[start + zeros]) == 0)
            zeros++;
        count = i - start - zeros;

        out = cmp_put_number(out, count > 9999 ? 9999 : count);
        for (val = start + zeros; (UINT)val < i; val++)
            *out++ = L'0' + cmp_digit_value(str[val]);
        out = cmp_put_number(out, 9999 - (zeros > 9999 ? 9999 : zeros));
    }

    *key_len = out - key;
    return key;
}

/* Digits that are equal as numbers but written in different scripts: the code of the first differing one decides */
static int cmp_digit_scripts(const WCHAR *s1, UINT len1, const WCHAR *s2, UINT len2)
{
    UINT i = 0, j = 0;

    for (;;)
    {
        while (i < len1 && cmp_digit_value(s1[i]) < 0) i++;
        while (j < len2 && cmp_digit_value(s2[j]) < 0) j++;
        if (i >= len1 || j >= len2)
            return 0;
        if (s1[i] != s2[j])
            return s1[i] < s2[j] ? -1 : 1;
        i++;
        j++;
    }
}

static int cmp_strings(const struct cmp_value *a, const struct cmp_value *b)
{
    UINT len1 = 0, len2 = 0;
    WCHAR *key1 = cmp_make_key(a->str, a->len, &len1);
    WCHAR *key2 = cmp_make_key(b->str, b->len, &len2);
    int ret;

    if (key1 && key2)
        ret = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, key1, len1, key2, len2);
    else
        ret = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a->str, a->len, b->str, b->len);
    HeapFree(GetProcessHeap(), 0, key1);
    HeapFree(GetProcessHeap(), 0, key2);

    if (ret && ret != CSTR_EQUAL)
        return ret - CSTR_EQUAL;
    return cmp_digit_scripts(a->str, a->len, b->str, b->len);
}

static BOOL cmp_is_wide_unsigned(const struct cmp_value *val)
{
    return val->vt == VT_UI2 || val->vt == VT_UI4 || val->vt == VT_UI8;
}

/*
 * What Windows reports as "equal" for an unsigned 16, 32 or 64 bit integer against another kind of
 * value: strings, booleans, VT_INT, singles, currency and decimals. (Against doubles and dates it gives
 * the right order for the 16 and 32 bit types; the 64 bit type is compared with the raw bits of the double.)
 */
static BOOL cmp_is_unsigned_exception(const struct cmp_value *u, const struct cmp_value *other)
{
    if (!cmp_is_wide_unsigned(u))
        return FALSE;
    if (other->kind == CMP_STRING || other->vt == VT_BOOL || other->vt == VT_INT)
        return TRUE;
    return other->vt == VT_R4 || other->vt == VT_CY || other->vt == VT_DECIMAL;
}

/* The element types whose arrays Windows compares; the others (currency, decimal, error, VT_INT, ...) are all equal */
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

static void cmp_element(VARTYPE vt, const void *data, struct cmp_value *val)
{
    VARIANT var;

    memset(&var, 0, sizeof(var));
    V_VT(&var) = vt;
    memcpy(&V_UI8(&var), data, cmp_element_size(vt));
    cmp_classify(&var, val, FALSE);
}

static void cmp_array_shape(const struct cmp_value *val, UINT *dims, LONG *lbound, ULONG *count)
{
    *dims = 1;
    *lbound = 0;
    *count = 0;
    if (!val->array)
        return;
    *dims = SafeArrayGetDim(val->array);
    if (*dims != 1)
        return;
    *count = val->array->rgsabound[0].cElements;
    *lbound = val->array->rgsabound[0].lLbound;
}

/*
 * Arrays of the supported element types are compared element by element (as single values) and then by length.
 * A multidimensional array sorts below a one-dimensional one, two of them are equal, and so are arrays
 * whose first index is not 0 (the last case is only partly measured: a lower bound of -2 compares the elements).
 */
static int cmp_arrays(const struct cmp_value *a, const struct cmp_value *b)
{
    VARTYPE vt1 = a->vt & VT_TYPEMASK, vt2 = b->vt & VT_TYPEMASK;
    UINT size1 = cmp_element_size(vt1), size2 = cmp_element_size(vt2), dims1, dims2;
    LONG lbound1, lbound2;
    ULONG count1, count2, i, common;
    void *data1 = NULL, *data2 = NULL;
    int ret = 0;

    if (!size1 || !size2)
        return 0;

    cmp_array_shape(a, &dims1, &lbound1, &count1);
    cmp_array_shape(b, &dims2, &lbound2, &count2);
    if (dims1 != 1 || dims2 != 1)
    {
        if (dims1 != 1 && dims2 != 1)
            return 0;
        return dims1 != 1 ? -1 : 1;
    }
    if (lbound1 || lbound2)
        return 0;
    if ((count1 && SafeArrayGetElemsize(a->array) != size1) || (count2 && SafeArrayGetElemsize(b->array) != size2))
        return 0;

    common = count1 < count2 ? count1 : count2;
    if (common)
    {
        if (FAILED(SafeArrayAccessData(a->array, &data1)))
            return 0;
        if (FAILED(SafeArrayAccessData(b->array, &data2)))
        {
            SafeArrayUnaccessData(a->array);
            return 0;
        }
        for (i = 0; i < common && !ret; i++)
        {
            struct cmp_value e1, e2;

            cmp_element(vt1, (const BYTE *)data1 + (SIZE_T)i * size1, &e1);
            cmp_element(vt2, (const BYTE *)data2 + (SIZE_T)i * size2, &e2);
            ret = cmp_values(&e1, &e2);
        }
        SafeArrayUnaccessData(b->array);
        SafeArrayUnaccessData(a->array);
        if (ret)
            return ret;
    }

    if (count1 == count2)
        return 0;
    return count1 < count2 ? -1 : 1;
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

    /* VT_I1 and VT_ERROR compare only with their own type */
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
