/*
 * PROJECT:     ReactOS WoW64 fixture
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Command line delivery to a child created by a parent of the other bitness, and to a
 *              process that starts its own image again with the same arguments (the Firefox launcher pattern)
 *
 * Usage (build for amd64 and i386, run the amd64 file as the parent):
 *   wow64_command_line.exe drive CHILD.EXE   run the scenarios, print "CMDLINE: PASS|FAIL"
 *   wow64_command_line.exe report ...         (child) print GetCommandLineW and argv
 *   wow64_command_line.exe relaunch ...       (child) start this image again as "report" with the same arguments
 * The same files run on Windows.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

static unsigned Failures;
#define CHECK(x) do { if (!(x)) { printf("CMDLINE: FAIL line %u: %s (error %lu)\n", __LINE__, #x, GetLastError()); ++Failures; } } while (0)

/* Quote one argument by the rules of CommandLineToArgvW / the MSVC CRT. */
static void AppendArg(WCHAR *Out, size_t Max, const WCHAR *Arg)
{
    size_t n = wcslen(Out);
    BOOL Quote = !*Arg || wcspbrk(Arg, L" \t\"");
    const WCHAR *p;
    if (n + 1 < Max) Out[n++] = L' ';
    if (Quote) Out[n++] = L'"';
    for (p = Arg; *p; ++p)
    {
        unsigned Slashes = 0;
        while (*p == L'\\') { ++Slashes; ++p; }
        if (!*p) { while (Slashes--) { Out[n++] = L'\\'; if (Quote) Out[n++] = L'\\'; } Slashes = 0; break; }
        if (*p == L'"') { while (Slashes--) { Out[n++] = L'\\'; Out[n++] = L'\\'; } Out[n++] = L'\\'; }
        else while (Slashes--) Out[n++] = L'\\';
        Out[n++] = *p;
    }
    if (Quote) Out[n++] = L'"';
    Out[n] = 0;
}

static int Report(int argc, WCHAR **argv)
{
    int i;
    printf("CL=[%ls]\n", GetCommandLineW());
    printf("ARGC=%d\n", argc);
    for (i = 0; i < argc; ++i) printf("ARG%d=[%ls]\n", i, argv[i]);
    fflush(stdout);
    return 0;
}

/* "parent PID": query the image name of process PID (the driver) and of this process, in both name formats. */
static int Parent(int argc, WCHAR **argv)
{
    typedef BOOL (WINAPI *QFPIN)(HANDLE, DWORD, LPWSTR, PDWORD);
    QFPIN Query = (QFPIN)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryFullProcessImageNameW");
    HANDLE Parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, argc > 2 ? _wtoi(argv[2]) : 0);
    WCHAR Name[MAX_PATH];
    DWORD Size;
    unsigned i;
    if (!Query || !Parent) { printf("PARENT-OPEN-ERR=%lu\n", GetLastError()); return 1; }
    for (i = 0; i < 4; ++i)
    {
        HANDLE Process = i < 2 ? Parent : GetCurrentProcess();
        DWORD Flags = (i & 1) ? 1 /* PROCESS_NAME_NATIVE */ : 0;
        Size = MAX_PATH;
        if (Query(Process, Flags, Name, &Size))
            printf("IMAGE%u=[%ls] len=%lu\n", i, Name, Size);
        else
            printf("IMAGE%u-ERR=%lu\n", i, GetLastError());
    }
    Size = 3;   /* too small: must fail with ERROR_INSUFFICIENT_BUFFER */
    if (Query(Parent, 0, Name, &Size)) printf("SMALL-ERR=none\n");
    else printf("SMALL-ERR=%lu\n", GetLastError());
    fflush(stdout);
    return 0;
}

static int Relaunch(int argc, WCHAR **argv)
{
    int i;
    WCHAR Self[MAX_PATH], Cmd[4096];
    STARTUPINFOW Si = {sizeof(Si)};
    PROCESS_INFORMATION Pi;
    DWORD Exit = 1;
    GetModuleFileNameW(NULL, Self, MAX_PATH);
    Cmd[0] = 0;
    AppendArg(Cmd, 4096, Self);
    AppendArg(Cmd, 4096, L"report");
    for (i = 2; i < argc; ++i) AppendArg(Cmd, 4096, argv[i]);
    memmove(Cmd, Cmd + 1, (wcslen(Cmd)) * sizeof(WCHAR));  /* drop the leading blank */
    if (!CreateProcessW(Self, Cmd, NULL, NULL, TRUE, 0, NULL, NULL, &Si, &Pi)) return 90;
    WaitForSingleObject(Pi.hProcess, 30000);
    GetExitCodeProcess(Pi.hProcess, &Exit);
    CloseHandle(Pi.hProcess); CloseHandle(Pi.hThread);
    return Exit;
}

static const WCHAR *Args[] =
{
    L"-wait-for-browser", L"-no-remote", L"-profile", L"C:\\harness\\p 32",
    L"file:///C:/harness/page/local.html?ff32", L"https://example.com/?a=1&b=2", L"quoted \"arg\"", L"last"
};
#define NARGS (sizeof(Args) / sizeof(Args[0]))

/* Run CHILD with the command line Cmd; read everything it prints. */
static BOOL Run(const WCHAR *Child, const WCHAR *Application, WCHAR *Cmd, char *Out, DWORD Max)
{
    HANDLE Rd, Wr;
    SECURITY_ATTRIBUTES Sa = {sizeof(Sa), NULL, TRUE};
    STARTUPINFOW Si = {sizeof(Si)};
    PROCESS_INFORMATION Pi;
    DWORD Got, Total = 0, Exit = 1;
    CHECK(CreatePipe(&Rd, &Wr, &Sa, 0));
    SetHandleInformation(Rd, HANDLE_FLAG_INHERIT, 0);
    Si.dwFlags = STARTF_USESTDHANDLES;
    Si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    Si.hStdOutput = Si.hStdError = Wr;
    if (!CreateProcessW(Application, Cmd, NULL, NULL, TRUE, 0, NULL, NULL, &Si, &Pi))
    {
        printf("CMDLINE: FAIL CreateProcess %ls: %lu\n", Child, GetLastError());
        ++Failures;
        return FALSE;
    }
    CloseHandle(Wr);
    while (Total + 1 < Max && ReadFile(Rd, Out + Total, Max - 1 - Total, &Got, NULL) && Got) Total += Got;
    Out[Total] = 0;
    {   /* the child's stdio translates \n to \r\n */
        char *r, *w;
        for (r = w = Out; *r; ++r) if (*r != '\r') *w++ = *r;
        *w = 0;
    }
    CloseHandle(Rd);
    WaitForSingleObject(Pi.hProcess, 30000);
    GetExitCodeProcess(Pi.hProcess, &Exit);
    CloseHandle(Pi.hProcess); CloseHandle(Pi.hThread);
    return Exit == 0;
}

/* The child (or the grandchild of "relaunch") reports argv[0], "report", then the arguments. */
static void Scenario(const WCHAR *Child, const WCHAR *Mode, int Form)
{
    static char Out[16384];
    WCHAR Cmd[4096];
    char Name[64], Line[16384], *p;
    unsigned i, Bad = 0;
    int Argc = -1;
    /* Form 0: quoted image name, lpApplicationName NULL; 1: unquoted image name (what cmd.exe passes);
     * 2: quoted image name plus an explicit lpApplicationName; 3: unquoted image name, two blanks after it and
     * one at the end (what cmd.exe "start" produces); 4: the same with a quoted image name. */
    Cmd[0] = 0;
    if (Form == 1 || Form == 3) wcscpy(Cmd, Child); else { wcscpy(Cmd, L"\""); wcscat(Cmd, Child); wcscat(Cmd, L"\""); }
    wcscat(Cmd, Form >= 3 ? L"  " : L" "); wcscat(Cmd, Mode);
    for (i = 0; i < NARGS; ++i) AppendArg(Cmd, 4096, Args[i]);
    if (Form >= 3) wcscat(Cmd, L" ");
    sprintf(Name, "%ls form=%d", Mode, Form);
    Out[0] = 0;
    if (!Run(Child, Form == 2 ? Child : NULL, Cmd, Out, sizeof(Out)))
    {
        printf("CMDLINE: %s FAIL child did not run\n", Name);
        ++Failures;
        return;
    }
    p = strstr(Out, "ARGC=");
    if (p) Argc = atoi(p + 5);
    if (Argc != (int)NARGS + 2) { printf("CMDLINE: %s argc=%d expected=%u\n", Name, Argc, (unsigned)NARGS + 2); Bad = 1; }
    for (i = 0; i < NARGS; ++i)
    {
        sprintf(Line, "ARG%u=[%ls]\n", i + 2, Args[i]);
        if (!strstr(Out, Line)) { printf("CMDLINE: %s missing %s", Name, Line); Bad = 1; }
    }
    if (!wcscmp(Mode, L"report"))
    {
        /* the raw command line must reach the child unchanged */
        _snprintf(Line, sizeof(Line), "CL=[%ls]\n", Cmd);
        if (!strstr(Out, Line)) { printf("CMDLINE: %s raw command line differs\n", Name); Bad = 1; }
    }
    printf("CMDLINE: %s %s\n", Name, Bad ? "FAIL" : "ok");
    if (Bad) { printf("%s\n", Out); ++Failures; }
}

/* A child asks for the image name of its parent (the launcher check Firefox makes before it removes
 * "-wait-for-browser" from its arguments) and of itself. */
static void ParentScenario(const WCHAR *Child)
{
    static char Out[4096];
    WCHAR Cmd[1024], Me[MAX_PATH];
    char Line[1024];
    unsigned Bad = 0, i;
    GetModuleFileNameW(NULL, Me, MAX_PATH);
    _snwprintf(Cmd, 1024, L"\"%ls\" parent %lu", Child, GetCurrentProcessId());
    Out[0] = 0;
    if (!Run(Child, NULL, Cmd, Out, sizeof(Out))) { printf("CMDLINE: parent image FAIL child did not run\n"); ++Failures; return; }
    {   char *r, *w; for (r = w = Out; *r; ++r) if (*r != '\r') *w++ = *r; *w = 0; }
    _snprintf(Line, sizeof(Line), "IMAGE0=[%ls] ", Me);
    if (!strstr(Out, Line)) { printf("CMDLINE: parent image: IMAGE0 is not the parent path\n"); Bad = 1; }
    _snprintf(Line, sizeof(Line), "IMAGE2=[%ls] ", Child);
    if (!strstr(Out, Line)) { printf("CMDLINE: parent image: IMAGE2 is not the child path\n"); Bad = 1; }
    for (i = 1; i < 4; i += 2)
    {
        _snprintf(Line, sizeof(Line), "IMAGE%u=[\\Device\\", i);
        if (!strstr(Out, Line)) { printf("CMDLINE: parent image: IMAGE%u is not a native name\n", i); Bad = 1; }
    }
    if (!strstr(Out, "SMALL-ERR=122")) { printf("CMDLINE: parent image: short buffer did not give 122\n"); Bad = 1; }
    printf("CMDLINE: parent image %s\n", Bad ? "FAIL" : "ok");
    if (Bad) { printf("%s\n", Out); ++Failures; }
}

int wmain(int argc, WCHAR **argv)
{
    unsigned i;
    if (argc >= 2 && !wcscmp(argv[1], L"report")) return Report(argc, argv);
    if (argc >= 2 && !wcscmp(argv[1], L"relaunch")) return Relaunch(argc, argv);
    if (argc >= 2 && !wcscmp(argv[1], L"parent")) return Parent(argc, argv);
    if (argc >= 3 && !wcscmp(argv[1], L"drive"))
    {
        static const WCHAR *Modes[] = { L"report", L"relaunch" };
        int Form;
        for (i = 0; i < 2; ++i)
            for (Form = 0; Form < 5; ++Form)
                Scenario(argv[2], Modes[i], Form);
        ParentScenario(argv[2]);
        printf("CMDLINE: %s\n", Failures ? "FAIL" : "PASS");
        return Failures != 0;
    }
    printf("usage: cmdline_probe drive CHILD | report | relaunch ...\n");
    return 2;
}
