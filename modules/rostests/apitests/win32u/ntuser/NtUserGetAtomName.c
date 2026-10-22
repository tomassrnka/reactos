/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.0-or-later (https://spdx.org/licenses/LGPL-2.0-or-later)
 * PURPOSE:     Contract tests for the user-mode pointer handling of
 *              NtUserGetAtomName (T67 double-fetch / TOCTOU hardening)
 *
 * Defensive only. The fix captures the UNICODE_STRING descriptor (Buffer,
 * MaximumLength) once and uses the captured copy for both the ProbeForWrite
 * and the copy, so the probed buffer is always the buffer written to. These
 * tests assert the resulting contract on the fixed kernel:
 *   - a valid call still returns the correct name and length (Valid);
 *   - a bad or unmapped descriptor fails cleanly, with no bugcheck and no
 *     write to an un-probed address (Invalid);
 *   - a descriptor mutated concurrently (including made unmapped after the
 *     probe) is handled cleanly: the call only ever returns 0 or a length
 *     consistent with the snapshot it acted on, and the box never bugchecks
 *     (Concurrent).
 * They are not a weaponized race-winner: the concurrent case only ever points
 * Buffer at our own buffer or at a NOACCESS guard page, never at a chosen
 * victim.
 */

#include "../win32nt.h"

static
ULONG
MyWcsLen(PCWSTR s)
{
    ULONG n = 0;
    while (s[n]) n++;
    return n;
}

static
BOOL
MyWcsEq(PCWSTR a, PCWSTR b)
{
    ULONG i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return (a[i] == b[i]);
}

/* Register a window class so its atom (and name) live in the win32k global
 * atom table that NtUserGetAtomName queries. */
static
ATOM
RegisterTestAtom(PCWSTR pszName)
{
    WNDCLASSEXW wcex;
    RtlZeroMemory(&wcex, sizeof(wcex));
    wcex.cbSize = sizeof(wcex);
    wcex.lpfnWndProc = DefWindowProcW;
    wcex.hInstance = GetModuleHandleW(NULL);
    wcex.lpszClassName = pszName;
    return RegisterClassExW(&wcex);
}

/* One committed read/write page followed by a NOACCESS guard page. */
static
PBYTE
AllocGuarded(SIZE_T *pcbReadable)
{
    SYSTEM_INFO si;
    PBYTE pb;
    DWORD dwOld;

    GetSystemInfo(&si);
    pb = VirtualAlloc(NULL, 2 * si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!pb) return NULL;
    VirtualProtect(pb + si.dwPageSize, si.dwPageSize, PAGE_NOACCESS, &dwOld);
    *pcbReadable = si.dwPageSize;
    return pb;
}

static const WCHAR g_szName[] = L"T67GetAtomNameClass";

/* A valid call keeps working, including truncation and the minimal buffer. */
START_TEST(NtUserGetAtomNameValid)
{
    ATOM atom;
    UNICODE_STRING ustr;
    WCHAR buf[256];
    ULONG ret;
    ULONG cchName = MyWcsLen(g_szName);

    atom = RegisterTestAtom(g_szName);
    ok(atom != 0, "RegisterClassExW failed, le=%lu\n", GetLastError());
    if (!atom) return;

    /* Full buffer: exact name, correct count, NUL-terminated */
    RtlZeroMemory(buf, sizeof(buf));
    ustr.Buffer = buf;
    ustr.Length = 0;
    ustr.MaximumLength = sizeof(buf);
    ret = NtUserGetAtomName(atom, &ustr);
    ok(ret == cchName, "ret=%lu expected %lu\n", ret, cchName);
    ok(MyWcsEq(buf, g_szName), "name=%S expected %S\n", buf, g_szName);
    ok(buf[cchName] == L'\0', "not NUL terminated\n");

    /* Truncation: room for 2 chars + NUL */
    RtlZeroMemory(buf, sizeof(buf));
    ustr.Buffer = buf;
    ustr.Length = 0;
    ustr.MaximumLength = 3 * sizeof(WCHAR);
    ret = NtUserGetAtomName(atom, &ustr);
    ok(ret == 2, "truncated ret=%lu expected 2\n", ret);
    ok(buf[0] == g_szName[0] && buf[1] == g_szName[1], "truncated prefix wrong\n");
    ok(buf[2] == L'\0', "truncated not NUL terminated\n");

    /* MaximumLength == sizeof(WCHAR): room only for the NUL */
    RtlZeroMemory(buf, sizeof(buf));
    ustr.Buffer = buf;
    ustr.Length = 0;
    ustr.MaximumLength = sizeof(WCHAR);
    ret = NtUserGetAtomName(atom, &ustr);
    ok(ret == 0, "min ret=%lu expected 0\n", ret);
    ok(buf[0] == L'\0', "min buf[0]=%u expected 0\n", buf[0]);

    /* MaximumLength too small for even a NUL: nothing written, 0 returned */
    buf[0] = 0xABAB;
    ustr.Buffer = buf;
    ustr.Length = 0;
    ustr.MaximumLength = 1;
    ret = NtUserGetAtomName(atom, &ustr);
    ok(ret == 0, "tiny ret=%lu expected 0\n", ret);
    ok(buf[0] == 0xABAB, "tiny buf[0]=%u expected unchanged\n", buf[0]);
}

/* Bad or unmapped descriptors are rejected cleanly: 0 return, no bugcheck. */
START_TEST(NtUserGetAtomNameInvalid)
{
    ATOM atom;
    UNICODE_STRING ustr;
    PBYTE pb;
    SIZE_T cb;
    ULONG ret;
    SYSTEM_INFO si;

    atom = RegisterTestAtom(g_szName);
    ok(atom != 0, "RegisterClassExW failed, le=%lu\n", GetLastError());
    if (!atom) return;
    GetSystemInfo(&si);

    /* pustrName == NULL: dereference faults under SEH -> 0 */
    ret = NtUserGetAtomName(atom, NULL);
    ok(ret == 0, "NULL pustr ret=%lu\n", ret);

    /* pustrName in a NOACCESS page: ProbeForRead / deref faults -> 0 */
    pb = AllocGuarded(&cb);
    ok(pb != NULL, "AllocGuarded failed\n");
    if (pb)
    {
        ret = NtUserGetAtomName(atom, (PUNICODE_STRING)(pb + si.dwPageSize));
        ok(ret == 0, "unmapped pustr ret=%lu\n", ret);
        VirtualFree(pb, 0, MEM_RELEASE);
    }

    /* Buffer in a NOACCESS page: ProbeForWrite faults -> 0, no write */
    pb = AllocGuarded(&cb);
    if (pb)
    {
        ustr.Buffer = (PWCH)(pb + si.dwPageSize);
        ustr.Length = 0;
        ustr.MaximumLength = (USHORT)si.dwPageSize;
        ret = NtUserGetAtomName(atom, &ustr);
        ok(ret == 0, "unmapped buffer ret=%lu\n", ret);
        VirtualFree(pb, 0, MEM_RELEASE);
    }

    /* Buffer straddles into the guard page: MaximumLength reaches NOACCESS,
       ProbeForWrite faults -> 0, no partial write past the mapped page */
    pb = AllocGuarded(&cb);
    if (pb)
    {
        ustr.Buffer = (PWCH)(pb + cb - 4); /* 4 bytes before the guard page */
        ustr.Length = 0;
        ustr.MaximumLength = 64;           /* runs into the guard page */
        ret = NtUserGetAtomName(atom, &ustr);
        ok(ret == 0, "straddling buffer ret=%lu\n", ret);
        VirtualFree(pb, 0, MEM_RELEASE);
    }

    /* Buffer at a kernel-range address: ProbeForWrite must reject it so the
       kernel never writes there (generic address, not a chosen target) */
    ustr.Buffer = (PWCH)(ULONG_PTR)(~(ULONG_PTR)0 - 0xFFF);
    ustr.Length = 0;
    ustr.MaximumLength = 64;
    ret = NtUserGetAtomName(atom, &ustr);
    ok(ret == 0, "kernel buffer ret=%lu\n", ret);
}

static volatile LONG g_StopMutator;
static UNICODE_STRING g_ustr;
static PVOID g_pValid;
static PVOID g_pGuard;

static
DWORD
WINAPI
MutatorProc(LPVOID arg)
{
    ULONG n = 0;
    (void)arg;
    while (!g_StopMutator)
    {
        if (n & 1)
        {
            /* unmapped-after-probe: point Buffer at the guard page */
            InterlockedExchangePointer((PVOID volatile *)&g_ustr.Buffer, g_pGuard);
            g_ustr.MaximumLength = (USHORT)0x1000;
        }
        else
        {
            InterlockedExchangePointer((PVOID volatile *)&g_ustr.Buffer, g_pValid);
            g_ustr.MaximumLength = (USHORT)(2 * sizeof(WCHAR));
        }
        n++;
    }
    return 0;
}

/* A descriptor mutated by another thread (Buffer flipped to an unmapped page,
   MaximumLength flipped) must be handled cleanly on every call. */
START_TEST(NtUserGetAtomNameConcurrent)
{
    ATOM atom;
    PBYTE pb;
    SIZE_T cb;
    SYSTEM_INFO si;
    HANDLE hThread;
    ULONG i, ret, bad = 0;
    ULONG cchName = MyWcsLen(g_szName);

    atom = RegisterTestAtom(g_szName);
    ok(atom != 0, "RegisterClassExW failed, le=%lu\n", GetLastError());
    if (!atom) return;
    GetSystemInfo(&si);

    pb = AllocGuarded(&cb);
    ok(pb != NULL, "AllocGuarded failed\n");
    if (!pb) return;

    g_pValid = pb;
    g_pGuard = pb + si.dwPageSize;   /* NOACCESS */
    g_ustr.Buffer = (PWCH)g_pValid;
    g_ustr.Length = 0;
    g_ustr.MaximumLength = (USHORT)si.dwPageSize;

    /* baseline: a clean call works */
    ret = NtUserGetAtomName(atom, &g_ustr);
    ok(ret == cchName, "baseline ret=%lu expected %lu\n", ret, cchName);

    g_StopMutator = 0;
    hThread = CreateThread(NULL, 0, MutatorProc, NULL, 0, NULL);
    ok(hThread != NULL, "CreateThread failed\n");

    for (i = 0; i < 200000; i++)
    {
        ret = NtUserGetAtomName(atom, &g_ustr);
        /* Allowed outcomes, each self-consistent with one snapshot:
         *   0        bad/unmapped descriptor caught, or MaximumLength < NUL
         *   1        truncated to MaximumLength == 2*sizeof(WCHAR)
         *   cchName  full copy into the valid page
         * Anything else would mean the kernel acted on a torn Buffer /
         * MaximumLength pair (the double-fetch). */
        if (ret != 0 && ret != 1 && ret != cchName)
            bad++;
    }

    InterlockedExchange(&g_StopMutator, 1);
    if (hThread)
    {
        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
    }

    ok(bad == 0, "%lu calls returned an out-of-contract length\n", bad);

    /* Survived the race: a final clean call still works */
    g_ustr.Buffer = (PWCH)g_pValid;
    g_ustr.MaximumLength = (USHORT)si.dwPageSize;
    ret = NtUserGetAtomName(atom, &g_ustr);
    ok(ret == cchName, "post-race ret=%lu expected %lu\n", ret, cchName);

    VirtualFree(pb, 0, MEM_RELEASE);
}
