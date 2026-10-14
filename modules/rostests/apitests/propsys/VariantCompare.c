/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for propsys!VariantCompare
 *
 * Every expected value was measured on Windows Server 2008 R2 (6.1.7601), x86 and x64.
 * Arrays are not covered: Windows compares them, ReactOS reports them as equal.
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
    test_unsupported();

    FreeLibrary(module);
}
