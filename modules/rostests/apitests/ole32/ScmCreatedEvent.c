/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the COM service control manager readiness event
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <aclapi.h>

#ifndef EVENT_QUERY_STATE
#define EVENT_QUERY_STATE 0x0001
#endif

/* EventBasicInformation of NtQueryEvent */
typedef struct _TEST_EVENT_BASIC_INFORMATION
{
    ULONG EventType;    /* 0: notification, 1: synchronization */
    LONG EventState;
} TEST_EVENT_BASIC_INFORMATION;

typedef LONG (NTAPI *PFN_NtQueryEvent)(HANDLE, ULONG, PVOID, ULONG, PULONG);

START_TEST(ScmCreatedEvent)
{
    PFN_NtQueryEvent pNtQueryEvent;
    TEST_EVENT_BASIC_INFORMATION ebi;
    SID_IDENTIFIER_AUTHORITY WorldAuthority = { SECURITY_WORLD_SID_AUTHORITY };
    PSID pEveryone = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    PACL pDacl = NULL;
    ACL_SIZE_INFORMATION AclInfo;
    ACCESS_MASK EveryoneAllowed = 0;
    HANDLE hEvent;
    ULONG Length;
    DWORD dwError, i;
    LONG Status;

    hEvent = OpenEventW(SYNCHRONIZE | EVENT_QUERY_STATE | READ_CONTROL, FALSE, L"Global\\ScmCreatedEvent");
    ok(hEvent != NULL, "OpenEventW failed (%lu)\n", GetLastError());
    if (!hEvent)
        return;

    /* Signaled for good once the RPC subsystem is ready */
    ok_long(WaitForSingleObject(hEvent, 0), WAIT_OBJECT_0);
    pNtQueryEvent = (PFN_NtQueryEvent)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryEvent");
    if (pNtQueryEvent)
    {
        Status = pNtQueryEvent(hEvent, 0, &ebi, sizeof(ebi), &Length);
        ok_long(Status, 0);
        if (Status == 0)
        {
            ok_long(ebi.EventType, 0);
            ok_long(ebi.EventState, 1);
        }
    }

    /* Everyone may wait for it and query it, but not set or reset it */
    dwError = GetSecurityInfo(hEvent, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                              NULL, NULL, &pDacl, NULL, &pSD);
    ok_long(dwError, ERROR_SUCCESS);
    ok(AllocateAndInitializeSid(&WorldAuthority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &pEveryone),
       "AllocateAndInitializeSid failed\n");
    if (dwError == ERROR_SUCCESS && pDacl && pEveryone &&
        GetAclInformation(pDacl, &AclInfo, sizeof(AclInfo), AclSizeInformation))
    {
        for (i = 0; i < AclInfo.AceCount; ++i)
        {
            ACCESS_ALLOWED_ACE *pAce;

            if (!GetAce(pDacl, i, (PVOID *)&pAce))
                continue;
            if (pAce->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && EqualSid(&pAce->SidStart, pEveryone))
                EveryoneAllowed |= pAce->Mask;
        }
    }
    ok(pDacl != NULL, "No DACL\n");
    ok(EveryoneAllowed == (SYNCHRONIZE | READ_CONTROL | EVENT_QUERY_STATE),
       "Everyone is allowed 0x%lx\n", EveryoneAllowed);

    if (pEveryone)
        FreeSid(pEveryone);
    if (pSD)
        LocalFree(pSD);
    CloseHandle(hEvent);
}
