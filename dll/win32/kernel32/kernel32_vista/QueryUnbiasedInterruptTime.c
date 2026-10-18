/* ReactOS interrupt-time query. SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>

/* This API reads the Vista extension to the shared-data page. */
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN7
#define NTOS_MODE_USER
#include <ndk/kefuncs.h>

BOOL WINAPI
QueryUnbiasedInterruptTime(PULONGLONG UnbiasedTime)
{
    LARGE_INTEGER Time;
    ULONGLONG Bias, NextBias;

    if (!UnbiasedTime)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Retry if resume changes the sleep bias while reading the clock. The
     * kernel currently advances InterruptTime only with clock ticks and
     * leaves the bias zero; a future resume adjustment must publish its bias. */
    do
    {
        Bias = SharedUserData->InterruptTimeBias;
        Time = KiReadSystemTime(&SharedUserData->InterruptTime);
        NextBias = SharedUserData->InterruptTimeBias;
    } while (Bias != NextBias);

    *UnbiasedTime = Time.QuadPart - Bias;
    return TRUE;
}
