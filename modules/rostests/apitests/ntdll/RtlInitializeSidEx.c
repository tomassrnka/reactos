/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for ntdll!RtlInitializeSidEx (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The results were measured on Windows 10 22H2.
 */

#include "precomp.h"

typedef NTSTATUS (__cdecl *PFN_RtlInitializeSidEx)(PSID, PSID_IDENTIFIER_AUTHORITY, UCHAR, ...);

START_TEST(RtlInitializeSidEx)
{
    PFN_RtlInitializeSidEx pRtlInitializeSidEx;
    SID_IDENTIFIER_AUTHORITY Authority = { { 0, 0, 0, 0, 0, 5 } };
    /* room for the largest SID and 16 bytes behind it that must stay as they are */
    BYTE Buffer[SECURITY_MAX_SID_SIZE + 16];
    PISID Sid = (PISID)Buffer;
    NTSTATUS Status;
    ULONG i;

    pRtlInitializeSidEx = (PFN_RtlInitializeSidEx)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlInitializeSidEx");
    /* Windows 7 and older do not have it; ReactOS has it from export level 0x601 */
    if (!pRtlInitializeSidEx)
    {
        ok(!(is_reactos() && GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")),
           "RtlInitializeSidEx is not exported\n");
        skip("RtlInitializeSidEx is not exported\n");
        return;
    }

    /* the sub-authorities follow the count as 32-bit values */
    memset(Buffer, 0xcc, sizeof(Buffer));
    Status = pRtlInitializeSidEx(Sid, &Authority, 2, 32, 544);
    ok_hex(Status, STATUS_SUCCESS);
    ok_dec(Sid->Revision, 1);
    ok_dec(Sid->SubAuthorityCount, 2);
    ok(!memcmp(&Sid->IdentifierAuthority, &Authority, sizeof(Authority)), "bad authority\n");
    ok_dec(Sid->SubAuthority[0], 32);
    ok_dec(Sid->SubAuthority[1], 544);
    ok_dec(RtlLengthSid(Sid), 16);

    memset(Buffer, 0xcc, sizeof(Buffer));
    Status = pRtlInitializeSidEx(Sid, &Authority, 0);
    ok_hex(Status, STATUS_SUCCESS);
    ok_dec(Sid->Revision, 1);
    ok_dec(Sid->SubAuthorityCount, 0);

    /* fifteen is the most */
    memset(Buffer, 0xcc, sizeof(Buffer));
    Status = pRtlInitializeSidEx(Sid, &Authority, 15, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    ok_hex(Status, STATUS_SUCCESS);
    ok_dec(Sid->SubAuthorityCount, 15);
    for (i = 0; i < 15; i++)
        ok(Sid->SubAuthority[i] == i + 1, "sub-authority %lu is %lu\n", i, Sid->SubAuthority[i]);

    /* nothing is written behind the largest SID: 8 bytes of header and 15 sub-authorities */
    memset(Buffer, 0xcc, sizeof(Buffer));
    Status = pRtlInitializeSidEx(Sid, &Authority, 15, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    ok_hex(Status, STATUS_SUCCESS);
    for (i = 8 + 4 * 15; i < sizeof(Buffer); i++)
        ok(Buffer[i] == 0xcc, "byte %u behind the SID was written\n", i);

    /* a bigger count is refused and leaves the whole buffer alone */
    memset(Buffer, 0xcc, sizeof(Buffer));
    Status = pRtlInitializeSidEx(Sid, &Authority, 16, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    ok_hex(Status, STATUS_INVALID_PARAMETER);
    for (i = 0; i < sizeof(Buffer); i++)
        ok(Buffer[i] == 0xcc, "byte %u was written\n", i);
    memset(Buffer, 0xcc, sizeof(Buffer));
    Status = pRtlInitializeSidEx(Sid, &Authority, 255, 1);
    ok_hex(Status, STATUS_INVALID_PARAMETER);
    for (i = 0; i < sizeof(Buffer); i++)
        ok(Buffer[i] == 0xcc, "byte %u was written\n", i);
}
