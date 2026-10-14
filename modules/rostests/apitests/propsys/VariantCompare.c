/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for propsys!VariantCompare
 *
 * Every expected value was measured on Windows Server 2008 R2 (6.1.7601) and Windows 10, x86 and x64.
 */

#include <apitest.h>
#include <pseh/pseh2.h>
#include <oleauto.h>

typedef INT (WINAPI *VARIANTCOMPARE)(const VARIANT *, const VARIANT *);
static VARIANTCOMPARE pVariantCompare;

#define EXCEPTION_RESULT 0x7fffff00

static INT call_compare(const VARIANT *v1, const VARIANT *v2)
{
    INT ret = EXCEPTION_RESULT;

    _SEH2_TRY
    {
        ret = pVariantCompare(v1, v2);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        ret = EXCEPTION_RESULT;
    }
    _SEH2_END;

    return ret;
}

#define CHECK(v1, v2, expected, what) \
    do { \
        INT ret_ = call_compare(&(v1), &(v2)); \
        ok(ret_ == (expected), "%s: expected %d, got %d\n", (what), (expected), ret_); \
    } while (0)

/* Both orders: the second result is the mirror of the first */
#define CHECK2(v1, v2, expected, what) \
    do { \
        CHECK(v1, v2, expected, what); \
        CHECK(v2, v1, -(expected), what " (swapped)"); \
    } while (0)

/* VariantInit sets the type only on Windows 7; the rest must not be left to chance */
static VARIANT mk_empty(void)
{
    VARIANT v;
    ZeroMemory(&v, sizeof(v));
    return v;
}

static VARIANT mk_null(void)
{
    VARIANT v = mk_empty();
    V_VT(&v) = VT_NULL;
    return v;
}

#define MK(name, vt_, accessor, type) \
static VARIANT name(type x) \
{ \
    VARIANT v = mk_empty(); \
    V_VT(&v) = (vt_); \
    accessor(&v) = x; \
    return v; \
}

MK(mk_ui1, VT_UI1, V_UI1, BYTE)
MK(mk_i2, VT_I2, V_I2, SHORT)
MK(mk_ui2, VT_UI2, V_UI2, USHORT)
MK(mk_i4, VT_I4, V_I4, LONG)
MK(mk_ui4, VT_UI4, V_UI4, ULONG)
MK(mk_i8, VT_I8, V_I8, LONGLONG)
MK(mk_ui8, VT_UI8, V_UI8, ULONGLONG)
MK(mk_r4, VT_R4, V_R4, float)
MK(mk_r8, VT_R8, V_R8, double)
MK(mk_bool, VT_BOOL, V_BOOL, VARIANT_BOOL)
MK(mk_date, VT_DATE, V_DATE, DATE)
MK(mk_error, VT_ERROR, V_ERROR, SCODE)
MK(mk_i1, VT_I1, V_I1, CHAR)
MK(mk_uint, VT_UINT, V_UINT, UINT)

static VARIANT mk_bstr(const WCHAR *str)
{
    VARIANT v = mk_empty();
    V_VT(&v) = VT_BSTR;
    V_BSTR(&v) = str ? SysAllocString(str) : NULL;
    return v;
}

static VARIANT mk_cy(LONGLONG scaled)
{
    VARIANT v = mk_empty();
    V_VT(&v) = VT_CY;
    V_CY(&v).int64 = scaled;
    return v;
}

static VARIANT mk_dec(LONG n)
{
    VARIANT v = mk_empty();
    VarDecFromI4(n, &V_DECIMAL(&v));
    V_VT(&v) = VT_DECIMAL;
    return v;
}

static VARIANT mk_decimal(ULONG hi, ULONG mid, ULONG lo, BYTE scale)
{
    VARIANT v = mk_empty();

    V_VT(&v) = VT_DECIMAL;
    V_DECIMAL(&v).Hi32 = hi;
    V_DECIMAL(&v).Mid32 = mid;
    V_DECIMAL(&v).Lo32 = lo;
    V_DECIMAL(&v).scale = scale;
    V_DECIMAL(&v).sign = 0;
    return v;
}

/* A one-dimensional array of the given element type; the elements are the values of the list */
static VARIANT mk_array(VARTYPE vt, ULONG count, const LONGLONG *values, LONG lbound)
{
    VARIANT v = mk_empty();
    SAFEARRAYBOUND bound;
    SAFEARRAY *array;
    ULONG i;
    void *data;

    bound.cElements = count;
    bound.lLbound = lbound;
    array = SafeArrayCreate(vt, 1, &bound);
    V_VT(&v) = VT_ARRAY | vt;
    V_ARRAY(&v) = array;
    if (!array || !count)
        return v;
    if (FAILED(SafeArrayAccessData(array, &data)))
        return v;
    for (i = 0; i < count; i++)
    {
        switch (vt)
        {
        case VT_I1:
        case VT_UI1: ((BYTE *)data)[i] = (BYTE)values[i]; break;
        case VT_I2:
        case VT_UI2:
        case VT_BOOL: ((SHORT *)data)[i] = (SHORT)values[i]; break;
        case VT_I4:
        case VT_UI4:
        case VT_INT:
        case VT_UINT:
        case VT_ERROR: ((LONG *)data)[i] = (LONG)values[i]; break;
        case VT_I8:
        case VT_UI8:
        case VT_CY: ((LONGLONG *)data)[i] = values[i]; break;
        case VT_R4: ((float *)data)[i] = (float)values[i]; break;
        case VT_R8:
        case VT_DATE: ((double *)data)[i] = (double)values[i]; break;
        case VT_DECIMAL:
            ZeroMemory(&((DECIMAL *)data)[i], sizeof(DECIMAL));
            ((DECIMAL *)data)[i].Lo32 = (ULONG)values[i];
            break;
        case VT_BSTR:
        {
            WCHAR text[2] = { L'0' + (WCHAR)values[i], UNICODE_NULL };

            ((BSTR *)data)[i] = SysAllocString(text);
            break;
        }
        default:
            break;
        }
    }
    SafeArrayUnaccessData(array);
    return v;
}

#define ARRAY(name, vt, lbound, ...) \
    LONGLONG name##_values[] = { __VA_ARGS__, 0 }; \
    VARIANT name = mk_array(vt, ARRAYSIZE(name##_values) - 1, name##_values, lbound)

#define NO_ARRAY(name, vt) \
    VARIANT name = mk_array(vt, 0, NULL, 0)

static void test_empty(void)
{
    VARIANT empty = mk_empty(), null = mk_null(), i4 = mk_i4(0), str = mk_bstr(L""), nullstr = mk_bstr(NULL);
    VARIANT no = mk_bool(VARIANT_FALSE), zero = mk_r8(0.0);

    CHECK(empty, empty, 0, "empty, empty");
    CHECK(null, null, 0, "null, null");
    CHECK(empty, null, 0, "empty, null");
    CHECK(null, empty, 0, "null, empty");
    CHECK2(empty, i4, -1, "empty, I4 0");
    CHECK2(null, i4, -1, "null, I4 0");
    CHECK2(empty, str, -1, "empty, empty string");
    CHECK2(null, str, -1, "null, empty string");
    CHECK2(empty, nullstr, -1, "empty, NULL string");
    CHECK2(empty, no, -1, "empty, FALSE");
    CHECK2(empty, zero, -1, "empty, R8 0");
    VariantClear(&str);
    VariantClear(&nullstr);
}

static void test_same_type(void)
{
    VARIANT a, b;

#define SAME(maker, lo, hi, name) \
    a = maker(lo); b = maker(hi); \
    CHECK2(a, b, -1, name " lo, hi"); \
    CHECK(a, a, 0, name " lo, lo"); \
    CHECK(b, b, 0, name " hi, hi")

    SAME(mk_ui1, 5, 200, "UI1");
    SAME(mk_i2, -1, 5, "I2");
    SAME(mk_ui2, 5, 65535, "UI2");
    SAME(mk_i4, -1, 100000, "I4");
    SAME(mk_ui4, 5, 0xffffffffUL, "UI4");
    SAME(mk_i8, -1, 5, "I8");
    SAME(mk_i8, -0x7fffffffffffffffLL, 0x7fffffffffffffffLL, "I8 limits");
    SAME(mk_ui8, 1, 0xfffffffffffffffeULL, "UI8");
    SAME(mk_r4, 1.5f, 5.0f, "R4");
    SAME(mk_r8, 1.5, 5.5, "R8");
    SAME(mk_bool, VARIANT_TRUE, VARIANT_FALSE, "BOOL");
    SAME(mk_date, 1.0, 2.0, "DATE");
    SAME(mk_cy, 10000, 50000, "CY");
    SAME(mk_dec, -3, 5, "DECIMAL");
    SAME(mk_i1, -1, 5, "I1");
#undef SAME
}

static void test_mixed_numbers(void)
{
    VARIANT a, b;

    /* Integers of different types compare by value, including the signed against the unsigned */
    a = mk_ui1(5); b = mk_i2(-1); CHECK2(a, b, 1, "UI1 5, I2 -1");
    a = mk_ui1(5); b = mk_i2(5); CHECK(a, b, 0, "UI1 5, I2 5");
    a = mk_ui1(200); b = mk_i4(100000); CHECK2(a, b, -1, "UI1 200, I4 100000");
    a = mk_i2(5); b = mk_ui2(5); CHECK(a, b, 0, "I2 5, UI2 5");
    a = mk_i2(-1); b = mk_ui2(65535); CHECK2(a, b, -1, "I2 -1, UI2 65535");
    a = mk_i4(5); b = mk_ui4(5); CHECK(a, b, 0, "I4 5, UI4 5");
    a = mk_i4(-5); b = mk_ui4(5); CHECK2(a, b, -1, "I4 -5, UI4 5");
    a = mk_ui4(0xfffffff0UL); b = mk_i4(5); CHECK2(a, b, 1, "UI4 large, I4 5");
    a = mk_i8(-5); b = mk_ui8(5); CHECK2(a, b, -1, "I8 -5, UI8 5");
    a = mk_ui8(0xfffffffffffffffeULL); b = mk_i8(5); CHECK2(a, b, 1, "UI8 large, I8 5");
    a = mk_ui8(5); b = mk_i8(5); CHECK(a, b, 0, "UI8 5, I8 5");
    a = mk_i2(-1); b = mk_i8(-1); CHECK(a, b, 0, "I2 -1, I8 -1");
    a = mk_bool(VARIANT_TRUE); b = mk_i2(-1); CHECK(a, b, 0, "TRUE, I2 -1");
    a = mk_bool(VARIANT_FALSE); b = mk_i4(0); CHECK(a, b, 0, "FALSE, I4 0");
    a = mk_bool(VARIANT_TRUE); b = mk_i2(0); CHECK2(a, b, -1, "TRUE, I2 0");

    /* A boolean against a byte counts as unsigned: TRUE is above any byte */
    a = mk_bool(VARIANT_TRUE); b = mk_ui1(200); CHECK2(a, b, 1, "TRUE, UI1 200");
    a = mk_bool(VARIANT_FALSE); b = mk_ui1(5); CHECK2(a, b, -1, "FALSE, UI1 5");

    /* Unsigned 16 and 32 bit integers against doubles and dates compare by value */
    a = mk_ui2(5); b = mk_r8(1.5); CHECK2(a, b, 1, "UI2 5, R8 1.5");
    a = mk_ui2(5); b = mk_r8(5.5); CHECK2(a, b, -1, "UI2 5, R8 5.5");
    a = mk_ui4(5); b = mk_r8(1.5); CHECK2(a, b, 1, "UI4 5, R8 1.5");
    a = mk_ui4(5); b = mk_date(1.0); CHECK2(a, b, 1, "UI4 5, DATE 1");

    /* Integers against the other numeric types */
    a = mk_i2(5); b = mk_r8(5.5); CHECK2(a, b, -1, "I2 5, R8 5.5");
    a = mk_i4(5); b = mk_r8(5.0); CHECK(a, b, 0, "I4 5, R8 5");
    a = mk_i4(5); b = mk_r4(1.5f); CHECK2(a, b, 1, "I4 5, R4 1.5");
    a = mk_ui1(5); b = mk_r8(5.5); CHECK2(a, b, -1, "UI1 5, R8 5.5");
    a = mk_i8(5); b = mk_r8(1.5); CHECK2(a, b, 1, "I8 5, R8 1.5");
    a = mk_i4(16777217); b = mk_r4(16777216.0f); CHECK2(a, b, 1, "I4 16777217, R4 16777216");
    a = mk_i4(5); b = mk_cy(50000); CHECK(a, b, 0, "I4 5, CY 5");
    a = mk_i4(5); b = mk_cy(40000); CHECK2(a, b, 1, "I4 5, CY 4");
    a = mk_i4(5); b = mk_dec(5); CHECK(a, b, 0, "I4 5, DECIMAL 5");
    a = mk_i4(6); b = mk_dec(5); CHECK2(a, b, 1, "I4 6, DECIMAL 5");
    a = mk_i4(5); b = mk_date(1.0); CHECK2(a, b, 1, "I4 5, DATE 1");
    a = mk_date(5.0); b = mk_i4(5); CHECK(a, b, 0, "DATE 5, I4 5");
    a = mk_r8(1.5); b = mk_r4(1.5f); CHECK(a, b, 0, "R8 1.5, R4 1.5");
    a = mk_r8(0.1); b = mk_r4(0.1f); CHECK(a, b, 0, "R8 0.1, R4 0.1");
    a = mk_r8(5.5); b = mk_r4(5.0f); CHECK2(a, b, 1, "R8 5.5, R4 5");
    a = mk_r8(1.0); b = mk_date(1.0); CHECK(a, b, 0, "R8 1, DATE 1");
    a = mk_cy(50000); b = mk_r8(5.0); CHECK(a, b, 0, "CY 5, R8 5");
    a = mk_dec(5); b = mk_r8(5.5); CHECK2(a, b, -1, "DECIMAL 5, R8 5.5");
}

static void test_strings(void)
{
    static const struct
    {
        const WCHAR *s1, *s2;
        int expected;
    } tests[] =
    {
        { L"a", L"a", 0 },
        { L"a", L"A", 0 },
        { L"ab", L"AB", 0 },
        { L"a", L"b", -1 },
        { L"A", L"b", -1 },
        { L"ab", L"AC", -1 },
        { L"a", L"ab", -1 },
        { L"b", L"ab", 1 },
        { L"z", L"a", 1 },
        { L"", L"a", -1 },
        { L"", L"", 0 },
        { L"5", L"9", -1 },
        { L"5", L"10", -1 },
        { L"9", L"10", -1 },
        { L"2", L"10", -1 },
        { L"file2", L"file10", -1 },
        { L"File9", L"file10", -1 },
        { L"file2", L"File2", 0 },
        { L"a1", L"a2", -1 },
        { L"a2", L"a10", -1 },
        { L"1.5", L"1.25", -1 },
        { L"1", L"a", -1 },
        { L"a", L"b1", -1 },
        { L"01", L"1", -1 },
        { L"a01", L"a1", -1 },
        { L"a1", L"a 1", 1 },
        { L"a 1", L"a01", -1 },
        { L"a01", L"a 01", 1 },
        { L"a", L"a ", -1 },
        { L"01b", L"1a", -1 },
        { L"01a", L"1", -1 },
        { L"1b", L"01b", 1 },
        { L"0", L"00", 1 },
        { L"a0", L"a00", 1 },
        { L"file02", L"file2", -1 },
        { L"file2", L"file 2", 1 },
        { L"1 2", L"12", -1 },
        { L"1 2", L"1  2", 1 },
        { L"1.05", L"1.5", -1 },
        { L"100", L"99", 1 },
        { L"123456789012345678901234567890", L"123456789012345678901234567891", -1 },
        { L"0123456789012345678901234567891", L"123456789012345678901234567891", -1 },
        { L"1\x00b2", L"12", -1 },
        { L"\x00b2", L"1", 1 },
        { L"1\x0661", L"5", -1 },
        { L"\x0661", L"5", -1 },
        { L"\x0665", L"5", 1 },
        { L"\x0661", L"\x0662", -1 },
        { L"\x0665", L"\x0661", 1 },
        { L"1", L"\x0661", -1 },
        { L"a1", L"a\x0661", -1 },
        { L"\xff11", L"1", 1 },
        { L"\xff11", L"\xff12", -1 },
        { L"\x0966", L"\x0967", -1 },
    };
    VARIANT a, b;
    unsigned i;

    for (i = 0; i < ARRAYSIZE(tests); i++)
    {
        char what[64];

        a = mk_bstr(tests[i].s1);
        b = mk_bstr(tests[i].s2);
        sprintf(what, "string %u (%d)", i, tests[i].expected);
        CHECK(a, b, tests[i].expected, what);
        CHECK(b, a, -tests[i].expected, what);
        VariantClear(&a);
        VariantClear(&b);
    }

    /* A NULL BSTR is the empty string */
    a = mk_bstr(NULL); b = mk_bstr(L"");
    CHECK(a, b, 0, "NULL string, empty string");
    CHECK(b, a, 0, "empty string, NULL string");
    VariantClear(&b);
    b = mk_bstr(L"a");
    CHECK2(a, b, -1, "NULL string, a");
    VariantClear(&b);
}

static void test_number_and_string(void)
{
    VARIANT str = mk_bstr(L"5"), word = mk_bstr(L"a"), num, b_;

    /* Numbers sort before strings, whatever they say; the unsigned 16, 32 and 64 bit types report "equal" */
    num = mk_i4(5); CHECK2(num, str, -1, "I4 5, string 5");
    num = mk_i4(5); CHECK2(num, word, -1, "I4 5, string a");
    num = mk_i2(-1); CHECK2(num, word, -1, "I2 -1, string a");
    num = mk_ui1(5); CHECK2(num, str, -1, "UI1 5, string 5");
    num = mk_i8(5); CHECK2(num, str, -1, "I8 5, string 5");
    num = mk_r8(5.0); CHECK2(num, str, -1, "R8 5, string 5");
    num = mk_r4(5.0f); CHECK2(num, word, -1, "R4 5, string a");
    num = mk_bool(VARIANT_TRUE); CHECK2(num, word, -1, "TRUE, string a");
    num = mk_date(1.0); CHECK2(num, word, -1, "DATE 1, string a");
    num = mk_cy(50000); CHECK2(num, word, -1, "CY 5, string a");
    num = mk_dec(5); CHECK2(num, word, -1, "DECIMAL 5, string a");
    num = mk_ui2(5); CHECK(num, word, 0, "UI2 5, string a");
    num = mk_ui2(5); CHECK(word, num, 0, "string a, UI2 5");
    num = mk_ui4(5); CHECK(num, word, 0, "UI4 5, string a");
    num = mk_ui8(5); CHECK(word, num, 0, "string a, UI8 5");

    /* The unsigned types against the other kinds: equal, whatever the values */
    num = mk_ui2(65535); b_ = mk_r4(1.5f); CHECK(num, b_, 0, "UI2 65535, R4 1.5"); CHECK(b_, num, 0, "R4 1.5, UI2 65535");
    num = mk_ui4(0xffffffffUL); b_ = mk_cy(50000); CHECK(num, b_, 0, "UI4 max, CY 5"); CHECK(b_, num, 0, "CY 5, UI4 max");
    num = mk_ui2(65535); b_ = mk_dec(5); CHECK(num, b_, 0, "UI2 65535, DECIMAL 5"); CHECK(b_, num, 0, "DECIMAL 5, UI2 65535");
    num = mk_ui8(5); b_ = mk_r4(1.5f); CHECK(num, b_, 0, "UI8 5, R4 1.5"); CHECK(b_, num, 0, "R4 1.5, UI8 5");
    num = mk_ui2(65535); b_ = mk_empty(); V_VT(&b_) = VT_INT; V_INT(&b_) = 5; CHECK(num, b_, 0, "UI2 65535, INT 5"); CHECK(b_, num, 0, "INT 5, UI2 65535");

    /* The same for a boolean */
    num = mk_ui2(5); b_ = mk_bool(VARIANT_TRUE); CHECK(num, b_, 0, "UI2 5, TRUE"); CHECK(b_, num, 0, "TRUE, UI2 5");
    num = mk_ui4(5); b_ = mk_bool(VARIANT_FALSE); CHECK(num, b_, 0, "UI4 5, FALSE"); CHECK(b_, num, 0, "FALSE, UI4 5");
    num = mk_ui8(5); b_ = mk_bool(VARIANT_TRUE); CHECK(num, b_, 0, "UI8 5, TRUE"); CHECK(b_, num, 0, "TRUE, UI8 5");

    VariantClear(&str);
    VariantClear(&word);
}

/* The unsigned 64-bit integer is compared with the raw bits of a double or date (a quirk of Windows) */
static void test_unsigned64_real(void)
{
    VARIANT a, b;
    double d;
    ULONGLONG bits;

    a = mk_ui8(5); b = mk_r8(1.5); CHECK2(a, b, -1, "UI8 5, R8 1.5");
    a = mk_ui8(5); b = mk_r8(5.0); CHECK2(a, b, -1, "UI8 5, R8 5");
    a = mk_ui8(5); b = mk_r8(0.0); CHECK2(a, b, 1, "UI8 5, R8 0");
    a = mk_ui8(0); b = mk_r8(0.0); CHECK(a, b, 0, "UI8 0, R8 0");
    a = mk_ui8(5); b = mk_r8(-1.5); CHECK2(a, b, 1, "UI8 5, R8 -1.5");
    a = mk_ui8(0xfffffffffffffffeULL); b = mk_r8(-1.5); CHECK2(a, b, 1, "UI8 large, R8 -1.5");
    a = mk_ui8(0x7ffffffffffffffeULL); b = mk_r8(1.5); CHECK2(a, b, 1, "UI8 2^63-2, R8 1.5");
    a = mk_ui8(0xffffffffffffffffULL); b = mk_r8(1e30); CHECK2(a, b, 1, "UI8 max, R8 1e30");
    a = mk_ui8(5); b = mk_date(1.5); CHECK2(a, b, -1, "UI8 5, DATE 1.5");
    a = mk_ui8(5); b = mk_date(-1.5); CHECK2(a, b, 1, "UI8 5, DATE -1.5");

    d = 1.5;
    memcpy(&bits, &d, sizeof(bits));
    a = mk_ui8(bits); b = mk_r8(1.5); CHECK(a, b, 0, "UI8 bits of 1.5, R8 1.5");
    a = mk_ui8(bits - 1); b = mk_r8(1.5); CHECK2(a, b, -1, "UI8 bits of 1.5 - 1, R8 1.5");
    a = mk_ui8(bits + 1); b = mk_r8(1.5); CHECK2(a, b, 1, "UI8 bits of 1.5 + 1, R8 1.5");
    a = mk_ui8(bits); b = mk_date(1.5); CHECK(a, b, 0, "UI8 bits of 1.5, DATE 1.5");
    a = mk_ui8(bits + 1); b = mk_date(1.5); CHECK2(a, b, 1, "UI8 bits of 1.5 + 1, DATE 1.5");

    /* The sign bit of a zero does not count as a negative number */
    d = -0.0;
    memcpy(&bits, &d, sizeof(bits));
    a = mk_ui8(bits); b = mk_r8(d); CHECK(a, b, 0, "UI8 bits of -0, R8 -0");
    a = mk_ui8(bits - 1); b = mk_r8(d); CHECK2(a, b, -1, "UI8 bits of -0 - 1, R8 -0");
    a = mk_ui8(bits + 1); b = mk_r8(d); CHECK2(a, b, 1, "UI8 bits of -0 + 1, R8 -0");
}

/* Reals against 64-bit integers: Windows Server 2008 R2 on x86 compares the exact values, the others round to a double */
static void test_integer64_real(void)
{
    VARIANT a, b;
    BOOL exact;

    a = mk_i8(5); b = mk_r8(5.5); CHECK2(a, b, -1, "I8 5, R8 5.5");
    a = mk_i8(0); b = mk_r8(-0.5); CHECK2(a, b, 1, "I8 0, R8 -0.5");
    a = mk_i8(-5); b = mk_r8(-4.5); CHECK2(a, b, -1, "I8 -5, R8 -4.5");
    a = mk_i8(0x7fffffffffffffffLL); b = mk_r8(1e30); CHECK2(a, b, -1, "I8 max, R8 1e30");
    a = mk_i8(0x7fffffffffffffffLL); b = mk_r8(-1e30); CHECK2(a, b, 1, "I8 max, R8 -1e30");
    a = mk_i8(-0x7fffffffffffffffLL - 1); b = mk_r8(-9223372036854775808.0); CHECK(a, b, 0, "I8 min, R8 -2^63");
    a = mk_i8(-0x7fffffffffffffffLL - 1); b = mk_r8(-9223372036854777856.0); CHECK2(a, b, 1, "I8 min, R8 -2^63-2048");
    a = mk_i8(0x20000000000000LL); b = mk_r8(9007199254740992.0 + 2.0); CHECK2(a, b, -1, "I8 2^53, R8 2^53+2");
    a = mk_i8(0x20000000000000LL); b = mk_r8(9007199254740992.0); CHECK(a, b, 0, "I8 2^53, R8 2^53");

    /* Windows Server 2008 R2 on x86 compares the exact value (and so does ReactOS); Windows 10 on x86 and all x64 versions round to a double */
#ifdef _M_IX86
    exact = is_reactos() || GetNTVersion() <= _WIN32_WINNT_WIN7;
#else
    exact = FALSE;
#endif
    if (exact)
    {
        a = mk_i8(0x20000000000001LL); b = mk_r8(9007199254740992.0); CHECK2(a, b, 1, "I8 2^53+1, R8 2^53");
        a = mk_i8(0x20000000000001LL); b = mk_date(9007199254740992.0); CHECK2(a, b, 1, "I8 2^53+1, DATE 2^53");
        a = mk_i8(0x7fffffffffffffffLL); b = mk_r8(9223372036854775808.0); CHECK2(a, b, -1, "I8 max, R8 2^63");
        a = mk_i8(0x7fffffffffffffffLL); b = mk_r4(9223372036854775808.0f); CHECK2(a, b, -1, "I8 max, R4 2^63");
    }
    else
    {
        a = mk_i8(0x20000000000001LL); b = mk_r8(9007199254740992.0); CHECK(a, b, 0, "I8 2^53+1, R8 2^53");
        a = mk_i8(0x20000000000001LL); b = mk_date(9007199254740992.0); CHECK(a, b, 0, "I8 2^53+1, DATE 2^53");
        a = mk_i8(0x7fffffffffffffffLL); b = mk_r8(9223372036854775808.0); CHECK(a, b, 0, "I8 max, R8 2^63");
        a = mk_i8(0x7fffffffffffffffLL); b = mk_r4(9223372036854775808.0f); CHECK(a, b, 0, "I8 max, R4 2^63");
    }
}

/* Currency and decimals are compared exactly (and not as doubles), reals are rounded to their precision */
static void test_currency_decimal(void)
{
    VARIANT a, b;

    a = mk_cy(0x7fffffffffffffffLL); b = mk_cy(0x7ffffffffffffffeLL); CHECK2(a, b, 1, "CY max, CY max - 1");
    a = mk_cy(922337203685477580LL); b = mk_cy(922337203685477581LL); CHECK2(a, b, -1, "CY large, CY large + 1");
    a = mk_cy(0x20000000000001LL); b = mk_cy(0x20000000000002LL); CHECK2(a, b, -1, "CY 2^53+1, CY 2^53+2");
    a = mk_cy(-922337203685477580LL); b = mk_cy(-922337203685477581LL); CHECK2(a, b, 1, "CY -large, CY -large - 1");

    a = mk_cy(0x7fffffffffffffffLL); b = mk_r8(922337203685477.625); CHECK2(a, b, -1, "CY max, R8 above the range");
    a = mk_cy(0x7fffffffffffffffLL); b = mk_r8(922337203685477.5); CHECK2(a, b, 1, "CY max, R8 .5");
    a = mk_cy(1000); b = mk_r8(0.1); CHECK(a, b, 0, "CY 0.1, R8 0.1");
    a = mk_cy(1000); b = mk_r4(0.1f); CHECK(a, b, 0, "CY 0.1, R4 0.1");
    a = mk_cy(1000); b = mk_date(0.1); CHECK(a, b, 0, "CY 0.1, DATE 0.1");
    a = mk_cy(12346); b = mk_r8(1.2346); CHECK(a, b, 0, "CY 1.2346, R8 1.2346");
    a = mk_cy(12345678901234LL); b = mk_i8(1234567890); CHECK2(a, b, 1, "CY 1234567890.1234, I8 1234567890");
    a = mk_cy(12345678900000LL); b = mk_i8(1234567890); CHECK(a, b, 0, "CY 1234567890, I8 1234567890");
    a = mk_cy(0x7fffffffffffffffLL); b = mk_i8(92233720368547LL); CHECK2(a, b, 1, "CY max, I8 92233720368547");
    a = mk_cy(50000); b = mk_ui1(5); CHECK(a, b, 0, "CY 5, UI1 5");
    a = mk_cy(50000); b = mk_i2(5); CHECK(a, b, 0, "CY 5, I2 5");

    a = mk_decimal(0xffffffff, 0xffffffff, 0xfffffffe, 0); b = mk_decimal(0xffffffff, 0xffffffff, 0xffffffff, 0);
    CHECK2(a, b, -1, "DECIMAL 2^96-2, DECIMAL 2^96-1");
    a = mk_decimal(0, 1, 0, 0); b = mk_decimal(0, 1, 1, 0); CHECK2(a, b, -1, "DECIMAL 2^32, DECIMAL 2^32+1");
    a = mk_decimal(0, 0, 0x20000001, 0); b = mk_decimal(0, 0, 0x20000002, 0); CHECK2(a, b, -1, "DECIMAL 2^29+1, DECIMAL 2^29+2");
    a = mk_decimal(0, 0, 1, 28); b = mk_decimal(0, 0, 2, 28); CHECK2(a, b, -1, "DECIMAL 1e-28, DECIMAL 2e-28");
    a = mk_decimal(0, 0, 1, 5); b = mk_decimal(0, 0, 1, 6); CHECK2(a, b, 1, "DECIMAL 1e-5, DECIMAL 1e-6");
    a = mk_decimal(0, 0, 100, 2); b = mk_decimal(0, 0, 1, 0); CHECK(a, b, 0, "DECIMAL 1.00, DECIMAL 1");
    a = mk_decimal(0, 0, 5, 0); b = mk_decimal(0, 0, 5, 0); V_DECIMAL(&b).sign = DECIMAL_NEG; CHECK2(a, b, 1, "DECIMAL 5, DECIMAL -5");
    a = mk_decimal(0, 0, 0, 0); b = mk_decimal(0, 0, 0, 0); V_DECIMAL(&b).sign = DECIMAL_NEG; CHECK(a, b, 0, "DECIMAL 0, DECIMAL -0");
    a = mk_decimal(0xffffffff, 0xffffffff, 0xffffffff, 0); b = mk_r8(7.9228162514264337593543950336e28); CHECK2(a, b, -1, "DECIMAL 2^96-1, R8 2^96");
    a = mk_decimal(0xffffffff, 0xffffffff, 0xffffffff, 0); b = mk_r8(7.9e28); CHECK2(a, b, 1, "DECIMAL 2^96-1, R8 7.9e28");
    a = mk_decimal(0, 0, 1, 1); b = mk_r8(0.1); CHECK(a, b, 0, "DECIMAL 0.1, R8 0.1");
    a = mk_decimal(0, 0, 1, 1); b = mk_r4(0.1f); CHECK2(a, b, -1, "DECIMAL 0.1, R4 0.1");
    a = mk_decimal(0, 0, 1, 1); b = mk_date(0.1); CHECK(a, b, 0, "DECIMAL 0.1, DATE 0.1");
    a = mk_decimal(0, 0, 1, 28); b = mk_i4(0); CHECK2(a, b, 1, "DECIMAL 1e-28, I4 0");
    a = mk_decimal(0, 0, 1, 28); b = mk_r8(1e-28); CHECK(a, b, 0, "DECIMAL 1e-28, R8 1e-28");
    a = mk_decimal(0, 0x10000000, 0, 0); b = mk_i8(0x1000000000000000LL); CHECK(a, b, 0, "DECIMAL 2^60, I8 2^60");
    a = mk_decimal(0, 0x10000000, 0, 0); b = mk_i8(0x1000000000000001LL); CHECK2(a, b, -1, "DECIMAL 2^60, I8 2^60+1");
    a = mk_decimal(0, 0x10000000, 0, 0); b = mk_r8(1152921504606846976.0); CHECK2(a, b, -1, "DECIMAL 2^60, R8 2^60");
    a = mk_decimal(0, 0, 5, 0); b = mk_i8(5); CHECK(a, b, 0, "DECIMAL 5, I8 5");
    a = mk_decimal(0, 0, 5, 0); b = mk_r8(4.5); CHECK2(a, b, 1, "DECIMAL 5, R8 4.5");

    /* A currency against a decimal is exact */
    a = mk_cy(0x7fffffffffffffffLL); b = mk_decimal(0, 0x7fffffff, 0xffffffff, 4); CHECK(a, b, 0, "CY max, DECIMAL max");
    a = mk_cy(0x7ffffffffffffffeLL); b = mk_decimal(0, 0x7fffffff, 0xffffffff, 4); CHECK2(a, b, -1, "CY max - 1, DECIMAL max");
    a = mk_i8(922337203685477LL); b = mk_decimal(0, 0x7fffffff, 0xffffffff, 4); CHECK2(a, b, -1, "I8 922337203685477, DECIMAL max");
}

static void test_arrays(void)
{
    VARIANT empty = mk_empty(), null = mk_null(), i4 = mk_i4(5), str = mk_bstr(L"x"), a, b;
    LONGLONG one[1] = { 1 };

    {
        ARRAY(a1, VT_I4, 0, 5, 0, 0);
        ARRAY(b1, VT_I4, 0, 6, 0, 0);
        ARRAY(c1, VT_I4, 0, 5, 0, 0);
        CHECK2(a1, b1, -1, "I4 [5,0,0], [6,0,0]");
        CHECK(a1, c1, 0, "I4 [5,0,0], [5,0,0]");
        CHECK(a1, a1, 0, "I4 [5,0,0], itself");
        VariantClear(&a1); VariantClear(&b1); VariantClear(&c1);
    }
    {
        ARRAY(a1, VT_I4, 0, 0, 0, 5);
        ARRAY(b1, VT_I4, 0, 0, 0, 6);
        ARRAY(a2, VT_I4, 0, 0, 9, 0);
        ARRAY(b2, VT_I4, 0, 0, 1, 5);
        ARRAY(a3, VT_I4, 0, 9, 1, 0);
        ARRAY(b3, VT_I4, 0, 1, 9, 0);
        CHECK2(a1, b1, -1, "I4 [0,0,5], [0,0,6]");
        CHECK2(a2, b2, 1, "I4 [0,9,0], [0,1,5]: the first difference decides, not the last");
        CHECK2(a3, b3, 1, "I4 [9,1,0], [1,9,0]");
        VariantClear(&a1); VariantClear(&b1); VariantClear(&a2); VariantClear(&b2); VariantClear(&a3); VariantClear(&b3);
    }
    {
        ARRAY(neg, VT_I4, 0, -1);
        ARRAY(pos, VT_I4, 0, 1);
        ARRAY(one_, VT_I4, 0, 5);
        ARRAY(two, VT_I4, 0, 5, 0);
        ARRAY(nine, VT_I4, 0, 9);
        ARRAY(one_zero, VT_I4, 0, 1, 0);
        NO_ARRAY(none, VT_I4);
        NO_ARRAY(none2, VT_I4);
        ARRAY(two_b, VT_I4, 0, 1, 2);
        ARRAY(two_c, VT_I4, 0, 1, 3);
        CHECK2(neg, pos, -1, "I4 [-1], [1]: by value, not by bytes");
        CHECK2(one_, two, -1, "I4 [5], [5,0]: the shorter first");
        CHECK2(nine, one_zero, 1, "I4 [9], [1,0]: the elements before the length");
        CHECK2(none, pos, -1, "I4 [], [1]");
        CHECK(none, none2, 0, "I4 [], []");
        CHECK2(two_b, two_c, -1, "I4 [1,2], [1,3]");
        VariantClear(&neg); VariantClear(&pos); VariantClear(&one_); VariantClear(&two); VariantClear(&nine); VariantClear(&one_zero);
        VariantClear(&none); VariantClear(&none2); VariantClear(&two_b); VariantClear(&two_c);
    }
    {
        ARRAY(a1, VT_UI1, 0, 1, 2, 3);
        ARRAY(b1, VT_UI1, 0, 1, 2, 4);
        ARRAY(a2, VT_UI1, 0, 200);
        ARRAY(b2, VT_UI1, 0, 100);
        ARRAY(a3, VT_UI1, 0, 1);
        ARRAY(b3, VT_UI1, 0, 1, 0);
        CHECK2(a1, b1, -1, "UI1 [1,2,3], [1,2,4]");
        CHECK2(a2, b2, 1, "UI1 [200], [100]");
        CHECK2(a3, b3, -1, "UI1 [1], [1,0]");
        VariantClear(&a1); VariantClear(&b1); VariantClear(&a2); VariantClear(&b2); VariantClear(&a3); VariantClear(&b3);
    }
    {
        ARRAY(i2a, VT_I2, 0, 5, 9);
        ARRAY(i2b, VT_I2, 0, 6, 0);
        ARRAY(r8a, VT_R8, 0, 5);
        ARRAY(r8b, VT_R8, 0, 6);
        ARRAY(i4a, VT_I4, 0, 5);
        ARRAY(i2c, VT_I2, 0, 6);
        ARRAY(i2d, VT_I2, 0, 5);
        ARRAY(ui1, VT_UI1, 0, 5);
        ARRAY(i4b, VT_I4, 0, 6);
        CHECK2(i2a, i2b, -1, "I2 [5,9], [6,0]");
        CHECK2(r8a, r8b, -1, "R8 [5], [6]");
        CHECK2(i4a, i2c, -1, "I4 [5], I2 [6]: elements of different types compare by value");
        CHECK(i4a, i2d, 0, "I4 [5], I2 [5]");
        CHECK(i4a, ui1, 0, "I4 [5], UI1 [5]");
        CHECK2(i4b, i2d, 1, "I4 [6], I2 [5]");
        CHECK2(i4a, r8b, -1, "I4 [5], R8 [6]");
        VariantClear(&i2a); VariantClear(&i2b); VariantClear(&r8a); VariantClear(&r8b); VariantClear(&i4a);
        VariantClear(&i2c); VariantClear(&i2d); VariantClear(&ui1); VariantClear(&i4b);
    }
    {
        /* The rules for single values apply to the elements */
        ARRAY(ui4, VT_UI4, 0, (LONGLONG)0xfffffff0UL);
        ARRAY(i4, VT_I4, 0, 5);
        ARRAY(ui2, VT_UI2, 0, 5);
        ARRAY(boolean, VT_BOOL, 0, -1);
        ARRAY(i1a, VT_I1, 0, 5);
        ARRAY(i1b, VT_I1, 0, 6);
        ARRAY(ui8, VT_UI8, 0, 5);
        ARRAY(i8, VT_I8, 0, -6);
        ARRAY(r4, VT_R4, 0, 5);
        ARRAY(r8, VT_R8, 0, 6);
        ARRAY(bool0, VT_BOOL, 0, 0);
        ARRAY(i4_5, VT_I4, 0, 5);
        CHECK2(ui4, i4, 1, "UI4 [large], I4 [5]");
        CHECK(ui2, boolean, 0, "UI2 [5], BOOL [-1]");
        CHECK(i1a, i4_5, 0, "I1 [5], I4 [5]");
        CHECK2(i1a, i1b, -1, "I1 [5], I1 [6]");
        CHECK2(ui8, i8, 1, "UI8 [5], I8 [-6]");
        CHECK2(r4, r8, -1, "R4 [5], R8 [6]");
        CHECK2(boolean, bool0, -1, "BOOL [-1], BOOL [0]");
        VariantClear(&ui4); VariantClear(&i4); VariantClear(&ui2); VariantClear(&boolean); VariantClear(&i1a); VariantClear(&i1b);
        VariantClear(&ui8); VariantClear(&i8); VariantClear(&r4); VariantClear(&r8); VariantClear(&bool0); VariantClear(&i4_5);
    }
    {
        /* Arrays of strings, of currency, of decimals, of errors and of VT_INT */
        ARRAY(sa, VT_BSTR, 0, 0);
        ARRAY(sb, VT_BSTR, 0, 1);
        NO_ARRAY(sc, VT_BSTR);
        NO_ARRAY(sd, VT_BSTR);
        ARRAY(n5, VT_I4, 0, 5);
        ARRAY(n5b, VT_I4, 0, 5, 7);
        ARRAY(cy5, VT_CY, 0, 5);
        ARRAY(cy6, VT_CY, 0, 6);
        ARRAY(dec5, VT_DECIMAL, 0, 5);
        ARRAY(dec6, VT_DECIMAL, 0, 6);
        ARRAY(err5, VT_ERROR, 0, 5);
        ARRAY(err6, VT_ERROR, 0, 6);
        ARRAY(int5, VT_INT, 0, 5);
        ARRAY(int6, VT_INT, 0, 6);
        CHECK2(sa, sb, -1, "BSTR [0], [1]");
        CHECK(sc, sd, 0, "BSTR [], []");
        CHECK2(sc, n5, -1, "BSTR [], I4 [5]: the length decides when there is nothing to compare");
        CHECK2(n5, sb, -1, "I4 [5], BSTR [1]: numbers before strings");
        CHECK2(n5, n5b, -1, "I4 [5], I4 [5,7]");
        CHECK(cy5, cy6, 0, "CY [5], CY [6]");
        CHECK(dec5, dec6, 0, "DECIMAL [5], DECIMAL [6]");
        CHECK(err5, err6, 0, "ERROR [5], ERROR [6]");
        CHECK(int5, int6, 0, "INT [5], INT [6]");
        VariantClear(&sa); VariantClear(&sb); VariantClear(&sc); VariantClear(&sd); VariantClear(&n5); VariantClear(&n5b);
        VariantClear(&cy5); VariantClear(&cy6); VariantClear(&dec5); VariantClear(&dec6);
        VariantClear(&err5); VariantClear(&err6); VariantClear(&int5); VariantClear(&int6);
    }
    {
        /* Arrays of strings compare their strings, without regard to case */
        SAFEARRAYBOUND bound = { 2, 0 };
        SAFEARRAY *s1 = SafeArrayCreate(VT_BSTR, 1, &bound), *s2 = SafeArrayCreate(VT_BSTR, 1, &bound), *s3 = SafeArrayCreate(VT_BSTR, 1, &bound);
        LONG index = 0;
        BSTR abc = SysAllocString(L"abc"), abd = SysAllocString(L"abd"), upper = SysAllocString(L"ABC");

        SafeArrayPutElement(s1, &index, abc);
        SafeArrayPutElement(s2, &index, abd);
        SafeArrayPutElement(s3, &index, upper);
        a = mk_empty(); V_VT(&a) = VT_ARRAY | VT_BSTR; V_ARRAY(&a) = s1;
        b = mk_empty(); V_VT(&b) = VT_ARRAY | VT_BSTR; V_ARRAY(&b) = s2;
        CHECK2(a, b, -1, "BSTR [abc,0], [abd,0]");
        V_ARRAY(&b) = s3;
        CHECK(a, b, 0, "BSTR [abc,0], [ABC,0]");
        SafeArrayDestroy(s1); SafeArrayDestroy(s2); SafeArrayDestroy(s3);
        SysFreeString(abc); SysFreeString(abd); SysFreeString(upper);
    }
    {
        /* Multidimensional arrays sort below one-dimensional ones and are equal to each other */
        SAFEARRAYBOUND bounds[2] = { { 2, 0 }, { 2, 0 } };
        SAFEARRAY *m1 = SafeArrayCreate(VT_I4, 2, bounds), *m2 = SafeArrayCreate(VT_I4, 2, bounds);
        LONG index[2] = { 0, 0 }, value = 5;
        ARRAY(flat, VT_I4, 0, 5, 0, 0, 0);
        ARRAY(flat2, VT_I4, 0, 0, 0);

        SafeArrayPutElement(m1, index, &value);
        a = mk_empty(); V_VT(&a) = VT_ARRAY | VT_I4; V_ARRAY(&a) = m1;
        b = mk_empty(); V_VT(&b) = VT_ARRAY | VT_I4; V_ARRAY(&b) = m2;
        CHECK(a, b, 0, "two-dimensional, two-dimensional");
        CHECK2(a, flat, -1, "two-dimensional [5,0,0,0], one-dimensional [5,0,0,0]");
        CHECK2(a, flat2, -1, "two-dimensional, one-dimensional of 2");
        SafeArrayDestroy(m1); SafeArrayDestroy(m2);
        VariantClear(&flat); VariantClear(&flat2);
    }
    {
        /* The first index is not 0: equal */
        ARRAY(l1a, VT_I4, 1, 7, 0, 0);
        ARRAY(l1b, VT_I4, 1, 8, 0, 0);
        ARRAY(l0, VT_I4, 0, 0, 0, 0);
        CHECK(l1a, l1b, 0, "lower bound 1, lower bound 1");
        CHECK(l1a, l0, 0, "lower bound 1, lower bound 0");
        VariantClear(&l1a); VariantClear(&l1b); VariantClear(&l0);
    }
    {
        ARRAY(arr, VT_I4, 0, 5);
        ARRAY(arr_ui1, VT_UI1, 0, 5);
        VARIANT arr_null = mk_empty(), arr_ui1_null = mk_empty();
        VARIANT r8 = mk_r8(1.0), bo = mk_bool(VARIANT_TRUE), dt = mk_date(1.0), cy = mk_cy(10000), er = mk_error(1), i1 = mk_i1(1);

        V_VT(&arr_null) = VT_ARRAY | VT_I4;
        V_VT(&arr_ui1_null) = VT_ARRAY | VT_UI1;

        /* An array is not empty, and equal to every other kind of value */
        CHECK2(arr, empty, 1, "I4 [5], empty");
        CHECK2(arr, null, 1, "I4 [5], null");
        CHECK2(arr_ui1, empty, 1, "UI1 [5], empty");
        CHECK2(arr_null, empty, 1, "NULL I4 array, empty");
        CHECK2(arr_ui1_null, null, 1, "NULL UI1 array, null");
        CHECK(arr, i4, 0, "I4 [5], I4 5"); CHECK(i4, arr, 0, "I4 5, I4 [5]");
        CHECK(arr, str, 0, "I4 [5], string x"); CHECK(str, arr, 0, "string x, I4 [5]");
        CHECK(arr, r8, 0, "I4 [5], R8 1"); CHECK(r8, arr, 0, "R8 1, I4 [5]");
        CHECK(arr, bo, 0, "I4 [5], TRUE"); CHECK(bo, arr, 0, "TRUE, I4 [5]");
        CHECK(arr, dt, 0, "I4 [5], DATE 1"); CHECK(dt, arr, 0, "DATE 1, I4 [5]");
        CHECK(arr, cy, 0, "I4 [5], CY 1"); CHECK(cy, arr, 0, "CY 1, I4 [5]");
        CHECK(arr, er, 0, "I4 [5], ERROR"); CHECK(er, arr, 0, "ERROR, I4 [5]");
        CHECK(arr, i1, 0, "I4 [5], I1 1"); CHECK(i1, arr, 0, "I1 1, I4 [5]");

        /* A NULL array is an empty one */
        CHECK2(arr, arr_null, 1, "I4 [5], NULL I4 array");
        CHECK(arr_null, arr_null, 0, "NULL I4 array, NULL I4 array");
        CHECK(arr_null, arr_ui1_null, 0, "NULL I4 array, NULL UI1 array");
        {
            NO_ARRAY(none, VT_I4);
            CHECK(arr_null, none, 0, "NULL I4 array, empty I4 array");
            VariantClear(&none);
        }
        CHECK(arr, arr_ui1, 0, "I4 [5], UI1 [5]");
        VariantClear(&arr); VariantClear(&arr_ui1);
    }
    (void)a; (void)b; (void)one;
    VariantClear(&str);
}

static void test_unsupported(void)
{
    VARIANT a, b;
    LONG l = 5;

    /* The type VT_I1 compares only with itself */
    a = mk_i1(-1); b = mk_i1(5); CHECK2(a, b, -1, "I1 -1, I1 5");
    a = mk_i1(5); b = mk_i2(5); CHECK(a, b, 0, "I1 5, I2 5");
    a = mk_i1(5); b = mk_i2(-1); CHECK(a, b, 0, "I1 5, I2 -1");
    a = mk_i2(-1); b = mk_i1(5); CHECK(a, b, 0, "I2 -1, I1 5");
    b = mk_bstr(L"a"); a = mk_i1(5); CHECK(a, b, 0, "I1 5, string a");
    VariantClear(&b);

    /* An error compares only with an error, and these types are not compared at all */
    a = mk_error(5); b = mk_error(5); CHECK(a, b, 0, "ERROR, ERROR");
    a = mk_error(5); b = mk_error(6); CHECK2(a, b, -1, "ERROR 5, ERROR 6");
    a = mk_error(5); b = mk_i4(1); CHECK(a, b, 0, "ERROR, I4");
    a = mk_i4(1); b = mk_error(5); CHECK(a, b, 0, "I4, ERROR");
    a = mk_uint(5); b = mk_i4(1); CHECK(a, b, 0, "UINT 5, I4 1");
    a = mk_i4(100000); b = mk_uint(5); CHECK(a, b, 0, "I4 100000, UINT 5");

    a = mk_empty(); V_VT(&a) = VT_UNKNOWN;
    b = mk_i4(1); CHECK(a, b, 0, "UNKNOWN, I4");
    CHECK(b, a, 0, "I4, UNKNOWN");
    a = mk_empty(); V_VT(&a) = VT_BYREF | VT_I4; V_I4REF(&a) = &l;
    b = mk_i4(1); CHECK(a, b, 0, "BYREF I4, I4");
    CHECK(b, a, 0, "I4, BYREF I4");

    /* VT_INT as the left operand is a 32-bit integer; as the right one it is not compared */
    a = mk_empty(); V_VT(&a) = VT_INT; V_INT(&a) = 5;
    b = mk_i4(100000); CHECK(a, b, -1, "INT 5, I4 100000");
    CHECK(b, a, 0, "I4 100000, INT 5");

    /* Empty and null win over everything */
    a = mk_empty(); b = mk_error(5); CHECK(a, b, -1, "empty, ERROR");
    CHECK(b, a, 1, "ERROR, empty");
}

START_TEST(VariantCompare)
{
    HMODULE module = LoadLibraryA("propsys.dll");

    if (module)
        pVariantCompare = (VARIANTCOMPARE)GetProcAddress(module, "VariantCompare");
    if (!pVariantCompare)
    {
        skip("VariantCompare is not available\n");
        if (module)
            FreeLibrary(module);
        return;
    }

    test_empty();
    test_same_type();
    test_mixed_numbers();
    test_strings();
    test_number_and_string();
    test_unsigned64_real();
    test_integer64_real();
    test_currency_decimal();
    test_arrays();
    test_unsupported();

    FreeLibrary(module);
}
