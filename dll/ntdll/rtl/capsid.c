/*
 * PROJECT:     ReactOS NT Layer DLL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Capability security identifiers derived from a name (Windows 8 and later)
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/* INCLUDES *******************************************************************/

#include <ntdll.h>

#define NDEBUG
#include <debug.h>

/* PRIVATE FUNCTIONS **********************************************************/

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const ULONG Sha256K[64] =
{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static
VOID
CapSha256Block(
    _Inout_ ULONG State[8],
    _In_reads_bytes_(64) const UCHAR *Block)
{
    ULONG W[64], A, B, C, D, E, F, G, H, T1, T2;
    ULONG i;

    for (i = 0; i < 16; i++)
    {
        W[i] = ((ULONG)Block[4 * i] << 24) | ((ULONG)Block[4 * i + 1] << 16) |
               ((ULONG)Block[4 * i + 2] << 8) | (ULONG)Block[4 * i + 3];
    }
    for (; i < 64; i++)
    {
        ULONG S0 = ROR32(W[i - 15], 7) ^ ROR32(W[i - 15], 18) ^ (W[i - 15] >> 3);
        ULONG S1 = ROR32(W[i - 2], 17) ^ ROR32(W[i - 2], 19) ^ (W[i - 2] >> 10);
        W[i] = W[i - 16] + S0 + W[i - 7] + S1;
    }

    A = State[0]; B = State[1]; C = State[2]; D = State[3];
    E = State[4]; F = State[5]; G = State[6]; H = State[7];
    for (i = 0; i < 64; i++)
    {
        T1 = H + (ROR32(E, 6) ^ ROR32(E, 11) ^ ROR32(E, 25)) + ((E & F) ^ (~E & G)) + Sha256K[i] + W[i];
        T2 = (ROR32(A, 2) ^ ROR32(A, 13) ^ ROR32(A, 22)) + ((A & B) ^ (A & C) ^ (B & C));
        H = G; G = F; F = E; E = D + T1;
        D = C; C = B; B = A; A = T1 + T2;
    }
    State[0] += A; State[1] += B; State[2] += C; State[3] += D;
    State[4] += E; State[5] += F; State[6] += G; State[7] += H;
}

/* The digest is written as eight ULONG values in the byte order of the digest, as a SID sub-authority array holds it */
static
VOID
CapSha256(
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length,
    _Out_writes_bytes_(32) UCHAR Digest[32])
{
    ULONG State[8] =
    {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    UCHAR Tail[128];
    ULONG Full = Length / 64, Rest = Length % 64, TailLength, i;
    ULONGLONG Bits = (ULONGLONG)Length * 8;

    for (i = 0; i < Full; i++)
        CapSha256Block(State, Data + 64 * i);

    RtlZeroMemory(Tail, sizeof(Tail));
    if (Rest)
        RtlCopyMemory(Tail, Data + 64 * Full, Rest);
    Tail[Rest] = 0x80;
    TailLength = (Rest < 56) ? 64 : 128;
    for (i = 0; i < 8; i++)
        Tail[TailLength - 1 - i] = (UCHAR)(Bits >> (8 * i));
    CapSha256Block(State, Tail);
    if (TailLength == 128)
        CapSha256Block(State, Tail + 64);

    for (i = 0; i < 8; i++)
    {
        Digest[4 * i] = (UCHAR)(State[i] >> 24);
        Digest[4 * i + 1] = (UCHAR)(State[i] >> 16);
        Digest[4 * i + 2] = (UCHAR)(State[i] >> 8);
        Digest[4 * i + 3] = (UCHAR)State[i];
    }
}

/* PUBLIC FUNCTIONS ***********************************************************/

static const struct
{
    PCWSTR Name;
    ULONG Rid;
} WellKnownCapabilities[] =
{
    { L"INTERNETCLIENT", 1 },
    { L"INTERNETCLIENTSERVER", 2 },
    { L"PRIVATENETWORKCLIENTSERVER", 3 },
    { L"PICTURESLIBRARY", 4 },
    { L"VIDEOSLIBRARY", 5 },
    { L"MUSICLIBRARY", 6 },
    { L"DOCUMENTSLIBRARY", 7 },
    { L"ENTERPRISEAUTHENTICATION", 8 },
    { L"SHAREDUSERCERTIFICATES", 9 },
    { L"REMOVABLESTORAGE", 10 },
    { L"APPOINTMENTS", 11 },
    { L"CONTACTS", 12 }
};


/*
 * @implemented
 *
 * The hash of the upper-cased name (SHA-256 over its UTF-16 code units) becomes eight sub-authorities. The twelve
 * capabilities of Windows 8 (internetClient to contacts) have fixed SIDs, S-1-15-3-1 to S-1-15-3-12, and only their
 * group SIDs come from the hash. Measured on Windows 10 22H2.
 */
NTSTATUS
NTAPI
RtlDeriveCapabilitySidsFromName(
    _In_ PUNICODE_STRING CapabilityName,
    _Out_ PSID CapabilityGroupSid,
    _Out_ PSID CapabilitySid)
{
    static const SID_IDENTIFIER_AUTHORITY AppPackageAuthority = { SECURITY_APP_PACKAGE_AUTHORITY };
    static const SID_IDENTIFIER_AUTHORITY NtAuthority = { SECURITY_NT_AUTHORITY };
    UNICODE_STRING Upcased;
    NTSTATUS Status;
    ULONG Hash[8];
    ULONG i;
    PISID Sid;

    Status = RtlUpcaseUnicodeString(&Upcased, CapabilityName, TRUE);
    if (!NT_SUCCESS(Status))
        return Status;
    CapSha256((const UCHAR *)Upcased.Buffer, Upcased.Length, (PUCHAR)Hash);

    Sid = CapabilitySid;
    /* Windows clears the whole ten sub-authority area, also for a fixed capability */
    RtlZeroMemory(Sid, RtlLengthRequiredSid(2 + RTL_NUMBER_OF(Hash)));
    Sid->Revision = SID_REVISION;
    Sid->IdentifierAuthority = AppPackageAuthority;
    Sid->SubAuthority[0] = SECURITY_CAPABILITY_BASE_RID;
    Sid->SubAuthorityCount = 2;
    for (i = 0; i < RTL_NUMBER_OF(WellKnownCapabilities); i++)
    {
        UNICODE_STRING Known;

        RtlInitUnicodeString(&Known, WellKnownCapabilities[i].Name);
        if (RtlEqualUnicodeString(&Upcased, &Known, FALSE))
        {
            Sid->SubAuthority[1] = WellKnownCapabilities[i].Rid;
            break;
        }
    }
    if (i == RTL_NUMBER_OF(WellKnownCapabilities))
    {
        Sid->SubAuthorityCount = 2 + RTL_NUMBER_OF(Hash);
        Sid->SubAuthority[1] = 1024;
        RtlCopyMemory(&Sid->SubAuthority[2], Hash, sizeof(Hash));
    }
    RtlFreeUnicodeString(&Upcased);

    Sid = CapabilityGroupSid;
    Sid->Revision = SID_REVISION;
    Sid->IdentifierAuthority = NtAuthority;
    Sid->SubAuthorityCount = 1 + RTL_NUMBER_OF(Hash);
    Sid->SubAuthority[0] = SECURITY_BUILTIN_DOMAIN_RID;
    RtlCopyMemory(&Sid->SubAuthority[1], Hash, sizeof(Hash));

    return STATUS_SUCCESS;
}
