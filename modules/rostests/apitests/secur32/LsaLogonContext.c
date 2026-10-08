/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for LSA logon contexts that are created and deregistered
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#define WIN32_NO_STATUS
#include <apitest.h>
#include <ntstatus.h>
#include <ntsecapi.h>

#define ROUNDS 500

static volatile LONG Round;
static PCSTR FailedStep;
static NTSTATUS FailedStatus;

static BOOLEAN Check(PCSTR Step, NTSTATUS Status)
{
    if (Status == STATUS_SUCCESS)
        return TRUE;
    FailedStep = Step;
    FailedStatus = Status;
    return FALSE;
}

static NTSTATUS LookupMsv(HANDLE LsaHandle)
{
    LSA_STRING Name;
    ULONG Package;

    Name.Buffer = MSV1_0_PACKAGE_NAME;
    Name.Length = sizeof(MSV1_0_PACKAGE_NAME) - sizeof(CHAR);
    Name.MaximumLength = sizeof(MSV1_0_PACKAGE_NAME);
    return LsaLookupAuthenticationPackage(LsaHandle, &Name, &Package);
}

static BOOLEAN Deregister(PHANDLE LsaHandle)
{
    HANDLE Handle = *LsaHandle;

    *LsaHandle = NULL;
    return Check("LsaDeregisterLogonProcess", LsaDeregisterLogonProcess(Handle));
}

static DWORD WINAPI Worker(PVOID Param)
{
    HANDLE First = NULL, Second = NULL;
    DWORD Result = 1;

    for (Round = 0; Round < ROUNDS; Round++)
    {
        /* Two contexts at once, deregistered oldest first */
        if (!Check("LsaConnectUntrusted", LsaConnectUntrusted(&First)) ||
            !Check("LsaConnectUntrusted", LsaConnectUntrusted(&Second)) ||
            !Check("LsaLookupAuthenticationPackage", LookupMsv(First)) ||
            !Check("LsaLookupAuthenticationPackage", LookupMsv(Second)) ||
            !Deregister(&First) ||
            !Deregister(&Second))
        {
            goto Cleanup;
        }
    }

    /* The LSA replies to a deregistration before it frees the context, so
     * one more request shows that the last one was carried out */
    if (Check("LsaConnectUntrusted", LsaConnectUntrusted(&First)) &&
        Check("LsaLookupAuthenticationPackage", LookupMsv(First)))
    {
        Result = 0;
    }

Cleanup:
    if (First != NULL)
        LsaDeregisterLogonProcess(First);
    if (Second != NULL)
        LsaDeregisterLogonProcess(Second);
    return Result;
}

START_TEST(LsaLogonContext)
{
    HANDLE Thread;
    DWORD Wait;

    /* A stress test: a context left on the list after it is freed corrupts
     * the heap of lsass, which then asserts, faults or stops answering.
     * The requests run on a thread so that a hung LSA cannot hang the test. */
    Thread = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    ok(Thread != NULL, "CreateThread failed (%lu)\n", GetLastError());
    if (Thread == NULL)
        return;

    Wait = WaitForSingleObject(Thread, 180000);
    ok(Wait == WAIT_OBJECT_0, "The LSA stopped answering in round %ld of %d\n", Round, ROUNDS);
    if (Wait == WAIT_OBJECT_0)
    {
        ok(FailedStep == NULL, "%s failed in round %ld with 0x%08lx\n",
           FailedStep, Round, FailedStatus);
    }
    CloseHandle(Thread);
}
