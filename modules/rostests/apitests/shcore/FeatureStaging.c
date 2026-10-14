/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the feature staging functions of shcore (Windows 10)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2 (x64 and WOW64): a feature that no configuration names has its default state,
 * no variant and no notification, whatever the identifier and the change time are. The last error is not tested:
 * RecordFeatureUsage changes it on the first call.
 */

#include <apitest.h>

typedef UINT32 (WINAPI *PFN_GetFeatureEnabledState)(UINT32, UINT32);
typedef UINT32 (WINAPI *PFN_GetFeatureVariant)(UINT32, UINT32, UINT32 *, BOOL *);
typedef void (WINAPI *PFN_RecordFeatureUsage)(UINT32, UINT32, UINT32, const char *);
typedef void (WINAPI *PFN_RecordFeatureError)(UINT32, const void *);
typedef void (WINAPI *PFN_Unsubscribe)(PVOID);

START_TEST(FeatureStaging)
{
    HMODULE Shcore = LoadLibraryW(L"shcore.dll");
    PFN_GetFeatureEnabledState pGetFeatureEnabledState;
    PFN_GetFeatureVariant pGetFeatureVariant;
    PFN_RecordFeatureUsage pRecordFeatureUsage;
    PFN_RecordFeatureError pRecordFeatureError;
    PFN_Unsubscribe pUnsubscribe;
    static const UINT32 Features[] = { 0, 1, 12345678, 46283745, 0x7fffffff, 0xffffffff };
    static const UINT32 Times[] = { 0, 1, 2, 3 };
    UINT32 Payload, State;
    BOOL Notification;
    BYTE Error[128] = { 0 };
    ULONG i, j;

    /* ReactOS has the functions from export level 0x601, which a kernel32 with the Windows 7 functions shows */
    BOOL Required = is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules");

    if (!Shcore)
    {
        ok(!Required, "shcore.dll did not load: %lu\n", GetLastError());
        skip("shcore.dll is not available\n");
        return;
    }

    pGetFeatureEnabledState = (PFN_GetFeatureEnabledState)GetProcAddress(Shcore, "GetFeatureEnabledState");
    pGetFeatureVariant = (PFN_GetFeatureVariant)GetProcAddress(Shcore, "GetFeatureVariant");
    pRecordFeatureUsage = (PFN_RecordFeatureUsage)GetProcAddress(Shcore, "RecordFeatureUsage");
    pRecordFeatureError = (PFN_RecordFeatureError)GetProcAddress(Shcore, "RecordFeatureError");
    pUnsubscribe = (PFN_Unsubscribe)GetProcAddress(Shcore, "UnsubscribeFeatureStateChangeNotification");

    /* Windows 8.1 and older have none of them; ReactOS has them from export level 0x601, which a kernel32 with the Windows 7 functions shows */
    if (!pGetFeatureVariant || !pRecordFeatureUsage || !pRecordFeatureError || !pUnsubscribe)
    {
        ok(!Required, "the feature staging functions are not exported\n");
        skip("the feature staging functions are not exported\n");
        return;
    }

    for (i = 0; i < RTL_NUMBER_OF(Features); i++)
    {
        for (j = 0; j < RTL_NUMBER_OF(Times); j++)
        {
            Payload = 0x55555555;
            Notification = (BOOL)0x55;
            State = pGetFeatureVariant(Features[i], Times[j], &Payload, &Notification);
            ok(State == 0, "feature %u, time %u: state %u\n", Features[i], Times[j], State);
            ok(Payload == 0, "feature %u, time %u: payload %#x\n", Features[i], Times[j], Payload);
            ok(Notification == FALSE, "feature %u, time %u: notification %#x\n", Features[i], Times[j], Notification);

            if (pGetFeatureEnabledState)
            {
                State = pGetFeatureEnabledState(Features[i], Times[j]);
                ok(State == 0, "feature %u, time %u: enabled state %u\n", Features[i], Times[j], State);
            }
        }
    }

    /* the records are taken and nothing comes back */
    pRecordFeatureUsage(12345678, 1, 1, "probe");
    pRecordFeatureUsage(12345678, 0, 0, NULL);
    pRecordFeatureUsage(0, 99, 0xffffffff, "");
    pRecordFeatureError(12345678, Error);

    /* ending a subscription that was never made is taken (Windows 10 does so for a null handle) */
    pUnsubscribe(NULL);
}
