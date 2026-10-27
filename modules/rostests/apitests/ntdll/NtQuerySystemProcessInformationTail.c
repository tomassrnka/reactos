/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Regression test for the SystemProcessInformation NextEntryOffset
 *              chain terminating within the reported ReturnLength, even when the
 *              last enumerated process is a terminating/zombie process.
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * Before the fix in ExpQuerySystemProcessInformation (ntoskrnl/ex/sysinfo.c),
 * a trailing exiting process was skipped without zeroing the previous (real
 * last) entry's NextEntryOffset, leaving it pointing at offset == ReturnLength.
 * A consumer that walks the chain with the standard "offset += NextEntryOffset;
 * assert(offset < ReturnLength)" pattern (e.g. Firefox ProcInfo_win.cpp) then
 * reads one entry past the buffer / aborts. This test reproduces that layout by
 * keeping a just-exited child process alive-as-zombie (handle open) while it
 * repeatedly queries and walks the list.
 */

#include "precomp.h"

static
BOOLEAN
WalkTerminatesWithinBuffer(
    _In_reads_bytes_(BufLen) PUCHAR Buffer,
    _In_ ULONG BufLen)
{
    ULONG Offset = 0;

    for (;;)
    {
        PSYSTEM_PROCESS_INFORMATION Spi;

        /* This is exactly the bound a chain-walking consumer relies on. */
        if (Offset >= BufLen)
            return FALSE;

        Spi = (PSYSTEM_PROCESS_INFORMATION)(Buffer + Offset);
        if (Spi->NextEntryOffset == 0)
            return TRUE;

        Offset += Spi->NextEntryOffset;
    }
}

static
HANDLE
SpawnExitedChild(VOID)
{
    WCHAR CommandLine[] = L"cmd.exe /c exit";
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;

    if (!CreateProcessW(NULL, CommandLine, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        return NULL;
    }

    /* The child exits; keeping its handle open delays reaping, so it lingers
     * as a terminating process - and, being the most recently created, is a
     * strong candidate for the tail of the active process list. */
    WaitForSingleObject(ProcessInfo.hProcess, 5000);
    CloseHandle(ProcessInfo.hThread);
    return ProcessInfo.hProcess;
}

#define BATCHES           200
#define CHILDREN_PER_BATCH  4
#define QUERIES_PER_BATCH  40
#define INITIAL_BUFFER   (512 * 1024)

START_TEST(NtQuerySystemProcessInformationTail)
{
    ULONG Batch, Child, Query;
    ULONG Queries = 0, Violations = 0;

    for (Batch = 0; Batch < BATCHES && Violations == 0; Batch++)
    {
        HANDLE Zombies[CHILDREN_PER_BATCH] = { NULL };

        for (Child = 0; Child < CHILDREN_PER_BATCH; Child++)
            Zombies[Child] = SpawnExitedChild();

        for (Query = 0; Query < QUERIES_PER_BATCH && Violations == 0; Query++)
        {
            ULONG BufLen = INITIAL_BUFFER;
            PUCHAR Buffer = NULL;
            NTSTATUS Status = STATUS_UNSUCCESSFUL;

            for (;;)
            {
                ULONG ReturnLength = 0;

                free(Buffer);
                Buffer = malloc(BufLen);
                if (Buffer == NULL)
                    break;

                Status = NtQuerySystemInformation(SystemProcessInformation,
                                                  Buffer, BufLen, &ReturnLength);
                if (Status == STATUS_INFO_LENGTH_MISMATCH)
                {
                    BufLen += 8192;
                    continue;
                }

                /* On success the kernel reports the bytes written here, which is
                 * what a consumer uses as the walk bound. */
                BufLen = ReturnLength;
                break;
            }

            if (Buffer == NULL)
                continue;

            if (NT_SUCCESS(Status))
            {
                Queries++;
                if (!WalkTerminatesWithinBuffer(Buffer, BufLen))
                    Violations++;
            }

            free(Buffer);
        }

        for (Child = 0; Child < CHILDREN_PER_BATCH; Child++)
            if (Zombies[Child] != NULL)
                CloseHandle(Zombies[Child]);
    }

    trace("Walked %lu SystemProcessInformation snapshots under process churn\n", Queries);
    ok(Violations == 0,
       "SystemProcessInformation NextEntryOffset chain overran the reported ReturnLength "
       "in %lu of %lu snapshots (trailing exiting process left the last entry's "
       "NextEntryOffset non-zero)\n",
       Violations, Queries);
}
