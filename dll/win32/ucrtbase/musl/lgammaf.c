/*
 * ReactOS: imported from Wine (libs/musl/src/math/lgammaf.c, musl libc 1.2.3, MIT license,
 * see https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT); changes made here are marked "ReactOS:"
 */
#include <math.h>
#include "libm.h"

float __cdecl lgammaf(float x)
{
	int sign;
	float r;

	/* ReactOS: the pole is answered here, the log of 0 would call the handler of math errors that Windows does not call */
	if (x == 0) {
		errno = ERANGE;
		return INFINITY;
	}
	/* ReactOS: Wine keeps the sign in a global that Windows does not have */
	r = eval_as_float(__lgammaf_r(x, &sign));

	/* ReactOS: the poles and an overflow are range errors on Windows, the infinite arguments are not */
	if (overflowedf(r) && isfinite(x))
		errno = ERANGE;
	return r;
}
