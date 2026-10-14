/*
 * PROJECT:     ReactOS netapi32.dll API Tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Tests for the share calls that net.exe is built on
 */

#include <apitest.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <lm.h>

/* RPC statuses that Windows hands through unchanged */
#define STATUS_INVALID_BINDING  1702
#define STATUS_INVALID_NET_ADDR 1707

static void
CheckRemote(PCWSTR server, NET_API_STATUS expected, BOOL reactos_bad_netpath)
{
    LPBYTE buf = NULL;
    DWORD read = 0, total = 0, resume = 0;
    NET_API_STATUS st;

    /* ReactOS' RPC runtime reports every name it cannot bind as an unreachable server */
    if (reactos_bad_netpath && is_reactos())
        expected = ERROR_BAD_NETPATH;

    st = NetShareEnum((LPWSTR)server, 1, &buf, MAX_PREFERRED_LENGTH, &read, &total, &resume);
    ok(st == expected, "NetShareEnum(%ls) = %lu, expected %lu\n", server, st, expected);
    ok(buf == NULL && read == 0 && total == 0, "NetShareEnum(%ls) returned data\n", server);
    if (buf)
        NetApiBufferFree(buf);

    buf = NULL;
    st = NetShareGetInfo((LPWSTR)server, L"nosuchshare", 2, &buf);
    ok(st == expected, "NetShareGetInfo(%ls) = %lu, expected %lu\n", server, st, expected);
    if (buf)
        NetApiBufferFree(buf);

    st = NetShareDel((LPWSTR)server, L"nosuchshare", 0);
    ok(st == expected, "NetShareDel(%ls) = %lu, expected %lu\n", server, st, expected);
}

static void
Test_Remote(void)
{
    /* The server cannot be reached: Windows reports a bad network path */
    CheckRemote(L"\\\\nosuchhost.invalid", ERROR_BAD_NETPATH, FALSE);
    CheckRemote(L"nosuchhost.invalid", ERROR_BAD_NETPATH, FALSE);
    CheckRemote(L"\\\\localhost\\C$", ERROR_BAD_NETPATH, FALSE);

    /* Names that cannot be bound give the RPC status */
    CheckRemote(L"\\\\", STATUS_INVALID_BINDING, TRUE);
    CheckRemote(L"\\\\a\\b", STATUS_INVALID_NET_ADDR, TRUE);
}

static void
Test_Local(void)
{
    SHARE_INFO_2 add, *info = NULL;
    LPBYTE buf = NULL;
    DWORD read, total, resume = 0, parm = 0;
    NET_API_STATUS st;

    st = NetShareEnum(NULL, 2, &buf, MAX_PREFERRED_LENGTH, &read, &total, &resume);
    if (st == ERROR_CALL_NOT_IMPLEMENTED && is_reactos())
    {
        skip("The Server service does not implement the share calls\n");
        return;
    }
    ok(st == NERR_Success, "NetShareEnum returned %lu\n", st);
    if (buf)
        NetApiBufferFree(buf);

    memset(&add, 0, sizeof(add));
    add.shi2_netname = L"apitest_share";
    add.shi2_type = STYPE_DISKTREE;
    add.shi2_remark = L"first remark";
    add.shi2_max_uses = (DWORD)-1;
    add.shi2_path = L"C:\\";
    st = NetShareAdd(NULL, 2, (LPBYTE)&add, &parm);
    ok(st == NERR_Success, "NetShareAdd returned %lu\n", st);
    if (st != NERR_Success)
        return;

    st = NetShareAdd(NULL, 2, (LPBYTE)&add, &parm);
    ok(st == NERR_DuplicateShare, "second NetShareAdd returned %lu\n", st);

    /* A change goes through level 2 and keeps the other fields */
    st = NetShareGetInfo(NULL, L"apitest_share", 2, (LPBYTE *)&info);
    ok(st == NERR_Success, "NetShareGetInfo returned %lu\n", st);
    if (st == NERR_Success)
    {
        ok(!wcscmp(info->shi2_remark, L"first remark"), "remark is %ls\n", info->shi2_remark);
        info->shi2_max_uses = 5;
        st = NetShareSetInfo(NULL, L"apitest_share", 2, (LPBYTE)info, &parm);
        ok(st == NERR_Success, "NetShareSetInfo returned %lu\n", st);
        NetApiBufferFree(info);
    }

    st = NetShareGetInfo(NULL, L"apitest_share", 2, (LPBYTE *)&info);
    ok(st == NERR_Success, "NetShareGetInfo returned %lu\n", st);
    if (st == NERR_Success)
    {
        ok(info->shi2_max_uses == 5, "max uses is %lu\n", info->shi2_max_uses);
        ok(!wcscmp(info->shi2_remark, L"first remark"), "remark is %ls\n", info->shi2_remark);
        NetApiBufferFree(info);
    }

    st = NetShareDel(NULL, L"apitest_share", 0);
    ok(st == NERR_Success, "NetShareDel returned %lu\n", st);
    st = NetShareDel(NULL, L"apitest_share", 0);
    ok(st == NERR_NetNameNotFound, "second NetShareDel returned %lu\n", st);
}

START_TEST(NetShare)
{
    Test_Remote();
    Test_Local();
}
