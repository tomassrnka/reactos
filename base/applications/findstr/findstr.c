/*
 * PROJECT:     ReactOS FINDSTR command
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Searches files for literal strings or limited regular expressions
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <wincon.h>
#include <winuser.h>

#include <conutils.h>

#include "resource.h"

/* Search string types */
#define TYPE_DEFAULT 0
#define TYPE_LITERAL 1
#define TYPE_REGEX   2

/* Regular expression token kinds */
#define TK_CHAR   0
#define TK_ANY    1
#define TK_CLASS  2
#define TK_BOL    3
#define TK_EOL    4
#define TK_WBEG   5
#define TK_WEND   6

typedef struct _TOKEN
{
    UCHAR Kind;
    UCHAR Star;
    UCHAR Char;
    UCHAR LiteralStar;      /* A star at the beginning of the expression, taken literally */
    UCHAR Class[32];
} TOKEN, *PTOKEN;

typedef struct _PATTERN
{
    struct _PATTERN *Next;
    PSTR Text;          /* Search string, upper-cased for /I literal searches */
    SIZE_T Length;
    BOOL Regex;
    PTOKEN Tokens;
    SIZE_T TokenCount;
    PUCHAR States, NextStates;  /* Matcher state sets, TokenCount + 1 entries each */
} PATTERN, *PPATTERN;

typedef struct _NAME
{
    struct _NAME *Next;
    PWSTR Name;
} NAME, *PNAME;

static struct
{
    BOOL Begin, End, SubDirs, IgnoreCase, Exact, Invert;
    BOOL LineNumbers, FileNamesOnly, Offsets, SkipNonPrintable, Offline;
    WORD Color;
    BOOL UseColor;
} Opt;

static PPATTERN Patterns = NULL, *PatternTail = &Patterns;
static PNAME Files = NULL, *FileTail = &Files;
static PNAME Dirs = NULL, *DirTail = &Dirs;
static HANDLE hOut;
static BOOL OutIsConsole;
static WORD DefaultAttributes;
static BOOL ShowNames = FALSE;
static BOOL AnyMatch = FALSE;
static UCHAR UpperTable[256];
static UCHAR LowerTable[256];
static USHORT Collation[256];

static VOID Message(UINT Id, ...);
static INT Finish(INT ExitCode);

static PVOID
Alloc(SIZE_T Size)
{
    PVOID p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size ? Size : 1);
    if (!p)
    {
        Message(IDS_OUT_OF_MEMORY);
        ExitProcess(Finish(2));
    }
    return p;
}

/* Messages go to standard error at once on a console; otherwise they are collected and written
 * when the program ends, after the search output */
static PWSTR Messages = NULL;
static SIZE_T MessagesLength = 0;
static BOOL ErrIsConsole;

static VOID
Message(UINT Id, ...)
{
    WCHAR Format[256], Text[1024];
    va_list Args;
    SIZE_T Length;
    PWSTR New;

    if (!LoadStringW(GetModuleHandleW(NULL), Id, Format, ARRAYSIZE(Format)))
        return;
    va_start(Args, Id);
    _vsnwprintf(Text, ARRAYSIZE(Text) - 1, Format, Args);
    va_end(Args);
    Text[ARRAYSIZE(Text) - 1] = UNICODE_NULL;

    if (ErrIsConsole)
    {
        ConPuts(StdErr, Text);
        return;
    }

    Length = wcslen(Text);
    New = Messages ? HeapReAlloc(GetProcessHeap(), 0, Messages, (MessagesLength + Length + 1) * sizeof(WCHAR))
                   : HeapAlloc(GetProcessHeap(), 0, (Length + 1) * sizeof(WCHAR));
    if (!New)
        return;
    Messages = New;
    wcscpy(Messages + MessagesLength, Text);
    MessagesLength += Length;
}

static INT
Finish(INT ExitCode)
{
    if (Messages)
        ConPuts(StdErr, Messages);
    return ExitCode;
}

static VOID
InitTables(VOID)
{
    CHAR Buffer[256];
    UINT i;

    for (i = 0; i < 256; i++)
        Buffer[i] = (CHAR)i;
    CopyMemory(UpperTable, Buffer, sizeof(UpperTable));
    CopyMemory(LowerTable, Buffer, sizeof(LowerTable));
    CharUpperBuffA((PSTR)&UpperTable[1], 255);
    CharLowerBuffA((PSTR)&LowerTable[1], 255);

    /* Character ranges ([x-y]) follow the dictionary order of the Windows sort: punctuation,
     * then + < = >, then digits, then each letter in lower case before its upper case form.
     * The apostrophe and the hyphen have no place in that order and are in no range. */
    {
        static const CHAR Order[] = " !\"#$%&()*,./:;?@[\\]^_`{|}~+<=>0123456789"
                                    "aAbBcCdDeEfFgGhHiIjJkKlLmMnNoOpPqQrRsStTuUvVwWxXyYzZ";
        for (i = 0; i < 256; i++)
            Collation[i] = (USHORT)(0x100 + i);
        for (i = 0; Order[i]; i++)
            Collation[(UCHAR)Order[i]] = (USHORT)(1 + i);
        Collation['\''] = 0;
        Collation['-'] = 0;
    }
}

/* Output ********************************************************************/

static VOID
WriteBytes(LPCVOID Data, SIZE_T Length)
{
    DWORD Written;
    const BYTE *p = Data;

    while (Length)
    {
        DWORD Chunk = (Length > 0x10000) ? 0x10000 : (DWORD)Length;
        if (!WriteFile(hOut, p, Chunk, &Written, NULL) || !Written)
            return;
        p += Written;
        Length -= Written;
    }
}

static VOID
WriteName(PCWSTR Name)
{
    INT Length;
    PSTR Bytes;
    UINT CodePage = OutIsConsole ? GetConsoleOutputCP() : CP_OEMCP;

    if (OutIsConsole && Opt.UseColor)
        SetConsoleTextAttribute(hOut, Opt.Color);

    Length = WideCharToMultiByte(CodePage, 0, Name, -1, NULL, 0, NULL, NULL);
    if (Length > 1)
    {
        Bytes = Alloc(Length);
        WideCharToMultiByte(CodePage, 0, Name, -1, Bytes, Length, NULL, NULL);
        WriteBytes(Bytes, Length - 1);
        HeapFree(GetProcessHeap(), 0, Bytes);
    }

    if (OutIsConsole && Opt.UseColor)
        SetConsoleTextAttribute(hOut, DefaultAttributes);
}

static VOID
WriteNumber(ULONGLONG Value)
{
    CHAR Buffer[32];
    INT Length = _snprintf(Buffer, sizeof(Buffer), "%I64u", Value);

    if (OutIsConsole && Opt.UseColor)
        SetConsoleTextAttribute(hOut, Opt.Color);
    WriteBytes(Buffer, Length);
    if (OutIsConsole && Opt.UseColor)
        SetConsoleTextAttribute(hOut, DefaultAttributes);
}

/* Regular expressions ******************************************************/

static __inline VOID
ClassSet(PUCHAR Class, UCHAR c)
{
    Class[c >> 3] |= (UCHAR)(1 << (c & 7));
}

static __inline BOOL
ClassTest(const UCHAR *Class, UCHAR c)
{
    return (Class[c >> 3] & (1 << (c & 7))) != 0;
}

/* \< and \> treat ASCII letters, digits and the underscore as word characters */
static __inline BOOL
IsWordChar(UCHAR c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/* Converts a search string into tokens. The dialect: . * ^ $ [class] [^class] [x-y] \< \> \x.
 * Returns FALSE for an invalid expression: a star with nothing to repeat, an unclosed class or
 * a trailing backslash. */
static BOOL
CompileRegex(PPATTERN Pattern)
{
    const UCHAR *s = (const UCHAR *)Pattern->Text;
    SIZE_T Length = Pattern->Length, i = 0, n = 0;
    PTOKEN t;

    if (Length >= ((SIZE_T)-1) / sizeof(TOKEN) - 1)
    {
        Message(IDS_OUT_OF_MEMORY);
        ExitProcess(Finish(2));
    }
    Pattern->Tokens = Alloc((Length + 1) * sizeof(TOKEN));

    while (i < Length)
    {
        UCHAR c = s[i];
        t = &Pattern->Tokens[n];

        /* A star at the very beginning is an ordinary character; two stars in a row are invalid */
        if (c == '*' && n > 0)
        {
            if (Pattern->Tokens[n - 1].Star || Pattern->Tokens[n - 1].LiteralStar ||
                Pattern->Tokens[n - 1].Kind == TK_BOL ||
                Pattern->Tokens[n - 1].Kind == TK_WBEG || Pattern->Tokens[n - 1].Kind == TK_WEND)
            {
                return FALSE;
            }
            Pattern->Tokens[n - 1].Star = TRUE;
            i++;
            continue;
        }

        if (c == '\\' && i + 1 == Length)
            return FALSE;

        if (c == '^' && i == 0)
        {
            t->Kind = TK_BOL;
            i++;
        }
        else if (c == '$' && i == Length - 1)
        {
            t->Kind = TK_EOL;
            i++;
        }
        else if (c == '.')
        {
            t->Kind = TK_ANY;
            i++;
        }
        else if (c == '\\' && i + 1 < Length)
        {
            if (s[i + 1] == '<')
                t->Kind = TK_WBEG;
            else if (s[i + 1] == '>')
                t->Kind = TK_WEND;
            else
            {
                t->Kind = TK_CHAR;
                t->Char = s[i + 1];
            }
            i += 2;
        }
        else if (c == '[')
        {
            SIZE_T j = i + 1;
            BOOL Negate = FALSE;
            UCHAR Set[32];
            UINT k;

            ZeroMemory(Set, sizeof(Set));
            if (j < Length && s[j] == '^')
            {
                Negate = TRUE;
                j++;
            }
            while (j < Length && s[j] != ']')
            {
                UCHAR Low = s[j];
                if (Low == '\\' && j + 1 < Length)
                    Low = s[++j];
                if (j + 2 < Length && s[j + 1] == '-' && s[j + 2] != ']')
                {
                    UCHAR High = s[j + 2];
                    j += 2;
                    if (High == '\\' && j + 1 < Length)
                        High = s[++j];
                    for (k = 1; k < 256; k++)
                    {
                        if (Collation[k] && Collation[Low] && Collation[High] &&
                            Collation[k] >= Collation[Low] && Collation[k] <= Collation[High])
                        {
                            ClassSet(Set, (UCHAR)k);
                        }
                    }
                }
                else
                {
                    ClassSet(Set, Low);
                }
                j++;
            }
            if (j >= Length)
                return FALSE;

            {
                t->Kind = TK_CLASS;
                if (Opt.IgnoreCase)
                {
                    for (k = 0; k < 256; k++)
                    {
                        if (ClassTest(Set, (UCHAR)k))
                        {
                            ClassSet(Set, UpperTable[k]);
                            ClassSet(Set, LowerTable[k]);
                        }
                    }
                }
                for (k = 0; k < 32; k++)
                    t->Class[k] = Negate ? (UCHAR)~Set[k] : Set[k];
                i = j + 1;
            }
        }
        else
        {
            t->Kind = TK_CHAR;
            t->Char = c;
            t->LiteralStar = (c == '*');
            i++;
        }

        if (t->Kind == TK_CHAR && Opt.IgnoreCase)
            t->Char = UpperTable[t->Char];
        n++;
    }

    Pattern->TokenCount = n;
    Pattern->States = Alloc(n + 1);
    Pattern->NextStates = Alloc(n + 1);
    return TRUE;
}

static __inline BOOL
TokenMatchesChar(const TOKEN *t, UCHAR c)
{
    switch (t->Kind)
    {
        case TK_ANY:
            return c != '\r';
        case TK_CHAR:
            return (Opt.IgnoreCase ? UpperTable[c] : c) == t->Char;
        case TK_CLASS:
            return c != '\r' && ClassTest(t->Class, c);
    }
    return FALSE;
}

/* A line is everything up to the line feed, carriage returns included. The end of a line
 * ($, /E, /X) is a position followed by a carriage return, so a line that ends with a bare line
 * feed, or the last line of a file without a line end, has no end that can match */
typedef struct _LINE
{
    const UCHAR *Text;
    SIZE_T Length;
} LINE, *PLINE;

static __inline BOOL
EndAt(const LINE *Line, SIZE_T Pos)
{
    return Pos < Line->Length && Line->Text[Pos] == '\r';
}

/* Runs the expression over the line as a set of token positions (no backtracking): time is
 * linear in the line length times the number of tokens, and no recursion is involved. */
static BOOL
MatchRegex(const PATTERN *Pattern, const LINE *Line)
{
    const TOKEN *t = Pattern->Tokens;
    SIZE_T Count = Pattern->TokenCount, Pos, k;
    PUCHAR States = Pattern->States, Next = Pattern->NextStates, Swap;
    BOOL AnchorBegin = Opt.Begin || Opt.Exact;
    BOOL AnchorEnd = Opt.End || Opt.Exact;
    BOOL Any;

    ZeroMemory(States, Count + 1);
    for (Pos = 0; ; Pos++)
    {
        if (Pos == 0 || !AnchorBegin)
            States[0] = 1;

        /* Zero-width tokens and starred tokens can be passed without consuming a character */
        for (k = 0; k < Count; k++)
        {
            if (!States[k])
                continue;
            switch (t[k].Kind)
            {
                case TK_BOL:
                    if (Pos == 0)
                        States[k + 1] = 1;
                    break;
                case TK_EOL:
                    if (EndAt(Line, Pos))
                        States[k + 1] = 1;
                    break;
                case TK_WBEG:
                    /* A word does not begin right after a carriage return (it can end before one) */
                    if (Pos < Line->Length && IsWordChar(Line->Text[Pos]) &&
                        (Pos == 0 || (!IsWordChar(Line->Text[Pos - 1]) && Line->Text[Pos - 1] != '\r')))
                    {
                        States[k + 1] = 1;
                    }
                    break;
                case TK_WEND:
                    if (Pos > 0 && IsWordChar(Line->Text[Pos - 1]) &&
                        (Pos == Line->Length || !IsWordChar(Line->Text[Pos])))
                    {
                        States[k + 1] = 1;
                    }
                    break;
                default:
                    if (t[k].Star)
                        States[k + 1] = 1;
                    break;
            }
        }

        if (States[Count] && (!AnchorEnd || EndAt(Line, Pos)))
            return TRUE;
        if (Pos == Line->Length)
            return FALSE;

        /* Consume the character at Pos */
        ZeroMemory(Next, Count + 1);
        Any = FALSE;
        for (k = 0; k < Count; k++)
        {
            if (!States[k] || t[k].Kind > TK_CLASS)
                continue;
            if (TokenMatchesChar(&t[k], Line->Text[Pos]))
            {
                Next[t[k].Star ? k : k + 1] = 1;
                Any = TRUE;
            }
        }
        Swap = States;
        States = Next;
        Next = Swap;
        if (!Any && AnchorBegin)
            return FALSE;
    }
}

/* Literal strings **********************************************************/

static __inline BOOL
EqualAt(const UCHAR *Text, const PATTERN *Pattern)
{
    SIZE_T i;

    if (!Opt.IgnoreCase)
        return memcmp(Text, Pattern->Text, Pattern->Length) == 0;

    for (i = 0; i < Pattern->Length; i++)
    {
        if (UpperTable[Text[i]] != (UCHAR)Pattern->Text[i])
            return FALSE;
    }
    return TRUE;
}

static BOOL
MatchLiteral(const PATTERN *Pattern, const LINE *Line)
{
    SIZE_T i, Length = Line->Length;

    if (Pattern->Length > Length)
        return FALSE;
    if (Opt.Exact || Opt.Begin)
    {
        if (!EqualAt(Line->Text, Pattern))
            return FALSE;
        return !(Opt.Exact || Opt.End) || EndAt(Line, Pattern->Length);
    }

    for (i = 0; i + Pattern->Length <= Length; i++)
    {
        if (EqualAt(Line->Text + i, Pattern) && (!Opt.End || EndAt(Line, i + Pattern->Length)))
            return TRUE;
    }
    return FALSE;
}

static BOOL
LineMatches(const LINE *Line)
{
    PPATTERN p;

    for (p = Patterns; p; p = p->Next)
    {
        if (p->Regex ? MatchRegex(p, Line) : MatchLiteral(p, Line))
            return TRUE;
    }
    return FALSE;
}

/* Searching ****************************************************************/

static BOOL
HasNonPrintable(const UCHAR *Data, SIZE_T Length)
{
    SIZE_T i;

    for (i = 0; i < Length; i++)
    {
        UCHAR c = Data[i];
        if (c < 0x20 && c != '\t' && c != '\r' && c != '\n' && c != '\f' && c != 0x1A)
            return TRUE;
    }
    return FALSE;
}

/* Search state of one input */
typedef struct _SEARCH_INPUT
{
    PCWSTR Name;            /* NULL for standard input */
    ULONGLONG LineNumber;
    ULONGLONG Offset;       /* Offset of the next line in the input */
    BOOL Done;              /* /M printed the name: stop reading */
} SEARCH_INPUT, *PSEARCH_INPUT;

/* Searches the complete lines in Data (and, if Final, the last line without a line end).
 * Returns the number of bytes used. */
static SIZE_T
SearchLines(PSEARCH_INPUT Input, const UCHAR *Data, SIZE_T Size, BOOL Final)
{
    SIZE_T Start = 0;
    LINE Line;

    while (Start < Size && !Input->Done)
    {
        const UCHAR *Lf = memchr(Data + Start, '\n', Size - Start);
        SIZE_T End = Lf ? (SIZE_T)(Lf - Data) : Size;
        SIZE_T Next = Lf ? End + 1 : End;

        if (!Lf && !Final)
            break;

        Line.Text = Data + Start;
        Line.Length = End - Start;
        Input->LineNumber++;

        if (LineMatches(&Line) != Opt.Invert)
        {
            AnyMatch = TRUE;
            if (Opt.FileNamesOnly && Input->Name)
            {
                WriteName(Input->Name);
                WriteBytes("\r\n", 2);
                Input->Done = TRUE;
                return Next;
            }
            if (ShowNames && Input->Name)
            {
                WriteName(Input->Name);
                WriteBytes(":", 1);
            }
            if (Opt.LineNumbers)
            {
                WriteNumber(Input->LineNumber);
                WriteBytes(":", 1);
            }
            if (Opt.Offsets)
            {
                WriteNumber(Input->Offset);
                WriteBytes(":", 1);
            }
            WriteBytes(Data + Start, Next - Start);
        }
        Input->Offset += Next - Start;
        Start = Next;
    }

    return Start;
}

/* Reads more of hFile into a growing buffer; returns FALSE on a read error. *Read is 0 at the end. */
static BOOL
ReadMore(HANDLE hFile, PUCHAR *Buffer, SIZE_T *Capacity, SIZE_T Used, DWORD *Read)
{
    if (*Capacity - Used < 0x8000)
    {
        PUCHAR New;
        if (*Capacity > ((SIZE_T)-1) / 2)
            New = NULL;
        else
            New = HeapReAlloc(GetProcessHeap(), 0, *Buffer, *Capacity * 2);
        if (!New)
        {
            Message(IDS_OUT_OF_MEMORY);
            ExitProcess(Finish(2));
        }
        *Buffer = New;
        *Capacity *= 2;
    }

    if (!ReadFile(hFile, *Buffer + Used, (DWORD)min(*Capacity - Used, 0x10000), Read, NULL))
    {
        *Read = 0;
        /* The writer of a pipe went away: that is the end of the input */
        return GetLastError() == ERROR_BROKEN_PIPE;
    }
    return TRUE;
}

/* Reads a whole input; returns FALSE on a read error */
static BOOL
ReadAll(HANDLE hFile, PUCHAR *Data, SIZE_T *Size)
{
    SIZE_T Capacity = 0x10000, Used = 0;
    PUCHAR Buffer = Alloc(Capacity);
    DWORD Read;

    for (;;)
    {
        if (!ReadMore(hFile, &Buffer, &Capacity, Used, &Read))
        {
            HeapFree(GetProcessHeap(), 0, Buffer);
            return FALSE;
        }
        if (Read == 0)
            break;
        Used += Read;
    }

    *Data = Buffer;
    *Size = Used;
    return TRUE;
}

/* Searches an input line by line as it is read, so a pipe gives results before it ends.
 * Returns FALSE on a read error. */
static BOOL
SearchHandle(HANDLE hFile, PCWSTR Name)
{
    SEARCH_INPUT Input = {Name, 0, 0, FALSE};
    SIZE_T Capacity = 0x10000, Used = 0, Consumed;
    PUCHAR Buffer;
    DWORD Read;
    BOOL Result = TRUE;

    /* /P has to see the whole file before printing anything from it */
    if (Opt.SkipNonPrintable && Name)
    {
        if (!ReadAll(hFile, &Buffer, &Used))
            return FALSE;
        if (!HasNonPrintable(Buffer, Used))
            SearchLines(&Input, Buffer, Used, TRUE);
        HeapFree(GetProcessHeap(), 0, Buffer);
        return TRUE;
    }

    Buffer = Alloc(Capacity);
    while (!Input.Done)
    {
        if (!ReadMore(hFile, &Buffer, &Capacity, Used, &Read))
        {
            Result = FALSE;
            break;
        }
        if (Read == 0)
        {
            /* Unlike a file, standard input gets a line end after its last line */
            /* ReadMore left at least 0x8000 bytes free */
            if (!Name && Used && Buffer[Used - 1] != '\n')
            {
                Buffer[Used++] = '\r';
                Buffer[Used++] = '\n';
            }
            SearchLines(&Input, Buffer, Used, TRUE);
            break;
        }
        Used += Read;
        Consumed = SearchLines(&Input, Buffer, Used, FALSE);
        memmove(Buffer, Buffer + Consumed, Used - Consumed);
        Used -= Consumed;
    }

    HeapFree(GetProcessHeap(), 0, Buffer);
    return Result;
}

/* Opens and searches one file; returns FALSE if it cannot be opened or read */
static BOOL
SearchFile(PCWSTR Path, PCWSTR DisplayName)
{
    HANDLE hFile;
    BOOL Result;

    hFile = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;

    Result = SearchHandle(hFile, DisplayName);
    CloseHandle(hFile);
    return Result;
}

static BOOL
HasWildcards(PCWSTR s)
{
    return wcspbrk(s, L"*?") != NULL;
}

/* Dir + Name; Dir is empty or ends with a separator */
static PWSTR
JoinPath(PCWSTR Dir, PCWSTR Name)
{
    SIZE_T DirLength = wcslen(Dir);
    PWSTR Result = Alloc((DirLength + wcslen(Name) + 2) * sizeof(WCHAR));

    wcscpy(Result, Dir);
    if (DirLength && Dir[DirLength - 1] != L'\\' && Dir[DirLength - 1] != L'/' && Dir[DirLength - 1] != L':')
        wcscat(Result, L"\\");
    wcscat(Result, Name);
    return Result;
}

static BOOL
SkipFile(const WIN32_FIND_DATAW *Data)
{
    if (Data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return TRUE;
    if ((Data->dwFileAttributes & FILE_ATTRIBUTE_OFFLINE) && !Opt.Offline)
        return TRUE;
    return FALSE;
}

/* Searches the files that match Mask in one directory. Base is the directory used to open the
 * files (/D:), Dir the directory part shown in the output. With /S, subdirectories are searched
 * where they appear in the directory listing. Returns TRUE if any file matched the mask. */
static BOOL
SearchMask(PCWSTR Base, PCWSTR Dir, PCWSTR Mask)
{
    WIN32_FIND_DATAW Data;
    HANDLE hFind;
    PWSTR Spec, Shown, Path, *Matches = NULL;
    SIZE_T Count = 0, Capacity = 0, k;
    BOOL Any = FALSE;

    Shown = JoinPath(Dir, Mask);
    Spec = JoinPath(Base, Shown);
    hFind = FindFirstFileW(Spec, &Data);
    HeapFree(GetProcessHeap(), 0, Spec);
    HeapFree(GetProcessHeap(), 0, Shown);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (SkipFile(&Data))
                continue;
            Any = TRUE;
            if (!Opt.SubDirs)
            {
                Shown = JoinPath(Dir, Data.cFileName);
                Path = JoinPath(Base, Shown);
                if (!SearchFile(Path, Shown))
                    Message(IDS_CANNOT_OPEN, Shown);
                HeapFree(GetProcessHeap(), 0, Path);
                HeapFree(GetProcessHeap(), 0, Shown);
                continue;
            }
            if (Count == Capacity)
            {
                PWSTR *New;
                Capacity = Capacity ? Capacity * 2 : 16;
                New = Alloc(Capacity * sizeof(PWSTR));
                if (Matches)
                {
                    memcpy(New, Matches, Count * sizeof(PWSTR));
                    HeapFree(GetProcessHeap(), 0, Matches);
                }
                Matches = New;
            }
            Matches[Count++] = _wcsdup(Data.cFileName);
        } while (FindNextFileW(hFind, &Data));
        FindClose(hFind);
    }

    if (!Opt.SubDirs)
        return Any;

    /* Walk the directory in its own order: files that matched the mask, and subdirectories */
    Shown = JoinPath(Dir, L"*");
    Spec = JoinPath(Base, Shown);
    hFind = FindFirstFileW(Spec, &Data);
    HeapFree(GetProcessHeap(), 0, Spec);
    HeapFree(GetProcessHeap(), 0, Shown);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!wcscmp(Data.cFileName, L".") || !wcscmp(Data.cFileName, L".."))
                    continue;
                Shown = JoinPath(Dir, Data.cFileName);
                if (SearchMask(Base, Shown, Mask))
                    Any = TRUE;
                HeapFree(GetProcessHeap(), 0, Shown);
                continue;
            }
            for (k = 0; k < Count; k++)
            {
                if (Matches[k] && !wcscmp(Matches[k], Data.cFileName))
                {
                    Shown = JoinPath(Dir, Data.cFileName);
                    Path = JoinPath(Base, Shown);
                    if (!SearchFile(Path, Shown))
                        Message(IDS_CANNOT_OPEN, Shown);
                    HeapFree(GetProcessHeap(), 0, Path);
                    HeapFree(GetProcessHeap(), 0, Shown);
                    free(Matches[k]);
                    Matches[k] = NULL;
                    break;
                }
            }
        } while (FindNextFileW(hFind, &Data));
        FindClose(hFind);
    }

    for (k = 0; k < Count; k++)
        free(Matches[k]);
    if (Matches)
        HeapFree(GetProcessHeap(), 0, Matches);
    return Any;
}

/* Searches one file argument; Base is a /D: directory or an empty string */
static VOID
SearchSpec(PCWSTR Base, PCWSTR Spec)
{
    PWSTR Dir, Path, Slash;
    PCWSTR Mask, Sep;

    if (!Opt.SubDirs && !HasWildcards(Spec))
    {
        DWORD Attributes;
        Path = JoinPath(Base, Spec);
        Attributes = GetFileAttributesW(Path);
        /* A directory, or an offline file without /OFFLINE, is skipped without a message */
        if (Attributes != INVALID_FILE_ATTRIBUTES &&
            ((Attributes & FILE_ATTRIBUTE_DIRECTORY) ||
             ((Attributes & FILE_ATTRIBUTE_OFFLINE) && !Opt.Offline)))
            ;
        else if (!SearchFile(Path, Spec))
            Message(IDS_CANNOT_OPEN, Spec);
        HeapFree(GetProcessHeap(), 0, Path);
        return;
    }

    /* Split into the directory part (kept as typed) and the file mask */
    Slash = wcsrchr(Spec, L'\\');
    Sep = wcsrchr(Spec, L'/');
    if (Sep && (!Slash || Sep > Slash))
        Slash = (PWSTR)Sep;
    if (!Slash && Spec[0] && Spec[1] == L':')
        Slash = (PWSTR)&Spec[1];

    if (Slash)
    {
        SIZE_T DirLength = Slash - Spec + 1;
        Dir = Alloc((DirLength + 1) * sizeof(WCHAR));
        memcpy(Dir, Spec, DirLength * sizeof(WCHAR));
        Mask = Slash + 1;
    }
    else
    {
        Dir = Alloc(sizeof(WCHAR));
        Mask = Spec;
    }

    /* With /S a file argument that matches nothing is not an error */
    if (!SearchMask(Base, Dir, Mask) && !Opt.SubDirs)
        Message(IDS_CANNOT_OPEN, Spec);

    HeapFree(GetProcessHeap(), 0, Dir);
}

/* Command line *************************************************************/

static VOID
AddName(PNAME **Tail, PWSTR Name)
{
    PNAME n = Alloc(sizeof(NAME));
    n->Name = Name;
    **Tail = n;
    *Tail = &n->Next;
}

static VOID
AddPattern(PCSTR Text, SIZE_T Length)
{
    PPATTERN p = Alloc(sizeof(PATTERN));

    p->Text = Alloc(Length + 1);
    memcpy(p->Text, Text, Length);
    p->Length = Length;
    *PatternTail = p;
    PatternTail = &p->Next;
}

static PSTR
ToAnsi(PCWSTR Text)
{
    INT Length = WideCharToMultiByte(CP_ACP, 0, Text, -1, NULL, 0, NULL, NULL);
    PSTR Bytes = Alloc(Length + 1);

    WideCharToMultiByte(CP_ACP, 0, Text, -1, Bytes, Length, NULL, NULL);
    return Bytes;
}

/* Splits the plain search argument at spaces */
static VOID
AddSpaceSeparated(PCWSTR Text)
{
    PSTR Bytes = ToAnsi(Text), s, e;

    for (s = Bytes; *s; s = e)
    {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        for (e = s; *e && *e != ' '; e++)
            ;
        AddPattern(s, e - s);
    }
    HeapFree(GetProcessHeap(), 0, Bytes);
}

static BOOL
ReadListFile(PCWSTR Name, PUCHAR *Data, SIZE_T *Size)
{
    HANDLE hFile;

    BOOL Result;

    if (!wcscmp(Name, L"/"))
        return ReadAll(GetStdHandle(STD_INPUT_HANDLE), Data, Size);

    hFile = CreateFileW(Name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    Result = ReadAll(hFile, Data, Size);
    CloseHandle(hFile);
    return Result;
}

/* Calls Callback for every non-empty line of a list file (/F: and /G:) */
static BOOL
ForEachLine(PCWSTR Name, VOID (*Callback)(PCSTR, SIZE_T))
{
    PUCHAR Data;
    SIZE_T Size, Start = 0;

    if (!ReadListFile(Name, &Data, &Size))
        return FALSE;

    while (Start < Size)
    {
        SIZE_T End = Start, Length;
        while (End < Size && Data[End] != '\n')
            End++;
        Length = End - Start;
        if (Length && Data[Start + Length - 1] == '\r')
            Length--;
        if (Length)
            Callback((PCSTR)Data + Start, Length);
        Start = End + 1;
    }

    HeapFree(GetProcessHeap(), 0, Data);
    return TRUE;
}

static ULONG FileListCount = 0;

static VOID
AddFileFromList(PCSTR Text, SIZE_T Length)
{
    INT Wide = MultiByteToWideChar(CP_ACP, 0, Text, (INT)Length, NULL, 0);
    PWSTR Name = Alloc((Wide + 1) * sizeof(WCHAR));

    MultiByteToWideChar(CP_ACP, 0, Text, (INT)Length, Name, Wide);
    AddName(&FileTail, Name);
    FileListCount++;
}

static VOID
AddDirs(PCWSTR List)
{
    PCWSTR s = List, e;

    while (*s)
    {
        PWSTR Dir;
        e = wcschr(s, L';');
        if (!e)
            e = s + wcslen(s);
        if (e > s)
        {
            Dir = Alloc((e - s + 1) * sizeof(WCHAR));
            memcpy(Dir, s, (e - s) * sizeof(WCHAR));
            AddName(&DirTail, Dir);
        }
        s = *e ? e + 1 : e;
    }
}

/* Without /L or /R the first search string decides: an unescaped regular expression character,
 * or an escaped special character, makes all strings regular expressions */
static BOOL
ContainsRegexMeta(const PATTERN *p)
{
    SIZE_T i;

    for (i = 0; i < p->Length; i++)
    {
        switch (p->Text[i])
        {
            case '.': case '*': case '[':
                return TRUE;
            case '^':
                if (i == 0)
                    return TRUE;
                break;
            case '$':
                if (i == p->Length - 1)
                    return TRUE;
                break;
            case '\\':
                if (i + 1 < p->Length && strchr(".*[]^$\\<>", p->Text[i + 1]))
                    return TRUE;
                break;
        }
    }
    return FALSE;
}

int wmain(int argc, WCHAR *argv[])
{
    PCWSTR StringsFile = NULL, FilesFile = NULL;
    PCWSTR *Plain;
    BOOL HaveCString = FALSE;
    INT i, PlainCount = 0, Type = TYPE_DEFAULT;
    CONSOLE_SCREEN_BUFFER_INFO Info;
    DWORD Mode;
    PPATTERN p, *pp;
    PNAME n, d;

    ConInitStdStreams();
    InitTables();

    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    OutIsConsole = GetConsoleMode(hOut, &Mode);
    if (OutIsConsole && GetConsoleScreenBufferInfo(hOut, &Info))
        DefaultAttributes = Info.wAttributes;
    ErrIsConsole = GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &Mode);

    Plain = Alloc(argc * sizeof(PCWSTR));

    for (i = 1; i < argc; i++)
    {
        PCWSTR a = argv[i];

        /* Switches are only recognized before the first other argument */
        if (PlainCount == 0 && (a[0] == L'/' || (a[0] == L'-' && a[1])))
        {
            PCWSTR s = a + 1;

            if (!wcscmp(s, L"?"))
            {
                ConResPuts(StdOut, IDS_USAGE);
                return Finish(0);
            }
            if (!_wcsicmp(s, L"OFF") || !_wcsicmp(s, L"OFFLINE"))
            {
                Opt.Offline = TRUE;
                continue;
            }

            while (*s)
            {
                WCHAR c = towupper(*s);
                if (s[1] == L':' && wcschr(L"CGFDA", c))
                {
                    PCWSTR Value = s + 2;
                    if (!*Value)
                    {
                        Message(IDS_ARGUMENT_MISSING, *s);
                        return Finish(2);
                    }
                    switch (c)
                    {
                        case L'C':
                        {
                            PSTR Bytes = ToAnsi(Value);
                            AddPattern(Bytes, strlen(Bytes));
                            HeapFree(GetProcessHeap(), 0, Bytes);
                            HaveCString = TRUE;
                            break;
                        }
                        case L'G':
                            StringsFile = Value;
                            break;
                        case L'F':
                            FilesFile = Value;
                            break;
                        case L'D':
                            AddDirs(Value);
                            break;
                        case L'A':
                            if (!iswxdigit(*Value))
                            {
                                /* Not a color: the rest is read as switches */
                                s = Value;
                                continue;
                            }
                            Opt.Color = (WORD)wcstoul(Value, NULL, 16);
                            Opt.UseColor = TRUE;
                            break;
                    }
                    break;
                }
                switch (c)
                {
                    case L'B': Opt.Begin = TRUE; break;
                    case L'E': Opt.End = TRUE; break;
                    case L'L': Type = TYPE_LITERAL; break;
                    case L'R': Type = TYPE_REGEX; break;
                    case L'S': Opt.SubDirs = TRUE; break;
                    case L'I': Opt.IgnoreCase = TRUE; break;
                    case L'X': Opt.Exact = TRUE; break;
                    case L'V': Opt.Invert = TRUE; break;
                    case L'N': Opt.LineNumbers = TRUE; break;
                    case L'M': Opt.FileNamesOnly = TRUE; break;
                    case L'O': Opt.Offsets = TRUE; break;
                    case L'P': Opt.SkipNonPrintable = TRUE; break;
                    default:
                        Message(IDS_SWITCH_IGNORED, *s);
                        break;
                }
                s++;
            }
            continue;
        }

        Plain[PlainCount++] = a;
    }

    /* Without /C: or /G: the first plain argument holds the search strings */
    i = 0;
    if (!HaveCString && !StringsFile)
    {
        if (PlainCount == 0)
        {
            Message(IDS_BAD_COMMAND_LINE);
            return Finish(2);
        }
        AddSpaceSeparated(Plain[i++]);
    }
    for (; i < PlainCount; i++)
        AddName(&FileTail, _wcsdup(Plain[i]));

    if (StringsFile && !ForEachLine(StringsFile, AddPattern))
    {
        Message(IDS_CANNOT_READ_STRINGS, StringsFile);
        return Finish(2);
    }
    if (FilesFile && !ForEachLine(FilesFile, AddFileFromList))
    {
        Message(IDS_CANNOT_READ_FILES, FilesFile);
        return Finish(2);
    }

    /* /C: strings are literal unless /R is given */
    if (Type == TYPE_DEFAULT && Patterns)
        Type = (!HaveCString && ContainsRegexMeta(Patterns)) ? TYPE_REGEX : TYPE_LITERAL;

    /* Invalid regular expressions are dropped */
    for (pp = &Patterns; (p = *pp) != NULL; )
    {
        if (Type == TYPE_REGEX)
        {
            p->Regex = TRUE;
            if (!CompileRegex(p))
            {
                *pp = p->Next;
                continue;
            }
        }
        else
        {
            SIZE_T k, Out = 0;
            /* A literal string keeps its backslashes, except \\ and \" which stand for \ and " */
            for (k = 0; k < p->Length; k++)
            {
                if (p->Text[k] == '\\' && k + 1 < p->Length && (p->Text[k + 1] == '\\' || p->Text[k + 1] == '"'))
                    k++;
                p->Text[Out++] = Opt.IgnoreCase ? (CHAR)UpperTable[(UCHAR)p->Text[k]] : p->Text[k];
            }
            p->Length = Out;
        }
        pp = &p->Next;
    }

    if (!Patterns)
    {
        Message(IDS_NO_SEARCH_STRINGS);
        return Finish(2);
    }

    if (!Files)
    {
        SearchHandle(GetStdHandle(STD_INPUT_HANDLE), NULL);
        return Finish(AnyMatch ? 0 : 1);
    }

    ShowNames = (Files->Next != NULL) || Opt.SubDirs || HasWildcards(Files->Name) || FileListCount;

    if (Dirs)
    {
        for (d = Dirs; d; d = d->Next)
        {
            WriteBytes("  ", 2);
            WriteName(d->Name);
            WriteBytes(":\r\n", 3);
            for (n = Files; n; n = n->Next)
                SearchSpec(d->Name, n->Name);
        }
    }
    else
    {
        for (n = Files; n; n = n->Next)
            SearchSpec(L"", n->Name);
    }

    return Finish(AnyMatch ? 0 : 1);
}

/* EOF */
