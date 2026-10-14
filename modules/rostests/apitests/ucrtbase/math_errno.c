/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the error conventions of the legacy math functions of ucrtbase
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* The functions are called through pointers: the compiler does not know them and cannot fold the calls */

#include <apitest.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#define SENTINEL 0x1234

static HMODULE g_ucrt;
static void (__cdecl *p_setusermatherr)(int (__cdecl *)(struct _exception *));

static struct _exception g_exc;
static int g_called;
static int g_handler_result;
static int g_replace;
static double g_replacement;

static int __cdecl handler(struct _exception *e)
{
    g_exc = *e;
    g_called++;
    if (g_replace)
        e->retval = g_replacement;
    return g_handler_result;
}

static void set_handler(int result, int replace, double replacement)
{
    g_handler_result = result;
    g_replace = replace;
    g_replacement = replacement;
    p_setusermatherr(handler);
}

static void *get(const char *name)
{
    void *p = (void *)GetProcAddress(g_ucrt, name);

    ok(p != NULL, "%s is missing\n", name);
    return p;
}

static void begin(void)
{
    g_called = 0;
    memset(&g_exc, 0, sizeof(g_exc));
    errno = SENTINEL;
}

static uint64_t bits(double d)
{
    uint64_t u;

    memcpy(&u, &d, sizeof(u));
    return u;
}

static double from_bits(uint64_t u)
{
    double d;

    memcpy(&d, &u, sizeof(d));
    return d;
}

/* The expected errno is SENTINEL when the function leaves errno alone, the expected type is -1 without a call of the handler */
static void check(const char *name, double a, double b, int errno_expected, int type_expected)
{
    ok(errno == errno_expected, "%s(%g, %g): errno = %d, expected %d\n", name, a, b, errno, errno_expected);
    if (type_expected < 0)
    {
        ok(g_called == 0, "%s(%g, %g): the handler was called, type %d\n", name, a, b, g_exc.type);
        return;
    }
    ok(g_called == 1, "%s(%g, %g): the handler was called %d times\n", name, a, b, g_called);
    if (g_called != 1)
        return;
    ok(g_exc.type == type_expected, "%s(%g, %g): type = %d, expected %d\n", name, a, b, g_exc.type, type_expected);
    /* the fmod and _hypot of Windows pass garbage on amd64, and the functions of one argument garbage as the second */
    if (strcmp(name, "fmod") && strcmp(name, "_hypot"))
        ok(bits(g_exc.arg1) == bits(a), "%s(%g, %g): arg1 = %g\n", name, a, b, g_exc.arg1);
    if (!strcmp(name, "pow") || !strcmp(name, "ldexp"))
        ok(bits(g_exc.arg2) == bits(b), "%s(%g, %g): arg2 = %g\n", name, a, b, g_exc.arg2);
}

static void test_one_argument(void)
{
    static const struct
    {
        const char *name;
        double x;
        int error;
        int type;
    } rows[] = {
        /* a NaN is not reported */
        {"sin", NAN, SENTINEL, -1}, {"cos", NAN, SENTINEL, -1}, {"tan", NAN, SENTINEL, -1},
        {"acos", NAN, SENTINEL, -1}, {"asin", NAN, SENTINEL, -1}, {"log", NAN, SENTINEL, -1},
        {"log10", NAN, SENTINEL, -1}, {"exp", NAN, SENTINEL, -1}, {"atan", NAN, SENTINEL, -1},
        {"cosh", NAN, SENTINEL, -1}, {"sinh", NAN, SENTINEL, -1}, {"tanh", NAN, SENTINEL, -1},
        /* the infinities and the large values */
        {"sin", 1e300, SENTINEL, -1}, {"cos", -1e300, SENTINEL, -1}, {"tan", 1e300, SENTINEL, -1},
        {"acos", 1.0000000000000002, EDOM, _DOMAIN}, {"asin", -1.0000000000000002, EDOM, _DOMAIN},
        {"acos", 0.5, SENTINEL, -1}, {"asin", -0.5, SENTINEL, -1},
        {"log", -0.0, ERANGE, _SING}, {"log10", -0.0, ERANGE, _SING},
        {"log", -4.9406564584124654e-324, EDOM, _DOMAIN}, {"log10", -4.9406564584124654e-324, EDOM, _DOMAIN},
        {"log", 4.9406564584124654e-324, SENTINEL, -1}, {"log10", 1, SENTINEL, -1},
        {"sqrt", -4.9406564584124654e-324, EDOM, _DOMAIN}, {"sqrt", -0.0, SENTINEL, -1},
        {"sqrt", 4, SENTINEL, -1},
        {"exp", 709.78, SENTINEL, -1}, {"exp", 710, ERANGE, _OVERFLOW}, {"exp", -1000, SENTINEL, _UNDERFLOW},
        {"exp", -745.2, SENTINEL, _UNDERFLOW}, {"exp", -700, SENTINEL, -1},
        {"cosh", 710.5, ERANGE, _OVERFLOW}, {"cosh", -1000, ERANGE, _OVERFLOW}, {"cosh", 700, SENTINEL, -1},
        {"sinh", 1000, ERANGE, _OVERFLOW}, {"sinh", -1000, ERANGE, _OVERFLOW}, {"sinh", 700, SENTINEL, -1},
        {"tanh", 1000, SENTINEL, -1}, {"atan", 1e300, SENTINEL, -1},
        /* only the overflow, and only as errno */
        {"exp2", 1024, ERANGE, -1}, {"exp2", 1e5, ERANGE, -1}, {"exp2", -1075, ERANGE, -1}, {"exp2", -1e5, ERANGE, -1},
        {"exp2", -1074, SENTINEL, -1}, {"exp2", -1e300, SENTINEL, -1}, {"exp2", -INFINITY, SENTINEL, -1},
        {"exp2", INFINITY, SENTINEL, -1}, {"exp2", 1023, SENTINEL, -1},
    };
    int i;

    set_handler(0, 0, 0);
    for (i = 0; i < ARRAY_SIZE(rows); i++)
    {
        double (__cdecl *f)(double);
        const char *name = rows[i].name;
        double x = rows[i].x, r;

        f = get(name);
        if (!f)
            continue;
        begin();
        r = f(x);
        check(name, x, 0, rows[i].error, rows[i].type);
        if (x != x)
            ok(r != r, "%s(NaN) = %g\n", name, r);
        if (g_called)
            ok(r == g_exc.retval || (r != r && g_exc.retval != g_exc.retval),
               "%s(%g) = %g, the handler got %g\n", name, x, r, g_exc.retval);
    }
    p_setusermatherr(NULL);

    /* without a handler the errno is the same */
    for (i = 0; i < ARRAY_SIZE(rows); i++)
    {
        double (__cdecl *f)(double);
        const char *name = rows[i].name;
        int error = rows[i].error;

        f = get(name);
        if (!f)
            continue;
        errno = SENTINEL;
        f(rows[i].x);
        ok(errno == error, "%s(%g) without handler: errno = %d, expected %d\n", name, rows[i].x, errno, error);
    }
}

static void test_two_arguments(void)
{
    static const struct
    {
        const char *name;
        double a, b;
        int error;
        int type;
    } rows[] = {
        {"pow", NAN, 2, SENTINEL, -1}, {"pow", 2, NAN, SENTINEL, -1}, {"pow", NAN, NAN, SENTINEL, -1},
        {"pow", -8, 1.0 / 3, EDOM, _DOMAIN}, {"pow", -2, 0.5, EDOM, _DOMAIN}, {"pow", -2, 2, SENTINEL, -1},
        {"pow", -2, 3, SENTINEL, -1}, {"pow", 0, -3, ERANGE, _SING}, {"pow", -0.0, -3, ERANGE, _SING},
        {"pow", 0, -0.5, ERANGE, _SING}, {"pow", 0, 3, SENTINEL, -1}, {"pow", 10, 400, ERANGE, _OVERFLOW},
        {"pow", -10, 401, ERANGE, _OVERFLOW}, {"pow", 10, -400, SENTINEL, _UNDERFLOW},
        {"pow", 2, 0.5, SENTINEL, -1}, {"pow", INFINITY, 2, SENTINEL, -1}, {"pow", 2, INFINITY, SENTINEL, -1},
        {"pow", 2, -INFINITY, SENTINEL, -1}, {"pow", -INFINITY, 3, SENTINEL, -1},
        {"fmod", 1, 0, EDOM, _DOMAIN}, {"fmod", -1, 0, EDOM, _DOMAIN}, {"fmod", INFINITY, 1, EDOM, _DOMAIN},
        {"fmod", -INFINITY, 2, EDOM, _DOMAIN}, {"fmod", NAN, 1, SENTINEL, -1}, {"fmod", 1, NAN, SENTINEL, -1},
        {"fmod", 5, INFINITY, SENTINEL, -1}, {"fmod", -5, -INFINITY, SENTINEL, -1}, {"fmod", 5, 3, SENTINEL, -1},
        {"atan2", NAN, 1, SENTINEL, -1}, {"atan2", 1, NAN, SENTINEL, -1}, {"atan2", 0, 0, SENTINEL, -1},
        {"atan2", INFINITY, INFINITY, SENTINEL, -1},
    };
    int i;

    set_handler(0, 0, 0);
    for (i = 0; i < ARRAY_SIZE(rows); i++)
    {
        double (__cdecl *f)(double, double) = get(rows[i].name);

        if (!f)
            continue;
        begin();
        f(rows[i].a, rows[i].b);
        check(rows[i].name, rows[i].a, rows[i].b, rows[i].error, rows[i].type);
    }
    p_setusermatherr(NULL);
}

static void test_fmod_values(void)
{
    static const struct { double x, y, r; } rows[] = {
        {5, 3, 2}, {-5, 3, -2}, {5, -3, 2}, {-5, -3, -2}, {5.5, 2, 1.5}, {0, 1, 0}, {2, 1, 0}, {1, 2, 1}, {-1, 2, -1},
        {7, 7, 0}, {1e300, 1e300, 0}, {3, INFINITY, 3}, {-3, INFINITY, -3}, {3, -INFINITY, 3}, {0, INFINITY, 0},
        {4503599627370497.0, 2, 1}, {10.5, 0.25, 0}, {1e10, 3, 1},
    };
    double (__cdecl *f)(double, double) = get("fmod");
    float (__cdecl *ff)(float, float) = (void *)GetProcAddress(g_ucrt, "fmodf");
    int i;

    if (!f)
        return;
    for (i = 0; i < ARRAY_SIZE(rows); i++)
    {
        double r = f(rows[i].x, rows[i].y);

        ok(r == rows[i].r, "fmod(%g, %g) = %g, expected %g\n", rows[i].x, rows[i].y, r, rows[i].r);
        if (rows[i].r == 0)
            ok(signbit(r) == signbit(rows[i].x), "fmod(%g, %g): sign of zero\n", rows[i].x, rows[i].y);
    }
    if (ff)
    {
        float r;

        r = ff(5, 3); ok(r == 2, "fmodf(5, 3) = %g\n", r);
        r = ff(-5.5f, 2); ok(r == -1.5f, "fmodf(-5.5, 2) = %g\n", r);
        r = ff(2, 1); ok(r == 0, "fmodf(2, 1) = %g\n", r);
        r = ff(3, INFINITY); ok(r == 3, "fmodf(3, inf) = %g\n", r);
    }
}

static void test_handler_result(void)
{
    double (__cdecl *f1)(double);
    double (__cdecl *f2)(double, double);
    double r;

    /* the handler claims the error: errno stays and its result is returned */
    f1 = get("acos");
    set_handler(1, 1, 77);
    begin();
    r = f1(2);
    ok(r == 77, "acos(2) = %g\n", r);
    ok(errno == SENTINEL, "errno = %d\n", errno);

    f1 = get("sqrt");
    begin();
    r = f1(-1);
    ok(r == 77, "sqrt(-1) = %g\n", r);
    ok(errno == SENTINEL, "errno = %d\n", errno);

    f2 = get("fmod");
    begin();
    r = f2(1, 0);
    ok(r == 77, "fmod(1, 0) = %g\n", r);
    ok(errno == SENTINEL, "errno = %d\n", errno);

    /* the handler replaces the result and lets the function set errno */
    set_handler(0, 1, 88);
    f1 = get("acos");
    begin();
    r = f1(2);
    ok(r == 88, "acos(2) = %g\n", r);
    ok(errno == EDOM, "errno = %d\n", errno);

    f1 = get("sqrt");
    begin();
    r = f1(-1);
    ok(r == 88, "sqrt(-1) = %g\n", r);
    ok(errno == EDOM, "errno = %d\n", errno);

    f2 = get("fmod");
    begin();
    r = f2(1, 0);
    ok(r == 88, "fmod(1, 0) = %g\n", r);
    ok(errno == EDOM, "errno = %d\n", errno);

    /* no call, no replacement */
    f1 = get("acos");
    begin();
    r = f1(0.5);
    ok(r > 1.04 && r < 1.05, "acos(0.5) = %g\n", r);
    ok(g_called == 0, "the handler was called\n");
    p_setusermatherr(NULL);
}

static void test_ldexp_scalb(void)
{
    double (__cdecl *ldexp_)(double, int) = get("ldexp");
    double (__cdecl *scalb_)(double, long) = get("_scalb");
    float (__cdecl *exp2f_)(float) = get("exp2f");
    float (__cdecl *expf_)(float) = (void *)GetProcAddress(g_ucrt, "expf");
    double r;
    float rf;

    if (!ldexp_ || !scalb_)
        return;

    set_handler(0, 0, 0);

    begin();
    r = ldexp_(1, 1024);
    ok(r == INFINITY, "ldexp(1, 1024) = %g\n", r);
    check("ldexp", 1, 1024, ERANGE, _OVERFLOW);
    ok(g_exc.name && !strcmp(g_exc.name, "ldexp"), "name = %s\n", g_exc.name);

    begin();
    r = ldexp_(-1.5, 2000);
    ok(r == -INFINITY, "ldexp(-1.5, 2000) = %g\n", r);
    check("ldexp", -1.5, 2000, ERANGE, _OVERFLOW);

    begin();
    r = ldexp_(1, -1075);
    ok(r == 0, "ldexp(1, -1075) = %g\n", r);
    check("ldexp", 1, -1075, SENTINEL, _UNDERFLOW);

    begin();
    r = ldexp_(1, -1074);
    ok(bits(r) == 1, "ldexp(1, -1074) = %g\n", r);
    check("ldexp", 1, -1074, SENTINEL, _UNDERFLOW);

    begin();
    r = ldexp_(4.9406564584124654e-324, -1);
    ok(r == 0, "ldexp(min, -1) = %g\n", r);
    check("ldexp", 4.9406564584124654e-324, -1, SENTINEL, _UNDERFLOW);

    /* a result in the denormal range is cut off, not rounded */
    begin();
    r = ldexp_(from_bits(0x000fffffffffffffULL), -1);
    ok(bits(r) == 0x0007ffffffffffffULL, "ldexp(denormal, -1) = %016I64x\n", bits(r));
    check("ldexp", from_bits(0x000fffffffffffffULL), -1, SENTINEL, _UNDERFLOW);

    begin();
    r = ldexp_(from_bits(0x000fffffffffffffULL), -10);
    ok(bits(r) == 0x000003ffffffffffULL, "ldexp(denormal, -10) = %016I64x\n", bits(r));

    begin();
    r = ldexp_(from_bits(0x0010000000000003ULL), -1);
    ok(bits(r) == 0x0008000000000001ULL, "ldexp(DBL_MIN + 3, -1) = %016I64x\n", bits(r));

    begin();
    r = ldexp_(-3, -2);
    ok(r == -0.75, "ldexp(-3, -2) = %g\n", r);
    check("ldexp", -3, -2, SENTINEL, -1);

    begin();
    r = ldexp_(3, 4);
    ok(r == 48, "ldexp(3, 4) = %g\n", r);
    check("ldexp", 3, 4, SENTINEL, -1);

    begin();
    r = ldexp_(0, 1000000000);
    ok(r == 0, "ldexp(0, 1e9) = %g\n", r);
    check("ldexp", 0, 1000000000, SENTINEL, -1);

    begin();
    r = ldexp_(INFINITY, -5);
    ok(r == INFINITY, "ldexp(inf, -5) = %g\n", r);
    check("ldexp", INFINITY, -5, SENTINEL, -1);

    begin();
    r = ldexp_(NAN, 5);
    ok(r != r, "ldexp(NaN, 5) = %g\n", r);
    check("ldexp", NAN, 5, SENTINEL, -1);

    /* Windows reports a signaling NaN on amd64 only */
    begin();
    r = ldexp_(from_bits(0x7ff0000000000001ULL), 5);
    ok(r != r, "ldexp(sNaN, 5) = %g\n", r);
#ifdef _M_AMD64
    check("ldexp", from_bits(0x7ff0000000000001ULL), 5, EDOM, _DOMAIN);
#else
    check("ldexp", from_bits(0x7ff0000000000001ULL), 5, SENTINEL, -1);
#endif

    begin();
    r = scalb_(1, 1000000000);
    ok(r == INFINITY, "_scalb(1, 1e9) = %g\n", r);
    check("_scalb", 1, 1000000000, ERANGE, _OVERFLOW);

    begin();
    r = scalb_(-1, 1024);
    ok(r == -INFINITY, "_scalb(-1, 1024) = %g\n", r);
    check("_scalb", -1, 1024, ERANGE, _OVERFLOW);

    begin();
    r = scalb_(1, -2000);
    ok(r == 0, "_scalb(1, -2000) = %g\n", r);
    ok(errno == SENTINEL, "errno = %d\n", errno);

    begin();
    r = scalb_(3, 4);
    ok(r == 48, "_scalb(3, 4) = %g\n", r);
    check("_scalb", 3, 4, SENTINEL, -1);

    begin();
    r = scalb_(NAN, 4);
    ok(r != r, "_scalb(NaN, 4) = %g\n", r);
    check("_scalb", NAN, 4, SENTINEL, -1);

    begin();
    r = scalb_(INFINITY, 4);
    ok(r == INFINITY, "_scalb(inf, 4) = %g\n", r);
    check("_scalb", INFINITY, 4, SENTINEL, -1);

    begin();
    r = scalb_(from_bits(0x7ff0000000000001ULL), 4);
    ok(bits(r) == 0x7ff8000000000001ULL, "_scalb(sNaN, 4) = %016I64x\n", bits(r));
    check("_scalb", from_bits(0x7ff0000000000001ULL), 4, SENTINEL, -1);

    begin();
    r = ldexp_(from_bits(0x7ff0000000000001ULL), 4);
    ok(bits(r) == 0x7ff8000000000001ULL, "ldexp(sNaN, 4) = %016I64x\n", bits(r));

    /* the handler can replace the result of the overflow */
    set_handler(1, 1, 5);
    begin();
    r = ldexp_(1, 1024);
    ok(r == 5, "ldexp(1, 1024) = %g\n", r);
    ok(errno == SENTINEL, "errno = %d\n", errno);
    p_setusermatherr(NULL);

    /* exp2 and exp2f set errno for the overflow and report nothing */
    set_handler(0, 0, 0);
    {
        double (__cdecl *exp2_)(double) = get("exp2");

        begin();
        r = exp2_(-1074.5);
        ok(bits(r) == 1, "exp2(-1074.5) = %016I64x\n", bits(r));
        check("exp2", -1074.5, 0, SENTINEL, -1);

        begin();
        r = exp2_(-1074);
        ok(bits(r) == 1, "exp2(-1074) = %016I64x\n", bits(r));
        check("exp2", -1074, 0, SENTINEL, -1);

        begin();
        r = exp2_(-1075);
        ok(r == 0, "exp2(-1075) = %g\n", r);
        check("exp2", -1075, 0, ERANGE, -1);
    }
    if (exp2f_)
    {
        begin();
        rf = exp2f_(1000);
        ok(rf == INFINITY, "exp2f(1000) = %g\n", (double)rf);
        check("exp2f", 1000, 0, ERANGE, -1);

        begin();
        rf = exp2f_(-1000);
        ok(rf == 0, "exp2f(-1000) = %g\n", (double)rf);
        check("exp2f", -1000, 0, ERANGE, -1);

        begin();
        rf = exp2f_(-1e18f);
        ok(rf == 0, "exp2f(-1e18) = %g\n", (double)rf);
        check("exp2f", -1e18f, 0, SENTINEL, -1);

        begin();
        rf = exp2f_(-149);
        ok(rf > 0, "exp2f(-149) = %g\n", (double)rf);
        check("exp2f", -149, 0, SENTINEL, -1);

        begin();
        rf = exp2f_(5);
        ok(rf == 32, "exp2f(5) = %g\n", (double)rf);
        check("exp2f", 5, 0, SENTINEL, -1);
    }
    if (expf_)
    {
        begin();
        rf = expf_(-INFINITY);
        ok(rf == 0 && !signbit(rf), "expf(-inf) = %g\n", (double)rf);
        ok(errno == SENTINEL, "errno = %d\n", errno);
        begin();
        rf = expf_(INFINITY);
        ok(rf == INFINITY, "expf(inf) = %g\n", (double)rf);
        begin();
        rf = expf_(0);
        ok(rf == 1, "expf(0) = %g\n", (double)rf);
    }
    p_setusermatherr(NULL);
}

static double (__cdecl *g_hypot)(double, double);
static double g_hypot_x, g_hypot_y, g_hypot_result;
static int g_hypot_errno;

static DWORD WINAPI hypot_thread(void *arg)
{
    errno = SENTINEL;
    g_hypot_result = g_hypot(g_hypot_x, g_hypot_y);
    g_hypot_errno = errno;
    return 0;
}

/* The call runs in a thread: an iteration that never ends is a failure, not a hang of the test */
static int call_hypot(double x, double y, double *result)
{
    HANDLE thread;
    DWORD wait;

    g_hypot_x = x;
    g_hypot_y = y;
    thread = CreateThread(NULL, 0, hypot_thread, NULL, 0, NULL);
    wait = WaitForSingleObject(thread, 5000);
    if (wait != WAIT_OBJECT_0)
        TerminateThread(thread, 1);
    CloseHandle(thread);
    *result = g_hypot_result;
    errno = g_hypot_errno;
    return wait == WAIT_OBJECT_0;
}

static void test_hypot(void)
{
    static const struct { double x, y, r; } rows[] = {
        {3, 4, 5}, {-3, 4, 5}, {0, 0, 0}, {0, -7, 7}, {INFINITY, 1, INFINITY}, {-1, INFINITY, INFINITY},
        {-INFINITY, -INFINITY, INFINITY}, {INFINITY, NAN, INFINITY}, {NAN, -INFINITY, INFINITY},
        {NAN, 1, NAN}, {1, NAN, NAN}, {NAN, NAN, NAN},
    };
    const char *names[] = { "_hypot", "hypot" };
    double dummy;
    int i, n;

    for (n = 0; n < ARRAY_SIZE(names); n++)
    {
        g_hypot = get(names[n]);
        if (!g_hypot)
            continue;
        set_handler(0, 0, 0);
        for (i = 0; i < ARRAY_SIZE(rows); i++)
        {
            double r = 0;

            begin();
            if (!call_hypot(rows[i].x, rows[i].y, &r))
            {
                ok(0, "%s(%g, %g) does not return\n", names[n], rows[i].x, rows[i].y);
                continue;
            }
            if (rows[i].r != rows[i].r)
                ok(r != r, "%s(%g, %g) = %g\n", names[n], rows[i].x, rows[i].y, r);
            else
                ok(r == rows[i].r, "%s(%g, %g) = %g, expected %g\n", names[n], rows[i].x, rows[i].y, r, rows[i].r);
            check("_hypot", rows[i].x, rows[i].y, SENTINEL, -1);
        }

        begin();
        call_hypot(DBL_MAX, DBL_MAX, &dummy);
        check("_hypot", DBL_MAX, DBL_MAX, ERANGE, _OVERFLOW);
        p_setusermatherr(NULL);
    }
}

/* Outside the mode to nearest, the overflow still sets errno and calls the handler */
static void test_rounding_overflow(void)
{
    unsigned int (__cdecl *controlfp_)(unsigned int, unsigned int) = get("_controlfp");
    double (__cdecl *f1)(double);
    double (__cdecl *f2)(double, double);
    double (__cdecl *ldexp_)(double, int) = get("ldexp");
    float (__cdecl *exp2f_)(float) = get("exp2f");
    unsigned int old;
    double r;
    float rf;

    if (!controlfp_ || !ldexp_)
        return;

    old = controlfp_(0, 0) & _MCW_RC;
    controlfp_(_RC_DOWN, _MCW_RC);
    set_handler(0, 0, 0);

    f1 = get("exp");
    begin();
    f1(710);
    check("exp", 710, 0, ERANGE, _OVERFLOW);

    f1 = get("cosh");
    begin();
    f1(1000);
    check("cosh", 1000, 0, ERANGE, _OVERFLOW);

    f1 = get("sinh");
    begin();
    f1(1000);
    check("sinh", 1000, 0, ERANGE, _OVERFLOW);

    f2 = get("pow");
    begin();
    f2(10, 400);
    check("pow", 10, 400, ERANGE, _OVERFLOW);

    begin();
    r = ldexp_(1, 1024);
    ok(r == DBL_MAX, "ldexp(1, 1024) = %g\n", r);
    check("ldexp", 1, 1024, ERANGE, _OVERFLOW);

    f1 = get("exp2");
    begin();
    r = f1(1024);
    ok(bits(r) == 0x7ff0000000000000ULL, "exp2(1024) = %g\n", r);
    check("exp2", 1024, 0, ERANGE, -1);

    begin();
    rf = exp2f_(128);
    ok(isinf(rf) && rf > 0, "exp2f(128) = %g\n", (double)rf);
    check("exp2f", 128, 0, ERANGE, -1);

    /* the largest number is a result, not an error */
    f2 = get("pow");
    begin();
    r = f2(from_bits(0x7fefffffffffffffULL), 1);
    ok(bits(r) == 0x7fefffffffffffffULL, "pow(DBL_MAX, 1) = %g\n", r);
    check("pow", DBL_MAX, 1, SENTINEL, -1);

    f2 = get("_hypot");
    begin();
    r = f2(from_bits(0x7fefffffffffffffULL), 0);
    ok(bits(r) == 0x7fefffffffffffffULL, "_hypot(DBL_MAX, 0) = %g\n", r);
    check("_hypot", DBL_MAX, 0, SENTINEL, -1);

    begin();
    r = ldexp_(-1, 1024);
    ok(bits(r) == 0xfff0000000000000ULL, "ldexp(-1, 1024) = %g\n", r);
    check("ldexp", -1, 1024, ERANGE, _OVERFLOW);

    controlfp_(_RC_UP, _MCW_RC);
    f1 = get("exp");
    begin();
    f1(710);
    check("exp", 710, 0, ERANGE, _OVERFLOW);

    begin();
    r = ldexp_(1, 1024);
    ok(bits(r) == 0x7ff0000000000000ULL, "ldexp(1, 1024) = %g\n", r);
    check("ldexp", 1, 1024, ERANGE, _OVERFLOW);

    begin();
    r = ldexp_(-1, 1024);
    ok(bits(r) == 0xffefffffffffffffULL, "ldexp(-1, 1024) = %g\n", r);
    check("ldexp", -1, 1024, ERANGE, _OVERFLOW);

    controlfp_(_RC_CHOP, _MCW_RC);
    begin();
    r = ldexp_(1, 1024);
    ok(bits(r) == 0x7fefffffffffffffULL, "ldexp(1, 1024) = %g\n", r);
    check("ldexp", 1, 1024, ERANGE, _OVERFLOW);

    begin();
    r = ldexp_(-1, 1024);
    ok(bits(r) == 0xffefffffffffffffULL, "ldexp(-1, 1024) = %g\n", r);
    check("ldexp", -1, 1024, ERANGE, _OVERFLOW);

    controlfp_(old, _MCW_RC);
    p_setusermatherr(NULL);
}

#if defined(_M_IX86) && defined(__GNUC__)
static double ci_call(void *f, int nargs, double a, double b)
{
    double r;

    if (nargs == 1)
        __asm__ volatile ("fldl %2\n\tcall *%1\n\tfstpl %0"
                          : "=m"(r) : "r"(f), "m"(a) : "memory", "st", "st(1)", "eax", "ecx", "edx");
    else
        __asm__ volatile ("fldl %3\n\tfldl %2\n\tcall *%1\n\tfstpl %0"
                          : "=m"(r) : "r"(f), "m"(b), "m"(a) : "memory", "st", "st(1)", "eax", "ecx", "edx");
    return r;
}

/* The entry points of the compilers for x87 operands report as the functions do */
static void test_ci_functions(void)
{
    static const struct
    {
        const char *name;
        const char *reported;
        int nargs;
        double a, b;
        int error;
        int type;
    } rows[] = {
        {"_CIsqrt", "sqrt", 1, -1, 0, EDOM, _DOMAIN}, {"_CIsqrt", "sqrt", 1, 4, 0, SENTINEL, -1},
        {"_CIsin", "sin", 1, INFINITY, 0, EDOM, _DOMAIN}, {"_CIsin", "sin", 1, 1, 0, SENTINEL, -1},
        {"_CIcos", "cos", 1, -INFINITY, 0, EDOM, _DOMAIN}, {"_CItan", "tan", 1, INFINITY, 0, EDOM, _DOMAIN},
        {"_CIacos", "acos", 1, 2, 0, EDOM, _DOMAIN}, {"_CIasin", "asin", 1, -2, 0, EDOM, _DOMAIN},
        {"_CIlog", "log", 1, 0, 0, ERANGE, _SING}, {"_CIlog", "log", 1, -1, 0, EDOM, _DOMAIN},
        {"_CIlog10", "log10", 1, 0, 0, ERANGE, _SING}, {"_CIlog10", "log10", 1, -1, 0, EDOM, _DOMAIN},
        {"_CIexp", "exp", 1, 1000, 0, ERANGE, _OVERFLOW}, {"_CIexp", "exp", 1, -1000, 0, SENTINEL, _UNDERFLOW},
        {"_CIsinh", "sinh", 1, 1000, 0, ERANGE, _OVERFLOW}, {"_CIcosh", "cosh", 1, -1000, 0, ERANGE, _OVERFLOW},
        {"_CIpow", "pow", 2, 0, -1, ERANGE, _SING}, {"_CIpow", "pow", 2, 10, 400, ERANGE, _OVERFLOW},
        {"_CIpow", "pow", 2, -8, 1.0 / 3, EDOM, _DOMAIN}, {"_CIpow", "pow", 2, 2, 3, SENTINEL, -1},
        {"_CIfmod", "fmod", 2, 1, 0, EDOM, _DOMAIN}, {"_CIfmod", "fmod", 2, INFINITY, 1, EDOM, _DOMAIN},
        {"_CIfmod", "fmod", 2, 5, 3, SENTINEL, -1}, {"_CIatan2", "atan2", 2, 1, 2, SENTINEL, -1},
    };
    double r;
    int i;

    set_handler(0, 0, 0);
    for (i = 0; i < ARRAY_SIZE(rows); i++)
    {
        void *f = get(rows[i].name);

        if (!f)
            continue;
        begin();
        r = ci_call(f, rows[i].nargs, rows[i].a, rows[i].b);
        check(rows[i].reported, rows[i].a, rows[i].b, rows[i].error, rows[i].type);
        if (rows[i].nargs == 1 && rows[i].a == 4 && !strcmp(rows[i].name, "_CIsqrt"))
            ok(r == 2, "_CIsqrt(4) = %g\n", r);
        if (!strcmp(rows[i].name, "_CIatan2"))
            ok(fabs(r - 0.4636476090008061) < 1e-15, "_CIatan2(%g, %g) = %.17g\n", rows[i].a, rows[i].b, r);
        if (!strcmp(rows[i].name, "_CIpow") && rows[i].a == 2)
            ok(r == 8, "_CIpow(2, 3) = %g\n", r);
        if (!strcmp(rows[i].name, "_CIfmod") && rows[i].a == 5)
            ok(r == 2, "_CIfmod(5, 3) = %g\n", r);
        if (g_called)
            ok(g_exc.name && !strcmp(g_exc.name, rows[i].reported), "%s: name = %s\n", rows[i].name, g_exc.name);
    }
    p_setusermatherr(NULL);
}
#endif

START_TEST(math_errno)
{
    g_ucrt = GetModuleHandleW(L"ucrtbase.dll");
    if (!g_ucrt)
        g_ucrt = LoadLibraryW(L"ucrtbase.dll");
    if (!g_ucrt)
    {
        skip("ucrtbase.dll is not available\n");
        return;
    }

    p_setusermatherr = get("__setusermatherr");
    if (!p_setusermatherr)
        return;

    test_one_argument();
    test_two_arguments();
    test_fmod_values();
    test_handler_result();
    test_ldexp_scalb();
    test_hypot();
    test_rounding_overflow();
#if defined(_M_IX86) && defined(__GNUC__)
    test_ci_functions();
#endif
}
