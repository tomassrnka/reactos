/*
 * ReactOS: imported from Wine (libs/musl/src/math/tgammaf.c, musl libc 1.2.3, MIT license,
 * see https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT); changes made here are marked "ReactOS:"
 */
#include <math.h>
#include "libm.h"

float __cdecl tgammaf(float x)
{
	double d = tgamma(x);
	float f = eval_as_float((float)d);

	/* ReactOS: Windows gives +0 and a range error for every x < -38 that is not an integer, and a range
	   error for a result that left the range of float (the one of double has been checked) */
	if (x < -38 && !isnan(d)) {
		errno = ERANGE;
		return 0;
	}
	if (isfinite(x) && x != 0 && (isinf(f) || f == 0))
		errno = ERANGE;
	return f;
}
