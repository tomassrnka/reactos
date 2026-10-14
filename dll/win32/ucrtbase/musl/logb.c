/*
 * ReactOS: imported from Wine (libs/musl/src/math/logb.c, musl libc 1.2.3, MIT license,
 * see https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT); changes made here are marked "ReactOS:"
 */
#include <math.h>
#include "libm.h"

/*
special cases:
	logb(+-0) = -inf, and raise divbyzero
	logb(+-inf) = +inf
	logb(nan) = nan
*/

double __cdecl logb(double x)
{
	if (!isfinite(x))
		return x * x;
	if (x == 0)
		return math_error(_SING, "_logb", x, 0, -1 / (x * x));
	return ilogb(x);
}
