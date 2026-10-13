/*
 * ReactOS: imported from Wine (libs/musl/src/math/fminf.c, musl libc 1.2.3, MIT license,
 * see https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT); changes made here are marked "ReactOS:"
 */
#include <math.h>

float __cdecl fminf(float x, float y)
{
	if (isnan(x))
		return y;
	if (isnan(y))
		return x;
	/* handle signed zeros, see C99 Annex F.9.9.2 */
	if (signbit(x) != signbit(y))
		return signbit(x) ? x : y;
	return x < y ? x : y;
}
