/*
 * PROJECT:     ReactOS ucrtbase
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Error conventions of the legacy math functions: errno and the user handler of math errors
 * COPYRIGHT:   Based on Wine's dlls/msvcrt/math.c (Copyright 2000 Jon Griffiths, 2003 Alexandre Julliard
 *              and others); the conventions were measured on Windows 10
 */

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#ifdef _M_AMD64
#include <xmmintrin.h>
#endif

int __cdecl __acrt_invoke_user_matherr(struct _exception *);

/* The user handler may replace the result, and a nonzero return from it keeps errno untouched */
static double ucrt_math_error(int type, const char *name, double arg1, double arg2, double retval)
{
    struct _exception e;

    e.type = type;
    e.name = (char *)name;
    e.arg1 = arg1;
    e.arg2 = arg2;
    e.retval = retval;

    if (__acrt_invoke_user_matherr(&e))
        return e.retval;

    switch (type)
    {
        case _DOMAIN:
            errno = EDOM;
            break;
        case _SING:
        case _OVERFLOW:
            errno = ERANGE;
            break;
        default:
            /* no errno for _UNDERFLOW */
            break;
    }

    return e.retval;
}

static double from_bits(uint64_t bits)
{
    double d;

    memcpy(&d, &bits, sizeof(d));
    return d;
}

#define INDEFINITE from_bits(0xfff8000000000000ULL)

/* INFINITY of this tree is made by an overflow, which sets the flag and can trap */
#define POSITIVE_INFINITY from_bits(0x7ff0000000000000ULL)
#define NEGATIVE_INFINITY from_bits(0xfff0000000000000ULL)

/* 0 nearest, 1 down, 2 up, 3 toward zero, as in the control words */
static int rounding_mode(void)
{
#ifdef _M_AMD64
    return (_mm_getcsr() >> 13) & 3;
#elif defined(_M_IX86) && defined(__GNUC__)
    unsigned short cw;

    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    return (cw >> 10) & 3;
#else
    return (_control87(0, 0) & _MCW_RC) >> 8;
#endif
}

/* Outside the mode to nearest an overflow gives the largest number on the side of zero */
static double overflow_value(int negative)
{
    int mode = rounding_mode();

    if (mode == 3 || mode == (negative ? 2 : 1))
        return negative ? -DBL_MAX : DBL_MAX;
    return negative ? NEGATIVE_INFINITY : POSITIVE_INFINITY;
}

static int is_signaling(double x)
{
    uint64_t bits;

    memcpy(&bits, &x, sizeof(bits));
    return !(bits & 0x0008000000000000ULL);
}

static double quiet(double x)
{
    uint64_t bits;

    memcpy(&bits, &x, sizeof(bits));
    bits |= 0x0008000000000000ULL;
    memcpy(&x, &bits, sizeof(x));
    return x;
}

/* The result is made from integers, so the flags of the overflow and of the underflow are raised by an operation */
static void raise_flags(double a, double b)
{
    volatile double v = a;

    v *= b;
}

/* Windows truncates a result in the denormal range */
static double scale_truncate(double x, long long n, int *tiny, int *overflow)
{
    uint64_t bits, m;
    long long e;
    int sign;

    memcpy(&bits, &x, sizeof(bits));
    sign = bits >> 63;
    e = (bits >> 52) & 0x7ff;
    m = bits & 0x000fffffffffffffULL;
    if (e)
        m |= 0x0010000000000000ULL;
    else
    {
        e = 1;
        while (!(m & 0x0010000000000000ULL))
        {
            m <<= 1;
            e--;
        }
    }

    e += n;
    *tiny = e <= 0;
    *overflow = e >= 0x7ff;
    if (*overflow)
        return overflow_value(sign);
    if (e <= 0)
    {
        m = (1 - e) >= 64 ? 0 : m >> (1 - e);
        bits = m | (uint64_t)sign << 63;
    }
    else
        bits = (m & 0x000fffffffffffffULL) | (uint64_t)e << 52 | (uint64_t)sign << 63;
    memcpy(&x, &bits, sizeof(x));
    return x;
}

/* Windows reports the signaling NaN of ldexp on amd64 only, the underflow of _scalb on i386 only, and the name of _scalb is ldexp there */
static double scale(const char *name, double x, long n, int report_snan, int report_underflow)
{
    double r;
    int tiny, overflow;

    if (isnan(x))
    {
        if (report_snan && is_signaling(x))
            return ucrt_math_error(_DOMAIN, name, x, n, quiet(x));
        return quiet(x);
    }
    if (isinf(x) || x == 0)
        return x;

    r = scale_truncate(x, n, &tiny, &overflow);
    if (overflow)
    {
        raise_flags(DBL_MAX, 2.0);
        return ucrt_math_error(_OVERFLOW, name, x, n, r);
    }
    if (tiny && report_underflow)
    {
        raise_flags(DBL_MIN, DBL_MIN);
        return ucrt_math_error(_UNDERFLOW, name, x, n, r);
    }
    return r;
}

double __cdecl ucrt_ldexp(double x, int n)
{
#ifdef _M_AMD64
    return scale("ldexp", x, n, 1, 1);
#else
    return scale("ldexp", x, n, 0, 1);
#endif
}

double __cdecl _scalb(double x, long n)
{
#ifdef _M_AMD64
    return scale("_scalb", x, n, 0, 0);
#else
    return scale("ldexp", x, n, 0, 1);
#endif
}

/* Windows sets errno for the overflow and for the underflow to zero, but not for a large negative exponent, in every rounding mode */
static int exp2_error(double x, double overflow_from, double zero_up_to)
{
    if (x >= overflow_from && isfinite(x))
        return 1;
    return x <= zero_up_to && x >= -2147483648.0;
}

double __cdecl ucrt_exp2(double x)
{
    if (exp2_error(x, 1024, -1075))
        errno = ERANGE;
    if (x >= 1024 && isfinite(x))
        return POSITIVE_INFINITY;
    if (x > -1075 && x < -1074)
        return from_bits(1);
    return x <= -1075 ? 0 : exp2(x);
}

float __cdecl ucrt_exp2f(float x)
{
    if (exp2_error(x, 128, -150))
        errno = ERANGE;
    if (x >= 128 && isfinite(x))
        return (float)POSITIVE_INFINITY;
    return x <= -150 ? 0 : (float)exp2(x);
}

#ifdef _M_AMD64

/* Windows reports no NaN operand, no error of type 0 and no error of exp2, and ignores the result of the handler for tan */
static double report(const char *name, int type, int nargs, double arg1, double arg2, double retval)
{
    if (type == 0)
        return retval;

    if (type == _DOMAIN && (isnan(arg1) || (nargs > 1 && isnan(arg2))))
        return retval;

    if (!strcmp(name, "exp2") || !strcmp(name, "exp2f"))
        return retval;

    if (!strcmp(name, "tan") || !strcmp(name, "tanf"))
    {
        ucrt_math_error(type, name, arg1, arg2, retval);
        return retval;
    }

    return ucrt_math_error(type, name, arg1, arg2, retval);
}

double __cdecl _handle_error(char *fname, int opcode, unsigned long long value, int type, int flags, int error,
                             double arg1, double arg2, int nargs)
{
    return report(fname, type, nargs, arg1, arg2, from_bits(value));
}

float __cdecl _handle_errorf(char *fname, int opcode, unsigned long long value, int type, int flags, int error,
                             float arg1, float arg2, int nargs)
{
    float retval;

    memcpy(&retval, &value, sizeof(retval));
    return (float)report(fname, type, nargs, arg1, arg2, retval);
}

/* The x87 code of fmod reports x / infinity, which is x for C99 */
double __cdecl ucrt_fmod(double x, double y)
{
    if (isinf(y) && isfinite(x))
        return x;
    return fmod(x, y);
}

float __cdecl ucrt_fmodf(float x, float y)
{
    if (isinf(y) && isfinite(x))
        return x;
    return fmodf(x, y);
}

/* The SSE code of expf returns minus infinity for it */
float __cdecl ucrt_expf(float x)
{
    return isinf(x) && x < 0 ? 0.0f : expf(x);
}

#endif /* _M_AMD64 */

#ifdef _M_IX86

/* Outside the mode to nearest the largest number is an overflow only if the mode to nearest gives infinity */
static int is_overflow(double (__cdecl *f)(double, double), double x, double y, double r)
{
    unsigned int control;
    int overflow;

    if (isinf(r))
        return 1;
    if (fabs(r) != DBL_MAX || rounding_mode() == 0)
        return 0;

    control = _control87(0, 0) & _MCW_RC;
    _control87(_RC_NEAR, _MCW_RC);
    overflow = isinf(f(x, y));
    _control87(control, _MCW_RC);
    return overflow;
}

static double __cdecl raw_cosh(double x, double y) { return cosh(x); }
static double __cdecl raw_sinh(double x, double y) { return sinh(x); }
static double __cdecl raw_exp(double x, double y) { return exp(x); }

static int is_tiny(double r)
{
    return fabs(r) < DBL_MIN;
}

/* The x87 code reports nothing, so these check what the SSE code of Windows checks */
double __cdecl ucrt_sin(double x)
{
    double r = sin(x);

    return isinf(x) ? ucrt_math_error(_DOMAIN, "sin", x, 0, INDEFINITE) : r;
}

double __cdecl ucrt_cos(double x)
{
    double r = cos(x);

    return isinf(x) ? ucrt_math_error(_DOMAIN, "cos", x, 0, INDEFINITE) : r;
}

double __cdecl ucrt_tan(double x)
{
    double r = tan(x);

    return isinf(x) ? ucrt_math_error(_DOMAIN, "tan", x, 0, INDEFINITE) : r;
}

double __cdecl ucrt_acos(double x)
{
    double r = acos(x);

    return fabs(x) > 1 ? ucrt_math_error(_DOMAIN, "acos", x, 0, INDEFINITE) : r;
}

double __cdecl ucrt_asin(double x)
{
    double r = asin(x);

    return fabs(x) > 1 ? ucrt_math_error(_DOMAIN, "asin", x, 0, INDEFINITE) : r;
}

double __cdecl ucrt_atan2(double y, double x)
{
    double r = atan2(y, x);

    if (isfinite(y) && isfinite(x) && y != 0 && is_tiny(r))
        return ucrt_math_error(_UNDERFLOW, "atan2", y, x, r);
    return r;
}

double __cdecl ucrt_cosh(double x)
{
    double r = cosh(x);

    return isfinite(x) && is_overflow(raw_cosh, x, 0, r) ? ucrt_math_error(_OVERFLOW, "cosh", x, 0, r) : r;
}

double __cdecl ucrt_sinh(double x)
{
    double r = sinh(x);

    return isfinite(x) && is_overflow(raw_sinh, x, 0, r) ? ucrt_math_error(_OVERFLOW, "sinh", x, 0, r) : r;
}

double __cdecl ucrt_sqrt(double x)
{
    double r = sqrt(x);

    if (isnan(x))
        return ucrt_math_error(0, "sqrt", x, 0, r);
    return x < 0 ? ucrt_math_error(_DOMAIN, "sqrt", x, 0, INDEFINITE) : r;
}

double __cdecl ucrt_exp(double x)
{
    double r = exp(x);

    if (isfinite(x))
    {
        if (is_overflow(raw_exp, x, 0, r))
            return ucrt_math_error(_OVERFLOW, "exp", x, 0, r);
        if (is_tiny(r))
            return ucrt_math_error(_UNDERFLOW, "exp", x, 0, r);
    }
    return r;
}

double __cdecl ucrt_log(double x)
{
    double r = log(x);

    if (x == 0)
        return ucrt_math_error(_SING, "log", x, 0, r);
    return x < 0 ? ucrt_math_error(_DOMAIN, "log", x, 0, INDEFINITE) : r;
}

double __cdecl ucrt_log10(double x)
{
    double r = log10(x);

    if (x == 0)
        return ucrt_math_error(_SING, "log10", x, 0, r);
    return x < 0 ? ucrt_math_error(_DOMAIN, "log10", x, 0, INDEFINITE) : r;
}

double __cdecl ucrt_pow(double x, double y)
{
    double r = pow(x, y);

    if (isnan(x) || isnan(y))
        return r;
    if (x == 0 && y < 0 && isfinite(y))
        return ucrt_math_error(_SING, "pow", x, y, r);
    if (isfinite(x) && isfinite(y))
    {
        if (x < 0 && y != floor(y))
            return ucrt_math_error(_DOMAIN, "pow", x, y, INDEFINITE);
        if (is_overflow(pow, x, y, r))
            return ucrt_math_error(_OVERFLOW, "pow", x, y, r);
        if (x != 0 && is_tiny(r))
            return ucrt_math_error(_UNDERFLOW, "pow", x, y, r);
    }
    return r;
}

/* The loop of the x87 code may not end for these operands */
double __cdecl ucrt_fmod(double x, double y)
{
    if (isnan(x) || isnan(y))
        return quiet(isnan(x) ? x : y);
    if (isinf(x) || y == 0)
        return ucrt_math_error(_DOMAIN, "fmod", x, y, INDEFINITE);
    return isinf(y) ? x : fmod(x, y);
}

double __cdecl ucrt_hypot(double x, double y)
{
    double r = _hypot(x, y);

    return isfinite(x) && isfinite(y) && is_overflow(_hypot, x, y, r) ? ucrt_math_error(_OVERFLOW, "_hypot", x, y, r) : r;
}

/* The entry points of the compilers for x87 operands report as the functions do */
#ifdef __GNUC__
#define FPU_DOUBLE(var) double var; __asm__ __volatile__("fstpl %0;fwait" : "=m"(var) :)
#define FPU_DOUBLES(var1, var2) double var1, var2; \
    __asm__ __volatile__("fstpl %0;fwait" : "=m"(var2) :); \
    __asm__ __volatile__("fstpl %0;fwait" : "=m"(var1) :)
#else
#define FPU_DOUBLE(var) double var; __asm { fstp [var] } __asm { fwait }
#define FPU_DOUBLES(var1, var2) double var1, var2; \
    __asm { fstp [var2] } __asm { fwait } __asm { fstp [var1] } __asm { fwait }
#endif

#define CI_FUNCTION1(name) \
    double __cdecl ucrt_CI##name(void) { FPU_DOUBLE(x); return ucrt_##name(x); }
#define CI_FUNCTION2(name, a, b) \
    double __cdecl ucrt_CI##name(void) { FPU_DOUBLES(a, b); return ucrt_##name(a, b); }

CI_FUNCTION1(acos)
CI_FUNCTION1(asin)
CI_FUNCTION1(cos)
CI_FUNCTION1(cosh)
CI_FUNCTION1(exp)
CI_FUNCTION1(log)
CI_FUNCTION1(log10)
CI_FUNCTION1(sin)
CI_FUNCTION1(sinh)
CI_FUNCTION1(sqrt)
CI_FUNCTION1(tan)
CI_FUNCTION2(atan2, y, x)
CI_FUNCTION2(fmod, x, y)
CI_FUNCTION2(pow, x, y)

#endif /* _M_IX86 */
