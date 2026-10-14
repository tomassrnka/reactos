/*
 * PROJECT:     ReactOS propsys
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     VariantCompare
 *
 * The rules below were measured on Windows 7 (x86 and x64) by comparing
 * a grid of variants of every type; they are not taken from any Windows code.
 */

#include "precomp.h"

#include <float.h>
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
    CMP_STRING,
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
    const WCHAR *str;
};

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
    double d;

    memset(val, 0, sizeof(*val));
    val->vt = vt;
    val->kind = CMP_OTHER;

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
        if (SUCCEEDED(VarR8FromCy(V_CY(var), &d)))
            cmp_set_real(val, d);
        break;
    case VT_DECIMAL:
        if (SUCCEEDED(VarR8FromDec(&V_DECIMAL(var), &d)))
            cmp_set_real(val, d);
        break;
    case VT_BSTR:
        val->kind = CMP_STRING;
        val->str = V_BSTR(var) ? V_BSTR(var) : L"";
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

static double cmp_to_double(const struct cmp_value *val)
{
    double d;

    if (val->kind == CMP_REAL)
        return val->real;
    d = (double)val->magnitude;
    return val->negative ? -d : d;
}

static int cmp_reals(const struct cmp_value *a, const struct cmp_value *b)
{
    double d1 = cmp_to_double(a), d2 = cmp_to_double(b);

    /*
     * A single and a double that came from the same decimal text are equal on Windows.
     * Values beyond the range of a single are compared as doubles.
     */
    if (a->single != b->single && (a->kind == CMP_REAL && b->kind == CMP_REAL) &&
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

static BOOL cmp_is_digit(WCHAR ch)
{
    return ch >= L'0' && ch <= L'9';
}

static int cmp_char(WCHAR ch1, WCHAR ch2)
{
    int ret = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, &ch1, 1, &ch2, 1);

    return ret ? ret - CSTR_EQUAL : 0;
}

/* Case-insensitive; runs of digits compare as numbers, and with equal values the one with more leading zeros sorts first */
static int cmp_strings(const WCHAR *s1, const WCHAR *s2)
{
    int tie = 0, ret;

    while (*s1 && *s2)
    {
        const WCHAR *e1 = s1, *e2 = s2;

        if (cmp_is_digit(*s1) && cmp_is_digit(*s2))
        {
            const WCHAR *d1, *d2;

            while (*e1 == L'0') e1++;
            while (*e2 == L'0') e2++;
            if (!tie && (e1 - s1) != (e2 - s2))
                tie = (e1 - s1) > (e2 - s2) ? -1 : 1;
            d1 = e1;
            d2 = e2;
            while (cmp_is_digit(*e1)) e1++;
            while (cmp_is_digit(*e2)) e2++;
            if ((e1 - d1) != (e2 - d2))
                return (e1 - d1) > (e2 - d2) ? 1 : -1;
            for (; d1 < e1; d1++, d2++)
            {
                if (*d1 != *d2)
                    return *d1 > *d2 ? 1 : -1;
            }
        }
        else if (!cmp_is_digit(*s1) && !cmp_is_digit(*s2))
        {
            while (*e1 && !cmp_is_digit(*e1)) e1++;
            while (*e2 && !cmp_is_digit(*e2)) e2++;
            ret = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, s1, e1 - s1, s2, e2 - s2);
            if (ret && ret != CSTR_EQUAL)
                return ret - CSTR_EQUAL;
        }
        else
        {
            ret = cmp_char(*s1, *s2);
            if (ret)
                return ret;
            e1++;
            e2++;
        }
        s1 = e1;
        s2 = e2;
    }

    if (*s1)
        return 1;
    if (*s2)
        return -1;
    return tie;
}

static BOOL cmp_is_wide_unsigned(const struct cmp_value *val)
{
    return val->vt == VT_UI2 || val->vt == VT_UI4 || val->vt == VT_UI8;
}

/*
 * What Windows 7 reports as "equal" for an unsigned 16, 32 or 64 bit integer against another kind of
 * value: strings, booleans, VT_INT, singles, currency and decimals. (Against doubles and dates it gives
 * the right order for the 16 and 32 bit types and an inconsistent one for the 64 bit type, which is not copied.)
 */
static BOOL cmp_is_unsigned_exception(const struct cmp_value *u, const struct cmp_value *other)
{
    if (!cmp_is_wide_unsigned(u))
        return FALSE;
    if (other->kind == CMP_STRING || other->vt == VT_BOOL || other->vt == VT_INT)
        return TRUE;
    return other->vt == VT_R4 || other->vt == VT_CY || other->vt == VT_DECIMAL;
}

/* A boolean against a byte counts as the unsigned 16 bit value, so TRUE is the largest */
static void cmp_bool_as_unsigned(struct cmp_value *val)
{
    if (val->negative)
        val->magnitude = 0x10000 - val->magnitude;
    val->negative = FALSE;
}

INT WINAPI VariantCompare(REFVARIANT var1, REFVARIANT var2)
{
    struct cmp_value a, b;

    TRACE("%p, %p\n", var1, var2);

    cmp_classify(var1, &a, FALSE);
    cmp_classify(var2, &b, TRUE);

    if (a.kind == CMP_EMPTY)
        return b.kind == CMP_EMPTY ? 0 : -1;
    if (b.kind == CMP_EMPTY)
        return 1;
    if (a.kind == CMP_OTHER || b.kind == CMP_OTHER)
        return 0;

    /* VT_I1 and VT_ERROR compare only with their own type */
    if ((a.vt == VT_I1 || b.vt == VT_I1 || a.vt == VT_ERROR || b.vt == VT_ERROR) && a.vt != b.vt)
        return 0;

    if (a.kind == CMP_STRING && b.kind == CMP_STRING)
        return cmp_strings(a.str, b.str);

    if (cmp_is_unsigned_exception(&a, &b) || cmp_is_unsigned_exception(&b, &a))
        return 0;

    if (a.kind == CMP_INT && b.kind == CMP_INT)
    {
        if (a.vt == VT_UI1 && b.vt == VT_BOOL)
            cmp_bool_as_unsigned(&b);
        else if (a.vt == VT_BOOL && b.vt == VT_UI1)
            cmp_bool_as_unsigned(&a);
        return cmp_ints(&a, &b);
    }

    /* Numbers sort before strings */
    if (a.kind == CMP_STRING)
        return 1;
    if (b.kind == CMP_STRING)
        return -1;

    return cmp_reals(&a, &b);
}
