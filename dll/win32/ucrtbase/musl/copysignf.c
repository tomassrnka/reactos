/*
 * ReactOS: imported from Wine (libs/musl/src/math/copysignf.c, musl libc 1.2.3, MIT license,
 * see https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT); changes made here are marked "ReactOS:"
 */
#include <math.h>
#include <stdint.h>

float __cdecl copysignf(float x, float y)
{
	union {float f; uint32_t i;} ux={x}, uy={y};
	ux.i &= 0x7fffffff;
	ux.i |= uy.i & 0x80000000;
	return ux.f;
}
