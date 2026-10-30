/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The first AcquireCredentialsHandle reads the SCHANNEL protocol configuration and must not close
 *              a handle it never opened
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#include <apitest.h>
#include <ntsecapi.h>
#include <sspi.h>
#include <schannel.h>

#define HANDLE_COUNT 4096
#define POISON_SLOTS 8192

static HANDLE Handles[HANDLE_COUNT];
static volatile ULONG_PTR PoisonSink;

/*
 * read_config used to call RegCloseKey on an uninitialized HKEY when the SCHANNEL\Protocols key does not exist
 * (the default). Native NtOpenKey zeroes its output handle on failure, but the WoW64 NtOpenKey thunk of a 32-bit
 * process does not, so the stale stack value was closed: an unrelated handle of the process went invalid.
 * The handle table is filled densely, so that a small stale value is a live handle (the Google Updater case was 0xF0),
 * and the stack below the caller is filled with handle values.
 */
static DECLSPEC_NOINLINE void PoisonStack(void)
{
    volatile ULONG_PTR Block[POISON_SLOTS];
    ULONG i;

    for (i = 0; i < POISON_SLOTS; i++)
        Block[i] = (ULONG_PTR)Handles[i % HANDLE_COUNT];
    PoisonSink = Block[POISON_SLOTS / 2];
}

/* The configuration is read once per process, so this has to be the first credential the process acquires (the
 * test runner starts a process per test). Meaningful in a 32-bit process under WoW64 (the secur32 apitest built
 * for i386); it has to pass everywhere. */
START_TEST(ReadConfigHandles)
{
    CredHandle Cred;
    PSecPkgInfoW Info;
    TimeStamp Expiry;
    SECURITY_STATUS Status;
    DWORD Flags;
    ULONG i, Invalid = 0;

    for (i = 0; i < HANDLE_COUNT; i++)
    {
        Handles[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!Handles[i])
        {
            skip("CreateEvent %lu failed: %lu\n", i, GetLastError());
            goto Done;
        }
    }

    /* Load and initialize the provider first (that runs deep, and would overwrite the poison), without reading
     * the configuration */
    Status = QuerySecurityPackageInfoW((SEC_WCHAR *)UNISP_NAME_W, &Info);
    ok(Status == SEC_E_OK, "QuerySecurityPackageInfo returned 0x%08lx\n", Status);
    if (Status == SEC_E_OK)
        FreeContextBuffer(Info);

    PoisonStack();
    Status = AcquireCredentialsHandleW(NULL, (SEC_WCHAR *)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL, NULL, NULL, NULL,
                                       &Cred, &Expiry);
    ok(Status == SEC_E_OK, "AcquireCredentialsHandle returned 0x%08lx\n", Status);

    for (i = 0; i < HANDLE_COUNT; i++)
    {
        if (!GetHandleInformation(Handles[i], &Flags))
        {
            ok(0, "handle %lu (%p) is no longer valid: %lu\n", i, Handles[i], GetLastError());
            Invalid++;
        }
    }
    ok(Invalid == 0, "%lu of %u handles went invalid\n", Invalid, HANDLE_COUNT);

    if (Status == SEC_E_OK)
        FreeCredentialsHandle(&Cred);

Done:
    for (i = 0; i < HANDLE_COUNT; i++)
    {
        if (Handles[i])
            CloseHandle(Handles[i]);
    }
}
