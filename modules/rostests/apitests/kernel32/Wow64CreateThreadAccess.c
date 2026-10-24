/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Thread-creation access contract for the WoW64 NtCreateThread thunk (T104)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The WoW64 NtCreateThread thunk creates the native thread with THREAD_ALL_ACCESS
 * for its own setup, then returns a guest handle with the caller's requested
 * access by duplicating that internal handle. The duplicate used to request the
 * guest's DesiredAccess verbatim. A guest THREAD_ALL_ACCESS contains undefined
 * specific bits the source handle may not hold; once DuplicateHandle stopped
 * letting user mode elevate access beyond the source handle, that internal
 * duplicate began failing with STATUS_ACCESS_DENIED, so every WoW64 thread
 * creation failed -- kernel32 CreateThread returned NULL under WoW64 and
 * Chromium's broker init ("Couldn't initialize the broker services.") aborted
 * with zero content processes. The thunk must clamp the duplicate to the access
 * the source handle actually holds, so CreateThread succeeds and the returned
 * handle is usable.
 *
 * This is the regression guard: before the fix the first assertion failed under
 * WoW64 (CreateThread == NULL, GetLastError ERROR_ACCESS_DENIED). It uses the
 * same PE32 image for the native i386 and the WoW64 run; both must pass. The
 * granted-access value is intentionally not pinned to a single constant (it is
 * token- and version-dependent); the contract is that the handle carries the
 * rights a caller needs and nothing outside the thread object's valid mask.
 */

#include "precomp.h"
#include <ndk/obfuncs.h>

/* Full set of valid thread access bits (STANDARD_RIGHTS_ALL | SYNCHRONIZE | the
 * 0xffff specific range). A granted mask must stay within this. */
#define THREAD_VALID_ACCESS (STANDARD_RIGHTS_ALL | SYNCHRONIZE | 0xFFFFu)

/* The rights a thread handle must carry to be usable the way callers (and the
 * thunk itself) rely on: query, suspend/resume and wait. */
#define THREAD_REQUIRED_ACCESS (THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | SYNCHRONIZE)

static DWORD WINAPI AccessWorker(void *Parameter)
{
    return (DWORD)(ULONG_PTR)Parameter;
}

static ACCESS_MASK QueryGranted(HANDLE Handle)
{
    OBJECT_BASIC_INFORMATION Obi;
    NTSTATUS Status = NtQueryObject(Handle, ObjectBasicInformation, &Obi, sizeof(Obi), NULL);
    ok(NT_SUCCESS(Status), "NtQueryObject failed, Status %lx\n", Status);
    return NT_SUCCESS(Status) ? Obi.GrantedAccess : 0;
}

START_TEST(Wow64CreateThreadAccess)
{
    HANDLE Thread, Opened, Duplicated;
    DWORD ThreadId = 0;
    ACCESS_MASK Granted, OpenedGranted;
    BOOL Ok;

    /* kernel32 CreateThread requests THREAD_ALL_ACCESS internally; under WoW64 this
     * drives the NtCreateThread thunk. Before the fix this returned NULL with
     * ERROR_ACCESS_DENIED under WoW64 -- the broker-init regression itself. */
    SetLastError(0xdeadbeef);
    Thread = CreateThread(NULL, 0, AccessWorker, (PVOID)(ULONG_PTR)0x1234, 0, &ThreadId);
    ok(Thread != NULL, "CreateThread failed, GetLastError %lu (broker-init regression)\n", GetLastError());
    if (!Thread)
        return;
    ok(ThreadId != 0, "ThreadId was not reported\n");

    /* The returned handle must be usable: it has to carry the rights the thunk's
     * own setup and ordinary callers rely on, and must not carry bits outside the
     * thread object's valid access mask. */
    Granted = QueryGranted(Thread);
    ok((Granted & THREAD_REQUIRED_ACCESS) == THREAD_REQUIRED_ACCESS,
       "granted access %08lx lacks the rights the handle must carry (need %08lx)\n",
       Granted, (ACCESS_MASK)THREAD_REQUIRED_ACCESS);
    ok((Granted & ~(ACCESS_MASK)THREAD_VALID_ACCESS) == 0,
       "granted access %08lx has bits outside the thread valid access mask\n", Granted);

    /* SYNCHRONIZE is present, so the handle must be waitable. */
    ok(WaitForSingleObject(Thread, 10000) == WAIT_OBJECT_0, "thread wait failed\n");

    /* Opening the thread for THREAD_ALL_ACCESS must also succeed. */
    Opened = OpenThread(THREAD_ALL_ACCESS, FALSE, ThreadId);
    ok(Opened != NULL, "OpenThread(THREAD_ALL_ACCESS) failed, GetLastError %lu\n", GetLastError());
    if (Opened)
    {
        OpenedGranted = QueryGranted(Opened);
        ok((OpenedGranted & THREAD_REQUIRED_ACCESS) == THREAD_REQUIRED_ACCESS,
           "opened handle granted %08lx lacks required rights\n", OpenedGranted);
        ok((OpenedGranted & ~(ACCESS_MASK)THREAD_VALID_ACCESS) == 0,
           "opened handle granted %08lx has bits outside the thread valid access mask\n", OpenedGranted);
        CloseHandle(Opened);
    }

    /* DUPLICATE_SAME_ACCESS reproduces the source handle's access and must succeed
     * (this is the semantics the thunk now relies on instead of over-requesting). */
    Duplicated = NULL;
    Ok = DuplicateHandle(GetCurrentProcess(), Thread, GetCurrentProcess(),
                         &Duplicated, 0, FALSE, DUPLICATE_SAME_ACCESS);
    ok(Ok, "DuplicateHandle(DUPLICATE_SAME_ACCESS) failed, GetLastError %lu\n", GetLastError());
    if (Ok)
    {
        ok(QueryGranted(Duplicated) == Granted,
           "same-access duplicate granted %08lx, expected %08lx\n", QueryGranted(Duplicated), Granted);
        CloseHandle(Duplicated);
    }

    CloseHandle(Thread);
}
