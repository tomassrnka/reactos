/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for LogonUser of a new non-administrator user in several processes
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <lm.h>

#define CHILD_COUNT 20
#define CALL_TIMEOUT_MS 120000

static WCHAR TestPassword[] = L"Logon-User-Test-2026!";

typedef struct _NET_CALL
{
    BOOL Add;
    PWSTR Name;
    NET_API_STATUS Status;
} NET_CALL, *PNET_CALL;

static DWORD WINAPI NetCallThread(PVOID Param)
{
    PNET_CALL Call = Param;
    USER_INFO_1 Info;
    DWORD Err;

    if (!Call->Add)
    {
        Call->Status = NetUserDel(NULL, Call->Name);
        return 0;
    }
    ZeroMemory(&Info, sizeof(Info));
    Info.usri1_name = Call->Name;
    Info.usri1_password = TestPassword;
    Info.usri1_priv = USER_PRIV_USER;
    Info.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    Call->Status = NetUserAdd(NULL, 1, (PBYTE)&Info, &Err);
    return 0;
}

/* A SAM call that does not return is a failure, not a hang of the test.
 * Call must outlive a thread that does not return, so callers keep it static. */
static BOOL TimedNetCall(PNET_CALL Call)
{
    HANDLE Thread;
    DWORD Wait;

    Thread = CreateThread(NULL, 0, NetCallThread, Call, 0, NULL);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (!Thread)
        return FALSE;
    Wait = WaitForSingleObject(Thread, CALL_TIMEOUT_MS);
    CloseHandle(Thread);
    ok(Wait == WAIT_OBJECT_0, "%s(%ls) did not return: %lu\n", Call->Add ? "NetUserAdd" : "NetUserDel", Call->Name, Wait);
    return Wait == WAIT_OBJECT_0;
}

static BOOL CreateTestUser(PWSTR Name, SIZE_T NameLength)
{
    static NET_CALL Call;
    DWORD Try;

    for (Try = 0; Try < 5; Try++)
    {
        StringCchPrintfW(Name, NameLength, L"lgtest%lx", (GetTickCount() + Try * 7919) & 0xffffff);
        Call.Add = TRUE;
        Call.Name = Name;
        if (!TimedNetCall(&Call))
            return FALSE;
        if (Call.Status == NERR_Success)
            return TRUE;
        if (Call.Status != NERR_UserExists)
            break;
    }
    if (Call.Status == ERROR_ACCESS_DENIED)
    {
        skip("NetUserAdd needs an elevated administrator\n");
        return FALSE;
    }
    ok(0, "NetUserAdd(%ls) failed: %lu\n", Name, Call.Status);
    return FALSE;
}

static BOOL IsMember(HANDLE Token, WELL_KNOWN_SID_TYPE Type)
{
    UCHAR Sid[SECURITY_MAX_SID_SIZE];
    DWORD Size = sizeof(Sid);
    BOOL Member = FALSE;

    if (!CreateWellKnownSid(Type, NULL, Sid, &Size))
    {
        ok(0, "CreateWellKnownSid(%d) failed: %lu\n", Type, GetLastError());
        return FALSE;
    }
    if (!CheckTokenMembership(Token, Sid, &Member))
    {
        ok(0, "CheckTokenMembership(%d) failed: %lu\n", Type, GetLastError());
        return FALSE;
    }
    return Member;
}

/* Logs the user on, runs as it and checks the token. */
static VOID LogonAndCheck(PWSTR User)
{
    WCHAR Name[64], Domain[64];
    UCHAR Buffer[256];
    HANDLE Token, Ident, Thread;
    DWORD Length, DomainLength;
    SID_NAME_USE Use;
    BOOL Ok;

    Ok = LogonUserW(User, L".", TestPassword, LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &Token);
    ok(Ok, "LogonUserW(%ls) failed: %lu\n", User, GetLastError());
    if (!Ok)
        return;

    /* The groups, checked on an identification copy of the token. */
    Ok = DuplicateToken(Token, SecurityIdentification, &Ident);
    ok(Ok, "DuplicateToken failed: %lu\n", GetLastError());
    if (Ok)
    {
        ok(!IsMember(Ident, WinBuiltinAdministratorsSid), "The new user is an administrator\n");
        ok(IsMember(Ident, WinBuiltinUsersSid), "The new user is not in the Users group\n");
        ok(IsMember(Ident, WinInteractiveSid), "The token has no INTERACTIVE group\n");
        CloseHandle(Ident);
    }

    Ok = ImpersonateLoggedOnUser(Token);
    ok(Ok, "ImpersonateLoggedOnUser failed: %lu\n", GetLastError());
    if (Ok)
    {
        Ok = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &Thread);
        ok(Ok, "OpenThreadToken failed: %lu\n", GetLastError());
        RevertToSelf();
        if (Ok)
        {
            Ok = GetTokenInformation(Thread, TokenUser, Buffer, sizeof(Buffer), &Length);
            ok(Ok, "GetTokenInformation failed: %lu\n", GetLastError());
            CloseHandle(Thread);
        }
        if (Ok)
        {
            Length = RTL_NUMBER_OF(Name);
            DomainLength = RTL_NUMBER_OF(Domain);
            Ok = LookupAccountSidW(NULL, ((PTOKEN_USER)Buffer)->User.Sid, Name, &Length, Domain, &DomainLength, &Use);
            ok(Ok, "LookupAccountSidW failed: %lu\n", GetLastError());
            if (Ok)
            {
                ok(!_wcsicmp(Name, User), "Impersonated %ls, expected %ls\n", Name, User);
                ok(Use == SidTypeUser, "SID type %d\n", Use);
            }
        }
    }
    CloseHandle(Token);
}

static VOID ChildLogon(PCSTR UserA)
{
    WCHAR User[32];

    MultiByteToWideChar(CP_ACP, 0, UserA, -1, User, RTL_NUMBER_OF(User));
    LogonAndCheck(User);
}

START_TEST(LogonUser)
{
    static WCHAR User[32];
    CHAR UserA[32], CommandLine[MAX_PATH + 64];
    STARTUPINFOA Si;
    PROCESS_INFORMATION Pi;
    static NET_CALL Call;
    DWORD Wait, Code, i;
    int argc;
    char **argv;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3)
    {
        ChildLogon(argv[2]);
        return;
    }

    if (!CreateTestUser(User, RTL_NUMBER_OF(User)))
        return;
    WideCharToMultiByte(CP_ACP, 0, User, -1, UserA, sizeof(UserA), NULL, NULL);

    /* One process after another, each logging the new user on and exiting.
     * Each logon follows the exit of the previous process. The test process
     * itself does not log on, so its own exit cannot wait for the LSA. */
    for (i = 0; i < CHILD_COUNT; i++)
    {
        StringCchPrintfA(CommandLine, RTL_NUMBER_OF(CommandLine), "\"%s\" LogonUser %s", argv[0], UserA);
        ZeroMemory(&Si, sizeof(Si));
        Si.cb = sizeof(Si);
        if (!CreateProcessA(NULL, CommandLine, NULL, NULL, TRUE, 0, NULL, NULL, &Si, &Pi))
        {
            ok(0, "CreateProcessA failed: %lu\n", GetLastError());
            break;
        }
        CloseHandle(Pi.hThread);
        Wait = WaitForSingleObject(Pi.hProcess, CALL_TIMEOUT_MS);
        ok(Wait == WAIT_OBJECT_0, "Process %lu did not finish: %lu\n", i + 1, Wait);
        if (Wait != WAIT_OBJECT_0)
        {
            TerminateProcess(Pi.hProcess, 1);
            CloseHandle(Pi.hProcess);
            break;
        }
        if (!GetExitCodeProcess(Pi.hProcess, &Code))
            Code = MAXDWORD;
        ok(Code == 0, "Process %lu: exit code %lu\n", i + 1, Code);
        CloseHandle(Pi.hProcess);
    }

    Call.Add = FALSE;
    Call.Name = User;
    if (TimedNetCall(&Call))
        ok(Call.Status == NERR_Success, "NetUserDel(%ls) failed: %lu\n", User, Call.Status);
}
