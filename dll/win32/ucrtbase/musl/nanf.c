/*
 * ReactOS: imported from Wine (libs/musl/src/math/nanf.c, musl libc 1.2.3, MIT license,
 * see https://git.musl-libc.org/cgit/musl/tree/COPYRIGHT); changes made here are marked "ReactOS:"
 */
#include <math.h>

float __cdecl nanf(const char *s)
{
	return NAN;
}
