/*
 * ReactOS: imported from Wine (libs/musl/src/math/nan.c, musl libc 1.2.3, MIT license,
 * see https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT); changes made here are marked "ReactOS:"
 */
#include <math.h>

double __cdecl nan(const char *s)
{
	return NAN;
}
