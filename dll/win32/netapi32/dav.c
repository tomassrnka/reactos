/*
 * PROJECT:     NetAPI DLL
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     WebDAV redirector path conversion: UNC path <-> HTTP URL
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *
 * The accepted syntax and the error codes were measured on Windows 10 22H2 (see the
 * netapi32 DavPaths apitest):
 *   \\server[@SSL][@port][\DavWWWRoot][\path]  <->  http[s]://[user@]server[:port][/path]
 * Wine's version of these functions (and its netapi32:access test) follows older Windows
 * releases and disagrees with Windows 10 in the error codes and in what is accepted.
 */

#include "netapi32.h"

#include <wchar.h>
#include <strsafe.h>

WINE_DEFAULT_DEBUG_CHANNEL(netapi32);

static const WCHAR g_DavRoot[] = L"DavWWWRoot";
#define DAV_ROOT_LEN (_countof(g_DavRoot) - 1)

static BOOL
IsSlash(WCHAR Ch)
{
    return Ch == L'\\' || Ch == L'/';
}

/* Parses decimal digits up to 65535 */
static BOOL
ParsePort(const WCHAR *Start, const WCHAR *End, ULONG *Port)
{
    ULONG Value = 0;

    if (Start == End)
        return FALSE;
    for (; Start < End; Start++)
    {
        if (*Start < L'0' || *Start > L'9')
            return FALSE;
        Value = Value * 10 + (*Start - L'0');
        if (Value > 65535)
            return FALSE;
    }
    *Port = Value;
    return TRUE;
}

/* A server name is not empty, not just dots and has none of the characters of a URL's other parts */
static BOOL
IsValidServerName(const WCHAR *Start, const WCHAR *End)
{
    const WCHAR *Pos;
    BOOL OnlyDots = TRUE;

    if (Start == End)
        return FALSE;
    for (Pos = Start; Pos < End; Pos++)
    {
        if (*Pos != L'.')
            OnlyDots = FALSE;
        if (wcschr(L"*[]:\\/?#|<>\"", *Pos))
            return FALSE;
    }
    return !OnlyDots;
}

static BOOL
IsInvalidPathChar(WCHAR Ch)
{
    return Ch == L':' || Ch == L'<' || Ch == L'>' || Ch == L'|' || Ch == L'"';
}

static int
HexValue(WCHAR Ch)
{
    if (Ch >= L'0' && Ch <= L'9') return Ch - L'0';
    if (Ch >= L'a' && Ch <= L'f') return Ch - L'a' + 10;
    if (Ch >= L'A' && Ch <= L'F') return Ch - L'A' + 10;
    return -1;
}

static DWORD
FinishResult(const WCHAR *Result, SIZE_T Length, WCHAR *Buffer, DWORD *BufferLength)
{
    SIZE_T Needed = Length + 1;

    if (*BufferLength < Needed)
    {
        *BufferLength = (DWORD)Needed;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (!Buffer)
        return ERROR_INVALID_PARAMETER;

    RtlCopyMemory(Buffer, Result, Length * sizeof(WCHAR));
    Buffer[Length] = UNICODE_NULL;
    *BufferLength = (DWORD)Needed;
    return ERROR_SUCCESS;
}

/*
 * Appends the path of a UNC name to a URL: components are separated by a slash, a "." component
 * is dropped, ".." and empty components are refused and characters that a URL cannot carry are
 * escaped. The result starts with a slash unless the path is empty.
 */
static DWORD
AppendUrlPath(const WCHAR *Path, SIZE_T PathLength, WCHAR *Result, SIZE_T *Length)
{
    static const WCHAR Hex[] = L"0123456789ABCDEF";
    SIZE_T Index = 0, Start, End;

    while (Index < PathLength)
    {
        Start = Index;
        while (Index < PathLength && !IsSlash(Path[Index]))
            Index++;
        End = Index;
        if (Index < PathLength)
            Index++;

        if (End == Start)
            return ERROR_BAD_NET_NAME;
        if (End - Start == 1 && Path[Start] == L'.')
            continue;
        if (End - Start == 2 && Path[Start] == L'.' && Path[Start + 1] == L'.')
            return ERROR_BAD_NET_NAME;

        Result[(*Length)++] = L'/';
        for (; Start < End; Start++)
        {
            WCHAR Ch = Path[Start];

            if (IsInvalidPathChar(Ch))
                return ERROR_BAD_NET_NAME;
            if (Ch == L' ' || Ch == L'#' || Ch == L'%' || Ch == L'?' || Ch == L'@')
            {
                Result[(*Length)++] = L'%';
                Result[(*Length)++] = Hex[(Ch >> 4) & 0xF];
                Result[(*Length)++] = Hex[Ch & 0xF];
            }
            else
            {
                Result[(*Length)++] = Ch;
            }
        }
    }
    return ERROR_SUCCESS;
}

DWORD
WINAPI
DavGetHTTPFromUNCPath(
    _In_ PCWSTR UncPath,
    _Out_writes_opt_(*BufferLength) PWSTR Buffer,
    _Inout_ PDWORD BufferLength)
{
    const WCHAR *Pos, *ServerStart, *ServerEnd, *Token, *TokenEnd, *Path;
    BOOL Ssl = FALSE, HasPort = FALSE;
    ULONG Port = 0;
    SIZE_T PathLength, Length;
    WCHAR *Result;
    DWORD Status;

    TRACE("(%S, %p, %p)\n", UncPath, Buffer, BufferLength);

    if (!UncPath || !BufferLength)
        return ERROR_INVALID_PARAMETER;

    if (!IsSlash(UncPath[0]) || !IsSlash(UncPath[1]))
        return ERROR_BAD_NET_NAME;

    ServerStart = Pos = UncPath + 2;
    while (*Pos && !IsSlash(*Pos) && *Pos != L'@')
        Pos++;
    ServerEnd = Pos;
    if (!IsValidServerName(ServerStart, ServerEnd))
        return ERROR_BAD_NET_NAME;

    /* @SSL, @port or @SSL@port */
    if (*Pos == L'@')
    {
        Token = ++Pos;
        while (*Pos && !IsSlash(*Pos) && *Pos != L'@')
            Pos++;
        TokenEnd = Pos;

        if (TokenEnd - Token == 3 && !_wcsnicmp(Token, L"SSL", 3))
        {
            Ssl = TRUE;
            if (*Pos == L'@')
            {
                Token = ++Pos;
                while (*Pos && !IsSlash(*Pos) && *Pos != L'@')
                    Pos++;
                if (!ParsePort(Token, Pos, &Port))
                    return ERROR_INVALID_PARAMETER;
                HasPort = TRUE;
            }
        }
        else
        {
            if (!ParsePort(Token, TokenEnd, &Port))
                return ERROR_INVALID_PARAMETER;
            HasPort = TRUE;
        }
        if (*Pos == L'@')
            return ERROR_INVALID_PARAMETER;
    }

    if (IsSlash(*Pos))
        Pos++;
    Path = Pos;

    /* The DavWWWRoot component stands for the root of the share and is not part of the URL */
    if (!_wcsnicmp(Path, g_DavRoot, DAV_ROOT_LEN) && (!Path[DAV_ROOT_LEN] || IsSlash(Path[DAV_ROOT_LEN])))
    {
        Path += DAV_ROOT_LEN;
        if (IsSlash(*Path))
            Path++;
    }
    PathLength = wcslen(Path);
    if (PathLength && IsSlash(Path[PathLength - 1]))
        PathLength--;

    Result = RtlAllocateHeap(RtlGetProcessHeap(), 0,
                             (8 + (ServerEnd - ServerStart) + 6 + 3 * PathLength + PathLength + 2) * sizeof(WCHAR));
    if (!Result)
        return ERROR_NOT_ENOUGH_MEMORY;

    Length = 0;
    RtlCopyMemory(Result, Ssl ? L"https://" : L"http://", (Ssl ? 8 : 7) * sizeof(WCHAR));
    Length += Ssl ? 8 : 7;
    RtlCopyMemory(Result + Length, ServerStart, (ServerEnd - ServerStart) * sizeof(WCHAR));
    Length += ServerEnd - ServerStart;
    if (HasPort && Port != (Ssl ? 443 : 80))
    {
        StringCchPrintfW(Result + Length, 7, L":%lu", Port);
        Length += wcslen(Result + Length);
    }

    Status = AppendUrlPath(Path, PathLength, Result, &Length);
    if (Status == ERROR_SUCCESS)
        Status = FinishResult(Result, Length, Buffer, BufferLength);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Result);
    return Status;
}

DWORD
WINAPI
DavGetUNCFromHTTPPath(
    _In_ PCWSTR HttpPath,
    _Out_writes_opt_(*BufferLength) PWSTR Buffer,
    _Inout_ PDWORD BufferLength)
{
    const WCHAR *Pos, *ServerStart, *ServerEnd, *AuthorityEnd, *PortStart = NULL, *Path, *PathEnd;
    BOOL Ssl;
    ULONG Port = 0;
    SIZE_T Length, Index;
    WCHAR *Result;
    DWORD Status;

    TRACE("(%S, %p, %p)\n", HttpPath, Buffer, BufferLength);

    if (!HttpPath || !BufferLength)
        return ERROR_INVALID_PARAMETER;

    Pos = HttpPath;
    while (*Pos && *Pos != L':')
        Pos++;
    if (*Pos != L':')
        return ERROR_INVALID_PARAMETER;
    if (Pos - HttpPath == 4 && !_wcsnicmp(HttpPath, L"http", 4))
        Ssl = FALSE;
    else if (Pos - HttpPath == 5 && !_wcsnicmp(HttpPath, L"https", 5))
        Ssl = TRUE;
    else
        return ERROR_INVALID_PARAMETER;

    if (Pos[1] != L'/' || Pos[2] != L'/')
        return ERROR_BAD_NET_NAME;
    Pos += 3;

    /* authority = [user@]server[:port] */
    AuthorityEnd = Pos;
    while (*AuthorityEnd && *AuthorityEnd != L'/')
        AuthorityEnd++;
    ServerStart = Pos;
    for (Pos = ServerStart; Pos < AuthorityEnd; Pos++)
    {
        if (*Pos == L'@')
            ServerStart = Pos + 1;
    }
    if (ServerStart == AuthorityEnd || *ServerStart == L':')
        return ERROR_BAD_NETPATH;
    if (*ServerStart == L'[')
        return ERROR_BAD_NET_NAME;
    ServerEnd = ServerStart;
    while (ServerEnd < AuthorityEnd && *ServerEnd != L':')
        ServerEnd++;
    if (!IsValidServerName(ServerStart, ServerEnd))
        return ERROR_BAD_NET_NAME;
    if (ServerEnd < AuthorityEnd)
    {
        PortStart = ServerEnd + 1;
        if (!ParsePort(PortStart, AuthorityEnd, &Port))
            return ERROR_BAD_NETPATH;
    }

    /* The query and the fragment are not part of the path */
    Path = AuthorityEnd;
    PathEnd = Path;
    while (*PathEnd && *PathEnd != L'#' && *PathEnd != L'?')
        PathEnd++;
    if (PathEnd > Path && PathEnd[-1] == L'/')
        PathEnd--;

    Result = RtlAllocateHeap(RtlGetProcessHeap(), 0,
                             (2 + (ServerEnd - ServerStart) + 4 + 7 + DAV_ROOT_LEN + (PathEnd - Path) + 2) * sizeof(WCHAR));
    if (!Result)
        return ERROR_NOT_ENOUGH_MEMORY;

    Length = 0;
    Result[Length++] = L'\\';
    Result[Length++] = L'\\';
    RtlCopyMemory(Result + Length, ServerStart, (ServerEnd - ServerStart) * sizeof(WCHAR));
    Length += ServerEnd - ServerStart;
    if (Ssl)
    {
        RtlCopyMemory(Result + Length, L"@SSL", 4 * sizeof(WCHAR));
        Length += 4;
    }
    if (PortStart && Port != (Ssl ? 443 : 80))
    {
        StringCchPrintfW(Result + Length, 8, L"@%lu", Port);
        Length += wcslen(Result + Length);
    }
    Result[Length++] = L'\\';
    RtlCopyMemory(Result + Length, g_DavRoot, DAV_ROOT_LEN * sizeof(WCHAR));
    Length += DAV_ROOT_LEN;

    /* Components are separated by a slash, and an empty one or a character that a path cannot carry is refused;
     * %XX stands for a character */
    Status = ERROR_SUCCESS;
    for (Index = 0; Path + Index < PathEnd && Status == ERROR_SUCCESS; )
    {
        WCHAR Ch = Path[Index];

        if (Ch == L'/')
        {
            if (Path + Index + 1 < PathEnd && Path[Index + 1] == L'/')
            {
                Status = ERROR_BAD_NET_NAME;
                break;
            }
            Result[Length++] = L'\\';
            Index++;
            continue;
        }
        if (IsInvalidPathChar(Ch))
        {
            Status = ERROR_BAD_NET_NAME;
            break;
        }
        if (Ch == L'%' && Path + Index + 2 < PathEnd + 0 && HexValue(Path[Index + 1]) >= 0 && HexValue(Path[Index + 2]) >= 0)
        {
            Ch = (WCHAR)(HexValue(Path[Index + 1]) * 16 + HexValue(Path[Index + 2]));
            Index += 3;
        }
        else
        {
            Index++;
        }
        Result[Length++] = Ch;
    }
    if (Status == ERROR_SUCCESS)
        Status = FinishResult(Result, Length, Buffer, BufferLength);
    RtlFreeHeap(RtlGetProcessHeap(), 0, Result);
    return Status;
}
