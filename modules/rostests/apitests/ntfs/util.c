/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Helpers of the NTFS tests: test directory, test user, descriptors, opens
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define NT_USER_NAME L"ntfsapitest"
#define NT_USER_PASSWORD L"Ntfs-Api-Test-2026!"

PSID SidAdmins, SidSystem, SidEveryone, SidCreatorOwner, SidUser;
HANDLE UserToken;
static WCHAR TestRoot[MAX_PATH], TestDir[MAX_PATH];
static BOOLEAN Admin, Prepared;
static UCHAR UserSidBuffer[SECURITY_MAX_SID_SIZE];

static PSID NtWellKnownSid(WELL_KNOWN_SID_TYPE Type)
{
    DWORD Size = SECURITY_MAX_SID_SIZE;
    PSID Sid = LocalAlloc(LMEM_FIXED, Size);
    if (Sid && !CreateWellKnownSid(Type, NULL, Sid, &Size))
    {
        LocalFree(Sid);
        Sid = NULL;
    }
    return Sid;
}

BOOLEAN NtSetPrivilege(HANDLE Token, PCWSTR Name, BOOLEAN Enable)
{
    TOKEN_PRIVILEGES Tp;
    BOOL Ok;
    if (!LookupPrivilegeValueW(NULL, Name, &Tp.Privileges[0].Luid))
        return FALSE;
    Tp.PrivilegeCount = 1;
    Tp.Privileges[0].Attributes = Enable ? SE_PRIVILEGE_ENABLED : 0;
    Ok = AdjustTokenPrivileges(Token, FALSE, &Tp, sizeof(Tp), NULL, NULL);
    return Ok && GetLastError() == ERROR_SUCCESS;
}

static VOID NtPrepareUser(VOID)
{
    USER_INFO_1 Ui;
    USER_INFO_1003 Pw;
    NET_API_STATUS Net;
    DWORD Err = 0;
    UCHAR Buffer[256];
    DWORD Len;

    RtlZeroMemory(&Ui, sizeof(Ui));
    Ui.usri1_name = NT_USER_NAME;
    Ui.usri1_password = NT_USER_PASSWORD;
    Ui.usri1_priv = USER_PRIV_USER;
    Ui.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    Net = NetUserAdd(NULL, 1, (LPBYTE)&Ui, &Err);
    if (Net == NERR_UserExists)
    {
        Pw.usri1003_password = NT_USER_PASSWORD;
        Net = NetUserSetInfo(NULL, NT_USER_NAME, 1003, (LPBYTE)&Pw, &Err);
    }
    if (Net != NERR_Success)
    {
        trace("NetUserAdd/NetUserSetInfo failed %lu\n", Net);
        return;
    }
    if (!LogonUserW(NT_USER_NAME, L".", NT_USER_PASSWORD, LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &UserToken))
    {
        trace("LogonUserW failed %lu\n", GetLastError());
        UserToken = NULL;
        return;
    }
    if (!GetTokenInformation(UserToken, TokenUser, Buffer, sizeof(Buffer), &Len) ||
        !CopySid(sizeof(UserSidBuffer), UserSidBuffer, ((PTOKEN_USER)Buffer)->User.Sid))
    {
        trace("TokenUser failed %lu\n", GetLastError());
        CloseHandle(UserToken);
        UserToken = NULL;
        return;
    }
    SidUser = (PSID)UserSidBuffer;
}

static BOOLEAN NtPrepare(VOID)
{
    WCHAR Volume[MAX_PATH], FsName[32];
    HANDLE Process;
    BOOL IsMember = FALSE;
    DWORD Len;

    if (Prepared)
        return TestRoot[0] != 0;
    Prepared = TRUE;
    SidAdmins = NtWellKnownSid(WinBuiltinAdministratorsSid);
    SidSystem = NtWellKnownSid(WinLocalSystemSid);
    SidEveryone = NtWellKnownSid(WinWorldSid);
    SidCreatorOwner = NtWellKnownSid(WinCreatorOwnerSid);
    CheckTokenMembership(NULL, SidAdmins, &IsMember);
    Admin = IsMember ? TRUE : FALSE;

    Len = GetEnvironmentVariableW(L"NTFS_APITEST_DIR", TestRoot, MAX_PATH);
    if (!Len || Len >= MAX_PATH)
    {
        GetCurrentDirectoryW(MAX_PATH - 20, TestRoot);
        if (TestRoot[wcslen(TestRoot) - 1] != L'\\')
            wcscat(TestRoot, L"\\");
        wcscat(TestRoot, L"ntfs_apitest");
    }
    CreateDirectoryW(TestRoot, NULL);
    if (!GetVolumePathNameW(TestRoot, Volume, MAX_PATH) ||
        !GetVolumeInformationW(Volume, NULL, 0, NULL, NULL, NULL, FsName, RTL_NUMBER_OF(FsName)) ||
        wcscmp(FsName, L"NTFS"))
    {
        skip("%ls is not on an NTFS volume\n", TestRoot);
        TestRoot[0] = 0;
        return FALSE;
    }
    if (!Admin)
    {
        skip("The NTFS tests set up their files as an administrator\n");
        TestRoot[0] = 0;
        return FALSE;
    }
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Process))
    {
        /* For the cleanup of files whose descriptors deny everyone. */
        NtSetPrivilege(Process, L"SeBackupPrivilege", TRUE);
        NtSetPrivilege(Process, L"SeRestorePrivilege", TRUE);
        CloseHandle(Process);
    }
    NtPrepareUser();
    trace("NTFS tests in %ls, volume %ls, test user %s\n", TestRoot, Volume, UserToken ? "ready" : "unavailable");
    return TRUE;
}

BOOLEAN NtInit(PCSTR TestName)
{
    NT_ACE Aces[3];
    NTSTATUS Status;

    if (!NtPrepare())
        return FALSE;
    _snwprintf(TestDir, MAX_PATH - 1, L"%ls\\%hs", TestRoot, TestName);
    NtCleanup();
    Aces[0].Sid = SidAdmins; Aces[0].Mask = FILE_ALL_ACCESS; Aces[0].Flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE; Aces[0].Deny = FALSE;
    Aces[1].Sid = SidSystem; Aces[1].Mask = FILE_ALL_ACCESS; Aces[1].Flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE; Aces[1].Deny = FALSE;
    Aces[2].Sid = SidUser ? SidUser : SidAdmins;
    Aces[2].Mask = FILE_TRAVERSE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
    Aces[2].Flags = 0; Aces[2].Deny = FALSE;
    Status = NtMakeDir(TestDir, Aces, 3, TRUE);
    ok(Status == STATUS_SUCCESS, "Creating %ls: 0x%08lx\n", TestDir, Status);
    return NT_SUCCESS(Status);
}

VOID NtPath(PWSTR Out, PCWSTR Name)
{
    _snwprintf(Out, MAX_PATH - 1, L"%ls\\%ls", TestDir, Name);
}

PCWSTR NtTestDir(VOID)
{
    return TestDir;
}

BOOLEAN NtIsAdmin(VOID)
{
    return Admin;
}

BOOLEAN NtHaveUser(VOID)
{
    if (!UserToken)
        skip("No non-administrator test user\n");
    return UserToken != NULL;
}

VOID NtBeginUser(VOID)
{
    ok(ImpersonateLoggedOnUser(UserToken), "ImpersonateLoggedOnUser failed %lu\n", GetLastError());
}

VOID NtEndUser(VOID)
{
    RevertToSelf();
}

NTSTATUS NtOpen(PCWSTR Path, ACCESS_MASK Access, ULONG Share, ULONG Disposition, ULONG Options,
                PSECURITY_DESCRIPTOR Sd, PHANDLE Handle)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Iosb;
    HANDLE H = NULL;
    NTSTATUS Status;

    if (Handle)
        *Handle = NULL;
    if (!RtlDosPathNameToNtPathName_U(Path, &Name, NULL, NULL))
        return STATUS_OBJECT_PATH_SYNTAX_BAD;
    InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE, NULL, Sd);
    Status = NtCreateFile(&H, Access, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL, Share, Disposition, Options, NULL, 0);
    RtlFreeUnicodeString(&Name);
    if (NT_SUCCESS(Status))
    {
        if (Handle)
            *Handle = H;
        else
            NtClose(H);
    }
    return Status;
}

NTSTATUS NtOpenById(HANDLE VolumeOrDir, ULONGLONG FileId, ACCESS_MASK Access, ULONG Disposition, PHANDLE Handle)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Iosb;
    HANDLE H = NULL;
    NTSTATUS Status;

    if (Handle)
        *Handle = NULL;
    Name.Buffer = (PWSTR)&FileId;
    Name.Length = Name.MaximumLength = sizeof(FileId);
    InitializeObjectAttributes(&Oa, &Name, 0, VolumeOrDir, NULL);
    Status = NtCreateFile(&H, Access | SYNCHRONIZE, &Oa, &Iosb, NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          Disposition, FILE_OPEN_BY_FILE_ID | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (NT_SUCCESS(Status))
    {
        if (Handle)
            *Handle = H;
        else
            NtClose(H);
    }
    return Status;
}

PSECURITY_DESCRIPTOR NtMakeSd(const NT_ACE *Aces, ULONG Count, BOOLEAN Protected)
{
    return NtMakeSdControl(Aces, Count, Protected ? SE_DACL_PROTECTED : 0);
}

PSECURITY_DESCRIPTOR NtMakeSdControl(const NT_ACE *Aces, ULONG Count, SECURITY_DESCRIPTOR_CONTROL Control)
{
    SECURITY_DESCRIPTOR Abs;
    PACL Acl;
    ULONG AclSize = sizeof(ACL), i;
    DWORD Size = 0;
    PSECURITY_DESCRIPTOR Rel;

    for (i = 0; i < Count; i++)
        AclSize += sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(Aces[i].Sid);
    Acl = LocalAlloc(LMEM_FIXED, AclSize);
    if (!Acl || !InitializeAcl(Acl, AclSize, ACL_REVISION))
        return NULL;
    for (i = 0; i < Count; i++)
    {
        BOOL Ok = Aces[i].Deny ? AddAccessDeniedAceEx(Acl, ACL_REVISION, Aces[i].Flags, Aces[i].Mask, Aces[i].Sid)
                               : AddAccessAllowedAceEx(Acl, ACL_REVISION, Aces[i].Flags, Aces[i].Mask, Aces[i].Sid);
        if (!Ok)
        {
            LocalFree(Acl);
            return NULL;
        }
    }
    InitializeSecurityDescriptor(&Abs, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&Abs, TRUE, Acl, FALSE);
    if (Control)
        SetSecurityDescriptorControl(&Abs, Control, Control);
    MakeSelfRelativeSD(&Abs, NULL, &Size);
    Rel = LocalAlloc(LMEM_FIXED, Size);
    if (Rel && !MakeSelfRelativeSD(&Abs, Rel, &Size))
    {
        LocalFree(Rel);
        Rel = NULL;
    }
    LocalFree(Acl);
    return Rel;
}

NTSTATUS NtMakeDir(PCWSTR Path, const NT_ACE *Aces, ULONG Count, BOOLEAN Protected)
{
    return NtMakeDirControl(Path, Aces, Count, Protected ? SE_DACL_PROTECTED : 0);
}

NTSTATUS NtMakeDirControl(PCWSTR Path, const NT_ACE *Aces, ULONG Count, SECURITY_DESCRIPTOR_CONTROL Control)
{
    PSECURITY_DESCRIPTOR Sd = Count ? NtMakeSdControl(Aces, Count, Control) : NULL;
    NTSTATUS Status;
    if (Count && !Sd)
        return STATUS_NO_MEMORY;
    Status = NtOpen(Path, FILE_LIST_DIRECTORY | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    FILE_CREATE, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, Sd, NULL);
    if (Sd)
        LocalFree(Sd);
    return Status;
}

NTSTATUS NtMakeFile(PCWSTR Path, const NT_ACE *Aces, ULONG Count, BOOLEAN Protected, PCSTR Data)
{
    PSECURITY_DESCRIPTOR Sd = Count ? NtMakeSd(Aces, Count, Protected) : NULL;
    IO_STATUS_BLOCK Iosb;
    HANDLE H;
    NTSTATUS Status;
    if (Count && !Sd)
        return STATUS_NO_MEMORY;
    Status = NtOpen(Path, FILE_GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    FILE_CREATE, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, Sd, &H);
    if (Sd)
        LocalFree(Sd);
    if (NT_SUCCESS(Status))
    {
        if (Data)
            Status = NtWriteFile(H, NULL, NULL, NULL, &Iosb, (PVOID)Data, (ULONG)strlen(Data), NULL, NULL);
        NtClose(H);
    }
    return Status;
}

ACCESS_MASK NtGrantedAccess(HANDLE Handle)
{
    OBJECT_BASIC_INFORMATION Info;
    ULONG Len;
    if (!NT_SUCCESS(NtQueryObject(Handle, ObjectBasicInformation, &Info, sizeof(Info), &Len)))
        return 0;
    return Info.GrantedAccess;
}

ULONGLONG NtFileId(HANDLE Handle)
{
    FILE_INTERNAL_INFORMATION Info;
    IO_STATUS_BLOCK Iosb;
    if (!NT_SUCCESS(NtQueryInformationFile(Handle, &Iosb, &Info, sizeof(Info), FileInternalInformation)))
        return 0;
    return Info.IndexNumber.QuadPart;
}

PACL NtGetDacl(PCWSTR Path, PSTR Text, ULONG TextSize, PSECURITY_DESCRIPTOR *SdOut)
{
    PSECURITY_DESCRIPTOR Sd;
    BOOLEAN Present = FALSE, Defaulted;
    SECURITY_DESCRIPTOR_CONTROL Control = 0;
    PACL Dacl = NULL;
    ULONG Len = 0, i, Used = 0;
    HANDLE H;
    NTSTATUS Status;

    *SdOut = NULL;
    if (Text && TextSize)
        Text[0] = 0;
    Status = NtOpen(Path, READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                    FILE_OPEN_FOR_BACKUP_INTENT, NULL, &H);
    if (!NT_SUCCESS(Status))
        return NULL;
    NtQuerySecurityObject(H, DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION, NULL, 0, &Len);
    Sd = Len ? LocalAlloc(LMEM_FIXED, Len) : NULL;
    if (!Sd || !NT_SUCCESS(NtQuerySecurityObject(H, DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION, Sd, Len, &Len)))
    {
        if (Sd)
            LocalFree(Sd);
        NtClose(H);
        return NULL;
    }
    NtClose(H);
    RtlGetDaclSecurityDescriptor(Sd, &Present, &Dacl, &Defaulted);
    RtlGetControlSecurityDescriptor(Sd, &Control, &i);
    if (Text)
        Used += _snprintf(Text + Used, TextSize - Used, "ctl 0x%04x:", Control);
    for (i = 0; Text && Dacl && i < Dacl->AceCount && Used < TextSize - 1; i++)
    {
        PACCESS_ALLOWED_ACE Ace;
        PSTR SidText = NULL;
        if (!GetAce(Dacl, i, (PVOID *)&Ace))
            break;
        ConvertSidToStringSidA((PSID)&Ace->SidStart, &SidText);
        Used += _snprintf(Text + Used, TextSize - Used, " %c;0x%lx;0x%x;%s", Ace->Header.AceType == ACCESS_DENIED_ACE_TYPE ? 'D' : 'A',
                          Ace->Mask, Ace->Header.AceFlags, SidText ? SidText : "?");
        if (SidText)
            LocalFree(SidText);
    }
    if (Text)
        Text[TextSize - 1] = 0;
    *SdOut = Sd;
    return Present ? Dacl : NULL;
}

LONG NtFindAce(PACL Dacl, PSID Sid, ACCESS_MASK Mask, BYTE FlagMask, BYTE Flags, BOOLEAN Deny)
{
    ULONG i;
    for (i = 0; Dacl && i < Dacl->AceCount; i++)
    {
        PACCESS_ALLOWED_ACE Ace;
        if (!GetAce(Dacl, i, (PVOID *)&Ace))
            break;
        if ((Ace->Header.AceType == ACCESS_DENIED_ACE_TYPE) == Deny && Ace->Mask == Mask &&
            (Ace->Header.AceFlags & FlagMask) == Flags && EqualSid((PSID)&Ace->SidStart, Sid))
            return (LONG)i;
    }
    return -1;
}

/* Deletes Path (a file or a directory tree) with backup semantics. */
static VOID NtRemoveTree(PCWSTR Path, ULONG Depth)
{
    FILE_DISPOSITION_INFORMATION Disp = { TRUE };
    FILE_BASIC_INFORMATION Basic;
    IO_STATUS_BLOCK Iosb;
    HANDLE H;
    NTSTATUS Status;

    Status = NtOpen(Path, DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                    FILE_OPEN_FOR_BACKUP_INTENT | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, NULL, &H);
    if (!NT_SUCCESS(Status))
        return;
    if (NT_SUCCESS(NtQueryInformationFile(H, &Iosb, &Basic, sizeof(Basic), FileBasicInformation)))
    {
        if (Basic.FileAttributes & FILE_ATTRIBUTE_READONLY)
        {
            Basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
            Basic.FileAttributes |= FILE_ATTRIBUTE_NORMAL;
            NtSetInformationFile(H, &Iosb, &Basic, sizeof(Basic), FileBasicInformation);
        }
        if ((Basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(Basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && Depth < 16)
        {
            UCHAR Buffer[4096];
            BOOLEAN First = TRUE;
            for (;;)
            {
                PFILE_DIRECTORY_INFORMATION E = (PFILE_DIRECTORY_INFORMATION)Buffer;
                Status = NtQueryDirectoryFile(H, NULL, NULL, NULL, &Iosb, Buffer, sizeof(Buffer), FileDirectoryInformation,
                                              FALSE, NULL, First);
                First = FALSE;
                if (!NT_SUCCESS(Status))
                    break;
                for (;;)
                {
                    ULONG n = E->FileNameLength / sizeof(WCHAR);
                    if (!(n == 1 && E->FileName[0] == L'.') && !(n == 2 && E->FileName[0] == L'.' && E->FileName[1] == L'.'))
                    {
                        WCHAR Child[MAX_PATH];
                        if (wcslen(Path) + n + 2 < MAX_PATH)
                        {
                            _snwprintf(Child, MAX_PATH - 1, L"%ls\\%.*ls", Path, (int)n, E->FileName);
                            NtRemoveTree(Child, Depth + 1);
                        }
                    }
                    if (!E->NextEntryOffset)
                        break;
                    E = (PFILE_DIRECTORY_INFORMATION)((PUCHAR)E + E->NextEntryOffset);
                }
            }
        }
    }
    NtSetInformationFile(H, &Iosb, &Disp, sizeof(Disp), FileDispositionInformation);
    NtClose(H);
}

VOID NtCleanup(VOID)
{
    if (TestDir[0])
        NtRemoveTree(TestDir, 0);
}
