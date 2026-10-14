/*
 * PROJECT:     ReactOS user32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     RegisterSuspendResumeNotification and UnregisterSuspendResumeNotification (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Measured on Windows 10 22H2: the flags have to be DEVICE_NOTIFY_WINDOW_HANDLE or DEVICE_NOTIFY_CALLBACK, and the
 * recipient is not looked at when the registration is made. A registration ends once; ending it again, or ending
 * none, is an invalid parameter. Nothing suspends this system, so no notification is ever sent.
 */

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>

#define NDEBUG
#include <debug.h>

typedef PVOID HPOWERNOTIFY;

#ifndef DEVICE_NOTIFY_CALLBACK
#define DEVICE_NOTIFY_CALLBACK 0x00000002
#endif

typedef struct _SUSPEND_RESUME_NOTIFY
{
    struct _SUSPEND_RESUME_NOTIFY *Next;
    HANDLE Recipient;
    DWORD Flags;
} SUSPEND_RESUME_NOTIFY, *PSUSPEND_RESUME_NOTIFY;

static PSUSPEND_RESUME_NOTIFY NotifyList;
static CRITICAL_SECTION NotifyLock;
static CRITICAL_SECTION_DEBUG NotifyLockDebug =
{
    0, 0, &NotifyLock,
    { &NotifyLockDebug.ProcessLocksList, &NotifyLockDebug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": NotifyLock") }
};
static CRITICAL_SECTION NotifyLock = { &NotifyLockDebug, -1, 0, 0, 0, 0 };

static
VOID
LockNotify(VOID)
{
    EnterCriticalSection(&NotifyLock);
}

static
VOID
UnlockNotify(VOID)
{
    LeaveCriticalSection(&NotifyLock);
}

HPOWERNOTIFY
WINAPI
RegisterSuspendResumeNotification(
    _In_ HANDLE hRecipient,
    _In_ DWORD Flags)
{
    PSUSPEND_RESUME_NOTIFY Notify;

    if (Flags != DEVICE_NOTIFY_WINDOW_HANDLE && Flags != DEVICE_NOTIFY_CALLBACK)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    Notify = HeapAlloc(GetProcessHeap(), 0, sizeof(*Notify));
    if (!Notify)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    Notify->Recipient = hRecipient;
    Notify->Flags = Flags;

    LockNotify();
    Notify->Next = NotifyList;
    NotifyList = Notify;
    UnlockNotify();

    return (HPOWERNOTIFY)Notify;
}

BOOL
WINAPI
UnregisterSuspendResumeNotification(
    _In_ HPOWERNOTIFY Handle)
{
    PSUSPEND_RESUME_NOTIFY *Link, Notify = NULL;

    LockNotify();
    for (Link = &NotifyList; *Link; Link = &(*Link)->Next)
    {
        if (*Link == (PSUSPEND_RESUME_NOTIFY)Handle)
        {
            Notify = *Link;
            *Link = Notify->Next;
            break;
        }
    }
    UnlockNotify();

    if (!Notify)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    HeapFree(GetProcessHeap(), 0, Notify);
    return TRUE;
}
