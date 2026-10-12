/*
 * PROJECT:     ReactOS Tasklist Command
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Displays a list of currently running processes on the computer.
 * COPYRIGHT:   Copyright 2021 He Yang <1160386205@qq.com>
 *              Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "tasklist.h"

/* Output formats */
#define FORMAT_TABLE    0
#define FORMAT_LIST     1
#define FORMAT_CSV      2

/* Filter names, in the order of FilterNames */
#define FILTER_STATUS       0
#define FILTER_IMAGENAME    1
#define FILTER_PID          2
#define FILTER_SESSION      3
#define FILTER_SESSIONNAME  4
#define FILTER_CPUTIME      5
#define FILTER_MEMUSAGE     6
#define FILTER_USERNAME     7
#define FILTER_SERVICES     8
#define FILTER_WINDOWTITLE  9
#define FILTER_MODULES      10

static PCWSTR FilterNames[] =
{
    L"STATUS", L"IMAGENAME", L"PID", L"SESSION", L"SESSIONNAME", L"CPUTIME",
    L"MEMUSAGE", L"USERNAME", L"SERVICES", L"WINDOWTITLE", L"MODULES"
};

/* Operators, in the order of OperatorNames */
#define OP_EQ   0
#define OP_NE   1
#define OP_GT   2
#define OP_LT   3
#define OP_GE   4
#define OP_LE   5

static PCWSTR OperatorNames[] = { L"eq", L"ne", L"gt", L"lt", L"ge", L"le" };

/* Window status of a process */
#define TASK_STATUS_UNKNOWN     0
#define TASK_STATUS_RUNNING     1
#define TASK_STATUS_NOT_RESPONDING   2

/* Columns */
#define COL_IMAGENAME   0
#define COL_PID         1
#define COL_SESSIONNAME 2
#define COL_SESSION     3
#define COL_MEMUSAGE    4
#define COL_STATUS      5
#define COL_USERNAME    6
#define COL_CPUTIME     7
#define COL_WINDOWTITLE 8
#define COL_SERVICES    9
#define COL_MODULES     10

typedef struct _COLUMN
{
    UINT HeaderId;
    UINT Width;
    BOOL AlignRight;
} COLUMN;

static const COLUMN Columns[] =
{
    { IDS_HEADER_IMAGENAME,   COLUMNWIDTH_IMAGENAME,   FALSE },
    { IDS_HEADER_PID,         COLUMNWIDTH_PID,         TRUE  },
    { IDS_HEADER_SESSIONNAME, COLUMNWIDTH_SESSIONNAME, FALSE },
    { IDS_HEADER_SESSION,     COLUMNWIDTH_SESSION,     TRUE  },
    { IDS_HEADER_MEMUSAGE,    COLUMNWIDTH_MEMUSAGE,    TRUE  },
    { IDS_HEADER_STATUS,      COLUMNWIDTH_STATUS,      FALSE },
    { IDS_HEADER_USERNAME,    COLUMNWIDTH_USERNAME,    FALSE },
    { IDS_HEADER_CPUTIME,     COLUMNWIDTH_CPUTIME,     TRUE  },
    { IDS_HEADER_WINDOWTITLE, COLUMNWIDTH_WINDOWTITLE, FALSE },
    { IDS_HEADER_SERVICES,    COLUMNWIDTH_LIST,        FALSE },
    { IDS_HEADER_MODULES,     COLUMNWIDTH_LIST,        FALSE },
};

static const UINT ColumnsDefault[] = { COL_IMAGENAME, COL_PID, COL_SESSIONNAME, COL_SESSION, COL_MEMUSAGE };
static const UINT ColumnsVerbose[] = { COL_IMAGENAME, COL_PID, COL_SESSIONNAME, COL_SESSION, COL_MEMUSAGE,
                                       COL_STATUS, COL_USERNAME, COL_CPUTIME, COL_WINDOWTITLE };
static const UINT ColumnsServices[] = { COL_IMAGENAME, COL_PID, COL_SERVICES };
static const UINT ColumnsModules[] = { COL_IMAGENAME, COL_PID, COL_MODULES };

typedef struct _FILTER
{
    UINT Name;
    UINT Operator;
    PCWSTR Value;       /* without a trailing '*' */
    SIZE_T ValueLength;
    BOOL Wildcard;
    ULONGLONG Number;
} FILTER, *PFILTER;

typedef struct _NAME_LIST
{
    PWSTR *Names;
    DWORD Count;
} NAME_LIST, *PNAME_LIST;

typedef struct _TASK
{
    PCWSTR ImageName;
    ULONG_PTR Pid;
    ULONG SessionId;
    PCWSTR SessionName;
    SIZE_T MemUsageKB;
    ULONGLONG CpuSeconds;
    PWSTR UserName;         /* DOMAIN\user, or NULL for N/A */
    PCWSTR UserOnly;        /* the user part of UserName */
    UINT Status;
    PWSTR WindowTitle;      /* NULL for N/A */
    NAME_LIST Services;
    NAME_LIST Modules;
    BOOL ModulesKnown;
} TASK, *PTASK;

/* Options */
static BOOL bNoHeader = FALSE;
static BOOL bVerbose = FALSE;
static BOOL bServices = FALSE;
static BOOL bModules = FALSE;
static PCWSTR pszModulePattern = NULL;
static UINT OutputFormat = FORMAT_TABLE;
static PFILTER Filters = NULL;
static UINT FilterCount = 0;

/* What has to be collected for each task */
static BOOL bNeedUser = FALSE;
static BOOL bNeedWindow = FALSE;
static BOOL bNeedServices = FALSE;
static BOOL bNeedModules = FALSE;

static WCHAR szConsole[RES_STR_MAXLEN];
static WCHAR szServicesSession[RES_STR_MAXLEN];
static WCHAR szNotAvailable[RES_STR_MAXLEN];

static PVOID
AllocMem(SIZE_T Size)
{
    PVOID Ptr = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (!Ptr)
    {
        ConResMsgPrintf(StdErr, 0, IDS_OUT_OF_MEMORY);
        ExitProcess(1);
    }
    return Ptr;
}

static PWSTR
DuplicateString(PCWSTR String)
{
    SIZE_T Length = wcslen(String) + 1;
    PWSTR Copy = AllocMem(Length * sizeof(WCHAR));
    StringCchCopyW(Copy, Length, String);
    return Copy;
}

static VOID
AddName(PNAME_LIST List, PCWSTR Name)
{
    PWSTR *NewNames = AllocMem((List->Count + 1) * sizeof(PWSTR));
    if (List->Count)
    {
        CopyMemory(NewNames, List->Names, List->Count * sizeof(PWSTR));
        HeapFree(GetProcessHeap(), 0, List->Names);
    }
    NewNames[List->Count++] = DuplicateString(Name);
    List->Names = NewNames;
}

static int __cdecl
CompareNames(const void *a, const void *b)
{
    return _wcsicmp(*(PCWSTR*)a, *(PCWSTR*)b);
}

/* Error messages are followed by the usage hint */
static VOID
SyntaxError(UINT uId, ...)
{
    va_list args;

    va_start(args, uId);
    ConResMsgPrintfV(StdErr, 0, uId, &args);
    va_end(args);
    ConResMsgPrintf(StdErr, 0, IDS_USAGE_HINT);
}

static VOID
LoadResString(UINT uId, PWSTR Buffer, INT Length)
{
    if (!LoadStringW(NULL, uId, Buffer, Length))
        Buffer[0] = UNICODE_NULL;
}

/* "1,234 K" */
static VOID
FormatMemory(SIZE_T MemoryKB, PWSTR Buffer, SIZE_T Length)
{
    WCHAR Digits[32], Grouped[48];
    WCHAR Format[RES_STR_MAXLEN];
    INT n, i, j;

    StringCchPrintfW(Digits, _countof(Digits), L"%Iu", MemoryKB);
    n = (INT)wcslen(Digits);
    for (i = 0, j = 0; i < n; i++)
    {
        if (i && (n - i) % 3 == 0)
            Grouped[j++] = L',';
        Grouped[j++] = Digits[i];
    }
    Grouped[j] = UNICODE_NULL;

    LoadResString(IDS_MEMORY_STR, Format, _countof(Format));
    StringCchPrintfW(Buffer, Length, Format, Grouped);
}

static VOID
FormatCpuTime(ULONGLONG Seconds, PWSTR Buffer, SIZE_T Length)
{
    StringCchPrintfW(Buffer, Length, L"%I64u:%02u:%02u",
                     Seconds / 3600, (UINT)(Seconds / 60 % 60), (UINT)(Seconds % 60));
}

static PCWSTR
StatusText(UINT Status, PWSTR Buffer, INT Length)
{
    LoadResString((Status == TASK_STATUS_RUNNING) ? IDS_STATUS_RUNNING :
                  (Status == TASK_STATUS_NOT_RESPONDING) ? IDS_STATUS_NOT_RESPONDING : IDS_STATUS_UNKNOWN,
                  Buffer, Length);
    return Buffer;
}

/* Single-valued cell text; returns NULL for the list columns */
static PCWSTR
CellText(PTASK Task, UINT Column, PWSTR Buffer, SIZE_T Length)
{
    switch (Column)
    {
        case COL_IMAGENAME:
            return Task->ImageName;
        case COL_PID:
            StringCchPrintfW(Buffer, Length, L"%Iu", Task->Pid);
            return Buffer;
        case COL_SESSIONNAME:
            return Task->SessionName;
        case COL_SESSION:
            StringCchPrintfW(Buffer, Length, L"%lu", Task->SessionId);
            return Buffer;
        case COL_MEMUSAGE:
            FormatMemory(Task->MemUsageKB, Buffer, Length);
            return Buffer;
        case COL_STATUS:
            return StatusText(Task->Status, Buffer, (INT)Length);
        case COL_USERNAME:
            return Task->UserName ? Task->UserName : szNotAvailable;
        case COL_CPUTIME:
            FormatCpuTime(Task->CpuSeconds, Buffer, Length);
            return Buffer;
        case COL_WINDOWTITLE:
            return Task->WindowTitle ? Task->WindowTitle : szNotAvailable;
    }
    return NULL;
}

static PNAME_LIST
CellList(PTASK Task, UINT Column)
{
    if (Column == COL_SERVICES)
        return &Task->Services;
    if (Column == COL_MODULES && Task->ModulesKnown)
        return &Task->Modules;
    return NULL;
}

/* Numbers are never cut; text and headers are cut to the column */
static VOID
PrintCell(PCWSTR Text, const COLUMN *Col)
{
    if (Col->AlignRight)
        ConPrintf(StdOut, L"%*ls", Col->Width, Text);
    else
        ConPrintf(StdOut, L"%-*.*ls", Col->Width, Col->Width, Text);
}

static VOID
PrintHeaderCell(PCWSTR Text, const COLUMN *Col)
{
    ConPrintf(StdOut, Col->AlignRight ? L"%*.*ls" : L"%-*.*ls", Col->Width, Col->Width, Text);
}

/* Append Text to the CSV field in Field, doubling quotes */
static VOID
AppendCsvText(PWSTR *Field, SIZE_T *Used, SIZE_T *Size, PCWSTR Text)
{
    SIZE_T Need = *Used + 2 * wcslen(Text) + 1;
    PWSTR NewField;

    if (Need > *Size)
    {
        *Size = Need * 2;
        NewField = AllocMem(*Size * sizeof(WCHAR));
        if (*Field)
        {
            CopyMemory(NewField, *Field, *Used * sizeof(WCHAR));
            HeapFree(GetProcessHeap(), 0, *Field);
        }
        *Field = NewField;
    }

    for (; *Text; Text++)
    {
        if (*Text == L'"')
            (*Field)[(*Used)++] = L'"';
        (*Field)[(*Used)++] = *Text;
    }
    (*Field)[*Used] = UNICODE_NULL;
}

/* Whole strings go to the stream, so surrogate pairs stay together */
static VOID
PrintCsvField(PCWSTR *Texts, DWORD Count)
{
    PWSTR Field = NULL;
    SIZE_T Used = 0, Size = 0;
    DWORD i;

    AppendCsvText(&Field, &Used, &Size, L"");
    for (i = 0; i < Count; i++)
    {
        if (i)
            AppendCsvText(&Field, &Used, &Size, L",");
        AppendCsvText(&Field, &Used, &Size, Texts[i]);
    }

    ConPuts(StdOut, L"\"");
    ConPuts(StdOut, Field);
    ConPuts(StdOut, L"\"");
    HeapFree(GetProcessHeap(), 0, Field);
}

static VOID
PrintCsvValue(PCWSTR Text)
{
    PrintCsvField(&Text, 1);
}

/* Wrap "a, b, c" into lines of at most Width characters (a longer name stands alone);
   returns the allocated line and the index after its last name */
static PWSTR
NextWrappedLine(PNAME_LIST List, DWORD First, UINT Width, PDWORD Next)
{
    DWORD i = First;
    SIZE_T Used = 0, Size = 1;
    PWSTR Line;

    for (i = First; i < List->Count; i++)
        Size += wcslen(List->Names[i]) + 2;
    Line = AllocMem(Size * sizeof(WCHAR));

    for (i = First; i < List->Count; i++)
    {
        SIZE_T Need = wcslen(List->Names[i]) + ((i + 1 < List->Count) ? 1 : 0) + (Used ? 1 : 0);
        if (Used && Used + Need > Width)
            break;
        if (Used)
            StringCchCatW(Line, Size, L" ");
        StringCchCatW(Line, Size, List->Names[i]);
        if (i + 1 < List->Count)
            StringCchCatW(Line, Size, L",");
        Used += Need;
    }

    *Next = i;
    return Line;
}

static VOID
PrintHeaderTable(const UINT *Cols, UINT Count)
{
    WCHAR Header[RES_STR_MAXLEN];
    UINT i, j;

    for (i = 0; i < Count; i++)
    {
        if (i)
            ConPuts(StdOut, L" ");
        LoadResString(Columns[Cols[i]].HeaderId, Header, _countof(Header));
        PrintHeaderCell(Header, &Columns[Cols[i]]);
    }
    ConPuts(StdOut, L"\n");

    for (i = 0; i < Count; i++)
    {
        if (i)
            ConPuts(StdOut, L" ");
        for (j = 0; j < Columns[Cols[i]].Width; j++)
            ConPuts(StdOut, L"=");
    }
    ConPuts(StdOut, L"\n");
}

static VOID
PrintTaskTable(PTASK Task, const UINT *Cols, UINT Count)
{
    WCHAR Buffer[MAX_PATH];
    PWSTR Line;
    PNAME_LIST List = NULL;
    UINT i, Indent = 0, ListWidth = 0;
    DWORD Next = 0;

    for (i = 0; i < Count; i++)
    {
        PCWSTR Text = CellText(Task, Cols[i], Buffer, _countof(Buffer));

        if (i)
            ConPuts(StdOut, L" ");

        if (Text == NULL)
        {
            /* The list column is always the last one */
            List = CellList(Task, Cols[i]);
            ListWidth = Columns[Cols[i]].Width;
            if (List == NULL || List->Count == 0)
            {
                PrintCell(szNotAvailable, &Columns[Cols[i]]);
                List = NULL;
            }
            else
            {
                /* A name longer than the column gets a line of its own, uncut */
                Line = NextWrappedLine(List, 0, ListWidth, &Next);
                ConPrintf(StdOut, L"%-*ls", ListWidth, Line);
                HeapFree(GetProcessHeap(), 0, Line);
            }
        }
        else
        {
            PrintCell(Text, &Columns[Cols[i]]);
            Indent += Columns[Cols[i]].Width + 1;
        }
    }
    ConPuts(StdOut, L"\n");

    while (List && Next < List->Count)
    {
        Line = NextWrappedLine(List, Next, ListWidth, &Next);
        ConPrintf(StdOut, L"%*ls%-*ls\n", Indent, L"", ListWidth, Line);
        HeapFree(GetProcessHeap(), 0, Line);
    }
}

static VOID
PrintTaskList(PTASK Task, const UINT *Cols, UINT Count)
{
    WCHAR Buffer[MAX_PATH], Header[RES_STR_MAXLEN + 1];
    UINT i;
    DWORD j;

    ConPuts(StdOut, L"\n");
    for (i = 0; i < Count; i++)
    {
        PCWSTR Text = CellText(Task, Cols[i], Buffer, _countof(Buffer));

        LoadResString(Columns[Cols[i]].HeaderId, Header, _countof(Header) - 1);
        StringCchCatW(Header, _countof(Header), L":");
        ConPrintf(StdOut, L"%-*ls", COLUMNWIDTH_LISTLABEL, Header);

        if (Text == NULL)
        {
            PNAME_LIST List = CellList(Task, Cols[i]);
            if (List == NULL || List->Count == 0)
            {
                ConPrintf(StdOut, L"%ls\n", szNotAvailable);
            }
            else
            {
                for (j = 0; j < List->Count; j++)
                {
                    ConPrintf(StdOut, L"%*ls", j ? COLUMNWIDTH_LISTLABEL : 0, L"");
                    ConPuts(StdOut, List->Names[j]);
                    ConPuts(StdOut, L"\n");
                }
            }
        }
        else
        {
            /* Long titles do not fit the formatting buffer */
            ConPuts(StdOut, Text);
            ConPuts(StdOut, L"\n");
        }
    }
}

static VOID
PrintHeaderCsv(const UINT *Cols, UINT Count)
{
    WCHAR Header[RES_STR_MAXLEN];
    UINT i;

    for (i = 0; i < Count; i++)
    {
        if (i)
            ConPuts(StdOut, L",");
        LoadResString(Columns[Cols[i]].HeaderId, Header, _countof(Header));
        PrintCsvValue(Header);
    }
    ConPuts(StdOut, L"\n");
}

static VOID
PrintTaskCsv(PTASK Task, const UINT *Cols, UINT Count)
{
    WCHAR Buffer[MAX_PATH];
    UINT i;

    for (i = 0; i < Count; i++)
    {
        PCWSTR Text = CellText(Task, Cols[i], Buffer, _countof(Buffer));

        if (i)
            ConPuts(StdOut, L",");

        if (Text == NULL)
        {
            PNAME_LIST List = CellList(Task, Cols[i]);
            if (List == NULL || List->Count == 0)
            {
                PrintCsvValue(szNotAvailable);
            }
            else
            {
                PrintCsvField((PCWSTR*)List->Names, List->Count);
            }
        }
        else
        {
            PrintCsvValue(Text);
        }
    }
    ConPuts(StdOut, L"\n");
}

/* Data collection */

static VOID
GetTaskUser(PTASK Task)
{
    HANDLE hProcess, hToken;
    BYTE TokenBuffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    PTOKEN_USER pTokenUser = (PTOKEN_USER)TokenBuffer;
    WCHAR Name[UNLEN + 1], Domain[DNLEN + 1], Full[DNLEN + UNLEN + 2];
    DWORD NameLength = _countof(Name), DomainLength = _countof(Domain), Size;
    SID_NAME_USE Use;
    PSID Sid;
    BOOL bOk;

    if (Task->Pid == 0)
    {
        /* The idle process has no token; it is shown as the local system account */
        Size = SECURITY_MAX_SID_SIZE;
        if (!CreateWellKnownSid(WinLocalSystemSid, NULL, (PSID)TokenBuffer, &Size))
            return;
        Sid = (PSID)TokenBuffer;
    }
    else
    {
        hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, (DWORD)Task->Pid);
        if (!hProcess)
            hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)Task->Pid);
        if (!hProcess)
            return;

        bOk = OpenProcessToken(hProcess, TOKEN_QUERY, &hToken);
        CloseHandle(hProcess);
        if (!bOk)
            return;

        bOk = GetTokenInformation(hToken, TokenUser, TokenBuffer, sizeof(TokenBuffer), &Size);
        CloseHandle(hToken);
        if (!bOk)
            return;
        Sid = pTokenUser->User.Sid;
    }

    if (!LookupAccountSidW(NULL, Sid, Name, &NameLength, Domain, &DomainLength, &Use))
        return;

    if (Domain[0])
        StringCchPrintfW(Full, _countof(Full), L"%ls\\%ls", Domain, Name);
    else
        StringCchCopyW(Full, _countof(Full), Name);

    Task->UserName = DuplicateString(Full);
    Task->UserOnly = wcschr(Task->UserName, L'\\');
    Task->UserOnly = Task->UserOnly ? Task->UserOnly + 1 : Task->UserName;
}

typedef struct _WINDOW_SCAN
{
    PTASK Tasks;
    ULONG Count;
} WINDOW_SCAN, *PWINDOW_SCAN;

/* The whole title, without sending a message to the window; NULL if empty */
static PWSTR
GetTitle(HWND hWnd)
{
    INT Size = 256, Length;
    PWSTR Title;

    for (;;)
    {
        Title = AllocMem(Size * sizeof(WCHAR));
        Length = InternalGetWindowText(hWnd, Title, Size);
        if (Length < Size - 1 || Size >= 0x8000)
            break;
        HeapFree(GetProcessHeap(), 0, Title);
        Size *= 2;
    }

    if (Length <= 0)
    {
        HeapFree(GetProcessHeap(), 0, Title);
        return NULL;
    }
    return Title;
}

/* The first top-level window of a process, in Z order, gives its status and title */
static BOOL CALLBACK
EnumWindowsProc(HWND hWnd, LPARAM lParam)
{
    PWINDOW_SCAN Scan = (PWINDOW_SCAN)lParam;
    WCHAR Class[32];
    DWORD Pid = 0;
    ULONG i;

    /* IME windows belong to every GUI thread and say nothing about the task */
    if (GetClassNameW(hWnd, Class, _countof(Class)) &&
        (!_wcsicmp(Class, L"IME") || !_wcsicmp(Class, L"MSCTFIME UI")))
    {
        return TRUE;
    }

    /* The window may have closed since it was enumerated */
    if (!GetWindowThreadProcessId(hWnd, &Pid) || Pid == 0)
        return TRUE;

    for (i = 0; i < Scan->Count; i++)
    {
        PTASK Task = &Scan->Tasks[i];
        if (Task->Pid != Pid || Task->Status != TASK_STATUS_UNKNOWN)
            continue;

        Task->Status = IsHungAppWindow(hWnd) ? TASK_STATUS_NOT_RESPONDING : TASK_STATUS_RUNNING;
        Task->WindowTitle = GetTitle(hWnd);
        break;
    }
    return TRUE;
}

static VOID
GetTaskWindows(PTASK Tasks, ULONG Count)
{
    WINDOW_SCAN Scan = { Tasks, Count };
    EnumWindows(EnumWindowsProc, (LPARAM)&Scan);
}

static VOID
GetTaskServices(PTASK Tasks, ULONG Count)
{
    SC_HANDLE hManager;
    LPENUM_SERVICE_STATUS_PROCESSW Services;
    DWORD Size = 0x10000, Needed, Returned, Resume = 0, i;
    ULONG j;
    BOOL bOk;

    hManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!hManager)
        return;

    /* The list comes in pages: each call continues at the resume handle */
    Services = AllocMem(Size);
    for (;;)
    {
        Needed = Returned = 0;
        bOk = EnumServicesStatusExW(hManager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE,
                                    (LPBYTE)Services, Size, &Needed, &Returned, &Resume, NULL);
        if (!bOk && GetLastError() != ERROR_MORE_DATA)
            break;

        if (!bOk && Returned == 0)
        {
            /* Not even one entry fits */
            if (Needed <= Size || Needed > 0x400000)
                break;
            HeapFree(GetProcessHeap(), 0, Services);
            Size = Needed;
            Services = AllocMem(Size);
            continue;
        }

        for (i = 0; i < Returned; i++)
        {
            for (j = 0; j < Count; j++)
            {
                if (Tasks[j].Pid == Services[i].ServiceStatusProcess.dwProcessId &&
                    Tasks[j].Pid != 0)
                {
                    AddName(&Tasks[j].Services, Services[i].lpServiceName);
                    break;
                }
            }
        }

        if (bOk)
            break;
    }

    for (j = 0; j < Count; j++)
    {
        if (Tasks[j].Services.Count > 1)
            qsort(Tasks[j].Services.Names, Tasks[j].Services.Count, sizeof(PWSTR), CompareNames);
    }

    HeapFree(GetProcessHeap(), 0, Services);
    CloseServiceHandle(hManager);
}

static VOID
GetTaskModules(PTASK Task)
{
    HANDLE hSnapshot;
    MODULEENTRY32W Entry;
    UINT Attempt;

    if (Task->Pid == 0)
        return;

    /* A 64-bit build also lists the DLLs of 32-bit processes. The snapshot fails with
     * ERROR_BAD_LENGTH while the process loads or unloads a DLL, so try a few times.
     * ReactOS returns NULL on failure, Windows INVALID_HANDLE_VALUE. */
    for (Attempt = 0; ; Attempt++)
    {
        hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, (DWORD)Task->Pid);
        if (hSnapshot != INVALID_HANDLE_VALUE && hSnapshot != NULL)
            break;
        if (GetLastError() != ERROR_BAD_LENGTH || Attempt >= 4)
            return;
        Sleep(10);
    }

    /* The first module is the process image itself, which is not listed */
    Entry.dwSize = sizeof(Entry);
    if (Module32FirstW(hSnapshot, &Entry))
    {
        Task->ModulesKnown = TRUE;
        while (Module32NextW(hSnapshot, &Entry))
        {
            AddName(&Task->Modules, Entry.szModule);
        }
    }
    CloseHandle(hSnapshot);
}

/* Filtering */

static BOOL
MatchString(PCWSTR String, PFILTER Filter)
{
    if (String == NULL)
        return FALSE;
    if (Filter->Wildcard)
        return _wcsnicmp(String, Filter->Value, Filter->ValueLength) == 0;
    return wcslen(String) == Filter->ValueLength &&
           _wcsnicmp(String, Filter->Value, Filter->ValueLength) == 0;
}

static BOOL
MatchAnyName(PNAME_LIST List, PFILTER Filter)
{
    DWORD i;
    for (i = 0; i < List->Count; i++)
    {
        if (MatchString(List->Names[i], Filter))
            return TRUE;
    }
    return FALSE;
}

static BOOL
CompareNumber(ULONGLONG Value, PFILTER Filter)
{
    switch (Filter->Operator)
    {
        case OP_EQ: return Value == Filter->Number;
        case OP_NE: return Value != Filter->Number;
        case OP_GT: return Value > Filter->Number;
        case OP_LT: return Value < Filter->Number;
        case OP_GE: return Value >= Filter->Number;
        case OP_LE: return Value <= Filter->Number;
    }
    return FALSE;
}

static BOOL
MatchFilter(PTASK Task, PFILTER Filter)
{
    BOOL bMatch;

    switch (Filter->Name)
    {
        case FILTER_PID:
            return CompareNumber(Task->Pid, Filter);
        case FILTER_SESSION:
            return CompareNumber(Task->SessionId, Filter);
        case FILTER_MEMUSAGE:
            return CompareNumber(Task->MemUsageKB, Filter);
        case FILTER_CPUTIME:
            return CompareNumber(Task->CpuSeconds, Filter);
        case FILTER_STATUS:
            bMatch = (Task->Status == Filter->Number);
            break;
        case FILTER_IMAGENAME:
            bMatch = MatchString(Task->ImageName, Filter);
            break;
        case FILTER_SESSIONNAME:
            bMatch = Task->SessionName[0] && MatchString(Task->SessionName, Filter);
            break;
        case FILTER_USERNAME:
            bMatch = MatchString(wcschr(Filter->Value, L'\\') ? Task->UserName : Task->UserOnly, Filter);
            break;
        case FILTER_WINDOWTITLE:
            bMatch = MatchString(Task->WindowTitle, Filter);
            break;
        case FILTER_SERVICES:
            bMatch = MatchAnyName(&Task->Services, Filter);
            break;
        case FILTER_MODULES:
            bMatch = MatchAnyName(&Task->Modules, Filter);
            break;
        default:
            return FALSE;
    }

    return (Filter->Operator == OP_EQ) ? bMatch : !bMatch;
}

static BOOL
ParseNumber(PCWSTR String, ULONGLONG *Number)
{
    ULONGLONG Value = 0;

    if (*String == UNICODE_NULL)
        return FALSE;
    for (; *String; String++)
    {
        if (*String < L'0' || *String > L'9' || Value > (_UI64_MAX - 9) / 10)
            return FALSE;
        Value = Value * 10 + (*String - L'0');
    }
    *Number = Value;
    return TRUE;
}

static BOOL
ParseCpuTime(PCWSTR String, ULONGLONG *Seconds)
{
    WCHAR Part[3][21] = { { 0 } };
    ULONGLONG Value[3];
    UINT i = 0, j = 0;

    for (; *String; String++)
    {
        if (*String == L':')
        {
            if (++i >= 3)
                return FALSE;
            j = 0;
            continue;
        }
        if (j >= _countof(Part[0]) - 1)
            return FALSE;
        Part[i][j++] = *String;
        Part[i][j] = UNICODE_NULL;
    }
    if (i != 2)
        return FALSE;
    for (i = 0; i < 3; i++)
    {
        if (!ParseNumber(Part[i], &Value[i]))
            return FALSE;
    }
    if (Value[1] > 59 || Value[2] > 59 || Value[0] > MAXULONG)
        return FALSE;
    *Seconds = Value[0] * 3600 + Value[1] * 60 + Value[2];
    return TRUE;
}

/* "name op value"; the value may contain blanks and may end with one '*' */
static BOOL
ParseFilter(PCWSTR String, PFILTER Filter)
{
    WCHAR Word[16];
    PCWSTR p = String, Start;
    SIZE_T Length;
    UINT i;
    PWSTR Value;

    /* Filter name */
    while (*p == L' ' || *p == L'\t') p++;
    for (Start = p; *p && *p != L' ' && *p != L'\t'; p++);
    Length = p - Start;
    if (Length == 0 || Length >= _countof(Word))
        return FALSE;
    StringCchCopyNW(Word, _countof(Word), Start, Length);
    for (i = 0; i < _countof(FilterNames); i++)
    {
        if (!_wcsicmp(Word, FilterNames[i]))
            break;
    }
    if (i == _countof(FilterNames))
        return FALSE;
    Filter->Name = i;

    /* Operator */
    while (*p == L' ' || *p == L'\t') p++;
    for (Start = p; *p && *p != L' ' && *p != L'\t'; p++);
    Length = p - Start;
    if (Length == 0 || Length >= _countof(Word))
        return FALSE;
    StringCchCopyNW(Word, _countof(Word), Start, Length);
    for (i = 0; i < _countof(OperatorNames); i++)
    {
        if (!_wcsicmp(Word, OperatorNames[i]))
            break;
    }
    if (i == _countof(OperatorNames))
        return FALSE;
    Filter->Operator = i;

    /* Value, without the blanks around it */
    while (*p == L' ' || *p == L'\t') p++;
    Length = wcslen(p);
    while (Length && (p[Length - 1] == L' ' || p[Length - 1] == L'\t'))
        Length--;
    if (Length == 0)
        return FALSE;
    Value = AllocMem((Length + 1) * sizeof(WCHAR));
    StringCchCopyNW(Value, Length + 1, p, Length);
    Filter->Value = Value;
    Filter->ValueLength = Length;

    switch (Filter->Name)
    {
        case FILTER_PID:
        case FILTER_SESSION:
        case FILTER_MEMUSAGE:
            return ParseNumber(Value, &Filter->Number);

        case FILTER_CPUTIME:
            return ParseCpuTime(Value, &Filter->Number);

        case FILTER_STATUS:
            if (Filter->Operator > OP_NE)
                return FALSE;
            if (!_wcsicmp(Value, L"RUNNING"))
                Filter->Number = TASK_STATUS_RUNNING;
            else if (!_wcsicmp(Value, L"NOT RESPONDING"))
                Filter->Number = TASK_STATUS_NOT_RESPONDING;
            else if (!_wcsicmp(Value, L"UNKNOWN"))
                Filter->Number = TASK_STATUS_UNKNOWN;
            else
                return FALSE;
            return TRUE;

        default:
            if (Filter->Operator > OP_NE)
                return FALSE;
            /* Only one trailing wildcard is allowed */
            if (Value[Length - 1] == L'*')
            {
                Filter->Wildcard = TRUE;
                Filter->ValueLength = --Length;
                Value[Length] = UNICODE_NULL;
            }
            return wcschr(Value, L'*') == NULL;
    }
}

static BOOL
TaskMatchesFilters(PTASK Task)
{
    UINT i;
    for (i = 0; i < FilterCount; i++)
    {
        if (!MatchFilter(Task, &Filters[i]))
            return FALSE;
    }
    return TRUE;
}

/* Keep only the modules that match the /M pattern */
static BOOL
ApplyModulePattern(PTASK Task)
{
    FILTER Pattern = { 0 };
    DWORD i, j;
    SIZE_T Length = wcslen(pszModulePattern);

    Pattern.Value = pszModulePattern;
    Pattern.ValueLength = Length;
    if (Length && pszModulePattern[Length - 1] == L'*')
    {
        Pattern.Wildcard = TRUE;
        Pattern.ValueLength--;
    }

    for (i = 0, j = 0; i < Task->Modules.Count; i++)
    {
        if (MatchString(Task->Modules.Names[i], &Pattern))
            Task->Modules.Names[j++] = Task->Modules.Names[i];
    }
    Task->Modules.Count = j;
    return (j != 0);
}

/* Administrators may read the tokens and modules of service processes */
static VOID
EnableDebugPrivilege(VOID)
{
    HANDLE hToken;
    TOKEN_PRIVILEGES Privileges;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken))
        return;

    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &Privileges.Privileges[0].Luid))
        AdjustTokenPrivileges(hToken, FALSE, &Privileges, 0, NULL, NULL);

    CloseHandle(hToken);
}

static PSYSTEM_PROCESS_INFORMATION
QueryProcesses(VOID)
{
    ULONG BufferLength = 0, ResultLength = 0;
    PBYTE Buffer = NULL;
    NTSTATUS Status;
    UINT Retry;

    Status = NtQuerySystemInformation(SystemProcessInformation, NULL, 0, &ResultLength);

    /* Processes may appear between the calls: retry a few times */
    for (Retry = 0; Retry < NT_SYSTEM_QUERY_MAX_RETRY; Retry++)
    {
        BufferLength = ResultLength + 0x4000;
        if (Buffer)
            HeapFree(GetProcessHeap(), 0, Buffer);
        Buffer = AllocMem(BufferLength);

        Status = NtQuerySystemInformation(SystemProcessInformation, Buffer, BufferLength, &ResultLength);
        if (Status != STATUS_INFO_LENGTH_MISMATCH)
            break;
    }

    if (!NT_SUCCESS(Status))
    {
        ConResMsgPrintf(StdErr, 0, IDS_ENUM_FAILED);
        HeapFree(GetProcessHeap(), 0, Buffer);
        return NULL;
    }
    return (PSYSTEM_PROCESS_INFORMATION)Buffer;
}

/* Session names: the window station name where the system knows it,
   else Console for the console session and Services for session 0 */
static PCWSTR
GetSessionName(ULONG SessionId, DWORD ConsoleSession)
{
    static ULONG CachedIds[16];
    static PWSTR CachedNames[16];
    static UINT CachedCount = 0;
    PWSTR Name = NULL;
    DWORD Bytes = 0;
    UINT i;

    for (i = 0; i < CachedCount; i++)
    {
        if (CachedIds[i] == SessionId)
            return CachedNames[i];
    }

    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, SessionId, WTSWinStationName,
                                    &Name, &Bytes) && Name)
    {
        PWSTR Copy = Name[0] ? DuplicateString(Name) : NULL;
        WTSFreeMemory(Name);
        Name = Copy;
    }
    else
    {
        Name = NULL;
    }

    if (Name == NULL)
    {
        Name = (SessionId == ConsoleSession) ? szConsole :
               (SessionId == 0) ? szServicesSession : L"";
    }

    if (CachedCount < _countof(CachedIds))
    {
        CachedIds[CachedCount] = SessionId;
        CachedNames[CachedCount++] = Name;
    }
    return Name;
}

static BOOL
EnumProcessAndPrint(VOID)
{
    PSYSTEM_PROCESS_INFORMATION Buffer, pSPI;
    PTASK Tasks;
    ULONG Count = 0, i, Shown = 0;
    DWORD ConsoleSession = WTSGetActiveConsoleSessionId();
    const UINT *Cols;
    UINT ColCount;
    static WCHAR szIdle[RES_STR_MAXLEN];

    Buffer = QueryProcesses();
    if (!Buffer)
        return FALSE;

    for (pSPI = Buffer; ; pSPI = (PSYSTEM_PROCESS_INFORMATION)((PBYTE)pSPI + pSPI->NextEntryOffset))
    {
        Count++;
        if (pSPI->NextEntryOffset == 0)
            break;
    }

    LoadResString(IDS_IDLE_PROCESS, szIdle, _countof(szIdle));
    Tasks = AllocMem(Count * sizeof(TASK));
    for (i = 0, pSPI = Buffer; i < Count;
         i++, pSPI = (PSYSTEM_PROCESS_INFORMATION)((PBYTE)pSPI + pSPI->NextEntryOffset))
    {
        PTASK Task = &Tasks[i];
        SIZE_T Length = pSPI->ImageName.Length / sizeof(WCHAR);
        PWSTR Name;

        Task->Pid = (ULONG_PTR)pSPI->UniqueProcessId;
        if (Task->Pid == 0)
        {
            Task->ImageName = szIdle;
        }
        else
        {
            Name = AllocMem((Length + 1) * sizeof(WCHAR));
            if (Length)
                CopyMemory(Name, pSPI->ImageName.Buffer, Length * sizeof(WCHAR));
            Task->ImageName = Name;
        }
        Task->SessionId = pSPI->SessionId;
        Task->SessionName = GetSessionName(pSPI->SessionId, ConsoleSession);
        Task->MemUsageKB = pSPI->WorkingSetSize >> 10;
        Task->CpuSeconds = (pSPI->UserTime.QuadPart + pSPI->KernelTime.QuadPart) / 10000000;
        Task->Status = TASK_STATUS_UNKNOWN;

        if (bNeedUser)
            GetTaskUser(Task);
        if (bNeedModules)
            GetTaskModules(Task);
    }

    if (bNeedWindow)
        GetTaskWindows(Tasks, Count);
    if (bNeedServices)
        GetTaskServices(Tasks, Count);

    if (bVerbose)
    {
        Cols = ColumnsVerbose;
        ColCount = _countof(ColumnsVerbose);
    }
    else if (bServices)
    {
        Cols = ColumnsServices;
        ColCount = _countof(ColumnsServices);
    }
    else if (bModules)
    {
        Cols = ColumnsModules;
        ColCount = _countof(ColumnsModules);
    }
    else
    {
        Cols = ColumnsDefault;
        ColCount = _countof(ColumnsDefault);
    }

    for (i = 0; i < Count; i++)
    {
        PTASK Task = &Tasks[i];

        if (!TaskMatchesFilters(Task))
            continue;
        if (pszModulePattern && !ApplyModulePattern(Task))
            continue;

        if (Shown++ == 0)
        {
            if (OutputFormat == FORMAT_TABLE)
            {
                ConPuts(StdOut, L"\n");
                if (!bNoHeader)
                    PrintHeaderTable(Cols, ColCount);
            }
            else if (OutputFormat == FORMAT_CSV && !bNoHeader)
            {
                PrintHeaderCsv(Cols, ColCount);
            }
        }

        if (OutputFormat == FORMAT_TABLE)
            PrintTaskTable(Task, Cols, ColCount);
        else if (OutputFormat == FORMAT_LIST)
            PrintTaskList(Task, Cols, ColCount);
        else
            PrintTaskCsv(Task, Cols, ColCount);
    }

    if (Shown == 0)
        ConResMsgPrintf(StdOut, 0, IDS_NO_TASKS);

    /* The process exits right after this: the task data is not freed one by one */
    HeapFree(GetProcessHeap(), 0, Buffer);
    return TRUE;
}

/* Command line */

#define OPTION_INVALID  -1
#define OPTION_HELP     0
#define OPTION_NOHEADER 1
#define OPTION_VERBOSE  2
#define OPTION_SERVICES 3
#define OPTION_MODULES  4
#define OPTION_FORMAT   5
#define OPTION_FILTER   6
#define OPTION_SYSTEM   7
#define OPTION_USER     8
#define OPTION_PASSWORD 9

static PCWSTR OptionList[] = { L"?", L"nh", L"v", L"svc", L"m", L"fo", L"fi", L"s", L"u", L"p" };

static BOOL
IsOption(PCWSTR Arg)
{
    return (Arg[0] == L'/' || Arg[0] == L'-');
}

static INT
GetOptionType(PCWSTR szOption)
{
    UINT i;

    if (!IsOption(szOption))
        return OPTION_INVALID;

    for (i = 0; i < _countof(OptionList); i++)
    {
        if (!_wcsicmp(OptionList[i], szOption + 1))
            return i;
    }
    return OPTION_INVALID;
}

/* Returns -1 to go on, otherwise the exit code */
static INT
ProcessArguments(INT argc, WCHAR **argv)
{
    UINT Seen[_countof(OptionList)] = { 0 };
    PCWSTR *FilterStrings;
    PCWSTR pszFormat = NULL, pszFormatOption = NULL;
    BOOL bSystem = FALSE, bUser = FALSE, bPassword = FALSE;
    INT i;
    UINT j;

    FilterStrings = AllocMem(argc * sizeof(PCWSTR));

    for (i = 1; i < argc; i++)
    {
        INT Option = GetOptionType(argv[i]);

        if (Option == OPTION_INVALID)
        {
            SyntaxError(IDS_INVALID_ARGUMENT, argv[i]);
            return 1;
        }

        if (Option != OPTION_FILTER && Seen[Option]++)
        {
            SyntaxError(IDS_OPTION_TOO_MUCH, argv[i], 1);
            return 1;
        }

        switch (Option)
        {
            case OPTION_HELP:
                break;

            case OPTION_NOHEADER:
                bNoHeader = TRUE;
                break;

            case OPTION_VERBOSE:
                bVerbose = TRUE;
                break;

            case OPTION_SERVICES:
                bServices = TRUE;
                break;

            case OPTION_MODULES:
                bModules = TRUE;
                if (i + 1 < argc && !IsOption(argv[i + 1]))
                    pszModulePattern = argv[++i];
                break;

            case OPTION_PASSWORD:
                bPassword = TRUE;
                if (i + 1 < argc && !IsOption(argv[i + 1]))
                    i++;
                break;

            case OPTION_FORMAT:
            case OPTION_FILTER:
            case OPTION_SYSTEM:
            case OPTION_USER:
                if (i + 1 >= argc || IsOption(argv[i + 1]))
                {
                    SyntaxError(IDS_VALUE_EXPECTED, argv[i]);
                    return 1;
                }
                if (Option == OPTION_FORMAT)
                {
                    pszFormatOption = argv[i];
                    pszFormat = argv[i + 1];
                }
                else if (Option == OPTION_FILTER)
                {
                    FilterStrings[FilterCount++] = argv[i + 1];
                }
                else if (Option == OPTION_SYSTEM)
                {
                    bSystem = TRUE;
                }
                else
                {
                    bUser = TRUE;
                }
                i++;
                break;
        }
    }

    if (Seen[OPTION_HELP])
    {
        if (argc > 2)
        {
            SyntaxError(IDS_INVALID_SYNTAX);
            return 1;
        }
        ConResMsgPrintf(StdOut, 0, IDS_USAGE);
        ConResMsgPrintf(StdOut, 0, IDS_DESCRIPTION);
        return 0;
    }

    if (pszFormat)
    {
        if (!_wcsicmp(pszFormat, L"TABLE"))
            OutputFormat = FORMAT_TABLE;
        else if (!_wcsicmp(pszFormat, L"LIST"))
            OutputFormat = FORMAT_LIST;
        else if (!_wcsicmp(pszFormat, L"CSV"))
            OutputFormat = FORMAT_CSV;
        else
        {
            SyntaxError(IDS_VALUE_NOT_ALLOWED, pszFormat, pszFormatOption);
            return 1;
        }
    }

    if ((bVerbose + bServices + bModules) > 1)
    {
        SyntaxError(IDS_EXCLUSIVE_OPTIONS);
        return 1;
    }

    if (bNoHeader && OutputFormat == FORMAT_LIST)
    {
        SyntaxError(IDS_NOHEADER_LIST);
        return 1;
    }

    if (bUser && !bSystem)
    {
        SyntaxError(IDS_USER_WITHOUT_SYSTEM);
        return 1;
    }

    if (bPassword && !bUser)
    {
        SyntaxError(IDS_PASSWORD_WITHOUT_USER);
        return 1;
    }

    if (bSystem)
    {
        ConResMsgPrintf(StdErr, 0, IDS_REMOTE_NOT_SUPPORTED);
        return 1;
    }

    if (FilterCount)
    {
        Filters = AllocMem(FilterCount * sizeof(FILTER));
        for (j = 0; j < FilterCount; j++)
        {
            if (!ParseFilter(FilterStrings[j], &Filters[j]))
            {
                ConResMsgPrintf(StdErr, 0, IDS_INVALID_FILTER);
                return 1;
            }
            switch (Filters[j].Name)
            {
                case FILTER_USERNAME:
                    bNeedUser = TRUE;
                    break;
                case FILTER_STATUS:
                case FILTER_WINDOWTITLE:
                    bNeedWindow = TRUE;
                    break;
                case FILTER_SERVICES:
                    bNeedServices = TRUE;
                    break;
                case FILTER_MODULES:
                    bNeedModules = TRUE;
                    break;
            }
        }
    }

    bNeedUser |= bVerbose;
    bNeedWindow |= bVerbose;
    bNeedServices |= bServices;
    bNeedModules |= bModules;

    if (bNeedUser || bNeedModules)
        EnableDebugPrivilege();

    return EnumProcessAndPrint() ? 0 : 1;
}

int wmain(int argc, WCHAR **argv)
{
    /* Initialize the Console Standard Streams */
    ConInitStdStreams();

    LoadResString(IDS_SESSION_CONSOLE, szConsole, _countof(szConsole));
    LoadResString(IDS_SESSION_SERVICES, szServicesSession, _countof(szServicesSession));
    LoadResString(IDS_NOT_AVAILABLE, szNotAvailable, _countof(szNotAvailable));

    return ProcessArguments(argc, argv);
}
