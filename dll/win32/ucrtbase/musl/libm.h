/*
 * PROJECT:     ReactOS ucrtbase
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Support header for the musl math code in this directory
 * COPYRIGHT:   Parts imported from musl libc 1.2.3 (src/internal/libm.h) through Wine (libs/musl),
 *              see https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT
 */

#ifndef _UCRTBASE_MUSL_LIBM_H
#define _UCRTBASE_MUSL_LIBM_H

#include <stdint.h>
#include <float.h>
#include <math.h>
#include <errno.h>

/* musl exports a few old names as weak aliases, ucrtbase does not */
#define weak_alias(old, new)

/* Reports a math error the way the Windows CRT does: set errno, return the value */
double math_error(int type, const char *name, double arg1, double arg2, double retval);

/* The result of an overflow is infinity, and the maximum when the rounding mode is not to nearest; in the
   mode to nearest a result of exactly the maximum is a valid one */
static inline int overflowed(double r)
{
    return isinf(r) || (fabs(r) >= DBL_MAX && (_controlfp(0, 0) & _MCW_RC) != _RC_NEAR);
}

static inline int overflowedf(float r)
{
    return isinf(r) || (fabsf(r) >= FLT_MAX && (_controlfp(0, 0) & _MCW_RC) != _RC_NEAR);
}

/* Kernels and reentrant functions shared between the musl files */
double __sin(double x, double y, int iy);
double __cos(double x, double y);
float __sindf(double x);
float __cosdf(double x);
double __lgamma_r(double x, int *signgamp);
float __lgammaf_r(float x, int *signgamp);

/* Evaluate an expression as the specified type, dropping excess precision */
static inline float eval_as_float(float x)
{
    volatile float y = x;
    return y;
}

static inline double eval_as_double(double x)
{
    volatile double y = x;
    return y;
}

/* fp_barrier returns its input, but limits code transformations */
static inline float fp_barrierf(float x)
{
    volatile float y = x;
    return y;
}

static inline double fp_barrier(double x)
{
    volatile double y = x;
    return y;
}

/* fp_force_eval ensures that the input value is computed when that is otherwise unused */
static inline void fp_force_evalf(float x)
{
    volatile float y;
    y = x;
    (void)y;
}

static inline void fp_force_eval(double x)
{
    volatile double y;
    y = x;
    (void)y;
}

#define FORCE_EVAL(x) do {                        \
    if (sizeof(x) == sizeof(float)) {             \
        fp_force_evalf(x);                        \
    } else {                                      \
        fp_force_eval(x);                         \
    }                                             \
} while(0)

#define asuint(f) ((union{float _f; uint32_t _i;}){f})._i
#define asfloat(i) ((union{uint32_t _i; float _f;}){i})._f
#define asuint64(f) ((union{double _f; uint64_t _i;}){f})._i
#define asdouble(i) ((union{uint64_t _i; double _f;}){i})._f

#define EXTRACT_WORDS(hi,lo,d)                    \
do {                                              \
  uint64_t __u = asuint64(d);                     \
  (hi) = __u >> 32;                               \
  (lo) = (uint32_t)__u;                           \
} while (0)

#define GET_HIGH_WORD(hi,d)                       \
do {                                              \
  (hi) = asuint64(d) >> 32;                       \
} while (0)

#define GET_LOW_WORD(lo,d)                        \
do {                                              \
  (lo) = (uint32_t)asuint64(d);                   \
} while (0)

#define INSERT_WORDS(d,hi,lo)                     \
do {                                              \
  (d) = asdouble(((uint64_t)(hi)<<32) | (uint32_t)(lo)); \
} while (0)

#define SET_HIGH_WORD(d,hi)                       \
  INSERT_WORDS(d, hi, (uint32_t)asuint64(d))

#define SET_LOW_WORD(d,lo)                        \
  INSERT_WORDS(d, asuint64(d)>>32, lo)

#define GET_FLOAT_WORD(w,d)                       \
do {                                              \
  (w) = asuint(d);                                \
} while (0)

#define SET_FLOAT_WORD(d,w)                       \
do {                                              \
  (d) = asfloat(w);                               \
} while (0)

#endif
