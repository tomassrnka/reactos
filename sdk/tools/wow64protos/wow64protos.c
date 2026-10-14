/*
 * PROJECT:     ReactOS host tools
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Describe the arguments of system call prototypes for the WoW64 syscall tables
 *
 * Usage: wow64protos [-Dname[=value]]... [-Uname]... -t types.txt (-c calls.txt | -s table.h)...
 *                    -o out.csv header...
 *
 * Reads the prototypes of the calls named in calls.txt (one Nt name per line) or in the
 * SVC_ rows of a service table (-s) from the headers, and writes one CSV row per call and
 * per argument, with the size of every argument on i386 and amd64.
 *
 * The preprocessor subset (#if, #ifdef, #ifndef, #elif, #else, #endif, #define, #undef) is
 * evaluated with three values: a condition that cannot be decided keeps its prototype and is
 * reported. -Dname[=value] defines a macro. -Uname declares a macro whose definedness is
 * undecided (for example _WIN64, so that both architectures are described). A macro that a
 * header defines or undefines inside an undecided branch becomes undecided as well.
 *
 * Output rows (no field contains a comma, a semicolon or a bracket):
 *   P,call,file:line,argcount,variants,condition
 *       variants is above 1 when the headers declare the call in several ways, or when a
 *       condition inside the parameter list is undecided
 *   A,call,index,name,type,kind,size32,size64,slots32,slots64,abi64
 *   M,call,reason
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64
#define MAX_ARGS 32
#define MAX_EXPAND_DEPTH 16

enum { TK_IDENT, TK_NUMBER, TK_PUNCT, TK_STRING };
enum { ST_FALSE, ST_TRUE, ST_UNKNOWN };

typedef struct
{
    const char *Text;
    int Kind;
    int Line;
    int State;
    int Region;
    const char *Cond;
} TOKEN;

typedef struct
{
    char *Name;
    char *Value;
    int FunctionLike;
    int Undecided;
    int ValueUnknown;
} MACRO;

typedef struct
{
    char *Name;
    char *Kind;
    long Size32;
    long Size64;
} TYPEDEF_ENTRY;

typedef struct
{
    char *Name;
    char *Type;
    const char *Kind;
    long Size32;
    long Size64;
    int Resolved;
    const char *Abi64;
} ARGUMENT;

typedef struct
{
    char *Call;
    const char *File;
    int Line;
    int State;
    const char *Cond;
    int ArgCount;
    int ParamCond;
    ARGUMENT Args[MAX_ARGS];
    char *Signature;
} PROTOTYPE;

typedef struct
{
    char *Data;
    size_t Length;
    size_t Cap;
} BUFFER;

static TOKEN *g_Tokens;
static size_t g_TokenCount, g_TokenCap;
static MACRO *g_Macros;
static size_t g_MacroCount, g_MacroCap;
static TYPEDEF_ENTRY *g_Types;
static size_t g_TypeCount, g_TypeCap;
static char **g_Calls;
static size_t g_CallCount, g_CallCap;
static PROTOTYPE *g_Protos;
static size_t g_ProtoCount, g_ProtoCap;
static int g_Quiet;
/* Counts the conditional directives; tokens of one region have no directive between them */
static int g_Region;

static void *
Xrealloc(void *Ptr, size_t Size)
{
    Ptr = realloc(Ptr, Size ? Size : 1);
    if (!Ptr)
    {
        fprintf(stderr, "wow64protos: out of memory\n");
        exit(1);
    }
    return Ptr;
}

static char *
Xstrndup(const char *Text, size_t Length)
{
    char *Copy = Xrealloc(NULL, Length + 1);
    memcpy(Copy, Text, Length);
    Copy[Length] = 0;
    return Copy;
}

static char *
Xstrdup(const char *Text)
{
    return Xstrndup(Text, strlen(Text));
}

static void
BufferAppend(BUFFER *Buffer, const char *Text, size_t Length)
{
    if (Buffer->Length + Length + 1 > Buffer->Cap)
    {
        Buffer->Cap = (Buffer->Length + Length + 1) * 2;
        Buffer->Data = Xrealloc(Buffer->Data, Buffer->Cap);
    }
    memcpy(Buffer->Data + Buffer->Length, Text, Length);
    Buffer->Length += Length;
    Buffer->Data[Buffer->Length] = 0;
}

static void
BufferAppendText(BUFFER *Buffer, const char *Text)
{
    BufferAppend(Buffer, Text, strlen(Text));
}

static char *
BufferTake(BUFFER *Buffer)
{
    if (!Buffer->Data)
        Buffer->Data = Xstrdup("");
    return Buffer->Data;
}

static char *
ReadFileText(const char *Path)
{
    FILE *File = fopen(Path, "rb");
    char *Buffer;
    size_t Size = 0, Cap = 65536, Got;

    if (!File)
    {
        fprintf(stderr, "wow64protos: cannot open %s\n", Path);
        exit(1);
    }
    Buffer = Xrealloc(NULL, Cap);
    while ((Got = fread(Buffer + Size, 1, Cap - Size - 1, File)) > 0)
    {
        Size += Got;
        if (Cap - Size < 2)
        {
            Cap *= 2;
            Buffer = Xrealloc(Buffer, Cap);
        }
    }
    fclose(File);
    Buffer[Size] = 0;
    return Buffer;
}

static int
IsIdentStart(int c)
{
    return isalpha(c) || c == '_';
}

static int
IsIdentChar(int c)
{
    return isalnum(c) || c == '_';
}

/* The identifier at Text, whatever its length */
static char *
ReadName(const char *Text)
{
    size_t Length = 0;

    while (IsIdentChar((unsigned char)Text[Length]))
        Length++;
    return Xstrndup(Text, Length);
}

/* Remove backslash-newline as the preprocessor does before it looks at comments. The newlines
 * come back after the next line, so the line numbers of the rest of the file stay valid. */
static void
SpliceLines(char *Text)
{
    char *In = Text, *Out = Text;
    int Pending = 0;

    while (*In)
    {
        if (*In == '\\' && (In[1] == '\n' || (In[1] == '\r' && In[2] == '\n')))
        {
            In += In[1] == '\n' ? 2 : 3;
            Pending++;
        }
        else if (*In == '\n')
        {
            *Out++ = *In++;
            for (; Pending; Pending--)
                *Out++ = '\n';
        }
        else
        {
            *Out++ = *In++;
        }
    }
    for (; Pending; Pending--)
        *Out++ = '\n';
    *Out = 0;
}

/* Replace comments by spaces and keep every newline, so that line numbers stay valid */
static void
StripComments(char *Text)
{
    char *In = Text;

    while (*In)
    {
        if (*In == '"' || *In == '\'')
        {
            char Quote = *In++;
            while (*In && *In != Quote && *In != '\n')
            {
                if (*In == '\\' && In[1] && In[1] != '\n')
                    In++;
                In++;
            }
            if (*In == Quote)
                In++;
        }
        else if (In[0] == '/' && In[1] == '*')
        {
            In[0] = In[1] = ' ';
            In += 2;
            while (*In && !(In[0] == '*' && In[1] == '/'))
            {
                if (*In != '\n')
                    *In = ' ';
                In++;
            }
            if (*In)
            {
                In[0] = In[1] = ' ';
                In += 2;
            }
        }
        else if (In[0] == '/' && In[1] == '/')
        {
            while (*In && *In != '\n')
                *In++ = ' ';
        }
        else
        {
            In++;
        }
    }
}

static MACRO *
FindMacro(const char *Name)
{
    size_t i;

    for (i = 0; i < g_MacroCount; i++)
    {
        if (!strcmp(g_Macros[i].Name, Name))
            return &g_Macros[i];
    }
    return NULL;
}

/* In an undecided branch a definition may or may not happen: the macro keeps no known value */
static void
DefineMacro(const char *Name, const char *Value, int FunctionLike, int Undecided)
{
    MACRO *Macro = FindMacro(Name);

    if (!Macro)
    {
        if (g_MacroCount == g_MacroCap)
        {
            g_MacroCap = g_MacroCap ? g_MacroCap * 2 : 64;
            g_Macros = Xrealloc(g_Macros, g_MacroCap * sizeof(*g_Macros));
        }
        Macro = &g_Macros[g_MacroCount++];
        Macro->Name = Xstrdup(Name);
        Macro->Value = NULL;
        Macro->Undecided = Undecided;
        Macro->ValueUnknown = 0;
    }
    else if (Undecided)
    {
        Macro->ValueUnknown = 1;
    }
    else
    {
        Macro->Undecided = 0;
        Macro->ValueUnknown = 0;
    }
    free(Macro->Value);
    Macro->Value = Xstrdup(Value);
    Macro->FunctionLike = FunctionLike;
}

static void
UndefMacro(const char *Name, int Undecided)
{
    size_t i;

    for (i = 0; i < g_MacroCount; i++)
    {
        if (!strcmp(g_Macros[i].Name, Name))
        {
            if (Undecided)
            {
                g_Macros[i].Undecided = 1;
                return;
            }
            free(g_Macros[i].Name);
            free(g_Macros[i].Value);
            g_Macros[i] = g_Macros[--g_MacroCount];
            return;
        }
    }
}

/* #if expressions: macros are replaced by their text first, then C precedence applies */
typedef struct
{
    int Known;
    long long Value;
} VALUE;

typedef struct
{
    const char *Pos;
} EXPR;

static void
SkipSpace(EXPR *Expr)
{
    while (*Expr->Pos == ' ' || *Expr->Pos == '\t' || *Expr->Pos == '\r')
        Expr->Pos++;
}

static void
ExpandInto(BUFFER *Out, const char *Text, int Depth, const char *Active[MAX_EXPAND_DEPTH])
{
    const char *p = Text;

    while (*p)
    {
        if (isdigit((unsigned char)*p))
        {
            const char *Start = p;
            while (IsIdentChar((unsigned char)*p) || *p == '.')
                p++;
            BufferAppend(Out, Start, p - Start);
        }
        else if (IsIdentStart((unsigned char)*p))
        {
            const char *Start = p;
            size_t Length;
            char *Name;
            MACRO *Macro;
            int i, Recursive = 0;

            while (IsIdentChar((unsigned char)*p))
                p++;
            Length = p - Start;
            Name = Xstrndup(Start, Length);
            if (!strcmp(Name, "defined"))
            {
                /* The operand of defined is a name, not an expansion */
                const char *q = p;
                BufferAppend(Out, Start, Length);
                while (*q == ' ' || *q == '\t')
                    q++;
                if (*q == '(')
                    q++;
                while (*q == ' ' || *q == '\t')
                    q++;
                BufferAppend(Out, p, q - p);
                p = q;
                while (IsIdentChar((unsigned char)*p))
                    p++;
                BufferAppend(Out, q, p - q);
                free(Name);
                continue;
            }
            Macro = FindMacro(Name);
            for (i = 0; Macro && i < Depth; i++)
            {
                if (Active[i] == Macro->Name)
                    Recursive = 1;
            }
            if (Macro && !Macro->FunctionLike && !Macro->Undecided && !Macro->ValueUnknown && !Recursive &&
                Depth < MAX_EXPAND_DEPTH)
            {
                Active[Depth] = Macro->Name;
                BufferAppend(Out, " ", 1);
                ExpandInto(Out, Macro->Value, Depth + 1, Active);
                BufferAppend(Out, " ", 1);
            }
            else
            {
                BufferAppend(Out, Start, Length);
            }
            free(Name);
        }
        else
        {
            BufferAppend(Out, p, 1);
            p++;
        }
    }
}

static VALUE EvalTernary(EXPR *Expr);

static VALUE
EvalUnary(EXPR *Expr)
{
    VALUE Result = {0, 0};

    SkipSpace(Expr);
    if (*Expr->Pos == '(')
    {
        Expr->Pos++;
        Result = EvalTernary(Expr);
        SkipSpace(Expr);
        if (*Expr->Pos == ')')
            Expr->Pos++;
        else
            Result.Known = 0;
    }
    else if (*Expr->Pos == '!' && Expr->Pos[1] != '=')
    {
        Expr->Pos++;
        Result = EvalUnary(Expr);
        Result.Value = !Result.Value;
    }
    else if (*Expr->Pos == '~')
    {
        Expr->Pos++;
        Result = EvalUnary(Expr);
        Result.Value = ~Result.Value;
    }
    else if (*Expr->Pos == '-')
    {
        Expr->Pos++;
        Result = EvalUnary(Expr);
        Result.Value = (long long)(0 - (unsigned long long)Result.Value);
    }
    else if (*Expr->Pos == '+')
    {
        Expr->Pos++;
        Result = EvalUnary(Expr);
    }
    else if (isdigit((unsigned char)*Expr->Pos))
    {
        unsigned long long Value = 0;
        int Base = 10, Overflow = 0, Digits = 0;

        if (Expr->Pos[0] == '0' && (Expr->Pos[1] == 'x' || Expr->Pos[1] == 'X'))
        {
            Base = 16;
            Expr->Pos += 2;
        }
        else if (Expr->Pos[0] == '0')
        {
            Base = 8;
        }
        for (;; Expr->Pos++)
        {
            int c = (unsigned char)*Expr->Pos, Digit;
            if (isdigit(c))
                Digit = c - '0';
            else if (Base == 16 && isxdigit(c))
                Digit = tolower(c) - 'a' + 10;
            else
                break;
            if (Digit >= Base || Value > (0x7fffffffffffffffULL - Digit) / Base)
                Overflow = 1;
            else
                Value = Value * Base + Digit;
            Digits++;
        }
        Result.Known = !Overflow && (Digits || Base == 8);
        while (*Expr->Pos == 'u' || *Expr->Pos == 'U' || *Expr->Pos == 'l' || *Expr->Pos == 'L')
        {
            /* unsigned arithmetic is not modelled: the value is unknown */
            if (*Expr->Pos == 'u' || *Expr->Pos == 'U')
                Result.Known = 0;
            Expr->Pos++;
        }
        Result.Value = (long long)Value;
        if (IsIdentChar((unsigned char)*Expr->Pos))
        {
            Result.Known = 0;
            while (IsIdentChar((unsigned char)*Expr->Pos))
                Expr->Pos++;
        }
    }
    else if (IsIdentStart((unsigned char)*Expr->Pos))
    {
        char *Name = ReadName(Expr->Pos);

        Expr->Pos += strlen(Name);
        if (!strcmp(Name, "defined"))
        {
            MACRO *Macro;
            int Paren;

            free(Name);
            SkipSpace(Expr);
            Paren = (*Expr->Pos == '(');
            if (Paren)
                Expr->Pos++;
            SkipSpace(Expr);
            Name = ReadName(Expr->Pos);
            Expr->Pos += strlen(Name);
            SkipSpace(Expr);
            if (Paren && *Expr->Pos == ')')
                Expr->Pos++;
            Macro = FindMacro(Name);
            Result.Known = !Macro || !Macro->Undecided;
            Result.Value = Macro != NULL;
        }
        /* any other name is a macro without a known value */
        free(Name);
    }
    else if (*Expr->Pos == '\'')
    {
        Expr->Pos++;
        while (*Expr->Pos && *Expr->Pos != '\'')
            Expr->Pos++;
        if (*Expr->Pos)
            Expr->Pos++;
    }
    return Result;
}

/* Binary operators from the loosest (1) to the tightest (10) */
static int
BinaryOperator(EXPR *Expr, int *Precedence, int *Length)
{
    const char *p;

    SkipSpace(Expr);
    p = Expr->Pos;
    *Length = 1;
    if (p[0] == '|' && p[1] == '|') { *Length = 2; *Precedence = 1; return 'o'; }
    if (p[0] == '&' && p[1] == '&') { *Length = 2; *Precedence = 2; return 'a'; }
    if (p[0] == '|') { *Precedence = 3; return '|'; }
    if (p[0] == '^') { *Precedence = 4; return '^'; }
    if (p[0] == '&') { *Precedence = 5; return '&'; }
    if (p[0] == '=' && p[1] == '=') { *Length = 2; *Precedence = 6; return 'e'; }
    if (p[0] == '!' && p[1] == '=') { *Length = 2; *Precedence = 6; return 'n'; }
    if (p[0] == '<' && p[1] == '<') { *Length = 2; *Precedence = 8; return 'L'; }
    if (p[0] == '>' && p[1] == '>') { *Length = 2; *Precedence = 8; return 'R'; }
    if (p[0] == '<' && p[1] == '=') { *Length = 2; *Precedence = 7; return 'l'; }
    if (p[0] == '>' && p[1] == '=') { *Length = 2; *Precedence = 7; return 'g'; }
    if (p[0] == '<') { *Precedence = 7; return '<'; }
    if (p[0] == '>') { *Precedence = 7; return '>'; }
    if (p[0] == '+') { *Precedence = 9; return '+'; }
    if (p[0] == '-') { *Precedence = 9; return '-'; }
    if (p[0] == '*') { *Precedence = 10; return '*'; }
    if (p[0] == '/') { *Precedence = 10; return '/'; }
    if (p[0] == '%') { *Precedence = 10; return '%'; }
    return 0;
}

static VALUE
Combine2(int Op, VALUE A, VALUE B)
{
    VALUE R = {0, 0};
    unsigned long long UA = (unsigned long long)A.Value, UB = (unsigned long long)B.Value;

    if (Op == 'o')
    {
        if ((A.Known && A.Value) || (B.Known && B.Value))
        {
            R.Known = 1;
            R.Value = 1;
        }
        else if (A.Known && B.Known)
        {
            R.Known = 1;
        }
        return R;
    }
    if (Op == 'a')
    {
        if ((A.Known && !A.Value) || (B.Known && !B.Value))
        {
            R.Known = 1;
        }
        else if (A.Known && B.Known)
        {
            R.Known = 1;
            R.Value = 1;
        }
        return R;
    }
    if (!A.Known || !B.Known)
        return R;
    R.Known = 1;
    switch (Op)
    {
        case '|': R.Value = A.Value | B.Value; break;
        case '^': R.Value = A.Value ^ B.Value; break;
        case '&': R.Value = A.Value & B.Value; break;
        case 'e': R.Value = A.Value == B.Value; break;
        case 'n': R.Value = A.Value != B.Value; break;
        case 'l': R.Value = A.Value <= B.Value; break;
        case 'g': R.Value = A.Value >= B.Value; break;
        case '<': R.Value = A.Value < B.Value; break;
        case '>': R.Value = A.Value > B.Value; break;
        case '+': R.Value = (long long)(UA + UB); break;
        case '-': R.Value = (long long)(UA - UB); break;
        case '*': R.Value = (long long)(UA * UB); break;
        case 'L':
        case 'R':
            if (B.Value < 0 || B.Value > 62 || A.Value < 0)
                R.Known = 0;
            else
                R.Value = Op == 'L' ? (long long)(UA << B.Value) : A.Value >> B.Value;
            break;
        default:
            if (!B.Value || (A.Value == (-0x7fffffffffffffffLL - 1) && B.Value == -1))
                R.Known = 0;
            else
                R.Value = Op == '/' ? A.Value / B.Value : A.Value % B.Value;
            break;
    }
    return R;
}

static VALUE
EvalBinary(EXPR *Expr, int MinPrecedence)
{
    VALUE Left = EvalUnary(Expr), Right;

    for (;;)
    {
        int Precedence = 0, Length, Op = BinaryOperator(Expr, &Precedence, &Length);

        if (!Op || Precedence < MinPrecedence)
            return Left;
        Expr->Pos += Length;
        Right = EvalBinary(Expr, Precedence + 1);
        Left = Combine2(Op, Left, Right);
    }
}

static VALUE
EvalTernary(EXPR *Expr)
{
    VALUE Condition = EvalBinary(Expr, 1), First, Second;

    SkipSpace(Expr);
    if (*Expr->Pos != '?')
        return Condition;
    Expr->Pos++;
    First = EvalTernary(Expr);
    SkipSpace(Expr);
    if (*Expr->Pos != ':')
    {
        First.Known = 0;
        return First;
    }
    Expr->Pos++;
    Second = EvalTernary(Expr);
    /* The result has the common type of both arms, so an arm of unknown type makes the result unknown */
    if (!First.Known || !Second.Known)
    {
        First.Known = 0;
        return First;
    }
    if (Condition.Known)
        return Condition.Value ? First : Second;
    if (First.Value == Second.Value)
        return First;
    First.Known = 0;
    return First;
}

static VALUE
EvalText(const char *Text)
{
    BUFFER Expanded = {NULL, 0, 0};
    const char *Active[MAX_EXPAND_DEPTH];
    EXPR Expr;
    VALUE Result;

    ExpandInto(&Expanded, Text, 0, Active);
    Expr.Pos = BufferTake(&Expanded);
    Result = EvalTernary(&Expr);
    SkipSpace(&Expr);
    if (*Expr.Pos)
        Result.Known = 0;
    free(Expanded.Data);
    return Result;
}

static void
AddToken(const char *Text, size_t Length, int Kind, int Line, int State, const char *Cond)
{
    TOKEN *Token;

    if (g_TokenCount == g_TokenCap)
    {
        g_TokenCap = g_TokenCap ? g_TokenCap * 2 : 4096;
        g_Tokens = Xrealloc(g_Tokens, g_TokenCap * sizeof(*g_Tokens));
    }
    Token = &g_Tokens[g_TokenCount++];
    Token->Text = Xstrndup(Text, Length);
    Token->Kind = Kind;
    Token->Line = Line;
    Token->State = State;
    Token->Region = g_Region;
    Token->Cond = Cond;
}

typedef struct
{
    int State;
    int ParentState;
    int Taken;
    int Unknown;
    const char *OwnCond;
} COND_FRAME;

/* A condition text is for the report only: no comma, semicolon or bracket */
static char *
CleanCond(const char *Text)
{
    char *Out = Xstrdup(Text), *p;

    while (*Out == ' ' || *Out == '\t')
        memmove(Out, Out + 1, strlen(Out));
    for (p = Out; *p; p++)
    {
        if (*p == ',' || *p == ';' || *p == '[' || *p == ']' || *p == '\r' || *p == '\t')
            *p = ' ';
    }
    while (p > Out && p[-1] == ' ')
        *--p = 0;
    return Out;
}

static int
Combine(int Parent, int Branch)
{
    if (Parent == ST_FALSE || Branch == ST_FALSE)
        return ST_FALSE;
    if (Parent == ST_UNKNOWN || Branch == ST_UNKNOWN)
        return ST_UNKNOWN;
    return ST_TRUE;
}

static int
CondState(const char *Expression)
{
    VALUE Value = EvalText(Expression);

    if (!Value.Known)
        return ST_UNKNOWN;
    return Value.Value != 0 ? ST_TRUE : ST_FALSE;
}

static void
HandleDirective(const char *Line, COND_FRAME *Stack, int *Depth)
{
    char *Word, *Name;
    BUFFER Text = {NULL, 0, 0};
    const char *Rest;
    int State;

    Line++;
    while (*Line == ' ' || *Line == '\t')
        Line++;
    Word = ReadName(Line);
    Rest = Line + strlen(Word);
    while (*Rest == ' ' || *Rest == '\t')
        Rest++;
    State = *Depth ? Stack[*Depth - 1].State : ST_TRUE;
    Name = ReadName(Rest);
    if (!strcmp(Word, "if") || !strcmp(Word, "ifdef") || !strcmp(Word, "ifndef") || !strcmp(Word, "elif") ||
        !strcmp(Word, "else") || !strcmp(Word, "endif"))
        g_Region++;

    if (!strcmp(Word, "if") || !strcmp(Word, "ifdef") || !strcmp(Word, "ifndef"))
    {
        COND_FRAME *Frame;
        int Branch;

        if (*Depth >= MAX_DEPTH)
        {
            fprintf(stderr, "wow64protos: conditional nesting too deep\n");
            exit(1);
        }
        if (!strcmp(Word, "if"))
        {
            Branch = CondState(Rest);
            BufferAppendText(&Text, Rest);
        }
        else
        {
            int Negate = !strcmp(Word, "ifndef");
            MACRO *Macro = FindMacro(Name);
            if (Macro && Macro->Undecided)
                Branch = ST_UNKNOWN;
            else
                Branch = (Macro != NULL) != Negate ? ST_TRUE : ST_FALSE;
            BufferAppendText(&Text, Negate ? "!defined(" : "defined(");
            BufferAppendText(&Text, Name);
            BufferAppendText(&Text, ")");
        }
        Frame = &Stack[(*Depth)++];
        Frame->ParentState = State;
        Frame->State = Combine(State, Branch);
        Frame->Taken = Branch == ST_TRUE || State == ST_FALSE;
        Frame->Unknown = Branch == ST_UNKNOWN;
        Frame->OwnCond = CleanCond(BufferTake(&Text));
    }
    else if (!strcmp(Word, "elif") || !strcmp(Word, "else"))
    {
        COND_FRAME *Frame;
        int Branch;

        if (!*Depth)
        {
            free(Text.Data);
            free(Word);
            free(Name);
            return;
        }
        Frame = &Stack[*Depth - 1];
        BufferAppendText(&Text, "!(");
        BufferAppendText(&Text, Frame->OwnCond);
        BufferAppendText(&Text, ")");
        if (Word[2] == 's')
        {
            Branch = Frame->Taken ? ST_FALSE : (Frame->Unknown ? ST_UNKNOWN : ST_TRUE);
        }
        else
        {
            Branch = Frame->Taken ? ST_FALSE : CondState(Rest);
            if (Frame->Unknown && Branch == ST_TRUE)
                Branch = ST_UNKNOWN;
            BufferAppendText(&Text, " && ");
            BufferAppendText(&Text, Rest);
        }
        Frame->State = Combine(Frame->ParentState, Branch);
        if (Branch == ST_TRUE)
            Frame->Taken = 1;
        else if (Branch == ST_UNKNOWN)
            Frame->Unknown = 1;
        Frame->OwnCond = CleanCond(BufferTake(&Text));
    }
    else if (!strcmp(Word, "endif"))
    {
        if (*Depth)
            (*Depth)--;
    }
    else if (State != ST_FALSE && !strcmp(Word, "define") && Name[0])
    {
        DefineMacro(Name, Rest + strlen(Name), Rest[strlen(Name)] == '(', State == ST_UNKNOWN);
    }
    else if (State != ST_FALSE && !strcmp(Word, "undef") && Name[0])
    {
        UndefMacro(Name, State == ST_UNKNOWN);
    }
    free(Text.Data);
    free(Word);
    free(Name);
}

/* The condition that makes the current state false or undecided, for the report */
static const char *
StateCondition(const COND_FRAME *Stack, int Depth)
{
    int i, Want = Depth ? Stack[Depth - 1].State : ST_TRUE;

    for (i = 0; i < Depth; i++)
    {
        if (Stack[i].State == Want && Stack[i].ParentState != Want)
            return Stack[i].OwnCond;
    }
    return NULL;
}

static void
TokenizeLine(const char *Line, int LineNumber, int State, const char *Cond)
{
    const char *p = Line;

    while (*p)
    {
        if (isspace((unsigned char)*p))
        {
            p++;
        }
        else if (IsIdentStart((unsigned char)*p))
        {
            const char *Start = p;
            while (IsIdentChar((unsigned char)*p))
                p++;
            AddToken(Start, p - Start, TK_IDENT, LineNumber, State, Cond);
        }
        else if (isdigit((unsigned char)*p))
        {
            const char *Start = p;
            while (IsIdentChar((unsigned char)*p) || *p == '.')
                p++;
            AddToken(Start, p - Start, TK_NUMBER, LineNumber, State, Cond);
        }
        else if (*p == '"' || *p == '\'')
        {
            const char *Start = p++;
            while (*p && *p != *Start)
            {
                if (*p == '\\' && p[1])
                    p++;
                p++;
            }
            if (*p)
                p++;
            AddToken(Start, p - Start, TK_STRING, LineNumber, State, Cond);
        }
        else
        {
            AddToken(p, 1, TK_PUNCT, LineNumber, State, Cond);
            p++;
        }
    }
}

static void
ScanHeader(const char *Path)
{
    char *Text = ReadFileText(Path);
    char *Cursor = Text;
    COND_FRAME Stack[MAX_DEPTH];
    int Depth = 0, LineNumber = 1;

    AddToken("\x01" "BOF", 4, TK_PUNCT, 0, ST_TRUE, Path);
    SpliceLines(Text);
    StripComments(Text);
    while (*Cursor)
    {
        /* The splice made the logical lines; one line of text is one directive or declaration part */
        BUFFER Logical = {NULL, 0, 0};
        const char *Trimmed;
        int StartLine = LineNumber;
        char *End = strchr(Cursor, '\n');
        size_t Piece = End ? (size_t)(End - Cursor) : strlen(Cursor);

        if (Piece && Cursor[Piece - 1] == '\r')
            Piece--;
        BufferAppend(&Logical, Cursor, Piece);
        Cursor = End ? End + 1 : Cursor + strlen(Cursor);
        LineNumber++;
        Trimmed = BufferTake(&Logical);
        while (*Trimmed == ' ' || *Trimmed == '\t')
            Trimmed++;
        if (*Trimmed == '#')
        {
            HandleDirective(Trimmed, Stack, &Depth);
        }
        else
        {
            int State = Depth ? Stack[Depth - 1].State : ST_TRUE;
            TokenizeLine(Trimmed, StartLine, State, State == ST_TRUE ? NULL : StateCondition(Stack, Depth));
        }
        free(Logical.Data);
    }
    free(Text);
}

static int
CompareText(const void *A, const void *B)
{
    return strcmp(*(char *const *)A, *(char *const *)B);
}

static int
IsWanted(const char *Name)
{
    return bsearch(&Name, g_Calls, g_CallCount, sizeof(char *), CompareText) != NULL;
}

static void
AddCall(const char *Name)
{
    if (g_CallCount == g_CallCap)
    {
        g_CallCap = g_CallCap ? g_CallCap * 2 : 1024;
        g_Calls = Xrealloc(g_Calls, g_CallCap * sizeof(*g_Calls));
    }
    g_Calls[g_CallCount++] = Xstrdup(Name);
}

static void
LoadCalls(const char *Path)
{
    char *Text = ReadFileText(Path), *Line, *Next;

    for (Line = Text; *Line; Line = Next)
    {
        char *End;
        Next = strchr(Line, '\n');
        if (Next)
            *Next++ = 0;
        else
            Next = Line + strlen(Line);
        while (*Line == ' ' || *Line == '\t' || *Line == '\r')
            Line++;
        End = Line + strlen(Line);
        while (End > Line && isspace((unsigned char)End[-1]))
            *--End = 0;
        if (*Line)
            AddCall(Line);
    }
    free(Text);
}

/* The names of the SVC_(name, count) rows of a service table, as Nt calls */
static void
LoadServices(const char *Path)
{
    char *Text = ReadFileText(Path), *p;

    SpliceLines(Text);
    StripComments(Text);
    for (p = strstr(Text, "SVC_"); p; p = strstr(p + 4, "SVC_"))
    {
        char Name[130];
        size_t Length = 2;

        if (p > Text && IsIdentChar((unsigned char)p[-1]))
            continue;
        p += 4;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p++ != '(')
            continue;
        while (*p == ' ' || *p == '\t')
            p++;
        strcpy(Name, "Nt");
        while (IsIdentChar((unsigned char)*p) && Length < sizeof(Name) - 1)
            Name[Length++] = *p++;
        Name[Length] = 0;
        if (Length > 2)
            AddCall(Name);
    }
    free(Text);
}

static void
SortCalls(void)
{
    size_t i, Out = 0;

    qsort(g_Calls, g_CallCount, sizeof(char *), CompareText);
    for (i = 0; i < g_CallCount; i++)
    {
        if (!Out || strcmp(g_Calls[Out - 1], g_Calls[i]))
            g_Calls[Out++] = g_Calls[i];
    }
    g_CallCount = Out;
}

static int
KnownKind(const char *Kind)
{
    return !strcmp(Kind, "int") || !strcmp(Kind, "ptrint") || !strcmp(Kind, "pointer") ||
           !strcmp(Kind, "handle") || !strcmp(Kind, "struct") || !strcmp(Kind, "float");
}

static void
LoadTypes(const char *Path)
{
    char *Text = ReadFileText(Path), *Line, *Next;
    int LineNumber = 0;

    for (Line = Text; *Line; Line = Next)
    {
        char Name[128], Kind[32];
        long Size32, Size64;
        int Fields;

        Next = strchr(Line, '\n');
        if (Next)
            *Next++ = 0;
        else
            Next = Line + strlen(Line);
        LineNumber++;
        if (strchr(Line, '#'))
            *strchr(Line, '#') = 0;
        Fields = sscanf(Line, "%127s %31s %ld %ld", Name, Kind, &Size32, &Size64);
        if (Fields <= 0)
            continue;
        if (Fields != 4 || !KnownKind(Kind) || Size32 < 1 || Size64 < 1)
        {
            fprintf(stderr, "wow64protos: %s:%d: bad type line\n", Path, LineNumber);
            exit(1);
        }
        if (g_TypeCount == g_TypeCap)
        {
            g_TypeCap = g_TypeCap ? g_TypeCap * 2 : 512;
            g_Types = Xrealloc(g_Types, g_TypeCap * sizeof(*g_Types));
        }
        g_Types[g_TypeCount].Name = Xstrdup(Name);
        g_Types[g_TypeCount].Kind = Xstrdup(Kind);
        g_Types[g_TypeCount].Size32 = Size32;
        g_Types[g_TypeCount].Size64 = Size64;
        g_TypeCount++;
    }
    free(Text);
}

static const TYPEDEF_ENTRY *
FindType(const char *Name)
{
    size_t i;

    for (i = 0; i < g_TypeCount; i++)
    {
        if (!strcmp(g_Types[i].Name, Name))
            return &g_Types[i];
    }
    return NULL;
}

static int
StartsWith(const char *Text, const char *Prefix)
{
    return !strncmp(Text, Prefix, strlen(Prefix));
}

/* Annotation macros (SAL and DDK) are not part of the type; compiler type keywords are.
 * A name of another family stays in the declaration and makes the argument unresolved. */
static int
IsAnnotation(const char *Text)
{
    static const char *const Exact[] = {"IN", "OUT", "OPTIONAL", "UNALIGNED", "FAR", "NEAR", "RESTRICT", NULL};
    /* a family matches the name itself and the names that continue after an underscore */
    static const char *const Families[] = {"__in", "__out", "__inout", "__deref", "__ecount", "__bcount", "__field",
                                           "__reserved", "__success", "__nullterminated", "__nullnullterminated",
                                           "__checkReturn", "__callback", "__format_string", "__restrict",
                                           "_In", "_Out", "_Inout", "_Reserved", "_Pre", "_Post", "_Ret", "_Success",
                                           "_When", "_At", "_Must", "_Frees", "_Check", "_Field", "_Interlocked",
                                           "_Notnull", "_Null", "_Maybenull", "_Analysis", "_Use", "_Struct",
                                           "_Printf", "_Scanf", "_Readable", "_Writable", "_Outptr", "_Deref",
                                           "_Return", "_Unchanged", "_Releases", "_Acquires", "_IRQL", "_Kernel",
                                           "_Function", "_Dispatch", "_Guarded", "_Has", "_Lock", "_No", "_Literal",
                                           "_Raises", "_Strict", "_Always", "_Called", "_Valid", "_Satisfies",
                                           "_Group", "_On", "_Create", "_Loc", "_Enum", "_Typedef", NULL};
    int i;

    for (i = 0; Exact[i]; i++)
    {
        if (!strcmp(Text, Exact[i]))
            return 1;
    }
    for (i = 0; Families[i]; i++)
    {
        /* __in is an annotation, __in_opt too, but __int64 is a type */
        size_t Length = strlen(Families[i]);
        if (StartsWith(Text, Families[i]) && (!Text[Length] || Text[Length] == '_'))
            return 1;
    }
    return StartsWith(Text, "__drv_");
}

static int
IsTypeKeyword(const char *Text)
{
    static const char *const Words[] = {"int", "char", "long", "short", "double", "float", "__int64",
                                        "__int32", "__int16", "__int8", NULL};
    int i;

    for (i = 0; Words[i]; i++)
    {
        if (!strcmp(Text, Words[i]))
            return 1;
    }
    return 0;
}

static int
IsTagKeyword(const char *Text)
{
    return !strcmp(Text, "struct") || !strcmp(Text, "union") || !strcmp(Text, "enum");
}

static int
IsQualifier(const char *Text)
{
    return IsTagKeyword(Text) || !strcmp(Text, "const") || !strcmp(Text, "CONST") || !strcmp(Text, "volatile") ||
           !strcmp(Text, "VOLATILE");
}

static void
SetUnresolved(ARGUMENT *Arg, const char *Type)
{
    Arg->Type = Xstrdup(Type);
    Arg->Kind = "unresolved";
    Arg->Resolved = 0;
    Arg->Size32 = Arg->Size64 = 0;
}

/* One declaration: type words, stars, an optional name, an optional array suffix */
static void
Describe(ARGUMENT *Arg, const TOKEN **Tokens, size_t Count)
{
    BUFFER Key = {NULL, 0, 0};
    const char **Ids = Xrealloc(NULL, (Count + 1) * sizeof(*Ids));
    size_t IdCount = 0, i, NameIndex;
    int Stars = 0, Array = 0;
    const TYPEDEF_ENTRY *Type;

    Arg->Name = Xstrdup("-");
    Arg->Abi64 = "-";
    SetUnresolved(Arg, "-tbd-");
    for (i = 0; i < Count; i++)
    {
        const char *Text = Tokens[i]->Text;

        if (!strcmp(Text, "("))
        {
            /* Only the form (*name)(arguments) is a function pointer; any other group is not understood */
            size_t j;
            int HasStar = 0;
            for (j = i + 1; j < Count && strcmp(Tokens[j]->Text, ")"); j++)
            {
                if (!strcmp(Tokens[j]->Text, "*"))
                    HasStar = 1;
                else if (HasStar && Tokens[j]->Kind == TK_IDENT && !IsAnnotation(Tokens[j]->Text))
                {
                    free(Arg->Name);
                    Arg->Name = Xstrdup(Tokens[j]->Text);
                    break;
                }
            }
            free(Ids);
            if (!HasStar)
                return;
            free(Arg->Type);
            Arg->Type = Xstrdup("function*");
            Arg->Kind = "pointer";
            Arg->Size32 = 4;
            Arg->Size64 = 8;
            Arg->Resolved = 1;
            return;
        }
        if (!strcmp(Text, "["))
        {
            int Level = 1;
            Array = 1;
            for (i++; i < Count && Level; i++)
            {
                if (!strcmp(Tokens[i]->Text, "["))
                    Level++;
                else if (!strcmp(Tokens[i]->Text, "]"))
                    Level--;
            }
            i--;
        }
        else if (!strcmp(Text, "*"))
        {
            Stars++;
        }
        else if (Tokens[i]->Kind == TK_IDENT && !IsQualifier(Text))
        {
            Ids[IdCount++] = Text;
        }
    }
    if (!IdCount)
    {
        free(Ids);
        return;
    }
    /* The last identifier is the name unless it is the only one or a C type keyword */
    NameIndex = IdCount;
    if (IdCount >= 2 && !IsTypeKeyword(Ids[IdCount - 1]))
        NameIndex = IdCount - 1;
    if (NameIndex < IdCount)
    {
        free(Arg->Name);
        Arg->Name = Xstrdup(Ids[NameIndex]);
    }
    for (i = 0; i < NameIndex; i++)
    {
        if (i)
            BufferAppend(&Key, "_", 1);
        BufferAppendText(&Key, Ids[i]);
    }
    free(Ids);
    if (Stars || Array)
    {
        for (i = 0; i < (size_t)Stars + (size_t)Array; i++)
            BufferAppend(&Key, "*", 1);
        free(Arg->Type);
        Arg->Type = BufferTake(&Key);
        Arg->Kind = "pointer";
        Arg->Size32 = 4;
        Arg->Size64 = 8;
        Arg->Resolved = 1;
        return;
    }
    free(Arg->Type);
    Arg->Type = BufferTake(&Key);
    Type = FindType(Arg->Type);
    if (!Type)
    {
        if (!g_Quiet)
            fprintf(stderr, "wow64protos: unresolved type %s\n", Arg->Type);
        return;
    }
    Arg->Resolved = 1;
    Arg->Size32 = Type->Size32;
    Arg->Size64 = Type->Size64;
    if (!strcmp(Type->Kind, "int"))
        Arg->Kind = Type->Size32 > 4 ? "int64" : "int";
    else
        Arg->Kind = Type->Kind;
    if (!strcmp(Type->Kind, "struct"))
        Arg->Abi64 = (Type->Size64 == 1 || Type->Size64 == 2 || Type->Size64 == 4 || Type->Size64 == 8) ? "reg" : "ref";
}

/* A token of the parameter list that the preprocessor drops is not part of the prototype */
static void
ParseArguments(PROTOTYPE *Proto, const TOKEN *Open, const TOKEN *Close)
{
    size_t Total = (size_t)(Close - Open) + 1, PartCount = 0;
    const TOKEN **Part = Xrealloc(NULL, Total * sizeof(*Part));
    const TOKEN **Clean = Xrealloc(NULL, Total * sizeof(*Clean));
    const TOKEN *Tok;
    BUFFER Signature = {NULL, 0, 0};
    int Level = 0;

    Proto->ArgCount = 0;
    Proto->ParamCond = 0;
    for (Tok = Open + 1;; Tok++)
    {
        int End = Tok == Close;

        if (!End && Tok->State == ST_FALSE && Open->State != ST_FALSE)
            continue;
        /* a conditional that the tool cannot decide inside the list, or at its closing parenthesis */
        if (Tok->State == ST_UNKNOWN && Tok->Region != Open->Region)
            Proto->ParamCond = 1;
        if (!End)
        {
            BufferAppendText(&Signature, Tok->Text);
            BufferAppend(&Signature, " ", 1);
            if (!strcmp(Tok->Text, "("))
                Level++;
            else if (!strcmp(Tok->Text, ")"))
                Level--;
        }
        if (End || (!Level && !strcmp(Tok->Text, ",")))
        {
            /* Strip annotation macros with their argument groups, then describe the declaration */
            size_t CleanCount = 0, i;

            for (i = 0; i < PartCount; i++)
            {
                if (Part[i]->Kind == TK_IDENT && IsAnnotation(Part[i]->Text) && !(i && IsTagKeyword(Part[i - 1]->Text)))
                {
                    if (i + 1 < PartCount && !strcmp(Part[i + 1]->Text, "("))
                    {
                        int Nest = 0;
                        for (i++; i < PartCount; i++)
                        {
                            if (!strcmp(Part[i]->Text, "("))
                                Nest++;
                            else if (!strcmp(Part[i]->Text, ")") && --Nest == 0)
                                break;
                        }
                    }
                    continue;
                }
                Clean[CleanCount++] = Part[i];
            }
            if (CleanCount == 1 && !Proto->ArgCount && End &&
                (!strcmp(Clean[0]->Text, "VOID") || !strcmp(Clean[0]->Text, "void")))
                break;
            if (CleanCount)
            {
                if (Proto->ArgCount >= MAX_ARGS)
                {
                    fprintf(stderr, "wow64protos: too many arguments in %s\n", Proto->Call);
                    exit(1);
                }
                Describe(&Proto->Args[Proto->ArgCount++], Clean, CleanCount);
            }
            PartCount = 0;
            if (End)
                break;
        }
        else
        {
            Part[PartCount++] = Tok;
        }
    }
    Proto->Signature = BufferTake(&Signature);
    free(Part);
    free(Clean);
}

static void
CollectPrototypes(void)
{
    size_t i;
    int BraceDepth = 0;
    const char *File = "-";

    for (i = 0; i < g_TokenCount; i++)
    {
        const TOKEN *Tok = &g_Tokens[i];

        if (Tok->Kind == TK_PUNCT && !strcmp(Tok->Text, "\x01" "BOF"))
        {
            File = Tok->Cond;
            BraceDepth = 0;
            continue;
        }
        if (Tok->State != ST_FALSE)
        {
            if (!strcmp(Tok->Text, "{") && Tok->Kind == TK_PUNCT)
                BraceDepth++;
            else if (!strcmp(Tok->Text, "}") && Tok->Kind == TK_PUNCT && BraceDepth)
                BraceDepth--;
        }
        if (Tok->Kind == TK_IDENT && (Tok->State == ST_FALSE || !BraceDepth) &&
            i + 1 < g_TokenCount && !strcmp(g_Tokens[i + 1].Text, "(") && IsWanted(Tok->Text))
        {
            size_t j, Next;
            int Level = 0;
            PROTOTYPE *Proto;
            const char *Slash = strrchr(File, '/');

            for (j = i + 1; j < g_TokenCount; j++)
            {
                if (g_Tokens[j].State == ST_FALSE && Tok->State != ST_FALSE)
                    continue;
                if (!strcmp(g_Tokens[j].Text, "(") && g_Tokens[j].Kind == TK_PUNCT)
                    Level++;
                else if (!strcmp(g_Tokens[j].Text, ")") && g_Tokens[j].Kind == TK_PUNCT && --Level == 0)
                    break;
            }
            for (Next = j + 1; Next < g_TokenCount && g_Tokens[Next].State == ST_FALSE && Tok->State != ST_FALSE; Next++)
                ;
            if (Next >= g_TokenCount || strcmp(g_Tokens[Next].Text, ";"))
                continue;
            if (g_ProtoCount == g_ProtoCap)
            {
                g_ProtoCap = g_ProtoCap ? g_ProtoCap * 2 : 1024;
                g_Protos = Xrealloc(g_Protos, g_ProtoCap * sizeof(*g_Protos));
            }
            Proto = &g_Protos[g_ProtoCount++];
            memset(Proto, 0, sizeof(*Proto));
            Proto->Call = Xstrdup(Tok->Text);
            Proto->File = Slash ? Slash + 1 : File;
            Proto->Line = Tok->Line;
            Proto->State = Tok->State;
            Proto->Cond = Tok->Cond;
            g_Quiet = Tok->State == ST_FALSE;
            ParseArguments(Proto, &g_Tokens[i + 1], &g_Tokens[j]);
            g_Quiet = 0;
            i = j;
        }
    }
}

static void
EmitCall(FILE *Out, const char *Call)
{
    size_t i, First = (size_t)-1;
    int Variants = 0, Pass;
    const PROTOTYPE *Chosen;

    /* Prefer prototypes whose conditions are decided, then undecided ones */
    for (Pass = 0; Pass < 2 && First == (size_t)-1; Pass++)
    {
        int Want = Pass == 0 ? ST_TRUE : ST_UNKNOWN;
        for (i = 0; i < g_ProtoCount; i++)
        {
            size_t k;
            int Seen = 0;
            if (strcmp(g_Protos[i].Call, Call) || g_Protos[i].State != Want)
                continue;
            for (k = 0; k < i; k++)
            {
                if (!strcmp(g_Protos[k].Call, Call) && g_Protos[k].State == Want &&
                    !strcmp(g_Protos[k].Signature, g_Protos[i].Signature))
                    Seen = 1;
            }
            if (Seen)
                continue;
            if (First == (size_t)-1)
                First = i;
            Variants++;
        }
    }
    if (First == (size_t)-1)
    {
        const char *Cond = NULL;
        for (i = 0; i < g_ProtoCount; i++)
        {
            if (!strcmp(g_Protos[i].Call, Call))
            {
                Cond = g_Protos[i].Cond;
                break;
            }
        }
        fprintf(Out, "M,%s,%s%s\n", Call, Cond ? "inactive: " : "", Cond ? Cond : "no prototype");
        return;
    }
    Chosen = &g_Protos[First];
    fprintf(Out, "P,%s,%s:%d,%d,%d,%s\n", Call, Chosen->File, Chosen->Line, Chosen->ArgCount,
            Variants + (Chosen->ParamCond ? 1 : 0), Chosen->Cond ? Chosen->Cond : "-");
    for (i = 0; i < (size_t)Chosen->ArgCount; i++)
    {
        const ARGUMENT *Arg = &Chosen->Args[i];
        if (Arg->Resolved)
            fprintf(Out, "A,%s,%d,%s,%s,%s,%ld,%ld,%ld,1,%s\n", Call, (int)i, Arg->Name, Arg->Type, Arg->Kind,
                    Arg->Size32, Arg->Size64, (Arg->Size32 + 3) / 4, Arg->Abi64);
        else
            fprintf(Out, "A,%s,%d,%s,%s,unresolved,-tbd-,-tbd-,-tbd-,-tbd-,-\n", Call, (int)i, Arg->Name, Arg->Type);
    }
}

int
main(int argc, char **argv)
{
    const char *TypesPath = NULL, *OutPath = NULL;
    char **Headers;
    size_t HeaderCount = 0, i;
    FILE *Out;
    int a, HaveCalls = 0;

    Headers = Xrealloc(NULL, argc * sizeof(*Headers));
    for (a = 1; a < argc; a++)
    {
        if (!strncmp(argv[a], "-D", 2) && argv[a][2])
        {
            char *Eq = strchr(argv[a] + 2, '=');
            if (Eq)
            {
                *Eq = 0;
                DefineMacro(argv[a] + 2, Eq + 1, 0, 0);
            }
            else
            {
                DefineMacro(argv[a] + 2, "1", 0, 0);
            }
        }
        else if (!strncmp(argv[a], "-U", 2) && argv[a][2])
        {
            DefineMacro(argv[a] + 2, "", 0, 1);
        }
        else if (!strcmp(argv[a], "-t") && a + 1 < argc)
        {
            TypesPath = argv[++a];
        }
        else if (!strcmp(argv[a], "-c") && a + 1 < argc)
        {
            LoadCalls(argv[++a]);
            HaveCalls = 1;
        }
        else if (!strcmp(argv[a], "-s") && a + 1 < argc)
        {
            LoadServices(argv[++a]);
            HaveCalls = 1;
        }
        else if (!strcmp(argv[a], "-o") && a + 1 < argc)
        {
            OutPath = argv[++a];
        }
        else if (argv[a][0] == '-')
        {
            fprintf(stderr, "wow64protos: unknown option %s\n", argv[a]);
            return 1;
        }
        else
        {
            Headers[HeaderCount++] = argv[a];
        }
    }
    if (!TypesPath || !HaveCalls || !OutPath || !HeaderCount)
    {
        fprintf(stderr, "Usage: wow64protos [-Dname[=value]]... [-Uname]... -t types.txt "
                        "(-c calls.txt | -s table.h)... -o out.csv header...\n");
        return 1;
    }
    LoadTypes(TypesPath);
    SortCalls();
    for (i = 0; i < HeaderCount; i++)
        ScanHeader(Headers[i]);
    CollectPrototypes();
    Out = fopen(OutPath, "wb");
    if (!Out)
    {
        fprintf(stderr, "wow64protos: cannot write %s\n", OutPath);
        return 1;
    }
    for (i = 0; i < g_CallCount; i++)
        EmitCall(Out, g_Calls[i]);
    if (fclose(Out))
    {
        fprintf(stderr, "wow64protos: write error on %s\n", OutPath);
        return 1;
    }
    return 0;
}
