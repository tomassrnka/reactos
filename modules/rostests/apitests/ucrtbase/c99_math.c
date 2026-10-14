/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the C99 math functions of ucrtbase
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* The functions are called through pointers: the compiler does not know them and cannot fold the calls */

#include <apitest.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <limits.h>
#include <string.h>

#define DECL_D1(n) static double (__cdecl *p_##n)(double)
#define DECL_D2(n) static double (__cdecl *p_##n)(double, double)
#define DECL_F1(n) static float (__cdecl *p_##n)(float)
#define DECL_F2(n) static float (__cdecl *p_##n)(float, float)

DECL_D1(acosh); DECL_D1(asinh); DECL_D1(atanh); DECL_D1(cbrt); DECL_D1(erf); DECL_D1(erfc);
DECL_D1(expm1); DECL_D1(log1p); DECL_D1(rint); DECL_D1(nearbyint);
DECL_D2(copysign); DECL_D2(fdim); DECL_D2(fmax); DECL_D2(fmin); DECL_D2(remainder);
DECL_D1(lgamma); DECL_D1(lgammal); DECL_D1(tgamma); DECL_D1(tgammal); DECL_D1(logb); DECL_D1(logbl); DECL_D1(_logb);
DECL_F1(lgammaf); DECL_F1(tgammaf); DECL_F1(logbf); DECL_F1(_logbf);
DECL_F1(acoshf); DECL_F1(asinhf); DECL_F1(atanhf); DECL_F1(cbrtf); DECL_F1(erff); DECL_F1(erfcf);
DECL_F1(expm1f); DECL_F1(log1pf); DECL_F1(rintf); DECL_F1(nearbyintf);
DECL_F2(copysignf); DECL_F2(fdimf); DECL_F2(fmaxf); DECL_F2(fminf); DECL_F2(remainderf);
static double (__cdecl *p_nan)(const char *);
static float (__cdecl *p_nanf)(const char *);
static float (__cdecl *p_modff)(float, float *);
static int (__cdecl *p_ilogb)(double);
static int (__cdecl *p_ilogbf)(float);
static double (__cdecl *p_remquo)(double, double, int *);
static float (__cdecl *p_remquof)(float, float, int *);
static long (__cdecl *p_lround)(double);
static long (__cdecl *p_lroundf)(float);
static __int64 (__cdecl *p_llround)(double);
static __int64 (__cdecl *p_llroundf)(float);
static __int64 (__cdecl *p_llrint)(double);
static __int64 (__cdecl *p_llrintf)(float);
static unsigned int (__cdecl *p_statusfp)(void);
static unsigned int (__cdecl *p_clearfp)(void);
static unsigned int (__cdecl *p_controlfp)(unsigned int, unsigned int);

#define ok_near(Value, Expected, Eps) \
    do { double v_ = (Value), e_ = (Expected); \
         ok(fabs(v_ - e_) <= (Eps) * fabs(e_) || (e_ == 0 && fabs(v_) <= (Eps)), \
            #Value " = %.17g, expected %.17g\n", v_, e_); } while (0)

#define ok_nearf(Value, Expected, Eps) \
    do { float v_ = (Value), e_ = (Expected); \
         ok(fabsf(v_ - e_) <= (Eps) * fabsf(e_) || (e_ == 0 && fabsf(v_) <= (Eps)), \
            #Value " = %.9g, expected %.9g\n", (double)v_, (double)e_); } while (0)

/* A call with the errno that it leaves behind: SENTINEL when it does not touch errno */
#define SENTINEL 0x1234
#define CALL(Var, Expr) do { errno = SENTINEL; (Var) = (Expr); } while (0)
#define ok_errno(Expected) ok(errno == (Expected), "errno = %d, expected %d\n", errno, (Expected))

static BOOL is_neg(double x)
{
    return signbit(x) != 0;
}

static BOOL is_nan(double x)
{
    return x != x;
}

static void test_copysign(void)
{
    ok(p_copysign(3.0, -0.0) == -3.0, "copysign(3, -0) = %g\n", p_copysign(3.0, -0.0));
    ok(p_copysign(-3.0, 0.0) == 3.0, "copysign(-3, 0) = %g\n", p_copysign(-3.0, 0.0));
    ok(p_copysign(-3.0, -1.0) == -3.0, "copysign(-3, -1) = %g\n", p_copysign(-3.0, -1.0));
    ok(is_neg(p_copysign(0.0, -1.0)), "copysign(0, -1) is not negative\n");
    ok(!is_neg(p_copysign(-0.0, 1.0)), "copysign(-0, 1) is negative\n");
    ok(p_copysign(INFINITY, -1.0) == -INFINITY, "copysign(inf, -1) = %g\n", p_copysign(INFINITY, -1.0));
    ok(p_copysignf(2.5f, -1.0f) == -2.5f, "copysignf(2.5, -1) = %g\n", p_copysignf(2.5f, -1.0f));
    ok(p_copysignf(-2.5f, 1.0f) == 2.5f, "copysignf(-2.5, 1) = %g\n", p_copysignf(-2.5f, 1.0f));
}

static void test_cbrt(void)
{
    ok_near(p_cbrt(27.0), 3.0, 1e-14);
    ok_near(p_cbrt(-8.0), -2.0, 1e-14);
    ok_near(p_cbrt(1e300), 1e100, 1e-14);
    ok_near(p_cbrt(0.001), 0.1, 1e-14);
    ok(p_cbrt(0.0) == 0.0, "cbrt(0) = %g\n", p_cbrt(0.0));
    ok(is_neg(p_cbrt(-0.0)), "cbrt(-0) is not negative\n");
    ok(p_cbrt(INFINITY) == INFINITY, "cbrt(inf) = %g\n", p_cbrt(INFINITY));
    ok(is_nan(p_cbrt(NAN)), "cbrt(nan) is not a NaN\n");
    ok_nearf(p_cbrtf(27.0f), 3.0f, 1e-6f);
    ok_nearf(p_cbrtf(-1000.0f), -10.0f, 1e-6f);
}

static void test_expm1_log1p(void)
{
    double d;
    float f;

    ok(p_expm1(0.0) == 0.0, "expm1(0) = %g\n", p_expm1(0.0));
    ok_near(p_expm1(1.0), 1.718281828459045, 1e-15);
    ok_near(p_expm1(1e-10), 1.00000000005e-10, 1e-15);
    ok_near(p_expm1(-1.0), -0.6321205588285577, 1e-15);
    CALL(d, p_expm1(-INFINITY));
    ok(d == -1.0, "expm1(-inf) = %g\n", d);
    ok_errno(SENTINEL);
    CALL(d, p_expm1(INFINITY));
    ok(d == INFINITY, "expm1(inf) = %g\n", d);
    ok_errno(SENTINEL);
    CALL(d, p_expm1(1000.0));
    ok(d == INFINITY, "expm1(1000) = %g\n", d);
    ok_errno(ERANGE);
    CALL(d, p_expm1(-1000.0));
    ok(d == -1.0, "expm1(-1000) = %g\n", d);
    ok_errno(SENTINEL);
    ok_nearf(p_expm1f(1.0f), 1.7182818f, 1e-6f);
    CALL(f, p_expm1f(INFINITY));
    ok(f == INFINITY, "expm1f(inf) = %g\n", f);
    ok_errno(SENTINEL);
    CALL(f, p_expm1f(1000.0f));
    ok(f == INFINITY, "expm1f(1000) = %g\n", f);
    ok_errno(ERANGE);

    ok(p_log1p(0.0) == 0.0, "log1p(0) = %g\n", p_log1p(0.0));
    ok_near(p_log1p(1.0), 0.6931471805599453, 1e-15);
    ok_near(p_log1p(1e-10), 9.9999999995e-11, 1e-15);
    ok_near(p_log1p(-0.5), -0.6931471805599453, 1e-15);
    ok_nearf(p_log1pf(1.0f), 0.6931472f, 1e-6f);
    CALL(d, p_log1p(-1.0));
    ok(d == -INFINITY, "log1p(-1) = %g\n", d);
    ok_errno(ERANGE);
    CALL(d, p_log1p(-2.0));
    ok(is_nan(d), "log1p(-2) = %g\n", d);
    ok_errno(EDOM);
    CALL(d, p_log1p(NAN));
    ok(is_nan(d), "log1p(nan) = %g\n", d);
    ok_errno(SENTINEL);
    CALL(d, p_log1p(-NAN));
    ok(is_nan(d), "log1p(-nan) = %g\n", d);
    ok_errno(SENTINEL);
    CALL(d, p_log1p(INFINITY));
    ok(d == INFINITY, "log1p(inf) = %g\n", d);
    ok_errno(SENTINEL);
    CALL(f, p_log1pf(-1.0f));
    ok(f == -INFINITY, "log1pf(-1) = %g\n", f);
    ok_errno(ERANGE);
    CALL(f, p_log1pf(-2.0f));
    ok(is_nan(f), "log1pf(-2) = %g\n", f);
    ok_errno(EDOM);
}

static void test_hyperbolic_inverse(void)
{
    double d;
    float f;

    ok(p_acosh(1.0) == 0.0, "acosh(1) = %g\n", p_acosh(1.0));
    ok_near(p_acosh(2.0), 1.3169578969248166, 1e-15);
    ok_near(p_acosh(100.0), 5.298292365610484, 1e-15);
    ok_nearf(p_acoshf(2.0f), 1.3169579f, 1e-6f);
    CALL(d, p_acosh(0.5));
    ok(is_nan(d), "acosh(0.5) = %g\n", d);
    ok_errno(EDOM);
    CALL(d, p_acosh(-INFINITY));
    ok(is_nan(d), "acosh(-inf) = %g\n", d);
    ok_errno(EDOM);
    CALL(d, p_acosh(INFINITY));
    ok(d == INFINITY, "acosh(inf) = %g\n", d);
    ok_errno(SENTINEL);
    CALL(d, p_acosh(NAN));
    ok(is_nan(d), "acosh(nan) = %g\n", d);
    ok_errno(SENTINEL);
    CALL(f, p_acoshf(0.5f));
    ok(is_nan(f), "acoshf(0.5) = %g\n", f);
    ok_errno(EDOM);

    ok(p_asinh(0.0) == 0.0, "asinh(0) = %g\n", p_asinh(0.0));
    ok_near(p_asinh(1.0), 0.881373587019543, 1e-15);
    ok_near(p_asinh(-1.0), -0.881373587019543, 1e-15);
    ok_nearf(p_asinhf(1.0f), 0.8813736f, 1e-6f);
    CALL(d, p_asinh(INFINITY));
    ok(d == INFINITY, "asinh(inf) = %g\n", d);
    ok_errno(SENTINEL);

    ok(p_atanh(0.0) == 0.0, "atanh(0) = %g\n", p_atanh(0.0));
    ok_near(p_atanh(0.5), 0.5493061443340548, 1e-15);
    ok_near(p_atanh(-0.5), -0.5493061443340548, 1e-15);
    ok_nearf(p_atanhf(0.5f), 0.54930615f, 1e-6f);
    CALL(d, p_atanh(1.0));
    ok(d == INFINITY, "atanh(1) = %g\n", d);
    ok_errno(ERANGE);
    CALL(d, p_atanh(-1.0));
    ok(d == -INFINITY, "atanh(-1) = %g\n", d);
    ok_errno(ERANGE);
    CALL(d, p_atanh(2.0));
    ok(is_nan(d), "atanh(2) = %g\n", d);
    ok_errno(EDOM);
    CALL(f, p_atanhf(2.0f));
    ok(is_nan(f), "atanhf(2) = %g\n", f);
    ok_errno(EDOM);
    CALL(d, p_atanh(INFINITY));
    ok(is_nan(d), "atanh(inf) = %g\n", d);
    ok_errno(EDOM);
    CALL(d, p_atanh(-INFINITY));
    ok(is_nan(d), "atanh(-inf) = %g\n", d);
    ok_errno(EDOM);
    CALL(f, p_atanhf(-INFINITY));
    ok(is_nan(f), "atanhf(-inf) = %g\n", f);
    ok_errno(EDOM);
    CALL(d, p_atanh(NAN));
    ok(is_nan(d), "atanh(nan) = %g\n", d);
    ok_errno(SENTINEL);
}

static void test_erf(void)
{
    double d;
    float f;

    ok(p_erf(0.0) == 0.0, "erf(0) = %g\n", p_erf(0.0));
    ok_near(p_erf(1.0), 0.8427007929497149, 1e-15);
    ok_near(p_erf(-0.5), -0.5204998778130465, 1e-15);
    ok(p_erf(INFINITY) == 1.0, "erf(inf) = %g\n", p_erf(INFINITY));
    ok_near(p_erfc(1.0), 0.15729920705028513, 1e-14);
    ok(p_erfc(0.0) == 1.0, "erfc(0) = %g\n", p_erfc(0.0));
    ok(p_erfc(INFINITY) == 0.0, "erfc(inf) = %g\n", p_erfc(INFINITY));
    ok_near(p_erfc(-1.0), 1.8427007929497148, 1e-15);
    ok_nearf(p_erff(1.0f), 0.84270079f, 1e-6f);
    ok_nearf(p_erfcf(1.0f), 0.15729921f, 1e-5f);
    CALL(d, p_erfc(30.0));
    ok(d == 0.0, "erfc(30) = %g\n", d);
    ok_errno(ERANGE);
    CALL(f, p_erfcf(11.0f));
    ok(f == 0.0f, "erfcf(11) = %g\n", f);
    ok_errno(ERANGE);
    /* the results just before the cut-off at 28 underflow too */
    CALL(f, p_erfcf(10.1f));
    ok(f == 0.0f, "erfcf(10.1) = %g\n", f);
    ok_errno(ERANGE);
    CALL(d, p_erfc(27.25));
    ok(d == 0.0, "erfc(27.25) = %g\n", d);
    ok_errno(ERANGE);
}

static void test_fdim_fmax_fmin(void)
{
    ok(p_fdim(5.0, 3.0) == 2.0, "fdim(5, 3) = %g\n", p_fdim(5.0, 3.0));
    ok(p_fdim(3.0, 5.0) == 0.0, "fdim(3, 5) = %g\n", p_fdim(3.0, 5.0));
    ok(is_nan(p_fdim(NAN, 1.0)), "fdim(nan, 1) is not a NaN\n");
    ok(p_fdimf(5.0f, 3.0f) == 2.0f, "fdimf(5, 3) = %g\n", p_fdimf(5.0f, 3.0f));
    ok(p_fmax(1.0, 2.0) == 2.0, "fmax(1, 2) = %g\n", p_fmax(1.0, 2.0));
    ok(p_fmax(-1.0, -2.0) == -1.0, "fmax(-1, -2) = %g\n", p_fmax(-1.0, -2.0));
    ok(p_fmax(NAN, 2.0) == 2.0, "fmax(nan, 2) = %g\n", p_fmax(NAN, 2.0));
    ok(p_fmax(2.0, NAN) == 2.0, "fmax(2, nan) = %g\n", p_fmax(2.0, NAN));
    ok(p_fmaxf(1.0f, 2.0f) == 2.0f, "fmaxf(1, 2) = %g\n", p_fmaxf(1.0f, 2.0f));
    ok(p_fmin(1.0, 2.0) == 1.0, "fmin(1, 2) = %g\n", p_fmin(1.0, 2.0));
    ok(p_fmin(-1.0, -2.0) == -2.0, "fmin(-1, -2) = %g\n", p_fmin(-1.0, -2.0));
    ok(p_fmin(NAN, 2.0) == 2.0, "fmin(nan, 2) = %g\n", p_fmin(NAN, 2.0));
    ok(p_fmin(2.0, NAN) == 2.0, "fmin(2, nan) = %g\n", p_fmin(2.0, NAN));
    ok(p_fminf(1.0f, 2.0f) == 1.0f, "fminf(1, 2) = %g\n", p_fminf(1.0f, 2.0f));
}

static void test_ilogb(void)
{
    int i;

    ok(p_ilogb(8.0) == 3, "ilogb(8) = %d\n", p_ilogb(8.0));
    ok(p_ilogb(1.0) == 0, "ilogb(1) = %d\n", p_ilogb(1.0));
    ok(p_ilogb(0.5) == -1, "ilogb(0.5) = %d\n", p_ilogb(0.5));
    ok(p_ilogb(1000.0) == 9, "ilogb(1000) = %d\n", p_ilogb(1000.0));
    ok(p_ilogb(-16.0) == 4, "ilogb(-16) = %d\n", p_ilogb(-16.0));
    ok(p_ilogbf(10.0f) == 3, "ilogbf(10) = %d\n", p_ilogbf(10.0f));
    CALL(i, p_ilogb(0.0));
    ok(i == INT_MIN, "ilogb(0) = %d\n", i);
    ok_errno(SENTINEL);
    CALL(i, p_ilogb(INFINITY));
    ok(i == INT_MAX, "ilogb(inf) = %d\n", i);
    ok_errno(SENTINEL);
    CALL(i, p_ilogb(NAN));
    ok(i == INT_MAX, "ilogb(nan) = %d\n", i);
    ok_errno(SENTINEL);
}

static void test_remainder(void)
{
    double d;
    int q;

    ok(p_remainder(5.0, 3.0) == -1.0, "remainder(5, 3) = %g\n", p_remainder(5.0, 3.0));
    ok(p_remainder(7.0, 2.0) == -1.0, "remainder(7, 2) = %g\n", p_remainder(7.0, 2.0));
    ok(p_remainder(1.0, 3.0) == 1.0, "remainder(1, 3) = %g\n", p_remainder(1.0, 3.0));
    ok(p_remainder(-5.0, 3.0) == 1.0, "remainder(-5, 3) = %g\n", p_remainder(-5.0, 3.0));
    ok(p_remainderf(5.0f, 3.0f) == -1.0f, "remainderf(5, 3) = %g\n", p_remainderf(5.0f, 3.0f));
    CALL(d, p_remainder(1.0, 0.0));
    ok(is_nan(d), "remainder(1, 0) = %g\n", d);
    ok_errno(EDOM);
    CALL(d, p_remainder(INFINITY, 1.0));
    ok(is_nan(d), "remainder(inf, 1) = %g\n", d);
    ok_errno(EDOM);

    q = 0x1234;
    ok(p_remquo(5.0, 3.0, &q) == -1.0, "remquo(5, 3)\n");
    ok(q == 2, "q = %d\n", q);
    q = 0x1234;
    ok(p_remquo(-10.0, 3.0, &q) == -1.0, "remquo(-10, 3)\n");
    ok(q == -3, "q = %d\n", q);
    q = 0x1234;
    ok(p_remquof(5.0f, 3.0f, &q) == -1.0f, "remquof(5, 3)\n");
    ok(q == 2, "q = %d\n", q);
    q = 0x1234;
    CALL(d, p_remquo(1.0, 0.0, &q));
    ok(is_nan(d), "remquo(1, 0) = %g\n", d);
    ok_errno(EDOM);
    ok(q == 0, "q = %d\n", q);
}

static void test_rounding(void)
{
    __int64 i64;
    long l;

    ok(p_rint(2.5) == 2.0, "rint(2.5) = %g\n", p_rint(2.5));
    ok(p_rint(3.5) == 4.0, "rint(3.5) = %g\n", p_rint(3.5));
    ok(p_rint(-2.5) == -2.0, "rint(-2.5) = %g\n", p_rint(-2.5));
    ok(p_rint(2.4) == 2.0, "rint(2.4) = %g\n", p_rint(2.4));
    ok(p_rint(2.6) == 3.0, "rint(2.6) = %g\n", p_rint(2.6));
    ok(p_rint(1.0) == 1.0, "rint(1) = %g\n", p_rint(1.0));
    ok(p_rint(4503599627370497.0) == 4503599627370497.0, "rint(2^52 + 1) = %.17g\n", p_rint(4503599627370497.0));
    ok(is_neg(p_rint(-0.4)), "rint(-0.4) is not negative\n");
    ok(p_rintf(1.5f) == 2.0f, "rintf(1.5) = %g\n", p_rintf(1.5f));
    ok(p_rintf(0.5f) == 0.0f, "rintf(0.5) = %g\n", p_rintf(0.5f));
    ok(p_rintf(2.5f) == 2.0f, "rintf(2.5) = %g\n", p_rintf(2.5f));
    ok(p_rintf(-1.5f) == -2.0f, "rintf(-1.5) = %g\n", p_rintf(-1.5f));
    ok(p_rintf(8388609.0f) == 8388609.0f, "rintf(2^23 + 1) = %.9g\n", p_rintf(8388609.0f));
    ok(p_nearbyint(2.5) == 2.0, "nearbyint(2.5) = %g\n", p_nearbyint(2.5));
    ok(p_nearbyint(-3.5) == -4.0, "nearbyint(-3.5) = %g\n", p_nearbyint(-3.5));
    ok(p_nearbyintf(2.5f) == 2.0f, "nearbyintf(2.5) = %g\n", p_nearbyintf(2.5f));
    ok(p_nearbyintf(3.5f) == 4.0f, "nearbyintf(3.5) = %g\n", p_nearbyintf(3.5f));

    ok(p_lround(2.5) == 3, "lround(2.5) = %ld\n", p_lround(2.5));
    ok(p_lround(-2.5) == -3, "lround(-2.5) = %ld\n", p_lround(-2.5));
    ok(p_lround(2.4) == 2, "lround(2.4) = %ld\n", p_lround(2.4));
    ok(p_lround(0.49999999999999994) == 0, "lround(0.49999999999999994) = %ld\n", p_lround(0.49999999999999994));
    ok(p_lroundf(2.5f) == 3, "lroundf(2.5) = %ld\n", p_lroundf(2.5f));
    ok(p_lroundf(-0.4f) == 0, "lroundf(-0.4) = %ld\n", p_lroundf(-0.4f));
    ok(p_llround(2.5) == 3, "llround(2.5) = %I64d\n", p_llround(2.5));
    ok(p_llround(-2.5) == -3, "llround(-2.5) = %I64d\n", p_llround(-2.5));
    ok(p_llround(1e15 + 0.5) == 1000000000000001LL, "llround(1e15 + 0.5) = %I64d\n", p_llround(1e15 + 0.5));
    ok(p_llround(4503599627370497.0) == 4503599627370497LL, "llround(2^52 + 1) = %I64d\n", p_llround(4503599627370497.0));
    ok(p_llroundf(-3.5f) == -4, "llroundf(-3.5) = %I64d\n", p_llroundf(-3.5f));
    ok(p_llrint(2.5) == 2, "llrint(2.5) = %I64d\n", p_llrint(2.5));
    ok(p_llrint(3.5) == 4, "llrint(3.5) = %I64d\n", p_llrint(3.5));
    ok(p_llrint(-2.5) == -2, "llrint(-2.5) = %I64d\n", p_llrint(-2.5));
    ok(p_llrint(1e12) == 1000000000000LL, "llrint(1e12) = %I64d\n", p_llrint(1e12));
    ok(p_llrintf(1.5f) == 2, "llrintf(1.5) = %I64d\n", p_llrintf(1.5f));

    /* Results that do not fit: zero and EDOM */
    CALL(l, p_lround(1e20));
    ok(l == 0, "lround(1e20) = %ld\n", l);
    ok_errno(EDOM);
    CALL(i64, p_llround(1e30));
    ok(i64 == 0, "llround(1e30) = %I64d\n", i64);
    ok_errno(EDOM);
    CALL(i64, p_llrint(1e30));
    ok(i64 == 0, "llrint(1e30) = %I64d\n", i64);
    ok_errno(EDOM);
    CALL(i64, p_llround(NAN));
    ok(i64 == 0, "llround(nan) = %I64d\n", i64);
    ok_errno(EDOM);
}

static void test_rounding_modes(void)
{
    static const struct
    {
        unsigned int mode;
        double a, b, c, d;     /* rint of -1.25, 1.25, -1.5 and 2.5 */
    } tests[] =
    {
        { _RC_NEAR, -1.0, 1.0, -2.0, 2.0 },
        { _RC_DOWN, -2.0, 1.0, -2.0, 2.0 },
        { _RC_UP,   -1.0, 2.0, -1.0, 3.0 },
        { _RC_CHOP, -1.0, 1.0, -1.0, 2.0 },
    };
    unsigned int old, i;

    if (!p_controlfp)
        return;
    old = p_controlfp(0, 0) & _MCW_RC;
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
        p_controlfp(tests[i].mode, _MCW_RC);
        ok(p_rint(-1.25) == tests[i].a, "mode 0x%x: rint(-1.25) = %g\n", tests[i].mode, p_rint(-1.25));
        ok(p_rint(1.25) == tests[i].b, "mode 0x%x: rint(1.25) = %g\n", tests[i].mode, p_rint(1.25));
        ok(p_rint(-1.5) == tests[i].c, "mode 0x%x: rint(-1.5) = %g\n", tests[i].mode, p_rint(-1.5));
        ok(p_rint(2.5) == tests[i].d, "mode 0x%x: rint(2.5) = %g\n", tests[i].mode, p_rint(2.5));
        ok(p_rintf(-1.25f) == (float)tests[i].a, "mode 0x%x: rintf(-1.25) = %g\n", tests[i].mode, p_rintf(-1.25f));
        ok(p_rintf(2.5f) == (float)tests[i].d, "mode 0x%x: rintf(2.5) = %g\n", tests[i].mode, p_rintf(2.5f));
        ok(p_nearbyint(-1.25) == tests[i].a, "mode 0x%x: nearbyint(-1.25) = %g\n", tests[i].mode, p_nearbyint(-1.25));
        ok(p_nearbyint(2.5) == tests[i].d, "mode 0x%x: nearbyint(2.5) = %g\n", tests[i].mode, p_nearbyint(2.5));
        ok(p_nearbyintf(-1.25f) == (float)tests[i].a, "mode 0x%x: nearbyintf(-1.25) = %g\n", tests[i].mode, p_nearbyintf(-1.25f));
        ok(p_llrint(-1.25) == (__int64)tests[i].a, "mode 0x%x: llrint(-1.25) = %I64d\n", tests[i].mode, p_llrint(-1.25));
        ok(p_llrint(2.5) == (__int64)tests[i].d, "mode 0x%x: llrint(2.5) = %I64d\n", tests[i].mode, p_llrint(2.5));
        ok(p_llrintf(-1.25f) == (__int64)tests[i].a, "mode 0x%x: llrintf(-1.25) = %I64d\n", tests[i].mode, p_llrintf(-1.25f));
    }
    p_controlfp(old, _MCW_RC);
}

static void test_nearbyint_flags(void)
{
    unsigned int sw;

    p_clearfp();
    p_nearbyint(2.5);
    sw = p_statusfp();
    ok(!(sw & _SW_INEXACT), "nearbyint raised the inexact flag: 0x%x\n", sw);
    p_clearfp();
    p_nearbyintf(1.25f);
    sw = p_statusfp();
    ok(!(sw & _SW_INEXACT), "nearbyintf raised the inexact flag: 0x%x\n", sw);
    p_clearfp();
    p_rint(1.0);
    sw = p_statusfp();
    ok(!(sw & _SW_INVALID), "rint(1) raised the invalid flag: 0x%x\n", sw);
}

static void test_modff(void)
{
    float i, f;

    if (!p_modff)
        return;     /* the i386 ucrtbase does not export it */

    f = p_modff(2.5f, &i);
    ok(f == 0.5f && i == 2.0f, "modff(2.5) = %g, %g\n", f, i);
    f = p_modff(-2.5f, &i);
    ok(f == -0.5f && i == -2.0f, "modff(-2.5) = %g, %g\n", f, i);
    f = p_modff(-1.0f, &i);
    ok(f == 0.0f && is_neg(f) && i == -1.0f, "modff(-1) = %g (negative %d), %g\n", f, is_neg(f), i);
    f = p_modff(0.25f, &i);
    ok(f == 0.25f && i == 0.0f, "modff(0.25) = %g, %g\n", f, i);
    f = p_modff(-0.25f, &i);
    ok(f == -0.25f && is_neg(i), "modff(-0.25) = %g, %g\n", f, i);
    f = p_modff(INFINITY, &i);
    ok(f == 0.0f && i == INFINITY, "modff(inf) = %g, %g\n", f, i);
}

/* The status flags are read right after the call, before a comparison can raise anything */
#define NO_SPURIOUS(sw) (!((sw) & (_SW_OVERFLOW | _SW_INEXACT)))

static void test_exception_flags(void)
{
    unsigned int sw;
    double d;
    float f;
    long l;
    __int64 i64;
    int q;
    union { double d; unsigned __int64 i; } snan = { 0 };
    union { float f; unsigned int i; } snanf = { 0 };
    union { double d; unsigned __int64 i; } snan_back = { 0 };
    union { float f; unsigned int i; } snanf_back = { 0 };

    snan.i = 0x7ff0000000000001ULL;
    snanf.i = 0x7f800001;
    (void)snan.d;
    (void)snanf.f;
    (void)snan_back.d;
    (void)snanf_back.f;

    /* a result that does not fit is an invalid conversion */
    p_clearfp(); l = p_lround(1e20); sw = p_statusfp();
    ok(l == 0 && (sw & _SW_INVALID), "lround(1e20) = %ld, flags 0x%x\n", l, sw);
    p_clearfp(); l = p_lroundf(1e20f); sw = p_statusfp();
    ok(l == 0 && (sw & _SW_INVALID), "lroundf(1e20) = %ld, flags 0x%x\n", l, sw);
    p_clearfp(); i64 = p_llround(1e30); sw = p_statusfp();
    ok(i64 == 0 && (sw & _SW_INVALID), "llround(1e30) = %I64d, flags 0x%x\n", i64, sw);
    p_clearfp(); i64 = p_llroundf(1e30f); sw = p_statusfp();
    ok(i64 == 0 && (sw & _SW_INVALID), "llroundf(1e30) = %I64d, flags 0x%x\n", i64, sw);
    p_clearfp(); i64 = p_llrint(1e30); sw = p_statusfp();
    ok(i64 == 0 && (sw & _SW_INVALID), "llrint(1e30) = %I64d, flags 0x%x\n", i64, sw);
    p_clearfp(); i64 = p_llrintf(1e30f); sw = p_statusfp();
    ok(i64 == 0 && (sw & _SW_INVALID), "llrintf(1e30) = %I64d, flags 0x%x\n", i64, sw);
    p_clearfp(); i64 = p_llround(NAN); sw = p_statusfp();
    ok(i64 == 0 && (sw & _SW_INVALID), "llround(nan) = %I64d, flags 0x%x\n", i64, sw);

    /* on i386 a signaling NaN raises invalid and comes back quiet (frndint); the arguments are passed as integers
       so that loading them cannot raise the flag first */
#ifdef _M_IX86
    {
        double (__cdecl *rint_bits)(unsigned int, unsigned int);
        float (__cdecl *rintf_bits)(unsigned int);
        double (__cdecl *nearbyint_bits)(unsigned int, unsigned int);
        float (__cdecl *nearbyintf_bits)(unsigned int);

        *(void **)&rint_bits = (void *)p_rint;
        *(void **)&rintf_bits = (void *)p_rintf;
        *(void **)&nearbyint_bits = (void *)p_nearbyint;
        *(void **)&nearbyintf_bits = (void *)p_nearbyintf;
        p_clearfp(); d = rint_bits(1, 0x7ff00000); sw = p_statusfp();
        ok(is_nan(d) && (sw & _SW_INVALID), "rint(snan) = %g, flags 0x%x\n", d, sw);
        p_clearfp(); f = rintf_bits(0x7f800001); sw = p_statusfp();
        ok(is_nan(f) && (sw & _SW_INVALID), "rintf(snan) = %g, flags 0x%x\n", (double)f, sw);
        p_clearfp(); d = nearbyint_bits(1, 0x7ff00000); sw = p_statusfp();
        ok(is_nan(d) && (sw & _SW_INVALID), "nearbyint(snan) = %g, flags 0x%x\n", d, sw);
        p_clearfp(); f = nearbyintf_bits(0x7f800001); sw = p_statusfp();
        ok(is_nan(f) && (sw & _SW_INVALID), "nearbyintf(snan) = %g, flags 0x%x\n", (double)f, sw);
    }
#else
    /* the SSE routines of Windows pass a signaling NaN through unchanged and raise nothing */
    p_clearfp(); d = p_rint(snan.d); sw = p_statusfp(); snan_back.d = d;
    ok(snan_back.i == snan.i && !(sw & _SW_INVALID), "rint(snan) = 0x%I64x, flags 0x%x\n", snan_back.i, sw);
    p_clearfp(); f = p_rintf(snanf.f); sw = p_statusfp(); snanf_back.f = f;
    ok(snanf_back.i == snanf.i && !(sw & _SW_INVALID), "rintf(snan) = 0x%x, flags 0x%x\n", snanf_back.i, sw);
    p_clearfp(); d = p_nearbyint(snan.d); sw = p_statusfp(); snan_back.d = d;
    ok(snan_back.i == snan.i && !(sw & _SW_INVALID), "nearbyint(snan) = 0x%I64x, flags 0x%x\n", snan_back.i, sw);
    p_clearfp(); f = p_nearbyintf(snanf.f); sw = p_statusfp(); snanf_back.f = f;
    ok(snanf_back.i == snanf.i && !(sw & _SW_INVALID), "nearbyintf(snan) = 0x%x, flags 0x%x\n", snanf_back.i, sw);
#endif

    /* an erfc that underflows to zero */
    p_clearfp(); errno = SENTINEL; f = p_erfcf(30.0f); sw = p_statusfp();
    ok(f == 0.0f && errno == ERANGE && (sw & _SW_UNDERFLOW), "erfcf(30) = %g, errno %d, flags 0x%x\n", (double)f, errno, sw);
    /* the result is rounded to its type before it is returned, also where the x87 stack would hold more */
    p_clearfp(); d = p_erfcf(30.0f); sw = p_statusfp();
    ok(d == 0.0 && (sw & _SW_UNDERFLOW), "(double)erfcf(30) = %g, flags 0x%x\n", d, sw);
    p_clearfp(); errno = SENTINEL; d = p_erfc(30.0); sw = p_statusfp();
    ok(d == 0.0 && errno == ERANGE && (sw & _SW_UNDERFLOW), "erfc(30) = %g, errno %d, flags 0x%x\n", d, errno, sw);

    /* an infinite divisor leaves the dividend alone; Windows raises overflow and inexact in remquo (not tested),
       remquof and remainder raise nothing */
    q = 0x1234;
    p_clearfp(); d = p_remquo(DBL_MAX, INFINITY, &q); sw = p_statusfp();
    ok(d == DBL_MAX && q == 0, "remquo(max, inf) = %g, q = %d, flags 0x%x\n", d, q, sw);
    q = 0x1234;
    p_clearfp(); f = p_remquof(FLT_MAX, INFINITY, &q); sw = p_statusfp();
    ok(f == FLT_MAX && q == 0 && NO_SPURIOUS(sw), "remquof(max, inf) = %g, q = %d, flags 0x%x\n", (double)f, q, sw);
    p_clearfp(); d = p_remainder(DBL_MAX, INFINITY); sw = p_statusfp();
    ok(d == DBL_MAX && NO_SPURIOUS(sw), "remainder(max, inf) = %g, flags 0x%x\n", d, sw);
    p_clearfp(); f = p_remainderf(FLT_MAX, INFINITY); sw = p_statusfp();
    ok(f == FLT_MAX && NO_SPURIOUS(sw), "remainderf(max, inf) = %g, flags 0x%x\n", (double)f, sw);
    p_clearfp(); d = p_remainder(-DBL_MAX, -INFINITY); sw = p_statusfp();
    ok(d == -DBL_MAX && NO_SPURIOUS(sw), "remainder(-max, -inf) = %g, flags 0x%x\n", d, sw);
}

/* The expected values are those of Windows 10; the results of the transcendental functions differ in the last
   bits between the Windows versions and architectures, the errno and the special values do not */
static void test_gamma(void)
{
    double d, inf = INFINITY, qnan = p_nan("");
    float f;
    int i;
    static const double pole_args[] = { 0.0, -0.0, -1.0, -2.0, -3.0, -1e15, -9007199254740992.0 };
    static const double overflow_args[] = { 2.6e305, 1.79e308 };

    CALL(d, p_lgamma(1.0)); ok(d == 0.0, "lgamma(1) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_lgamma(2.0)); ok(d == 0.0, "lgamma(2) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_lgamma(3.0)); ok_near(d, 0.69314718055994529, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_lgamma(0.5)); ok_near(d, 0.57236494292470008, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_lgamma(10.0)); ok_near(d, 12.801827480081469, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_lgamma(100.0)); ok_near(d, 359.13420536957545, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_lgamma(171.5)); ok_near(d, 709.14316303092824, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_lgamma(-0.5)); ok_near(d, 1.2655121234846454, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_lgamma(-1.5)); ok_near(d, 0.86004701537648098, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_lgamma(-2.5)); ok_near(d, -0.056243716497674054, 1e-12); ok_errno(SENTINEL);
    CALL(d, p_lgamma(-175.5)); ok_near(d, -733.77915503209169, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_lgamma(1e-300)); ok_near(d, 690.77552789821368, 1e-14); ok_errno(SENTINEL);
    for (i = 0; i < (int)(sizeof(pole_args) / sizeof(*pole_args)); i++)
    {
        CALL(d, p_lgamma(pole_args[i]));
        ok(d == inf, "lgamma(%g) = %g\n", pole_args[i], d);
        ok_errno(ERANGE);
    }
    for (i = 0; i < (int)(sizeof(overflow_args) / sizeof(*overflow_args)); i++)
    {
        CALL(d, p_lgamma(overflow_args[i]));
        ok(d == inf, "lgamma(%g) = %g\n", overflow_args[i], d);
        ok_errno(ERANGE);
    }
    CALL(d, p_lgamma(inf)); ok(d == inf, "lgamma(inf) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_lgamma(-inf)); ok(d == inf, "lgamma(-inf) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_lgamma(qnan)); ok(is_nan(d), "lgamma(nan) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_lgammal(0.0)); ok(d == inf, "lgammal(0) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_lgammal(10.0)); ok_near(d, 12.801827480081469, 1e-14); ok_errno(SENTINEL);

    CALL(f, p_lgammaf(1.0f)); ok(f == 0.0f, "lgammaf(1) = %g\n", (double)f); ok_errno(SENTINEL);
    CALL(f, p_lgammaf(0.5f)); ok_nearf(f, 0.572364943f, 1e-5); ok_errno(SENTINEL);
    CALL(f, p_lgammaf(10.0f)); ok_nearf(f, 12.8018274f, 1e-5); ok_errno(SENTINEL);
    CALL(f, p_lgammaf(-1.5f)); ok_nearf(f, 0.860047f, 1e-5); ok_errno(SENTINEL);
    CALL(f, p_lgammaf(0.0f)); ok(f == INFINITY, "lgammaf(0) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_lgammaf(-0.0f)); ok(f == INFINITY, "lgammaf(-0) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_lgammaf(-2.0f)); ok(f == INFINITY, "lgammaf(-2) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_lgammaf(-1e10f)); ok(f == INFINITY, "lgammaf(-1e10) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_lgammaf(4.1e36f)); ok(f == INFINITY, "lgammaf(4.1e36) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_lgammaf(1e38f)); ok(f == INFINITY, "lgammaf(1e38) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_lgammaf(4e36f)); ok(f < INFINITY && f > 3e38f, "lgammaf(4e36) = %g\n", (double)f); ok_errno(SENTINEL);
    CALL(f, p_lgammaf(INFINITY)); ok(f == INFINITY, "lgammaf(inf) = %g\n", (double)f); ok_errno(SENTINEL);
    CALL(f, p_lgammaf(-INFINITY)); ok(f == INFINITY, "lgammaf(-inf) = %g\n", (double)f); ok_errno(SENTINEL);

    CALL(d, p_tgamma(1.0)); ok_near(d, 1.0, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_tgamma(3.0)); ok_near(d, 2.0, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_tgamma(5.0)); ok_near(d, 24.0, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_tgamma(10.0)); ok_near(d, 362880.0, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_tgamma(0.5)); ok_near(d, 1.7724538509055161, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_tgamma(100.0)); ok_near(d, 9.3326215443944153e+155, 1e-13); ok_errno(SENTINEL);
    CALL(d, p_tgamma(171.0)); ok_near(d, 7.2574156153079965e+306, 1e-13); ok_errno(SENTINEL);
    CALL(d, p_tgamma(171.5)); ok_near(d, 9.483367566824799e+307, 1e-13); ok_errno(SENTINEL);
    CALL(d, p_tgamma(-0.5)); ok_near(d, -3.5449077018110322, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_tgamma(-1.5)); ok_near(d, 2.3632718012073548, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_tgamma(-2.5)); ok_near(d, -0.9453087204829419, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_tgamma(-1e-5)); ok_near(d, -100000.57722555555, 1e-13); ok_errno(SENTINEL);
    CALL(d, p_tgamma(1e-308)); ok_near(d, 1e308, 1e-13); ok_errno(SENTINEL);
    CALL(d, p_tgamma(0.0)); ok(d == inf, "tgamma(0) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_tgamma(-0.0)); ok(d == -inf, "tgamma(-0) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_tgamma(9.9999999999999694e-311)); ok(d == inf, "tgamma(1e-310) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_tgamma(-9.9999999999999694e-311)); ok(d == -inf, "tgamma(-1e-310) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_tgamma(171.62)); ok(d < inf && d > 1e308, "tgamma(171.62) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_tgamma(171.63)); ok(d == inf, "tgamma(171.63) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_tgamma(172.0)); ok(d == inf, "tgamma(172) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_tgamma(1e18)); ok(d == inf, "tgamma(1e18) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_tgamma(1.79e308)); ok(d == inf, "tgamma(1.79e308) = %g\n", d); ok_errno(ERANGE);
    for (i = 0; i < (int)(sizeof(pole_args) / sizeof(*pole_args)); i++)
    {
        if (pole_args[i] == 0.0)
            continue;
        CALL(d, p_tgamma(pole_args[i]));
        ok(is_nan(d), "tgamma(%g) = %g\n", pole_args[i], d);
        ok_errno(EDOM);
    }
    CALL(d, p_tgamma(-1e300)); ok(is_nan(d), "tgamma(-1e300) = %g\n", d); ok_errno(EDOM);
    CALL(d, p_tgamma(-inf)); ok(is_nan(d), "tgamma(-inf) = %g\n", d); ok_errno(EDOM);
    CALL(d, p_tgamma(inf)); ok(d == inf, "tgamma(inf) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_tgamma(qnan)); ok(is_nan(d), "tgamma(nan) = %g\n", d); ok_errno(SENTINEL);
    /* the last argument that gives a subnormal result and the first one that is a range error */
    CALL(d, p_tgamma(-173.9)); ok_near(d, 2.6511406035844687e-315, 1e-6); ok_errno(SENTINEL);
    CALL(d, p_tgamma(-173.999)); ok_near(d, 1.5643740586684637e-313, 1e-6); ok_errno(SENTINEL);
    CALL(d, p_tgamma(-174.001)); ok(d == 0.0 && !is_neg(d), "tgamma(-174.001) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_tgamma(-174.5)); ok(d == 0.0 && !is_neg(d), "tgamma(-174.5) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_tgamma(-175.5)); ok(d == 0.0 && !is_neg(d), "tgamma(-175.5) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_tgamma(-1000.5)); ok(d == 0.0 && !is_neg(d), "tgamma(-1000.5) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_tgammal(-1.5)); ok_near(d, 2.3632718012073548, 1e-14); ok_errno(SENTINEL);
    CALL(d, p_tgammal(0.0)); ok(d == inf, "tgammal(0) = %g\n", d); ok_errno(ERANGE);

    CALL(f, p_tgammaf(5.0f)); ok_nearf(f, 24.0f, 1e-6); ok_errno(SENTINEL);
    CALL(f, p_tgammaf(0.5f)); ok_nearf(f, 1.77245385f, 1e-6); ok_errno(SENTINEL);
    CALL(f, p_tgammaf(-1.5f)); ok_nearf(f, 2.36327181f, 1e-6); ok_errno(SENTINEL);
    CALL(f, p_tgammaf(25.5f)); ok_nearf(f, 3.0867698e+24f, 1e-6); ok_errno(SENTINEL);
    CALL(f, p_tgammaf(0.0f)); ok(f == INFINITY, "tgammaf(0) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_tgammaf(-0.0f)); ok(f == -INFINITY, "tgammaf(-0) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_tgammaf(-1.0f)); ok(is_nan(f), "tgammaf(-1) = %g\n", (double)f); ok_errno(EDOM);
    CALL(f, p_tgammaf(-1e10f)); ok(is_nan(f), "tgammaf(-1e10) = %g\n", (double)f); ok_errno(EDOM);
    CALL(f, p_tgammaf(-INFINITY)); ok(is_nan(f), "tgammaf(-inf) = %g\n", (double)f); ok_errno(EDOM);
    CALL(f, p_tgammaf(INFINITY)); ok(f == INFINITY, "tgammaf(inf) = %g\n", (double)f); ok_errno(SENTINEL);
    CALL(f, p_tgammaf(36.0f)); ok(f == INFINITY, "tgammaf(36) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_tgammaf(1e30f)); ok(f == INFINITY, "tgammaf(1e30) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_tgammaf(1e-40f)); ok(f == INFINITY, "tgammaf(1e-40) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_tgammaf(-37.9f)); ok(f > 0.0f && f < 1e-40f, "tgammaf(-37.9) = %g\n", (double)f); ok_errno(SENTINEL);
    CALL(f, p_tgammaf(-38.1f)); ok(f == 0.0f && !is_neg(f), "tgammaf(-38.1) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_tgammaf(-100.5f)); ok(f == 0.0f && !is_neg(f), "tgammaf(-100.5) = %g\n", (double)f); ok_errno(ERANGE);

    /* the result is rounded to double before it is returned: the x87 range would keep it finite */
    d = p_tgamma(184.0) / 8.98846567431158e307;
    ok(d == inf, "tgamma(184) / 2^1023 = %g\n", d);
}

static void test_gamma_rounding_modes(void)
{
    unsigned int old = p_controlfp(0, 0);
    double d;
    float f;

    /* what overflowed is a range error also when the mode clamps it to the maximum */
    p_controlfp(_RC_DOWN, _MCW_RC);
    CALL(d, p_lgamma(2.6e305)); ok(d >= DBL_MAX, "lgamma(2.6e305) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_tgamma(172.0)); ok(d >= DBL_MAX, "tgamma(172) = %g\n", d); ok_errno(ERANGE);
    CALL(f, p_tgammaf(36.0f)); ok(f >= FLT_MAX, "tgammaf(36) = %g\n", (double)f); ok_errno(ERANGE);
    p_controlfp(old, _MCW_RC);
}

static struct _exception g_exception;
static int g_matherr_calls;

static int __cdecl matherr_callback(struct _exception *exception)
{
    g_exception = *exception;
    g_matherr_calls++;
    return 0;
}

/* Only the legacy logb reports to the handler of math errors: logb and the gamma functions do not */
static void test_matherr_reports(void)
{
    void (__cdecl *p_setusermatherr)(int (__cdecl *)(struct _exception *));
    double d;
    float f;

    p_setusermatherr = (void *)GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"), "__setusermatherr");
    if (!p_setusermatherr)
    {
        skip("__setusermatherr is missing\n");
        return;
    }
    p_setusermatherr(matherr_callback);

    g_matherr_calls = 0;
    d = p__logb(0.0);
    ok(g_matherr_calls == 1, "_logb(0) called the handler %d times\n", g_matherr_calls);
    ok(g_exception.type == _SING && !strcmp(g_exception.name, "_logb") && g_exception.arg1 == 0.0,
       "_logb(0): type %d, name %s, arg1 %g\n", g_exception.type, g_exception.name, g_exception.arg1);
    ok(d == -INFINITY, "_logb(0) = %g\n", d);
    if (p__logbf)
    {
        g_matherr_calls = 0;
        f = p__logbf(0.0f);
        ok(g_matherr_calls == 1, "_logbf(0) called the handler %d times\n", g_matherr_calls);
        ok(g_exception.type == _SING && !strcmp(g_exception.name, "_logbf"),
           "_logbf(0): type %d, name %s\n", g_exception.type, g_exception.name);
        ok(f == -INFINITY, "_logbf(0) = %g\n", (double)f);
    }

    g_matherr_calls = 0;
    p_logb(0.0); p_logbf(0.0f); p_lgamma(0.0); p_lgamma(-1.0); p_lgammaf(0.0f);
    p_tgamma(0.0); p_tgamma(-1.0); p_tgamma(172.0); p_tgamma(-175.5); p_tgammaf(-1.0f); p_tgammaf(36.0f);
    ok(g_matherr_calls == 0, "logb and the gamma functions called the handler %d times\n", g_matherr_calls);

    p_setusermatherr(NULL);
}

static void test_logb(void)
{
    double d;
    float f;

    CALL(d, p_logb(1.0)); ok(d == 0.0, "logb(1) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_logb(8.0)); ok(d == 3.0, "logb(8) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_logb(0.75)); ok(d == -1.0, "logb(0.75) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_logb(-1e300)); ok(d == 996.0, "logb(-1e300) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_logb(4.9406564584124654e-324)); ok(d == -1074.0, "logb(min subnormal) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_logb(9.9999999999999694e-311)); ok(d == -1030.0, "logb(1e-310) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_logb(0.0)); ok(d == -INFINITY, "logb(0) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_logb(-0.0)); ok(d == -INFINITY, "logb(-0) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_logb(INFINITY)); ok(d == INFINITY, "logb(inf) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_logb(-INFINITY)); ok(d == INFINITY, "logb(-inf) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_logb(p_nan(""))); ok(is_nan(d), "logb(nan) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p_logbl(0.0)); ok(d == -INFINITY, "logbl(0) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p_logbl(16.0)); ok(d == 4.0, "logbl(16) = %g\n", d); ok_errno(SENTINEL);

    CALL(f, p_logbf(1.0f)); ok(f == 0.0f, "logbf(1) = %g\n", (double)f); ok_errno(SENTINEL);
    CALL(f, p_logbf(-10.0f)); ok(f == 3.0f, "logbf(-10) = %g\n", (double)f); ok_errno(SENTINEL);
    CALL(f, p_logbf(0.5f)); ok(f == -1.0f, "logbf(0.5) = %g\n", (double)f); ok_errno(SENTINEL);
    CALL(f, p_logbf(1.4012985e-45f)); ok(f == -149.0f, "logbf(min subnormal) = %g\n", (double)f); ok_errno(SENTINEL);
    CALL(f, p_logbf(0.0f)); ok(f == -INFINITY, "logbf(0) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_logbf(-0.0f)); ok(f == -INFINITY, "logbf(-0) = %g\n", (double)f); ok_errno(ERANGE);
    CALL(f, p_logbf(INFINITY)); ok(f == INFINITY, "logbf(inf) = %g\n", (double)f); ok_errno(SENTINEL);
    CALL(f, p_logbf(-INFINITY)); ok(f == INFINITY, "logbf(-inf) = %g\n", (double)f); ok_errno(SENTINEL);
    CALL(f, p_logbf(p_nanf(""))); ok(is_nan(f), "logbf(nan) = %g\n", (double)f); ok_errno(SENTINEL);

    /* the legacy name keeps the sign of an infinity, and for floats reports a NaN as a domain error */
    CALL(d, p__logb(0.0)); ok(d == -INFINITY, "_logb(0) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p__logb(-0.0)); ok(d == -INFINITY, "_logb(-0) = %g\n", d); ok_errno(ERANGE);
    CALL(d, p__logb(100.0)); ok(d == 6.0, "_logb(100) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p__logb(INFINITY)); ok(d == INFINITY, "_logb(inf) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p__logb(-INFINITY)); ok(d == -INFINITY, "_logb(-inf) = %g\n", d); ok_errno(SENTINEL);
    CALL(d, p__logb(p_nan(""))); ok(is_nan(d), "_logb(nan) = %g\n", d); ok_errno(SENTINEL);
    if (p__logbf)
    {
        CALL(f, p__logbf(0.0f)); ok(f == -INFINITY, "_logbf(0) = %g\n", (double)f); ok_errno(ERANGE);
        CALL(f, p__logbf(0.5f)); ok(f == -1.0f, "_logbf(0.5) = %g\n", (double)f); ok_errno(SENTINEL);
        CALL(f, p__logbf(-INFINITY)); ok(f == -INFINITY, "_logbf(-inf) = %g\n", (double)f); ok_errno(SENTINEL);
        CALL(f, p__logbf(INFINITY)); ok(f == INFINITY, "_logbf(inf) = %g\n", (double)f); ok_errno(SENTINEL);
        CALL(f, p__logbf(p_nanf(""))); ok(is_nan(f), "_logbf(nan) = %g\n", (double)f); ok_errno(EDOM);
    }
}

static void test_nan(void)
{
    ok(is_nan(p_nan("")), "nan() is not a NaN\n");
    ok(is_nan(p_nan("123")), "nan(123) is not a NaN\n");
    ok(is_nan(p_nanf("")), "nanf() is not a NaN\n");
}

START_TEST(c99_math)
{
    HMODULE hUcrt = GetModuleHandleW(L"ucrtbase.dll");

    if (!hUcrt)
        hUcrt = LoadLibraryW(L"ucrtbase.dll");
    if (!hUcrt)
    {
        skip("ucrtbase.dll is not available\n");
        return;
    }

#define GET(n) do { *(void **)&p_##n = (void *)GetProcAddress(hUcrt, #n); ok(p_##n != NULL, #n " is missing\n"); if (!p_##n) return; } while (0)
    GET(acosh); GET(asinh); GET(atanh); GET(cbrt); GET(erf); GET(erfc); GET(expm1); GET(log1p); GET(rint); GET(nearbyint);
    GET(copysign); GET(fdim); GET(fmax); GET(fmin); GET(remainder);
    GET(acoshf); GET(asinhf); GET(atanhf); GET(cbrtf); GET(erff); GET(erfcf); GET(expm1f); GET(log1pf); GET(rintf); GET(nearbyintf);
    GET(copysignf); GET(fdimf); GET(fmaxf); GET(fminf); GET(remainderf);
    *(void **)&p_modff = (void *)GetProcAddress(hUcrt, "modff");
    GET(nan); GET(nanf); GET(ilogb); GET(ilogbf); GET(remquo); GET(remquof);
    GET(lround); GET(lroundf); GET(llround); GET(llroundf); GET(llrint); GET(llrintf);
    GET(lgamma); GET(lgammal); GET(tgamma); GET(tgammal); GET(logb); GET(logbl); GET(_logb);
    GET(lgammaf); GET(tgammaf); GET(logbf);
    *(void **)&p__logbf = (void *)GetProcAddress(hUcrt, "_logbf");
    *(void **)&p_statusfp = (void *)GetProcAddress(hUcrt, "_statusfp");
    *(void **)&p_clearfp = (void *)GetProcAddress(hUcrt, "_clearfp");
    *(void **)&p_controlfp = (void *)GetProcAddress(hUcrt, "_controlfp");

    test_copysign();
    test_cbrt();
    test_expm1_log1p();
    test_hyperbolic_inverse();
    test_erf();
    test_fdim_fmax_fmin();
    test_ilogb();
    test_remainder();
    test_rounding();
    test_rounding_modes();
    test_nearbyint_flags();
    test_modff();
    test_exception_flags();
    test_gamma();
    test_gamma_rounding_modes();
    test_logb();
    test_matherr_reports();
    test_nan();
}
