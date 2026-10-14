/*
 * ReactOS: imported from Wine (libs/musl/src/math/lgamma.c, musl libc 1.2.3, MIT license,
 * see https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT); changes made here are marked "ReactOS:"
 */
#include <math.h>
#include "libm.h"

double __cdecl lgamma(double x)
{
	int sign;
	double r;

	/* ReactOS: the pole is answered here, the log of 0 would call the handler of math errors that Windows does not call */
	if (x == 0) {
		volatile double zero = 0.0, divided;

		errno = ERANGE;
		/* the divide-by-zero flag, as on Windows; the result is the bits of the infinity (INFINITY is made by an overflow, which round-down clamps) */
		divided = 1.0 / zero;
		(void)divided;
		return asdouble(0x7ff0000000000000ULL);
	}
	/* ReactOS: Wine keeps the sign in a global that Windows does not have */
	r = eval_as_double(__lgamma_r(x, &sign));

	/* ReactOS: the poles and an overflow are range errors on Windows, the infinite arguments are not */
	if (overflowed(r) && isfinite(x))
		errno = ERANGE;
	return r;
}
