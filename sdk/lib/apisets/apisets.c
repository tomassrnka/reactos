/*
 * PROJECT:     ReactOS apisets
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Resolving the apiset to a ReactOS system dll
 * COPYRIGHT:   Copyright 2024 Mark Jansen <mark.jansen@reactos.org>
 */

#include <ndk/umtypes.h>
#include <ndk/rtlfuncs.h>
#include "apisetsp.h"


const ULONGLONG API_ = (ULONGLONG)0x2D004900500041; /// L"API-"
const ULONGLONG EXT_ = (ULONGLONG)0x2D005400580045; /// L"EXT-";

WORD PrefixSize = sizeof(L"api-") - sizeof(WCHAR);
WORD ExtensionSize = sizeof(L".dll") - sizeof(WCHAR);


// The official prototype according to the Windows kit 8.1 is:
//NTSTATUS
//ApiSetResolveToHost (
//    _In_ PCAPI_SET_NAMESPACE_ARRAY Schema,
//    _In_ PCUNICODE_STRING FileNameIn,
//    _In_opt_ PCUNICODE_STRING ParentName,
//    _Out_ PBOOLEAN Resolved,
//    _Out_ PUNICODE_STRING HostBinary
//    );


NTSTATUS
ApiSetResolveToHost(
    _In_ DWORD ApisetVersion,
    _In_ PCUNICODE_STRING ApiToResolve,
    _Out_ PBOOLEAN Resolved,
    _Out_ PUNICODE_STRING Output
)
{
    return ApiSetResolveToHostEx(ApisetVersion, FALSE, ApiToResolve, Resolved, Output);
}

NTSTATUS
ApiSetResolveToHostEx(
    _In_ DWORD ApisetVersion,
    _In_ BOOLEAN ExactName,
    _In_ PCUNICODE_STRING ApiToResolve,
    _Out_ PBOOLEAN Resolved,
    _Out_ PUNICODE_STRING Output
)
{
    if (ApiToResolve->Length < PrefixSize)
    {
        *Resolved = FALSE;
        return STATUS_SUCCESS;
    }

    // Grab the first four chars from the string, converting the first 3 to uppercase
    PWSTR ApiSetNameBuffer = ApiToResolve->Buffer;
    ULONGLONG ApiSetNameBufferPrefix = ((ULONGLONG *)ApiSetNameBuffer)[0] & 0xFFFFFFDFFFDFFFDF;
    // Check if it matches either 'api-' or 'ext-'
    if (!(ApiSetNameBufferPrefix == API_ || ApiSetNameBufferPrefix == EXT_))
    {
        *Resolved = FALSE;
        return STATUS_SUCCESS;
    }

    // If there is an extension, cut it off (we store apisets without extension)
    UNICODE_STRING Tmp = *ApiToResolve;
    const WCHAR *Extension = Tmp.Buffer + (Tmp.Length - ExtensionSize) / sizeof(WCHAR);
    if (!_wcsnicmp(Extension, L".dll", ExtensionSize / sizeof(WCHAR)))
        Tmp.Length -= ExtensionSize;

    // A contract name is a file name, never a path
    for (USHORT n = 0; n < Tmp.Length / sizeof(WCHAR); ++n)
    {
        if (Tmp.Buffer[n] == L'\\' || Tmp.Buffer[n] == L'/')
        {
            *Resolved = FALSE;
            return STATUS_SUCCESS;
        }
    }

    // Windows 10 ignores the minor version, the part after the last hyphen: every minor of a contract resolves to
    // the host of the contract. Look for the exact name first, then for the newest row of the same contract.
    UNICODE_STRING Contract = Tmp;
    while (Contract.Length > PrefixSize && Contract.Buffer[Contract.Length / sizeof(WCHAR) - 1] != L'-')
        Contract.Length -= sizeof(WCHAR);
    if (Contract.Length <= PrefixSize)
    {
        *Resolved = FALSE;
        return STATUS_SUCCESS;
    }

    // Binary search for the first row that is not below the contract name; the rows of the contract follow it.
    // Ideally we should use bsearch here, but that drags in another dependency and we do not want that here.
    LONG UBnd = g_ApisetsCount;
    LONG LBnd = 0;
    while (LBnd < UBnd)
    {
        LONG Index = (UBnd - LBnd) / 2 + LBnd;

        if (RtlCompareUnicodeString(&g_Apisets[Index].Name, &Contract, TRUE) < 0)
            LBnd = Index + 1;
        else
            UBnd = Index;
    }

    const ROS_APISET *Best = NULL;
    ULONG BestMinor = 0;
    for (LONG Index = LBnd; Index < g_ApisetsCount; ++Index)
    {
        const ROS_APISET *Row = &g_Apisets[Index];
        if (!RtlPrefixUnicodeString(&Contract, &Row->Name, TRUE))
            break;

        // Check if this version is included
        if (!(Row->dwOsVersions & ApisetVersion))
            continue;

        if (RtlEqualUnicodeString(&Tmp, &Row->Name, TRUE))
        {
            Best = Row;
            break;
        }
        if (ExactName)
            continue;

        // Only rows whose remainder is a minor version belong to the contract
        USHORT Pos = Contract.Length / sizeof(WCHAR);
        USHORT End = Row->Name.Length / sizeof(WCHAR);
        ULONG Minor = 0;
        if (Pos == End || End - Pos > 5)
            continue;
        for (; Pos < End; ++Pos)
        {
            WCHAR Ch = Row->Name.Buffer[Pos];
            if (Ch < L'0' || Ch > L'9')
                break;
            Minor = Minor * 10 + (Ch - L'0');
        }
        if (Pos == End && (!Best || Minor > BestMinor))
        {
            Best = Row;
            BestMinor = Minor;
        }
    }

    if (Best)
    {
        // Return a static string (does not have to be freed)
        *Resolved = TRUE;
        *Output = Best->Target;
        return STATUS_SUCCESS;
    }
    *Resolved = FALSE;
    return STATUS_SUCCESS;
}
