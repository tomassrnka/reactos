
#include <stdint.h>
#include <intrin.h>
#include <malloc.h>
#define _USE_MATH_DEFINES
#include <math.h>

// atexit is needed by libsupc++
extern int __cdecl _crt_atexit(void (__cdecl*)(void));
int __cdecl atexit(void (__cdecl* function)(void))
{
    return _crt_atexit(function);
}

void* __cdecl operator_new(size_t size)
{
    return malloc(size);
}

void _cdecl operator_delete(void *mem)
{
    free(mem);
}

#ifdef _M_IX86
void _chkesp_failed(void)
{
    __debugbreak();
}
#endif

int __cdecl __acrt_initialize_sse2(void)
{
    return 0;
}

// The following stubs cannot be implemented as stubs by spec2def, because they are intrinsics

#ifdef _MSC_VER
#pragma warning(disable:4163) // not available as an intrinsic function
#pragma warning(disable:4164) // intrinsic function not declared
#pragma function(fma)
#pragma function(fmaf)
#pragma function(log2)
#pragma function(log2f)
#endif

double fma(double x, double y, double z)
{
    // Simplistic implementation
    return (x * y) + z;
}

float fmaf(float x, float y, float z)
{
    // Simplistic implementation
    return (x * y) + z;
}

double log2(double x)
{
    int e;
    double m;

    /* Zero, negative, infinite and NaN arguments keep the behavior of log() */
    if (!(x > 0.0) || x == INFINITY)
        return log(x) * M_LOG2E;

    /* x = m * 2^e with m in [sqrt(0.5), sqrt(2)): exact for the powers of two */
    m = frexp(x, &e);
    if (m < M_SQRT1_2)
    {
        m *= 2.0;
        e--;
    }
    return e + log(m) * M_LOG2E;
}

float log2f(float x)
{
    return (float)log2((double)x);
}
