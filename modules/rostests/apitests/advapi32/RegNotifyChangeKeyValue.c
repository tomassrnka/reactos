/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test that RegNotifyChangeKeyValue resets its event when it arms a watch
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#ifndef REG_NOTIFY_THREAD_AGNOSTIC
#define REG_NOTIFY_THREAD_AGNOSTIC 0x10000000
#endif

static const WCHAR KeyName[] = L"Software\\ReactOS_RegNotifyChangeKeyValue";
#define FILTER (REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET)

START_TEST(RegNotifyChangeKeyValue)
{
    HKEY hKey;
    HANDLE hEvent;
    DWORD Value = 1, Wait;
    LONG Err;
    int i;

    RegDeleteKeyW(HKEY_CURRENT_USER, KeyName);
    Err = RegCreateKeyExW(HKEY_CURRENT_USER, KeyName, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey, NULL);
    ok_dec(Err, ERROR_SUCCESS);
    if (Err != ERROR_SUCCESS)
        return;

    /* A manual-reset event that is still signaled from the previous watch */
    hEvent = CreateEventW(NULL, TRUE, TRUE, NULL);
    ok(hEvent != NULL, "CreateEvent failed %lu\n", GetLastError());

    /* Arming the watch resets the event */
    Err = RegNotifyChangeKeyValue(hKey, FALSE, FILTER | REG_NOTIFY_THREAD_AGNOSTIC, hEvent, TRUE);
    ok_dec(Err, ERROR_SUCCESS);
    Wait = WaitForSingleObject(hEvent, 0);
    ok_hex(Wait, WAIT_TIMEOUT);

    /* A change signals it */
    Err = RegSetValueExW(hKey, L"v", 0, REG_DWORD, (BYTE *)&Value, sizeof(Value));
    ok_dec(Err, ERROR_SUCCESS);
    Wait = WaitForSingleObject(hEvent, 5000);
    ok_hex(Wait, WAIT_OBJECT_0);

    /* Re-arming without resetting by hand must not complete at once: the typical
     * "wait, handle, re-arm" loop would spin forever otherwise */
    for (i = 0; i < 3; i++)
    {
        Err = RegNotifyChangeKeyValue(hKey, FALSE, FILTER | REG_NOTIFY_THREAD_AGNOSTIC, hEvent, TRUE);
        ok_dec(Err, ERROR_SUCCESS);
        Wait = WaitForSingleObject(hEvent, 100);
        ok_hex(Wait, WAIT_TIMEOUT);
        Value++;
        Err = RegSetValueExW(hKey, L"v", 0, REG_DWORD, (BYTE *)&Value, sizeof(Value));
        ok_dec(Err, ERROR_SUCCESS);
        Wait = WaitForSingleObject(hEvent, 5000);
        ok_hex(Wait, WAIT_OBJECT_0);
    }

    /* The same without the thread agnostic flag */
    SetEvent(hEvent);
    Err = RegNotifyChangeKeyValue(hKey, FALSE, FILTER, hEvent, TRUE);
    ok_dec(Err, ERROR_SUCCESS);
    Wait = WaitForSingleObject(hEvent, 0);
    ok_hex(Wait, WAIT_TIMEOUT);

    CloseHandle(hEvent);
    RegCloseKey(hKey);
    RegDeleteKeyW(HKEY_CURRENT_USER, KeyName);
}
