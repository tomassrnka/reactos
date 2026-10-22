/*
 * PROJECT:     ReactOS user profile library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     AppContainer security identifiers
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The identifier of an AppContainer is derived from its name: the name is converted to lower case and hashed
 * with SHA-256 over its UTF-16 little-endian bytes, and the first seven 32-bit words of the hash follow the
 * authority 15 and the first sub-authority 2 (measured on Windows 10; names of 1 to 64 characters).
 */

#include "precomp.h"

#define APPCONTAINER_NAME_MAX 64
#define APPCONTAINER_HASH_WORDS 7

/* SHA-256 after FIPS 180-4 */
typedef struct _SHA256_STATE
{
    ULONG H[8];
    ULONGLONG Bytes;
    UCHAR Block[64];
    ULONG Used;
} SHA256_STATE;

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

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static
VOID
Sha256Transform(
    _Inout_ SHA256_STATE *State,
    _In_reads_bytes_(64) const UCHAR *Block)
{
    ULONG W[64], a, b, c, d, e, f, g, h, t1, t2;
    ULONG i;

    for (i = 0; i < 16; i++)
    {
        W[i] = ((ULONG)Block[i * 4] << 24) | ((ULONG)Block[i * 4 + 1] << 16) |
               ((ULONG)Block[i * 4 + 2] << 8) | (ULONG)Block[i * 4 + 3];
    }
    for (; i < 64; i++)
    {
        ULONG s0 = ROTR(W[i - 15], 7) ^ ROTR(W[i - 15], 18) ^ (W[i - 15] >> 3);
        ULONG s1 = ROTR(W[i - 2], 17) ^ ROTR(W[i - 2], 19) ^ (W[i - 2] >> 10);

        W[i] = W[i - 16] + s0 + W[i - 7] + s1;
    }

    a = State->H[0]; b = State->H[1]; c = State->H[2]; d = State->H[3];
    e = State->H[4]; f = State->H[5]; g = State->H[6]; h = State->H[7];
    for (i = 0; i < 64; i++)
    {
        t1 = h + (ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25)) + ((e & f) ^ (~e & g)) + Sha256K[i] + W[i];
        t2 = (ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    State->H[0] += a; State->H[1] += b; State->H[2] += c; State->H[3] += d;
    State->H[4] += e; State->H[5] += f; State->H[6] += g; State->H[7] += h;
}

static
VOID
Sha256Init(
    _Out_ SHA256_STATE *State)
{
    State->H[0] = 0x6a09e667; State->H[1] = 0xbb67ae85; State->H[2] = 0x3c6ef372; State->H[3] = 0xa54ff53a;
    State->H[4] = 0x510e527f; State->H[5] = 0x9b05688c; State->H[6] = 0x1f83d9ab; State->H[7] = 0x5be0cd19;
    State->Bytes = 0;
    State->Used = 0;
}

static
VOID
Sha256Update(
    _Inout_ SHA256_STATE *State,
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    State->Bytes += Length;
    while (Length)
    {
        ULONG Chunk = min(Length, 64 - State->Used);

        RtlCopyMemory(State->Block + State->Used, Data, Chunk);
        State->Used += Chunk;
        Data += Chunk;
        Length -= Chunk;
        if (State->Used == 64)
        {
            Sha256Transform(State, State->Block);
            State->Used = 0;
        }
    }
}

/* The first words of the hash, as 32-bit values of the little-endian byte order of the digest */
static
VOID
Sha256Final(
    _Inout_ SHA256_STATE *State,
    _Out_writes_(Words) PULONG Output,
    _In_ ULONG Words)
{
    ULONGLONG Bits = State->Bytes * 8;
    UCHAR Digest[32];
    ULONG i;

    State->Block[State->Used++] = 0x80;
    if (State->Used > 56)
    {
        RtlZeroMemory(State->Block + State->Used, 64 - State->Used);
        Sha256Transform(State, State->Block);
        State->Used = 0;
    }
    RtlZeroMemory(State->Block + State->Used, 56 - State->Used);
    for (i = 0; i < 8; i++)
        State->Block[56 + i] = (UCHAR)(Bits >> (56 - i * 8));
    Sha256Transform(State, State->Block);

    for (i = 0; i < 8; i++)
    {
        Digest[i * 4] = (UCHAR)(State->H[i] >> 24);
        Digest[i * 4 + 1] = (UCHAR)(State->H[i] >> 16);
        Digest[i * 4 + 2] = (UCHAR)(State->H[i] >> 8);
        Digest[i * 4 + 3] = (UCHAR)State->H[i];
    }
    for (i = 0; i < Words; i++)
    {
        Output[i] = (ULONG)Digest[i * 4] | ((ULONG)Digest[i * 4 + 1] << 8) |
                    ((ULONG)Digest[i * 4 + 2] << 16) | ((ULONG)Digest[i * 4 + 3] << 24);
    }
}

/*
 * @implemented
 */
HRESULT
WINAPI
DeriveAppContainerSidFromAppContainerName(
    _In_ PCWSTR pszAppContainerName,
    _Outptr_ PSID *ppsidAppContainerSid)
{
    static SID_IDENTIFIER_AUTHORITY AppPackageAuthority = { { 0, 0, 0, 0, 0, 15 } };
    WCHAR Lower[APPCONTAINER_NAME_MAX];
    ULONG Hash[APPCONTAINER_HASH_WORDS];
    SHA256_STATE State;
    SIZE_T Length;
    ULONG i;
    PSID Sid;

    if (!pszAppContainerName || !ppsidAppContainerSid)
        return E_INVALIDARG;

    for (Length = 0; Length <= APPCONTAINER_NAME_MAX && pszAppContainerName[Length]; Length++)
        ;
    if (Length == 0 || Length > APPCONTAINER_NAME_MAX)
        return E_INVALIDARG;

    for (i = 0; i < Length; i++)
        Lower[i] = RtlDowncaseUnicodeChar(pszAppContainerName[i]);

    Sha256Init(&State);
    Sha256Update(&State, (const UCHAR *)Lower, (ULONG)(Length * sizeof(WCHAR)));
    Sha256Final(&State, Hash, APPCONTAINER_HASH_WORDS);

    if (!AllocateAndInitializeSid(&AppPackageAuthority, 8, 2, Hash[0], Hash[1], Hash[2], Hash[3],
                                  Hash[4], Hash[5], Hash[6], &Sid))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    *ppsidAppContainerSid = Sid;
    return S_OK;
}
