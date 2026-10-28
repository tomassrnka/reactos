/*
 * PROJECT:     ReactOS netapi32.dll API Tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for NetLocalGroupGetMembers and for NetUserDel of a user
 *              that has logged on or is a member of a local group
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <lm.h>

#define ADMIN_RID 500
#define LOOP_COUNT 60

static WCHAR TestUser[] = L"apitest_lgm";
static WCHAR TestPassword[] = L"Apitest-Lgm-2026!";

static BOOL
SidHasRid(PSID Sid, DWORD Rid)
{
    return IsValidSid(Sid) &&
           *GetSidSubAuthorityCount(Sid) > 0 &&
           *GetSidSubAuthority(Sid, *GetSidSubAuthorityCount(Sid) - 1) == Rid;
}

static void
CheckLevel0(LPBYTE buf, DWORD read)
{
    PLOCALGROUP_MEMBERS_INFO_0 info = (PLOCALGROUP_MEMBERS_INFO_0)buf;
    BOOL found = FALSE;
    DWORD i;

    for (i = 0; i < read; i++)
    {
        ok(IsValidSid(info[i].lgrmi0_sid), "level 0 entry %lu has an invalid SID\n", i);
        if (SidHasRid(info[i].lgrmi0_sid, ADMIN_RID))
            found = TRUE;
    }
    ok(found, "the Administrator (RID %d) is not a member of Administrators\n", ADMIN_RID);
}

static void
CheckLevel123(DWORD level, LPBYTE buf, DWORD read)
{
    DWORD i;

    for (i = 0; i < read; i++)
    {
        PCWSTR name;

        if (level == 1)
        {
            PLOCALGROUP_MEMBERS_INFO_1 info = (PLOCALGROUP_MEMBERS_INFO_1)buf + i;
            ok(IsValidSid(info->lgrmi1_sid), "level 1 entry %lu has an invalid SID\n", i);
            ok(info->lgrmi1_sidusage == SidTypeUser || info->lgrmi1_sidusage == SidTypeGroup ||
               info->lgrmi1_sidusage == SidTypeAlias || info->lgrmi1_sidusage == SidTypeWellKnownGroup,
               "level 1 entry %lu has SID usage %d\n", i, info->lgrmi1_sidusage);
            name = info->lgrmi1_name;
            ok(!wcschr(name, L'\\'), "level 1 name %ls has a domain\n", name);
        }
        else if (level == 2)
        {
            PLOCALGROUP_MEMBERS_INFO_2 info = (PLOCALGROUP_MEMBERS_INFO_2)buf + i;
            ok(IsValidSid(info->lgrmi2_sid), "level 2 entry %lu has an invalid SID\n", i);
            name = info->lgrmi2_domainandname;
            ok(wcschr(name, L'\\') != NULL, "level 2 name %ls has no domain\n", name);
        }
        else
        {
            PLOCALGROUP_MEMBERS_INFO_3 info = (PLOCALGROUP_MEMBERS_INFO_3)buf + i;
            name = info->lgrmi3_domainandname;
            ok(wcschr(name, L'\\') != NULL, "level 3 name %ls has no domain\n", name);
        }
        ok(name[0] != UNICODE_NULL, "level %lu entry %lu has an empty name\n", level, i);
    }
}

static BOOL
IsMember(PCWSTR group, PCWSTR user)
{
    LPBYTE buf = NULL;
    DWORD read = 0, total = 0, i;
    BOOL found = FALSE;
    NET_API_STATUS st;

    st = NetLocalGroupGetMembers(NULL, group, 1, &buf, MAX_PREFERRED_LENGTH, &read, &total, NULL);
    ok(st == NERR_Success, "NetLocalGroupGetMembers(%ls) returned %lu\n", group, st);
    if (st != NERR_Success)
        return FALSE;
    for (i = 0; i < read; i++)
    {
        if (!_wcsicmp(((PLOCALGROUP_MEMBERS_INFO_1)buf)[i].lgrmi1_name, user))
            found = TRUE;
    }
    NetApiBufferFree(buf);
    return found;
}

static void
Test_GetMembers(void)
{
    LPBYTE buf;
    DWORD read, total, level, i;
    NET_API_STATUS st;

    /* The enumeration context is reused by every call: a loop must neither
       crash nor corrupt the heap, and gives the same answer each time */
    for (i = 0; i < LOOP_COUNT; i++)
    {
        for (level = 0; level <= 3; level++)
        {
            buf = NULL;
            read = total = 0xdead;
            st = NetLocalGroupGetMembers(NULL, L"Administrators", level, &buf,
                                         MAX_PREFERRED_LENGTH, &read, &total, NULL);
            ok(st == NERR_Success, "[%lu] level %lu returned %lu\n", i, level, st);
            if (st != NERR_Success)
                return;
            ok(buf != NULL, "[%lu] level %lu returned no buffer\n", i, level);
            ok(read == total && read >= 1, "[%lu] level %lu: read %lu, total %lu\n", i, level, read, total);
            if (buf != NULL && i == 0)
            {
                if (level == 0)
                    CheckLevel0(buf, read);
                else
                    CheckLevel123(level, buf, read);
            }
            if (buf != NULL)
                NetApiBufferFree(buf);
        }
    }

    /* A group without members succeeds with nothing */
    buf = (LPBYTE)(ULONG_PTR)1;
    read = total = 0xdead;
    st = NetLocalGroupGetMembers(NULL, L"Replicator", 1, &buf, MAX_PREFERRED_LENGTH, &read, &total, NULL);
    ok(st == NERR_Success, "empty group returned %lu\n", st);
    ok(read == 0 && total == 0, "empty group: read %lu, total %lu\n", read, total);
    ok(buf == NULL, "empty group returned a buffer\n");

    /* Errors */
    buf = NULL;
    st = NetLocalGroupGetMembers(NULL, L"apitest_no_such_group", 0, &buf, MAX_PREFERRED_LENGTH, &read, &total, NULL);
    ok(st == NERR_GroupNotFound, "unknown group returned %lu\n", st);
    ok(buf == NULL, "unknown group returned a buffer\n");

    st = NetLocalGroupGetMembers(NULL, L"Administrators", 7, &buf, MAX_PREFERRED_LENGTH, &read, &total, NULL);
    ok(st == ERROR_INVALID_LEVEL, "level 7 returned %lu\n", st);
    ok(buf == NULL, "level 7 returned a buffer\n");
}

static NET_API_STATUS
AddTestUser(void)
{
    USER_INFO_1 info;
    DWORD err;

    memset(&info, 0, sizeof(info));
    info.usri1_name = TestUser;
    info.usri1_password = TestPassword;
    info.usri1_priv = USER_PRIV_USER;
    info.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD;
    return NetUserAdd(NULL, 1, (LPBYTE)&info, &err);
}

static void
Test_UserDel(void)
{
    LOCALGROUP_MEMBERS_INFO_3 member;
    LPBYTE buf = NULL;
    HANDLE token;
    NET_API_STATUS st;
    BOOL ret;

    NetUserDel(NULL, TestUser);

    st = AddTestUser();
    if (st == ERROR_ACCESS_DENIED)
    {
        skip("NetUserAdd needs an elevated administrator\n");
        return;
    }
    ok(st == NERR_Success, "NetUserAdd returned %lu\n", st);
    if (st != NERR_Success)
        return;

    /* A plain user */
    st = NetUserDel(NULL, TestUser);
    ok(st == NERR_Success, "NetUserDel of a new user returned %lu\n", st);
    st = NetUserDel(NULL, TestUser);
    ok(st == NERR_UserNotFound, "second NetUserDel returned %lu\n", st);

    /* A user that has logged on */
    st = AddTestUser();
    ok(st == NERR_Success, "NetUserAdd returned %lu\n", st);
    if (st != NERR_Success)
        return;
    token = NULL;
    ret = LogonUserW(TestUser, L".", TestPassword, LOGON32_LOGON_INTERACTIVE,
                     LOGON32_PROVIDER_DEFAULT, &token);
    if (!ret)
        skip("LogonUser failed: %lu\n", GetLastError());
    else
        CloseHandle(token);
    st = NetUserDel(NULL, TestUser);
    ok(st == NERR_Success, "NetUserDel after logon returned %lu\n", st);
    buf = NULL;
    st = NetUserGetInfo(NULL, TestUser, 0, &buf);
    ok(st == NERR_UserNotFound, "the deleted user is still there: %lu\n", st);
    if (buf)
        NetApiBufferFree(buf);

    /* A user that is a member of a local group leaves it with the account */
    st = AddTestUser();
    ok(st == NERR_Success, "NetUserAdd returned %lu\n", st);
    if (st != NERR_Success)
        return;
    member.lgrmi3_domainandname = TestUser;
    st = NetLocalGroupAddMembers(NULL, L"Users", 3, (LPBYTE)&member, 1);
    ok(st == NERR_Success, "NetLocalGroupAddMembers returned %lu\n", st);
    ok(IsMember(L"Users", TestUser), "the user is not a member of Users\n");
    st = NetUserDel(NULL, TestUser);
    ok(st == NERR_Success, "NetUserDel of a group member returned %lu\n", st);
    ok(!IsMember(L"Users", TestUser), "the deleted user is still a member of Users\n");
    if (st != NERR_Success)
    {
        NetLocalGroupDelMembers(NULL, L"Users", 3, (LPBYTE)&member, 1);
        NetUserDel(NULL, TestUser);
    }
}

START_TEST(NetLocalGroup)
{
    Test_GetMembers();
    Test_UserDel();
}
