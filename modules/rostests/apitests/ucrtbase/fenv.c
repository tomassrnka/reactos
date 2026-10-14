/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the floating-point environment functions of ucrtbase
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <errno.h>
#include <fenv.h>
#include <float.h>
#include <math.h>

int __cdecl __fpe_flt_rounds(void);

#if defined(_M_IX86) || defined(_M_AMD64)

static unsigned int get_mxcsr(void)
{
    unsigned int value;

#ifdef __GNUC__
    __asm__ __volatile__("stmxcsr %0" : "=m"(value));
#else
    value = _mm_getcsr();
#endif
    return value;
}

static void set_mxcsr(unsigned int value)
{
#ifdef __GNUC__
    __asm__ __volatile__("ldmxcsr %0" : : "m"(value));
#else
    _mm_setcsr(value);
#endif
}

#ifdef _M_IX86
static unsigned short get_x87_cw(void)
{
    unsigned short value;

#ifdef __GNUC__
    __asm__ __volatile__("fnstcw %0" : "=m"(value));
#else
    __asm fnstcw [value];
#endif
    return value;
}

static unsigned short get_x87_sw(void)
{
    unsigned short value;

#ifdef __GNUC__
    __asm__ __volatile__("fnstsw %0" : "=m"(value));
#else
    __asm fnstsw [value];
#endif
    return value;
}
#endif /* _M_IX86 */

#endif /* x86 */


/* The words of an fenv_t: the shared low bits and a high byte for each unit, the x87 one only on i386 */
#ifdef _M_IX86
#define FENV_BITS(b)    (((unsigned long)(b) << 24) | ((unsigned long)(b) << 16) | (b))
#define FENV_UP         0x80800200UL
#define FENV_DOWN       0x40400100UL
#else
#define FENV_BITS(b)    (((unsigned long)(b) << 24) | (b))
#define FENV_UP         0x80000200UL
#define FENV_DOWN       0x40000100UL
#endif
#define FENV_CHOP       (FENV_UP | FENV_DOWN)
/* the status of flags that only the SSE unit holds */
#define FENV_SSE_BITS(b) (((unsigned long)(b) << 24) | (b))

#define MXCSR_FLAGS 0x3f

static fenv_t saved_env;

static void test_default_env(void)
{
    fenv_t env;
    int ret;

    _clearfp();
    ret = fegetenv(&env);
    ok(ret == 0, "fegetenv returned %d\n", ret);
    ok(env._Fe_ctl == FE_DFL_ENV->_Fe_ctl, "_Fe_ctl = %lx, expected %lx\n", env._Fe_ctl, FE_DFL_ENV->_Fe_ctl);
    ok(env._Fe_stat == 0, "_Fe_stat = %lx\n", env._Fe_stat);
    ok(get_mxcsr() == 0x1f80, "MXCSR = %x\n", get_mxcsr());
#ifdef _M_IX86
    ok(get_x87_cw() == 0x27f, "x87 control word = %x\n", get_x87_cw());
#endif
}

static void test_rounding(void)
{
    static const struct
    {
        int mode;
        int flt_rounds;
        unsigned int mxcsr_rc;
        unsigned short x87_rc;
        unsigned long fenv_rc;
    } modes[] =
    {
        { FE_TONEAREST,  1, 0x0000, 0x000, 0 },
        { FE_DOWNWARD,   3, 0x2000, 0x400, FENV_DOWN },
        { FE_UPWARD,     2, 0x4000, 0x800, FENV_UP },
        { FE_TOWARDZERO, 0, 0x6000, 0xc00, FENV_CHOP },
    };
    static const int invalid_modes[] = { 0x400, 0x10000, 0x103, -1, 0x1000000 };
    volatile double one = 1.0, three = 3.0;
    double third[4];
    unsigned int i;
    fenv_t env;
    int ret;

    for (i = 0; i < ARRAY_SIZE(modes); i++)
    {
        ret = fesetround(modes[i].mode);
        ok(ret == 0, "fesetround(%x) returned %d\n", modes[i].mode, ret);
        ok(fegetround() == modes[i].mode, "mode %x: fegetround returned %x\n", modes[i].mode, fegetround());
        ok(__fpe_flt_rounds() == modes[i].flt_rounds, "mode %x: __fpe_flt_rounds returned %d\n",
           modes[i].mode, __fpe_flt_rounds());
        ok((_controlfp(0, 0) & _MCW_RC) == (unsigned int)modes[i].mode, "mode %x: _controlfp returned %x\n",
           modes[i].mode, _controlfp(0, 0));
        ok((get_mxcsr() & 0x6000) == modes[i].mxcsr_rc, "mode %x: MXCSR = %x\n", modes[i].mode, get_mxcsr());
#ifdef _M_IX86
        ok((get_x87_cw() & 0xc00) == modes[i].x87_rc, "mode %x: x87 control word = %x\n", modes[i].mode, get_x87_cw());
#endif
        fegetenv(&env);
        ok(env._Fe_ctl == (FE_DFL_ENV->_Fe_ctl | modes[i].fenv_rc), "mode %x: _Fe_ctl = %lx\n", modes[i].mode, env._Fe_ctl);
        third[i] = one / three;
    }
    ok(third[2] > third[1], "1/3 rounded up %.17g, down %.17g\n", third[2], third[1]);
    ok(third[3] == third[1], "1/3 rounded to zero %.17g, down %.17g\n", third[3], third[1]);
    ok(third[0] == third[1] || third[0] == third[2], "1/3 rounded to nearest %.17g\n", third[0]);

    fesetround(FE_UPWARD);
    for (i = 0; i < ARRAY_SIZE(invalid_modes); i++)
    {
        ret = fesetround(invalid_modes[i]);
        ok(ret != 0, "fesetround(%x) returned %d\n", invalid_modes[i], ret);
        ok(fegetround() == FE_UPWARD, "fesetround(%x) changed the mode to %x\n", invalid_modes[i], fegetround());
    }
    fesetround(FE_TONEAREST);
}

static void test_masks(void)
{
    static const struct { int except; unsigned int mxcsr_mask; unsigned short x87_mask; } masks[] =
    {
        { FE_INVALID,   0x0080, 0x01 },
        { FE_DIVBYZERO, 0x0200, 0x04 },
        { FE_OVERFLOW,  0x0400, 0x08 },
        { FE_UNDERFLOW, 0x0800, 0x10 },
        { FE_INEXACT,   0x1000, 0x20 },
    };
    unsigned int i;
    fenv_t env;

    for (i = 0; i < ARRAY_SIZE(masks); i++)
    {
        /* an unmasked exception that does not happen is harmless as long as nothing raises it meanwhile */
        _controlfp(0, masks[i].except);
        fegetenv(&env);
        ok(env._Fe_ctl == (FE_DFL_ENV->_Fe_ctl & ~FENV_BITS(masks[i].except)),
           "unmasked %x: _Fe_ctl = %lx\n", masks[i].except, env._Fe_ctl);
        ok(get_mxcsr() == (0x1f80 & ~masks[i].mxcsr_mask), "unmasked %x: MXCSR = %x\n", masks[i].except, get_mxcsr());
#ifdef _M_IX86
        ok(get_x87_cw() == (0x27f & ~masks[i].x87_mask), "unmasked %x: x87 control word = %x\n", masks[i].except, get_x87_cw());
#endif
        fesetenv(FE_DFL_ENV);
    }

    /* flushing of the denormals is part of the environment */
    _controlfp(_DN_FLUSH, _MCW_DN);
    fegetenv(&env);
    ok(env._Fe_ctl == (FE_DFL_ENV->_Fe_ctl | 0x400), "flush: _Fe_ctl = %lx\n", env._Fe_ctl);
    ok((get_mxcsr() & 0x8040) == 0x8040, "flush: MXCSR = %x\n", get_mxcsr());
    fesetenv(FE_DFL_ENV);
    ok((_controlfp(0, 0) & _MCW_DN) == _DN_SAVE, "_controlfp returned %x after the default environment\n", _controlfp(0, 0));
    ok((get_mxcsr() & 0x8040) == 0, "MXCSR = %x after the default environment\n", get_mxcsr());
    _controlfp(_DN_FLUSH, _MCW_DN);
    fegetenv(&env);
    fesetenv(FE_DFL_ENV);
    fesetenv(&env);
    ok((_controlfp(0, 0) & _MCW_DN) == _DN_FLUSH, "_controlfp returned %x after the saved environment\n", _controlfp(0, 0));
    fesetenv(FE_DFL_ENV);
}

static void test_fesetenv(void)
{
    static const struct
    {
        unsigned long ctl, stat;
        int ret;
        BOOL all_unmasked;  /* TRUE when none of the exceptions is masked afterwards */
    } tests[] =
    {
        { 0x00000001UL, 0, 1, TRUE },   /* a unit bit without its marker */
        { 0x0000003fUL, 0, 1, TRUE },
        { 0x00000000UL, 0, 0, TRUE },   /* an environment of zeros is the one that masks nothing */
        { 0x12345678UL, 0, 1, FALSE },
    };
    unsigned int i;
    fenv_t env, cur;
    int ret;

    fegetenv(&saved_env);
    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        env._Fe_ctl = tests[i].ctl;
        env._Fe_stat = tests[i].stat;
        ret = fesetenv(&env);
        /* the integer code below must not touch the floating point unit: nothing is masked now */
        if (tests[i].all_unmasked)
            ok((_controlfp(0, 0) & (FE_ALL_EXCEPT)) == 0, "{%lx,%lx}: _controlfp returned %x\n", tests[i].ctl, tests[i].stat, _controlfp(0, 0));
        fesetenv(&saved_env);
        ok(ret == tests[i].ret, "fesetenv({%lx,%lx}) returned %d\n", tests[i].ctl, tests[i].stat, ret);
    }

    /* what is understood is applied, the return value only tells that more was given */
    env._Fe_ctl = FE_DFL_ENV->_Fe_ctl;
    env._Fe_stat = 0x123;
    ret = fesetenv(&env);
    fegetenv(&cur);
    ok(ret == 1, "fesetenv with a bad status returned %d\n", ret);
    ok(cur._Fe_stat == 0, "_Fe_stat = %lx\n", cur._Fe_stat);
    fesetenv(&saved_env);

    env._Fe_ctl = FE_DFL_ENV->_Fe_ctl;
    env._Fe_stat = 0;
    ret = fesetenv(&env);
    ok(ret == 0, "fesetenv of the default environment returned %d\n", ret);
    fegetenv(&cur);
    ok(cur._Fe_ctl == FE_DFL_ENV->_Fe_ctl, "_Fe_ctl = %lx\n", cur._Fe_ctl);

#ifdef _M_AMD64
    /* the x87 unit has no place in the environment of this architecture */
    env._Fe_ctl = 0x3f3f103fUL;
    ret = fesetenv(&env);
    fegetenv(&cur);
    ok(ret == 1, "fesetenv with x87 bits returned %d\n", ret);
    ok(cur._Fe_ctl == 0x3f00003fUL, "_Fe_ctl = %lx\n", cur._Fe_ctl);
#endif
    fesetenv(&saved_env);
}

static void test_flags(void)
{
    fenv_t env, env2;
    fexcept_t flags;
    unsigned int ret;
    int iret;

    /* flags that the SSE unit raised are seen, kept by the reads and cleared as a whole by _clearfp */
    _clearfp();
    set_mxcsr(get_mxcsr() | MXCSR_FLAGS);
    ok(fetestexcept(FE_ALL_EXCEPT) == FE_ALL_EXCEPT, "fetestexcept returned %x\n", fetestexcept(FE_ALL_EXCEPT));
    ok(_statusfp() == (FE_ALL_EXCEPT | _SW_DENORMAL), "_statusfp returned %x\n", _statusfp());
    fegetenv(&env);
    ok(env._Fe_stat == (FENV_SSE_BITS(FE_ALL_EXCEPT) | 0x20000020UL), "_Fe_stat = %lx\n", env._Fe_stat);
    ok(fetestexcept(FE_ALL_EXCEPT) == FE_ALL_EXCEPT, "fetestexcept returned %x after reading the control word\n", fetestexcept(FE_ALL_EXCEPT));
    ok((get_mxcsr() & MXCSR_FLAGS) == MXCSR_FLAGS, "MXCSR = %x\n", get_mxcsr());
    iret = fegetexceptflag(&flags, FE_ALL_EXCEPT);
    ok(iret == 0 && flags == FENV_SSE_BITS(FE_ALL_EXCEPT), "fegetexceptflag returned %d, flags %lx\n", iret, flags);
    ret = _clearfp();
    ok(ret == (FE_ALL_EXCEPT | _SW_DENORMAL), "_clearfp returned %x\n", ret);
    ok((get_mxcsr() & MXCSR_FLAGS) == 0, "MXCSR = %x\n", get_mxcsr());
    ok(fetestexcept(FE_ALL_EXCEPT) == 0, "fetestexcept returned %x\n", fetestexcept(FE_ALL_EXCEPT));

    /* feclearexcept leaves the denormal flag, which is not an exception of the C99 set */
    set_mxcsr(get_mxcsr() | MXCSR_FLAGS);
    feclearexcept(FE_INVALID | FE_OVERFLOW);
    ok((get_mxcsr() & MXCSR_FLAGS) == (MXCSR_FLAGS & ~0x9), "feclearexcept(INVALID | OVERFLOW): MXCSR = %x\n", get_mxcsr());
    feclearexcept(FE_ALL_EXCEPT);
    ok((get_mxcsr() & MXCSR_FLAGS) == 0x2, "feclearexcept(ALL): MXCSR = %x\n", get_mxcsr());
    _clearfp();

    /* an environment keeps the flags */
    set_mxcsr(get_mxcsr() | 0x5);
    fegetenv(&env);
    ok(env._Fe_stat == FENV_SSE_BITS(FE_INVALID | FE_DIVBYZERO), "_Fe_stat = %lx\n", env._Fe_stat);
    _clearfp();
    ok(fetestexcept(FE_ALL_EXCEPT) == 0, "fetestexcept returned %x\n", fetestexcept(FE_ALL_EXCEPT));
    iret = fesetenv(&env);
    ok(iret == 0, "fesetenv returned %d\n", iret);
    ok(fetestexcept(FE_ALL_EXCEPT) == (FE_INVALID | FE_DIVBYZERO), "fetestexcept returned %x after fesetenv\n", fetestexcept(FE_ALL_EXCEPT));
    fegetenv(&env2);
    ok(env2._Fe_ctl == env._Fe_ctl && env2._Fe_stat == env._Fe_stat, "environments differ: %lx %lx, %lx %lx\n",
       env._Fe_ctl, env._Fe_stat, env2._Fe_ctl, env2._Fe_stat);
    _clearfp();

    /* the flags of the exception set are put into both units on i386 */
    feclearexcept(FE_ALL_EXCEPT);
    flags = FENV_BITS(FE_INEXACT);
    iret = fesetexceptflag(&flags, FE_INEXACT);
    ok(iret == 0, "fesetexceptflag returned %d\n", iret);
    ok((get_mxcsr() & MXCSR_FLAGS) == 0x20, "MXCSR = %x\n", get_mxcsr());
#ifdef _M_IX86
    ok((get_x87_sw() & 0x3f) == 0x20, "x87 status word = %x\n", get_x87_sw());
#endif
    feclearexcept(FE_ALL_EXCEPT);
    ok((get_mxcsr() & MXCSR_FLAGS) == 0, "MXCSR = %x\n", get_mxcsr());
#ifdef _M_IX86
    ok((get_x87_sw() & 0x3f) == 0, "x87 status word = %x\n", get_x87_sw());
#endif
}

static void test_feholdexcept(void)
{
    fenv_t env;
    int ret;

    /* with everything masked the flags are saved and cleared, and the masks stay */
    fesetenv(FE_DFL_ENV);
    _clearfp();
    set_mxcsr(get_mxcsr() | 0x5);
    ret = feholdexcept(&env);
    ok(ret == 0, "feholdexcept returned %d\n", ret);
    ok(env._Fe_ctl == FE_DFL_ENV->_Fe_ctl, "_Fe_ctl = %lx\n", env._Fe_ctl);
    ok(env._Fe_stat == FENV_SSE_BITS(FE_INVALID | FE_DIVBYZERO), "_Fe_stat = %lx\n", env._Fe_stat);
    ok(fetestexcept(FE_ALL_EXCEPT) == 0, "fetestexcept returned %x\n", fetestexcept(FE_ALL_EXCEPT));
    ok(get_mxcsr() == 0x1f80, "MXCSR = %x\n", get_mxcsr());

    /* an unmasked exception makes it fail, nothing is changed */
    _controlfp(0, _EM_ZERODIVIDE);
    set_mxcsr(get_mxcsr() | 0x5);
    ret = feholdexcept(&env);
    ok(ret == 1, "feholdexcept returned %d\n", ret);
    ok(env._Fe_ctl == (FE_DFL_ENV->_Fe_ctl & ~FENV_BITS(FE_DIVBYZERO)), "_Fe_ctl = %lx\n", env._Fe_ctl);
    ok(env._Fe_stat == FENV_SSE_BITS(FE_INVALID | FE_DIVBYZERO), "_Fe_stat = %lx\n", env._Fe_stat);
    ok(fetestexcept(FE_ALL_EXCEPT) == (FE_INVALID | FE_DIVBYZERO), "fetestexcept returned %x\n", fetestexcept(FE_ALL_EXCEPT));
    ok(get_mxcsr() == 0x1d85, "MXCSR = %x\n", get_mxcsr());
    set_mxcsr(get_mxcsr() & ~MXCSR_FLAGS);
    fesetenv(FE_DFL_ENV);
}

static void test_fpreset(void)
{
    fenv_t env;

    fesetround(FE_UPWARD);
    _controlfp(_DN_FLUSH, _MCW_DN);
    _controlfp(0, _EM_INEXACT);
    _fpreset();
    fegetenv(&env);
    ok(env._Fe_ctl == FE_DFL_ENV->_Fe_ctl, "_Fe_ctl = %lx after _fpreset\n", env._Fe_ctl);
    ok(env._Fe_stat == 0, "_Fe_stat = %lx after _fpreset\n", env._Fe_stat);
    ok(get_mxcsr() == 0x1f80, "MXCSR = %x after _fpreset\n", get_mxcsr());
#ifdef _M_IX86
    ok(get_x87_cw() == 0x27f, "x87 control word = %x after _fpreset\n", get_x87_cw());
#endif
}

START_TEST(fenv)
{
    fenv_t env;

#if !defined(_M_IX86) && !defined(_M_AMD64)
    skip("The floating-point environment is only known on x86\n");
    return;
#else
    fegetenv(&env);
    test_default_env();
    test_rounding();
    test_masks();
    test_fesetenv();
    test_flags();
    test_feholdexcept();
    test_fpreset();
    fesetenv(&env);
#endif
}
