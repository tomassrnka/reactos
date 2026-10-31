/*
 * PROJECT:     ReactOS system libraries
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     GetAppContainerNamedObjectPath (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#include <ndk/sefuncs.h>

#define NDEBUG
#include <debug.h>

/* An AppContainer SID is S-1-15-2 and seven more sub-authorities (eight in all); a child has four more */
#define AC_PACKAGE_SUBAUTHORITIES   8
#define AC_CHILD_SUBAUTHORITIES     4

static const WCHAR AppContainerObjectDirectory[] = L"AppContainerNamedObjects\\";

/* FUNCTIONS ******************************************************************/

static
BOOL
IsAppContainerSid(
    _In_ PSID Sid)
{
    static const SID_IDENTIFIER_AUTHORITY AppPackageAuthority = { SECURITY_APP_PACKAGE_AUTHORITY };
    PISID AcSid = Sid;
    UCHAR Count;

    if (!RtlValidSid(Sid))
        return FALSE;

    Count = AcSid->SubAuthorityCount;
    return RtlEqualMemory(&AcSid->IdentifierAuthority, &AppPackageAuthority, sizeof(AppPackageAuthority)) &&
           (AcSid->SubAuthority[0] == SECURITY_APP_PACKAGE_BASE_RID) &&
           ((Count == AC_PACKAGE_SUBAUTHORITIES) || (Count == AC_PACKAGE_SUBAUTHORITIES + AC_CHILD_SUBAUTHORITIES));
}

/*
 * @implemented
 *
 * The path of the object directory an AppContainer creates its named objects in, relative to the session's
 * object namespace: "AppContainerNamedObjects\<package SID>", and for a child SID the four extra
 * sub-authorities follow after a backslash ("\1-2-3-4"). It is measured on Windows 10 22H2 and does not depend on
 * the session. The SID is the one given or, when only a token (or none: the calling thread's) is given, its
 * AppContainer SID; the two together are an invalid parameter, as is a length without a buffer. ObjectPathLength and *ReturnLength count characters,
 * the terminating null included in the latter.
 */
BOOL
WINAPI
GetAppContainerNamedObjectPath(
    _In_opt_ HANDLE Token,
    _In_opt_ PSID AppContainerSid,
    _In_ ULONG ObjectPathLength,
    _Out_writes_opt_(ObjectPathLength) LPWSTR ObjectPath,
    _Out_ PULONG ReturnLength)
{
    union
    {
        PSID TokenAppContainer; /* TOKEN_APPCONTAINER_INFORMATION */
        UCHAR Buffer[sizeof(PSID) + SECURITY_MAX_SID_SIZE];
    } AcInfo;
    UCHAR Parent[FIELD_OFFSET(SID, SubAuthority) + AC_PACKAGE_SUBAUTHORITIES * sizeof(ULONG)];
    WCHAR SidBuffer[128];
    WCHAR Children[64];
    UNICODE_STRING SidString, Number;
    HANDLE OwnToken = NULL;
    TOKEN_TYPE TokenKind;
    PISID AcSid;
    SIZE_T Length, ChildLength = 0, i;
    ULONG Size;
    NTSTATUS Status;

    if (!ReturnLength || (Token && AppContainerSid) || (!ObjectPath && ObjectPathLength))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (AppContainerSid)
    {
        AcSid = AppContainerSid;
    }
    else
    {
        /* The AppContainer SID of the token, of the thread's token when impersonating, else of the process */
        if (!Token)
        {
            Status = NtOpenThreadToken(NtCurrentThread(), TOKEN_QUERY, TRUE, &OwnToken);
            if (!NT_SUCCESS(Status))
                Status = NtOpenProcessToken(NtCurrentProcess(), TOKEN_QUERY, &OwnToken);
            if (!NT_SUCCESS(Status))
            {
                BaseSetLastNTError(Status);
                return FALSE;
            }
            Token = OwnToken;
        }

        /* A handle that is no token, or without TOKEN_QUERY, fails before anything else */
        Status = NtQueryInformationToken(Token, TokenType, &TokenKind, sizeof(TokenKind), &Size);
        if (NT_SUCCESS(Status))
        {
            Status = NtQueryInformationToken(Token,
                                             TokenAppContainerSid,
                                             &AcInfo,
                                             sizeof(AcInfo),
                                             &Size);
        }
        if (OwnToken)
            NtClose(OwnToken);

        /* No kernel here knows the class (the WoW64 thunk does not either): no token is an AppContainer's */
        if (!NT_SUCCESS(Status) && (Status != STATUS_INVALID_INFO_CLASS) && (Status != STATUS_NOT_SUPPORTED))
        {
            BaseSetLastNTError(Status);
            return FALSE;
        }

        if (!NT_SUCCESS(Status) || !AcInfo.TokenAppContainer)
        {
            SetLastError(ERROR_NOT_APPCONTAINER);
            return FALSE;
        }

        AcSid = AcInfo.TokenAppContainer;
    }

    if (!IsAppContainerSid(AcSid))
    {
        SetLastError(ERROR_NOT_APPCONTAINER);
        return FALSE;
    }

    /* The first eight sub-authorities are the package SID */
    RtlCopyMemory(Parent, AcSid, sizeof(Parent));
    ((PISID)Parent)->SubAuthorityCount = AC_PACKAGE_SUBAUTHORITIES;

    SidString.Buffer = SidBuffer;
    SidString.Length = 0;
    SidString.MaximumLength = sizeof(SidBuffer);
    Status = RtlConvertSidToUnicodeString(&SidString, Parent, FALSE);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* "\" and the child sub-authorities, joined with "-" */
    if (AcSid->SubAuthorityCount > AC_PACKAGE_SUBAUTHORITIES)
    {
        Children[ChildLength++] = L'\\';
        for (i = AC_PACKAGE_SUBAUTHORITIES; i < AcSid->SubAuthorityCount; i++)
        {
            if (i > AC_PACKAGE_SUBAUTHORITIES)
                Children[ChildLength++] = L'-';
            Number.Buffer = Children + ChildLength;
            Number.Length = 0;
            Number.MaximumLength = (USHORT)((RTL_NUMBER_OF(Children) - ChildLength) * sizeof(WCHAR));
            Status = RtlIntegerToUnicodeString(AcSid->SubAuthority[i], 10, &Number);
            if (!NT_SUCCESS(Status))
            {
                BaseSetLastNTError(Status);
                return FALSE;
            }
            ChildLength += Number.Length / sizeof(WCHAR);
        }
    }

    Length = RTL_NUMBER_OF(AppContainerObjectDirectory) - 1 + SidString.Length / sizeof(WCHAR) + ChildLength;
    *ReturnLength = (ULONG)(Length + 1);
    if (!ObjectPath || (ObjectPathLength < Length + 1))
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    RtlCopyMemory(ObjectPath, AppContainerObjectDirectory, sizeof(AppContainerObjectDirectory) - sizeof(WCHAR));
    ObjectPath += RTL_NUMBER_OF(AppContainerObjectDirectory) - 1;
    RtlCopyMemory(ObjectPath, SidString.Buffer, SidString.Length);
    ObjectPath += SidString.Length / sizeof(WCHAR);
    RtlCopyMemory(ObjectPath, Children, ChildLength * sizeof(WCHAR));
    ObjectPath[ChildLength] = UNICODE_NULL;
    return TRUE;
}
