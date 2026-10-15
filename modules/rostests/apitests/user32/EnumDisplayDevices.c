/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for EnumDisplayDevices called from several threads at once
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define THREADS 4

static WCHAR AdapterName[32];
static DISPLAY_DEVICEW Monitor0;
static BOOL HaveMonitor0, HaveMonitor1;
static volatile LONG Stop, Calls, Mismatches;

static BOOL QueryMonitor(DWORD Index, DISPLAY_DEVICEW *Device)
{
    ZeroMemory(Device, sizeof(*Device));
    Device->cb = sizeof(*Device);
    return EnumDisplayDevicesW(AdapterName, Index, Device, 0);
}

static DWORD WINAPI Worker(LPVOID Param)
{
    DISPLAY_DEVICEW Device;
    DEVMODEW DevMode;
    BOOL Ret;

    while (!Stop)
    {
        Ret = QueryMonitor(0, &Device);
        if (Ret != HaveMonitor0 ||
            (Ret && (wcscmp(Device.DeviceName, Monitor0.DeviceName) ||
                     Device.StateFlags != Monitor0.StateFlags ||
                     wcscmp(Device.DeviceID, Monitor0.DeviceID))))
        {
            InterlockedIncrement(&Mismatches);
        }

        if (QueryMonitor(1, &Device) != HaveMonitor1)
            InterlockedIncrement(&Mismatches);

        /* The registry settings of a named device also look the monitors up */
        if (Param)
        {
            ZeroMemory(&DevMode, sizeof(DevMode));
            DevMode.dmSize = sizeof(DevMode);
            EnumDisplaySettingsW(AdapterName, ENUM_REGISTRY_SETTINGS, &DevMode);
        }

        InterlockedIncrement(&Calls);
    }
    return 0;
}

START_TEST(EnumDisplayDevices)
{
    DISPLAY_DEVICEW Adapter, Device;
    HANDLE Threads[THREADS];
    WCHAR Value[16];
    DWORD Seconds = 15, Count, i;

    /* ENUMDD_SECONDS makes the concurrent part run longer for stress runs */
    if (GetEnvironmentVariableW(L"ENUMDD_SECONDS", Value, ARRAYSIZE(Value)) && _wtoi(Value) > 0)
        Seconds = _wtoi(Value);

    ZeroMemory(&Adapter, sizeof(Adapter));
    Adapter.cb = sizeof(Adapter);
    if (!EnumDisplayDevicesW(NULL, 0, &Adapter, 0))
    {
        skip("No display adapter\n");
        return;
    }
    wcscpy(AdapterName, Adapter.DeviceName);

    HaveMonitor0 = QueryMonitor(0, &Monitor0);
    HaveMonitor1 = QueryMonitor(1, &Device);
    trace("Adapter %ls, monitor 0 %ls (%d), monitor 1 (%d)\n",
          AdapterName, HaveMonitor0 ? Monitor0.DeviceName : L"-", HaveMonitor0, HaveMonitor1);

    /* A second sequential query gives the same answer */
    ok(QueryMonitor(0, &Device) == HaveMonitor0, "Monitor 0 changed between two calls\n");
    if (HaveMonitor0)
        ok(!wcscmp(Device.DeviceName, Monitor0.DeviceName), "Name %ls, expected %ls\n", Device.DeviceName, Monitor0.DeviceName);

    for (i = 0; i < THREADS; i++)
    {
        Threads[i] = CreateThread(NULL, 0, Worker, (LPVOID)(ULONG_PTR)(i == THREADS - 1), 0, NULL);
        ok(Threads[i] != NULL, "CreateThread failed with %lu\n", GetLastError());
    }

    Sleep(Seconds * 1000);
    Stop = TRUE;

    for (i = 0; i < THREADS; i++)
    {
        if (!Threads[i])
            continue;
        ok(WaitForSingleObject(Threads[i], 30000) == WAIT_OBJECT_0, "Thread %lu did not stop\n", i);
        CloseHandle(Threads[i]);
    }

    Count = Calls;
    trace("%lu rounds in %lu s, %ld mismatches\n", Count, Seconds, Mismatches);
    ok(Count > 0, "No round completed\n");
    ok(Mismatches == 0, "%ld answers differed from the first one\n", Mismatches);
}
