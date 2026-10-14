/*
 * ReactOS: imported from Wine (libs/musl/src/math/logbf.c, musl libc 1.2.3, MIT license,
 * see https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT); changes made here are marked "ReactOS:"
 */
#include <math.h>
#include "libm.h"

float __cdecl logbf(float x)
{
	if (!isfinite(x))
		return x * x;
	if (x == 0) {
		errno = ERANGE;
		return -1/(x*x);
	}
	return ilogbf(x);
}
