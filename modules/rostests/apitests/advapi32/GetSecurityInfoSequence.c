/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The security descriptor calls behind PowerShell's Get-Acl, with a timeout guard
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Get-Acl on a file stopped the 32-bit pwsh under WoW64. The cause was elsewhere (wintrust lacked
 * WTHelperGetFileHash, see WTHelperGetFileHash.c), but the whole call sequence is kept here: size
 * query then exact buffer for GetFileSecurity and NtQuerySecurityObject, GetNamedSecurityInfo,
 * the SDDL text, LookupAccountSid and LsaLookupSids of owner and group. The values are those of
 * Windows 10 22H2 (probed with this program run as a 32-bit process). A call that does not return
 * within 30 seconds fails the test instead of stopping the test run.
 */

#include "precomp.h"
#include <aclapi.h>
#include <sddl.h>
#include <ntsecapi.h>

typedef LONG (NTAPI *PFN_NtQuerySecurityObject)(HANDLE, ULONG, PVOID, ULONG, PULONG);

typedef struct
{
    WCHAR Path[MAX_PATH];
    SE_OBJECT_TYPE Type;
    BOOL Sizes;     /* file system objects: the handle and size query calls */
} PROBE;

static void CheckFileSecurity(const WCHAR *Path)
{
    SECURITY_INFORMATION Info = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    BYTE Tiny[4], *Buffer;
    DWORD Needed = 0, Needed2 = 0;
    BOOL Ret;

    SetLastError(0xdeadbeef);
    Ret = GetFileSecurityW(Path, Info, NULL, 0, &Needed);
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "GetFileSecurityW size query: %d, %lu\n", Ret, GetLastError());
    ok(Needed >= sizeof(SECURITY_DESCRIPTOR_RELATIVE), "Needed %lu\n", Needed);

    SetLastError(0xdeadbeef);
    Ret = GetFileSecurityW(Path, Info, Tiny, sizeof(Tiny), &Needed2);
    ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "GetFileSecurityW tiny buffer: %d, %lu\n", Ret, GetLastError());
    ok(Needed2 == Needed, "Needed %lu then %lu\n", Needed, Needed2);

    Buffer = HeapAlloc(GetProcessHeap(), 0, Needed + 64);
    if (!Buffer)
        return;
    Needed2 = 0;
    Ret = GetFileSecurityW(Path, Info, Buffer, Needed + 64, &Needed2);
    ok(Ret, "GetFileSecurityW failed: %lu\n", GetLastError());
    ok(Needed2 == Needed, "Length %lu, size query %lu\n", Needed2, Needed);
    ok(Ret && IsValidSecurityDescriptor(Buffer), "Invalid descriptor\n");
    HeapFree(GetProcessHeap(), 0, Buffer);
}

static void CheckNtQuery(const WCHAR *Path)
{
    static const ULONG Infos[] = { OWNER_SECURITY_INFORMATION, GROUP_SECURITY_INFORMATION, DACL_SECURITY_INFORMATION,
                                   OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION };
    PFN_NtQuerySecurityObject pNtQuerySecurityObject =
        (PFN_NtQuerySecurityObject)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySecurityObject");
    HANDLE File;
    ULONG i, Needed, Dummy;
    LONG Status;
    BYTE Tiny[8], *Buffer;

    File = CreateFileW(Path, READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    ok(File != INVALID_HANDLE_VALUE, "Open failed: %lu\n", GetLastError());
    if (File == INVALID_HANDLE_VALUE || !pNtQuerySecurityObject)
        return;

    for (i = 0; i < ARRAYSIZE(Infos); i++)
    {
        Needed = 0xdeadbeef;
        Status = pNtQuerySecurityObject(File, Infos[i], NULL, 0, &Needed);
        ok(Status == STATUS_BUFFER_TOO_SMALL, "Info %lx: size query status %lx\n", Infos[i], Status);
        ok(Needed >= sizeof(SECURITY_DESCRIPTOR_RELATIVE) && Needed < 0x10000, "Info %lx: needed %lu\n", Infos[i], Needed);

        Dummy = 0xdeadbeef;
        Status = pNtQuerySecurityObject(File, Infos[i], Tiny, sizeof(Tiny), &Dummy);
        ok(Status == STATUS_BUFFER_TOO_SMALL, "Info %lx: tiny buffer status %lx\n", Infos[i], Status);
        ok(Dummy == Needed, "Info %lx: needed %lu then %lu\n", Infos[i], Needed, Dummy);

        Buffer = HeapAlloc(GetProcessHeap(), 0, Needed);
        if (!Buffer)
            break;
        Dummy = 0xdeadbeef;
        Status = pNtQuerySecurityObject(File, Infos[i], Buffer, Needed, &Dummy);
        ok(Status == STATUS_SUCCESS, "Info %lx: exact buffer status %lx\n", Infos[i], Status);
        ok(Dummy == Needed, "Info %lx: length %lu, size query %lu\n", Infos[i], Dummy, Needed);
        ok(Status || IsValidSecurityDescriptor(Buffer), "Info %lx: invalid descriptor\n", Infos[i]);
        HeapFree(GetProcessHeap(), 0, Buffer);
    }
    CloseHandle(File);
}

static void CheckNames(PSID Owner, PSID Group)
{
    PSID Sids[2] = { Owner, Group };
    LSA_OBJECT_ATTRIBUTES Attributes;
    LSA_HANDLE Policy = NULL;
    PLSA_REFERENCED_DOMAIN_LIST Domains = NULL;
    PLSA_TRANSLATED_NAME Names = NULL;
    NTSTATUS Status;
    ULONG i;

    for (i = 0; i < 2; i++)
    {
        WCHAR Name[256], Domain[256];
        DWORD NameLen = 0, DomainLen = 0, NameLen2 = ARRAYSIZE(Name), DomainLen2 = ARRAYSIZE(Domain);
        SID_NAME_USE Use = 0;
        BOOL Ret;

        SetLastError(0xdeadbeef);
        Ret = LookupAccountSidW(NULL, Sids[i], NULL, &NameLen, NULL, &DomainLen, &Use);
        ok(!Ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "LookupAccountSidW %lu size query: %d, %lu\n", i, Ret, GetLastError());
        ok(NameLen > 1, "Name length %lu\n", NameLen);
        Ret = LookupAccountSidW(NULL, Sids[i], Name, &NameLen2, Domain, &DomainLen2, &Use);
        ok(Ret, "LookupAccountSidW %lu failed: %lu\n", i, GetLastError());
        ok(!Ret || (NameLen2 + 1 == NameLen && Use != 0), "Name %ls (%lu, query %lu), use %d\n", Name, NameLen2, NameLen, Use);
    }

    memset(&Attributes, 0, sizeof(Attributes));
    Status = LsaOpenPolicy(NULL, &Attributes, POLICY_LOOKUP_NAMES, &Policy);
    ok(Status == STATUS_SUCCESS, "LsaOpenPolicy: %lx\n", Status);
    if (Status)
        return;
    Status = LsaLookupSids(Policy, 2, Sids, &Domains, &Names);
    ok(Status == STATUS_SUCCESS, "LsaLookupSids: %lx\n", Status);
    if (Status == STATUS_SUCCESS)
    {
        ok(Domains && Domains->Entries >= 1, "Domains\n");
        for (i = 0; Names && Domains && i < 2; i++)
        {
            ok(Names[i].Use != SidTypeUnknown && Names[i].Use != SidTypeInvalid, "Name %lu: use %d\n", i, Names[i].Use);
            ok(Names[i].Name.Length > 0, "Name %lu is empty\n", i);
            ok(Names[i].DomainIndex >= 0 && (ULONG)Names[i].DomainIndex < Domains->Entries,
               "Name %lu: domain index %ld of %lu\n", i, (long)Names[i].DomainIndex, Domains->Entries);
        }
    }
    if (Names)
        LsaFreeMemory(Names);
    if (Domains)
        LsaFreeMemory(Domains);
    LsaClose(Policy);
}

static DWORD WINAPI ProbeThread(LPVOID Context)
{
    PROBE *Probe = Context;
    PSID Owner = NULL, Group = NULL;
    PACL Dacl = NULL;
    PSECURITY_DESCRIPTOR Sd = NULL;
    LPWSTR Sddl = NULL;
    TRUSTEE_W Trustee;
    ACCESS_MASK Mask = 0;
    DWORD Error;

    if (Probe->Sizes)
    {
        CheckFileSecurity(Probe->Path);
        CheckNtQuery(Probe->Path);
    }

    Error = GetNamedSecurityInfoW(Probe->Path, Probe->Type,
                                  OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                  &Owner, &Group, &Dacl, NULL, &Sd);
    ok(Error == ERROR_SUCCESS, "GetNamedSecurityInfoW: %lu\n", Error);
    if (Error)
        return 0;
    ok(Owner && IsValidSid(Owner), "Owner\n");
    ok(Group && IsValidSid(Group), "Group\n");
    ok(Dacl && IsValidAcl(Dacl), "Dacl\n");

    ok(ConvertSecurityDescriptorToStringSecurityDescriptorW(Sd, SDDL_REVISION_1,
           OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &Sddl, NULL),
       "SDDL: %lu\n", GetLastError());
    ok(!Sddl || wcsncmp(Sddl, L"O:", 2) == 0, "SDDL %ls\n", Sddl);
    if (Sddl)
        LocalFree(Sddl);

    if (Owner && Group)
        CheckNames(Owner, Group);

    if (Owner && Dacl)
    {
        BuildTrusteeWithSidW(&Trustee, Owner);
        Error = GetEffectiveRightsFromAclW(Dacl, &Trustee, &Mask);
        ok(Error == ERROR_SUCCESS, "GetEffectiveRightsFromAclW: %lu\n", Error);
    }
    LocalFree(Sd);
    return 0;
}

static void RunGuarded(const WCHAR *Path, SE_OBJECT_TYPE Type, BOOL Sizes)
{
    PROBE Probe;
    HANDLE Thread;
    DWORD Wait;

    wcscpy(Probe.Path, Path);
    Probe.Type = Type;
    Probe.Sizes = Sizes;
    Thread = CreateThread(NULL, 0, ProbeThread, &Probe, 0, NULL);
    ok(Thread != NULL, "CreateThread: %lu\n", GetLastError());
    if (!Thread)
        return;
    Wait = WaitForSingleObject(Thread, 30000);
    ok(Wait == WAIT_OBJECT_0, "%ls: the call sequence did not return within 30 seconds\n", Path);
    if (Wait == WAIT_OBJECT_0)
        CloseHandle(Thread);
    else
        ExitProcess(1); /* leave the stuck thread behind */
}

START_TEST(GetSecurityInfoSequence)
{
    WCHAR Dir[MAX_PATH], File[MAX_PATH];
    HANDLE Handle;
    DWORD Written;

    ok(GetTempPathW(MAX_PATH, Dir) != 0, "GetTempPath\n");
    ok(GetTempFileNameW(Dir, L"acl", 0, File) != 0, "GetTempFileName\n");
    Handle = CreateFileW(File, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (Handle == INVALID_HANDLE_VALUE)
    {
        skip("No temporary file: %lu\n", GetLastError());
        return;
    }
    WriteFile(Handle, "acl", 3, &Written, NULL);
    CloseHandle(Handle);

    RunGuarded(File, SE_FILE_OBJECT, TRUE);
    RunGuarded(Dir, SE_FILE_OBJECT, TRUE);
    RunGuarded(L"MACHINE\\SOFTWARE", SE_REGISTRY_KEY, FALSE);

    DeleteFileW(File);
}
