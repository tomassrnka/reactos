/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for fmod / fmodf
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#if !defined(_CRTBLD) && !defined(_M_IX86)
#define _CRTBLD // we do not want the inline fmodf
#endif
#include "math_helpers.h"

static volatile double s_x, s_y;
static volatile float s_xf, s_yf;

static double CallFmod(double x, double y)
{
    s_x = x;
    s_y = y;
    return fmod(s_x, s_y);
}

static float CallFmodf(float x, float y)
{
    s_xf = x;
    s_yf = y;
    return fmodf(s_xf, s_yf);
}

static const struct
{
    double x, y, result;
} s_fmod_tests[] =
{
    { 0.0, 12.0, 0.0 },
    { 5.0, 12.0, 5.0 },
    { 12.0, 12.0, 0.0 },
    { 13.5, 12.0, 1.5 },
    { -13.5, 12.0, -1.5 },
    { 13.5, -12.0, 1.5 },
    { 1.0e12, 12.0, 4.0 },
    { 3.7e14, 12.0, 4.0 },
    { 1.0e300, 3.0, 0.0 },
    { 5.5, 1.0e300, 5.5 },
};

void Test_fmod(void)
{
    double z;
    int i;

    for (i = 0; i < _countof(s_fmod_tests); i++)
    {
        z = CallFmod(s_fmod_tests[i].x, s_fmod_tests[i].y);
        ok(z == s_fmod_tests[i].result, "fmod(%.17g, %.17g) = %.17g, expected %.17g\n",
           s_fmod_tests[i].x, s_fmod_tests[i].y, z, s_fmod_tests[i].result);
    }

    z = CallFmod(5.0, 0.0);
    ok(_isnan(z), "fmod(5, 0) = %.17g, expected a NaN\n", z);
    z = CallFmod(u64_to_dbl(0x7ff0000000000000), 2.0);
    ok(_isnan(z), "fmod(inf, 2) = %.17g, expected a NaN\n", z);
    z = CallFmod(u64_to_dbl(0x7ff8000000000000), 2.0);
    ok(_isnan(z), "fmod(NaN, 2) = %.17g, expected a NaN\n", z);
    z = CallFmod(2.0, u64_to_dbl(0x7ff8000000000000));
    ok(_isnan(z), "fmod(2, NaN) = %.17g, expected a NaN\n", z);
}

void Test_fmodf(void)
{
    float z;
    int i;

    for (i = 0; i < _countof(s_fmod_tests); i++)
    {
        if (fabs(s_fmod_tests[i].x) > 1.0e30 || fabs(s_fmod_tests[i].y) > 1.0e30 || fabs(s_fmod_tests[i].x) > 1.0e7)
            continue;
        z = CallFmodf((float)s_fmod_tests[i].x, (float)s_fmod_tests[i].y);
        ok(z == (float)s_fmod_tests[i].result, "fmodf(%.9g, %.9g) = %.9g, expected %.9g\n",
           s_fmod_tests[i].x, s_fmod_tests[i].y, (double)z, s_fmod_tests[i].result);
    }

    z = CallFmodf(5.0f, 0.0f);
    ok(_isnan(z), "fmodf(5, 0) = %.9g, expected a NaN\n", (double)z);
    z = CallFmodf(13.5f, 12.0f);
    ok(z == 1.5f, "fmodf(13.5, 12) = %.9g, expected 1.5\n", (double)z);
}

START_TEST(fmod)
{
    Test_fmod();
    Test_fmodf();
}
