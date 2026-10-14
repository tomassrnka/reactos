/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     WoW64 client procedure registration and process isolation
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */
#include <apitest.h>
#include <winbase.h>
#include <winuser.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _M_IX86
/* All syscall arguments use the native amd64 layout, as the wide guest will.
 * No user32 DLL is imported: registration must be the first in this process. */
#define PTR64(p) ((ULONGLONG)(ULONG_PTR)(p))
#define CLIENT_COUNT 23
#define WORKER_COUNT 11

typedef struct
{
    USHORT Length, MaximumLength;
    ULONG Padding;
    ULONGLONG Buffer;
} STRING64;

typedef struct
{
    UINT Size, Style;
    ULONGLONG WndProc;
    INT ClsExtra, WndExtra;
    ULONGLONG Instance, Icon, Cursor, Background, MenuName, ClassName, IconSm;
} CLASS64;
C_ASSERT(sizeof(STRING64) == 16);
C_ASSERT(sizeof(CLASS64) == 80);

enum
{
#define WOW64_SYSCALL(Name, Source, Target, SourceSlots, TargetSlots, Flags) Native_##Name = Target,
#include <reactos/wow64/win32k32to64.inc>
#undef WOW64_SYSCALL
};

extern ULONG __cdecl ClientPfnCall64(ULONG Number, ULONGLONG *Args, ULONGLONG *Callbacks);
extern void ClientPfnCallback64(void);
static ULONGLONG Callbacks[32];

static ULONG
Register(ULONGLONG *A, ULONGLONG *W, ULONGLONG *Worker, ULONG Module)
{
    ULONGLONG Args[6] = { PTR64(A), PTR64(W), PTR64(Worker), Module };
    return ClientPfnCall64(Native_NtUserInitializeClientPfnArrays, Args, Callbacks);
}

static void
CheckClass(ULONG Module, ULONG Atom, BOOL Ansi, ULONGLONG Expected)
{
    STRING64 Name = { 0, 0, 0, Atom };
    CLASS64 Class = { sizeof(Class) };
    ULONGLONG Args[6] = { Module, PTR64(&Name), PTR64(&Class), 0, Ansi };
    ULONG Result = ClientPfnCall64(Native_NtUserGetClassInfo, Args, Callbacks);
    ok(Result != 0, "GetClassInfo(%lx, %u) failed: %lu\n", Atom, Ansi, GetLastError());
    if (!Result) return;
    ok(Class.WndProc == Expected, "class %lx/%u proc %I64x, expected %I64x\n",
       Atom, Ansi, Class.WndProc, Expected);
    ok(Class.Instance == 0, "user32 instance was not normalized: %I64x\n", Class.Instance);
}

static void
Child(ULONG Seed, HANDLE Ready, HANDLE Release)
{
    static const struct { ULONG Atom, Index; } Classes[] =
    {
        { 32769, 3 }, /* Desktop / FNID_DESKTOP */
        { 32771, 6 }, /* Switch / FNID_SWITCH */
        { 32768, 2 }, /* Menu / FNID_MENU */
        { 32772, 1 }, /* IconTitle / FNID_ICONTITLE */
    };
    ULONGLONG A[CLIENT_COUNT], W[CLIENT_COUNT], Worker[WORKER_COUNT];
    ULONG i, Status, Module = 0x31000000 + Seed * 0x10000;
    for (i = 0; i < ARRAYSIZE(Callbacks); ++i) Callbacks[i] = PTR64(ClientPfnCallback64);
    for (i = 0; i < CLIENT_COUNT; ++i)
    {
        A[i] = 0x21000000 + Seed * 0x10000 + i * 0x10;
        W[i] = A[i] + 0x1000;
    }
    for (i = 0; i < WORKER_COUNT; ++i) Worker[i] = A[i] + 0x2000;

    /* Each fault must leave this process uninitialized and allow a retry. */
    Status = Register(NULL, W, Worker, Module);
    ok(Status == 0xc0000005, "bad A table: %lx\n", Status);
    Status = Register(A, NULL, Worker, Module);
    ok(Status == 0xc0000005, "bad W table: %lx\n", Status);
    Status = Register(A, W, NULL, Module);
    ok(Status == 0xc0000005, "bad worker table: %lx\n", Status);
    Status = Register(A, W, Worker, Module);
    ok(Status == 0, "registration failed: %lx\n", Status);
    if (Status) return;

    /* Signal only after registration; both processes stay alive together. */
    SetEvent(Ready);
    ok(WaitForSingleObject(Release, 30000) == WAIT_OBJECT_0, "release wait failed\n");
    for (i = 0; i < ARRAYSIZE(Classes); ++i)
    {
        CheckClass(Module, Classes[i].Atom, FALSE, W[Classes[i].Index]);
        CheckClass(Module, Classes[i].Atom, TRUE, A[Classes[i].Index]);
    }

    /* Explicit module queries above must normalize the registered module to
     * NULL. Also exercise the default-module lookup. */
    CheckClass(0, 32768, FALSE, W[2]);

    /* The first successful registration wins, without probing later inputs. */
    Status = Register(NULL, NULL, NULL, 0);
    ok(Status == 0, "repeat registration probed input: %lx\n", Status);
    Status = Register(W, A, Worker, Module + 0x10000);
    ok(Status == 0, "repeat registration: %lx\n", Status);
    CheckClass(Module, 32768, TRUE, A[2]);
    CheckClass(Module, 32768, FALSE, W[2]);
}

#endif /* _M_IX86 */

START_TEST(ClientPfn)
{
    if (!is_reactos())
    {
        skip("Requires ReactOS syscall numbers\n");
        return;
    }
#ifdef _M_IX86
    {
        BOOL Wow64 = FALSE;
        char **Argv;
        int Argc = winetest_get_mainargs(&Argv);
        IsWow64Process(GetCurrentProcess(), &Wow64);
        if (Wow64 && Argc == 6 && !strcmp(Argv[2], "child"))
        {
            Child(strtoul(Argv[3], NULL, 10),
                  (HANDLE)(ULONG_PTR)strtoul(Argv[4], NULL, 16),
                  (HANDLE)(ULONG_PTR)strtoul(Argv[5], NULL, 16));
        }
        else
            skip("Run the amd64 host with win32k_wow64_client.exe beside it\n");
    }
#else
    {
        static const ULONG Atoms[] = {32769, 32771, 32768, 32772};
        BOOL (WINAPI *GetClass)(HINSTANCE, LPCWSTR, LPWNDCLASSEXW);
        WNDCLASSEXW Before[ARRAYSIZE(Atoms)], After;
        HMODULE User32 = LoadLibraryW(L"user32.dll");
        WCHAR Path[MAX_PATH], Command[MAX_PATH + 120], *File;
        SECURITY_ATTRIBUTES Sa = { sizeof(Sa), NULL, TRUE };
        STARTUPINFOW Startup = { sizeof(Startup) };
        PROCESS_INFORMATION Children[2] = {{0}};
        HANDLE Ready[2] = {0}, Release;
        ULONG i, ExitCode;

        /* A native host is needed on the phase-2 runtime: guest CreateProcess
         * and guest threads are separate work. Loading native user32 also
         * ensures that the native session table already exists. */
        ok(User32 != NULL, "LoadLibrary(user32): %lu\n", GetLastError());
        if (!User32) return;
        GetClass = (void *)GetProcAddress(User32, "GetClassInfoExW");
        ok(GetClass != NULL, "GetClassInfoExW missing\n");
        if (!GetClass) { FreeLibrary(User32); return; }
        for (i = 0; i < ARRAYSIZE(Atoms); ++i)
        {
            ZeroMemory(&Before[i], sizeof(Before[i]));
            Before[i].cbSize = sizeof(Before[i]);
            ok(GetClass(NULL, (LPCWSTR)(ULONG_PTR)Atoms[i], &Before[i]), "native class %lu missing\n", Atoms[i]);
        }

        GetModuleFileNameW(NULL, Path, ARRAYSIZE(Path));
        File = wcsrchr(Path, L'\\');
        if (!File || (File - Path) + ARRAYSIZE(L"win32k_wow64_client.exe") >= ARRAYSIZE(Path))
        {
            ok(0, "helper path too long\n");
            FreeLibrary(User32);
            return;
        }
        wcscpy(File + 1, L"win32k_wow64_client.exe");
        if (GetFileAttributesW(Path) == INVALID_FILE_ATTRIBUTES)
        {
            skip("Place the PE32 fixture beside this host as win32k_wow64_client.exe\n");
            FreeLibrary(User32);
            return;
        }
        Release = CreateEventW(&Sa, TRUE, FALSE, NULL);
        ok(Release != NULL, "CreateEvent: %lu\n", GetLastError());
        if (!Release) { FreeLibrary(User32); return; }
        for (i = 0; i < ARRAYSIZE(Children); ++i)
        {
            Ready[i] = CreateEventW(&Sa, TRUE, FALSE, NULL);
            ok(Ready[i] != NULL, "CreateEvent: %lu\n", GetLastError());
            if (!Ready[i]) break;
            _snwprintf(Command, ARRAYSIZE(Command), L"\"%s\" ClientPfn child %lu %Ix %Ix",
                       Path, i + 1, (ULONG_PTR)Ready[i], (ULONG_PTR)Release);
            if (!CreateProcessW(NULL, Command, NULL, NULL, TRUE, 0, NULL, NULL, &Startup, &Children[i]))
            {
                ok(0, "CreateProcess: %lu\n", GetLastError());
                break;
            }
            ok(WaitForSingleObject(Ready[i], 30000) == WAIT_OBJECT_0, "child %lu registration wait\n", i);
        }
        SetEvent(Release);
        for (i = 0; i < ARRAYSIZE(Children); ++i)
        {
            if (Children[i].hProcess)
            {
                DWORD Wait = WaitForSingleObject(Children[i].hProcess, 30000);
                ok(Wait == WAIT_OBJECT_0, "child %lu hung\n", i);
                if (Wait != WAIT_OBJECT_0)
                {
                    TerminateProcess(Children[i].hProcess, 1);
                    WaitForSingleObject(Children[i].hProcess, 30000);
                }
                GetExitCodeProcess(Children[i].hProcess, &ExitCode);
                ok(ExitCode == 0, "child %lu failed: %lx\n", i, ExitCode);
                CloseHandle(Children[i].hThread);
                CloseHandle(Children[i].hProcess);
            }
            if (Ready[i]) CloseHandle(Ready[i]);
        }
        CloseHandle(Release);
        for (i = 0; i < ARRAYSIZE(Atoms); ++i)
        {
            ZeroMemory(&After, sizeof(After));
            After.cbSize = sizeof(After);
            ok(GetClass(NULL, (LPCWSTR)(ULONG_PTR)Atoms[i], &After), "native class %lu missing\n", Atoms[i]);
            ok(After.lpfnWndProc == Before[i].lpfnWndProc, "native procedure changed for %lu\n", Atoms[i]);
            ok(After.hInstance == Before[i].hInstance, "native module changed for %lu\n", Atoms[i]);
        }
        FreeLibrary(User32);
    }
#endif
}
