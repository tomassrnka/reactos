/*
 * PROJECT:     ReactOS NT User Mode Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Asking whether an API set contract is known (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * After ApiSetQueryApiSetPresence of Wine's ntdll (dlls/ntdll/loader.c), with the answers of Windows 10 that
 * Wine's test for it records. The contracts are the table that the loader resolves with, for the versions of
 * Windows that the process is set up for.
 *
 * The first function answers for a contract family: it ignores the extension of a file and the minor version of
 * the name, and never fails. The extended one needs a name that starts like a contract and has no extension, and
 * knows the contract by its name or by a newer revision of its family; it fails with an invalid parameter and leaves the answers alone otherwise. A contract
 * that the schema of Windows lists without a module is not in the table here, so it is not reported as in the
 * schema.
 */

#include <ntdll.h>
#include <apisets.h>

#define NDEBUG
#include <debug.h>

#define MAX_CONTRACT_CHARS 160
#define MAX_MINOR_VERSION 15

DWORD LdrpApisetVersion(VOID);

static
BOOLEAN
IsKnownContract(
    _In_ PCUNICODE_STRING Name)
{
    UNICODE_STRING Host;
    BOOLEAN Resolved = FALSE;
    DWORD Versions = LdrpApisetVersion();

    if (!Versions)
        return FALSE;

    if (!NT_SUCCESS(ApiSetResolveToHost(Versions, Name, &Resolved, &Host)))
        return FALSE;

    return Resolved;
}

static
BOOLEAN
StartsLikeContract(
    _In_ PCUNICODE_STRING Name)
{
    ULONG Chars = Name->Length / sizeof(WCHAR);

    return Chars > 4 &&
           (!_wcsnicmp(Name->Buffer, L"api-", 4) || !_wcsnicmp(Name->Buffer, L"ext-", 4));
}

/*
 * The schema of a version of Windows lists the newest revision of a contract (the table here too), and the older
 * revisions of the family are known as well; a revision that is newer than the listed one is not.
 */
static
BOOLEAN
IsKnownContractOrNewer(
    _In_ PCUNICODE_STRING Name)
{
    WCHAR Buffer[MAX_CONTRACT_CHARS];
    UNICODE_STRING Newer;
    ULONG Chars = Name->Length / sizeof(WCHAR), Family, Minor = 0, Digit, Revision;

    if (IsKnownContract(Name))
        return TRUE;

    if (Chars >= RTL_NUMBER_OF(Buffer) - 4)
        return FALSE;

    for (Family = Chars; Family > 0 && Name->Buffer[Family - 1] != L'-'; Family--)
        ;
    if (Family == 0 || Family == Chars || Chars - Family > 2)
        return FALSE;
    for (Digit = Family; Digit < Chars; Digit++)
    {
        if (Name->Buffer[Digit] < L'0' || Name->Buffer[Digit] > L'9')
            return FALSE;
        Minor = Minor * 10 + (Name->Buffer[Digit] - L'0');
    }

    RtlCopyMemory(Buffer, Name->Buffer, Family * sizeof(WCHAR));
    for (Revision = Minor + 1; Revision <= MAX_MINOR_VERSION; Revision++)
    {
        ULONG Length = Family;

        if (Revision >= 10)
            Buffer[Length++] = L'0' + (WCHAR)(Revision / 10);
        Buffer[Length++] = L'0' + (WCHAR)(Revision % 10);

        Newer.Buffer = Buffer;
        Newer.Length = Newer.MaximumLength = (USHORT)(Length * sizeof(WCHAR));
        if (IsKnownContract(&Newer))
            return TRUE;
    }

    return FALSE;
}

NTSTATUS
NTAPI
ApiSetQueryApiSetPresenceEx(
    _In_ PCUNICODE_STRING Namespace,
    _Out_ PBOOLEAN InSchema,
    _Out_ PBOOLEAN Present)
{
    ULONG i, Chars = Namespace->Length / sizeof(WCHAR);
    BOOLEAN Known;

    if (!StartsLikeContract(Namespace))
        return STATUS_INVALID_PARAMETER;

    for (i = 0; i < Chars; i++)
    {
        if (Namespace->Buffer[i] == L'.')
            return STATUS_INVALID_PARAMETER;
    }

    Known = IsKnownContractOrNewer(Namespace);
    *InSchema = Known;
    *Present = Known;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
ApiSetQueryApiSetPresence(
    _In_ PCUNICODE_STRING Namespace,
    _Out_ PBOOLEAN Present)
{
    WCHAR Buffer[MAX_CONTRACT_CHARS];
    UNICODE_STRING Name;
    ULONG Chars = Namespace->Length / sizeof(WCHAR), Family, Minor;

    *Present = FALSE;

    if (!StartsLikeContract(Namespace) || Chars >= RTL_NUMBER_OF(Buffer) - 4)
        return STATUS_SUCCESS;

    /* Cut at the extension of a file, then at the minor version: the family is what is looked for */
    for (Family = Chars; Family > 0 && Namespace->Buffer[Family - 1] != L'.'; Family--)
        ;
    if (Family)
        Chars = Family - 1;
    for (Family = Chars; Family > 0 && Namespace->Buffer[Family - 1] != L'-'; Family--)
        ;
    if (Family == 0 || Family == Chars)
        return STATUS_SUCCESS;
    for (Minor = Family; Minor < Chars; Minor++)
    {
        if (Namespace->Buffer[Minor] < L'0' || Namespace->Buffer[Minor] > L'9')
            return STATUS_SUCCESS;
    }

    RtlCopyMemory(Buffer, Namespace->Buffer, Family * sizeof(WCHAR));
    for (Minor = 0; Minor <= MAX_MINOR_VERSION; Minor++)
    {
        ULONG Length = Family;

        if (Minor >= 10)
            Buffer[Length++] = L'0' + (WCHAR)(Minor / 10);
        Buffer[Length++] = L'0' + (WCHAR)(Minor % 10);

        Name.Buffer = Buffer;
        Name.Length = Name.MaximumLength = (USHORT)(Length * sizeof(WCHAR));
        if (IsKnownContract(&Name))
        {
            *Present = TRUE;
            break;
        }
    }

    return STATUS_SUCCESS;
}
