/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for ExSetTimerResolution
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <kmt_test.h>

START_TEST(ExTimerResolution)
{
    ULONG DefaultIncrement, MinimumIncrement, Increment;

    DefaultIncrement = KeQueryTimeIncrement();

    /* Asking for the default leaves the rate as it is, so this pair only reads the increment */
    Increment = ExSetTimerResolution(DefaultIncrement, TRUE);
    ExSetTimerResolution(0, FALSE);
    if (skip(Increment == DefaultIncrement,
             "Another caller has raised the clock rate (%lu, default %lu)\n",
             Increment, DefaultIncrement))
    {
        return;
    }

    /* One request, one release */
    MinimumIncrement = ExSetTimerResolution(0, TRUE);
    Increment = ExSetTimerResolution(0, FALSE);
    if (skip(MinimumIncrement < DefaultIncrement,
             "The HAL cannot raise the clock rate (%lu, default %lu)\n",
             MinimumIncrement, DefaultIncrement))
    {
        return;
    }
    ok_eq_ulong(Increment, DefaultIncrement);

    /* Two nested requests */
    Increment = ExSetTimerResolution(0, TRUE);
    ok_eq_ulong(Increment, MinimumIncrement);
    Increment = ExSetTimerResolution(0, TRUE);
    ok_eq_ulong(Increment, MinimumIncrement);
    Increment = ExSetTimerResolution(0, FALSE);
    ok_eq_ulong(Increment, MinimumIncrement);
    Increment = ExSetTimerResolution(0, FALSE);
    ok_eq_ulong(Increment, DefaultIncrement);

    /* Three nested requests */
    ExSetTimerResolution(0, TRUE);
    ExSetTimerResolution(0, TRUE);
    Increment = ExSetTimerResolution(0, TRUE);
    ok_eq_ulong(Increment, MinimumIncrement);
    Increment = ExSetTimerResolution(0, FALSE);
    ok_eq_ulong(Increment, MinimumIncrement);
    Increment = ExSetTimerResolution(0, FALSE);
    ok_eq_ulong(Increment, MinimumIncrement);
    Increment = ExSetTimerResolution(0, FALSE);
    ok_eq_ulong(Increment, DefaultIncrement);
}
