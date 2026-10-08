/*
 * PROJECT:     ReactOS tests (fork-only)
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     kvfuzz - a seeded, reproducible user-mode fuzzer for the NT and
 *              win32k system calls, generated from ReactOS's own syscall tables.
 *              Meant to run in a guest booted with the kernel verifier build.
 *
 * The call sequence is a pure function of the seed: replaying a seed with the
 * same range reproduces the exact calls, so a crash is reproduced from its seed
 * and call index, and minimised by bisecting the range (--from). Each call is
 * printed to the debugger (serial log) before it runs, so the last calls before
 * a bugcheck survive in the log.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "kvfuzz_syscalls.h"   /* generated: KfSyscalls[], KfSyscallCount */

/* ------- seeded PRNG (splitmix64) ------- */
static unsigned long long g_state;
static unsigned long long Next(void)
{
    unsigned long long z = (g_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static unsigned Rnd(unsigned n) { return n ? (unsigned)(Next() % n) : 0; }

/* --quiet drops the per-call serial print so throughput is CPU-bound rather
 * than limited by the debugger channel; g_exec counts the calls that ran. */
static int g_quiet;
static unsigned g_exec;

/* ------- object pool and buffers ------- */
#define POOL_MAX 64
static HANDLE g_pool[POOL_MAX];
static unsigned g_poolCount;
static void PoolAdd(HANDLE h) { if (h && h != INVALID_HANDLE_VALUE && g_poolCount < POOL_MAX) g_pool[g_poolCount++] = h; }

#define NBUF 6
static unsigned char *g_rw[NBUF];   /* writable pages */
static unsigned char *g_ro[NBUF];   /* read-only pages */

static void InitPool(void)
{
    char name[64];
    unsigned i;
    HANDLE h;

    PoolAdd(GetCurrentProcess());
    PoolAdd(GetCurrentThread());
    for (i = 0; i < 4; i++)
    {
        h = CreateEventA(NULL, FALSE, FALSE, NULL); PoolAdd(h);
        h = CreateMutexA(NULL, FALSE, NULL); PoolAdd(h);
        h = CreateSemaphoreA(NULL, 0, 4, NULL); PoolAdd(h);
        h = CreateWaitableTimerA(NULL, FALSE, NULL); PoolAdd(h);
        sprintf(name, "kvfuzz_sec_%u", i);
        h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 0x1000, name); PoolAdd(h);
    }
    sprintf(name, "%s\\kvfuzz_file.tmp", getenv("TEMP") ? getenv("TEMP") : "C:");
    h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); PoolAdd(h);
    {
        HKEY k;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM", 0, KEY_READ, &k) == ERROR_SUCCESS) PoolAdd((HANDLE)k);
    }

    for (i = 0; i < NBUF; i++)
    {
        g_rw[i] = (unsigned char *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_ro[i] = (unsigned char *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_rw[i]) memset(g_rw[i], 0, 0x1000);
        if (g_ro[i]) { DWORD old; memset(g_ro[i], 0, 0x1000); VirtualProtect(g_ro[i], 0x1000, PAGE_READONLY, &old); }
    }
}

/* ------- typed argument generation ------- */
static ULONG_PTR GenArg(void)
{
    static const ULONG_PTR sizes[] = { 0, 1, 2, 4, 8, 0x10, 0xff, 0x1000, 0xfff, 0x1001,
                                       0x7fffffff, (ULONG_PTR)-1, 0x10000, 0x80000000 };
    switch (Rnd(13))
    {
        case 0: return (ULONG_PTR)g_pool[Rnd(g_poolCount ? g_poolCount : 1)];     /* real handle */
        case 1: return (ULONG_PTR)(Rnd(2) ? GetCurrentProcess() : GetCurrentThread());
        case 2: return 0;                                                          /* NULL */
        case 3: return (ULONG_PTR)0xDEADBEEF;                                      /* bad handle */
        case 4: { unsigned char *b = g_rw[Rnd(NBUF)]; return (ULONG_PTR)b; }       /* writable */
        case 5: { unsigned char *b = g_ro[Rnd(NBUF)]; return (ULONG_PTR)b; }       /* read-only */
        case 6: { unsigned char *b = g_rw[Rnd(NBUF)]; return b ? (ULONG_PTR)(b + 0x1000 - Rnd(8)) : 0; } /* near page end */
        case 7: return (ULONG_PTR)0x1;                                             /* unmapped low */
        case 8: return (ULONG_PTR)0x80000000;                                      /* kernel range */
        case 9: return (ULONG_PTR)Rnd(17);                                         /* small int */
        case 10: return sizes[Rnd(sizeof(sizes)/sizeof(sizes[0]))];                /* boundary size */
        case 11: return (ULONG_PTR)(Next() & 0xFFFF);                              /* flags-ish */
        default: return (ULONG_PTR)Next();                                         /* random word */
    }
}

/* ------- call trampoline for NT (ntdll stubs) ------- */
typedef ULONG_PTR (NTAPI *F0)(void);
typedef ULONG_PTR (NTAPI *F1)(ULONG_PTR);
typedef ULONG_PTR (NTAPI *F2)(ULONG_PTR,ULONG_PTR);
typedef ULONG_PTR (NTAPI *F3)(ULONG_PTR,ULONG_PTR,ULONG_PTR);
typedef ULONG_PTR (NTAPI *F4)(ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR);
typedef ULONG_PTR (NTAPI *FN)(ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,
                              ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,
                              ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR,ULONG_PTR);
#define A(i) a[i]
static ULONG_PTR CallNt(void *fn, ULONG_PTR *a, int n)
{
    switch (n)
    {
        case 0: return ((F0)fn)();
        case 1: return ((F1)fn)(A(0));
        case 2: return ((F2)fn)(A(0),A(1));
        case 3: return ((F3)fn)(A(0),A(1),A(2));
        case 4: return ((F4)fn)(A(0),A(1),A(2),A(3));
        default:
            /* Up to 17 args; extra unused parameters are harmless. */
            return ((FN)fn)(A(0),A(1),A(2),A(3),A(4),A(5),A(6),A(7),A(8),A(9),
                            A(10),A(11),A(12),A(13),A(14),A(15),A(16));
    }
}

/* ------- raw win32k syscall (i386 int 2e only) ------- */
static ULONG_PTR CallW32(unsigned number, ULONG_PTR *a, int n)
{
#if defined(_M_IX86) || defined(__i386__)
    ULONG_PTR ret;
    (void)n;
    __asm__ __volatile__ ("int $0x2e" : "=a"(ret) : "a"(number), "d"(a) : "memory", "cc");
    return ret;
#else
    (void)number; (void)a; (void)n;
    return 0;   /* win32k raw invocation is i386-only in this tool */
#endif
}

/* ------- target selection ------- */
static unsigned short g_sel[4096];
static unsigned g_selCount;
static void BuildSelection(const char *target)
{
    unsigned i;
    g_selCount = 0;
    for (i = 0; i < KfSyscallCount && g_selCount < 4096; i++)
    {
        if (!strcmp(target, "nt") && KfSyscalls[i].Target != KF_NT) continue;
        if (!strcmp(target, "w32") && KfSyscalls[i].Target != KF_W32) continue;
        g_sel[g_selCount++] = (unsigned short)i;
    }
}

/* ------- call sequence ------- */
static int IsDangerous(const char *name)
{
    static const char *bad[] = {
        "Shutdown", "TerminateProcess", "TerminateThread", "RaiseHardError",
        "SetSystemPowerState", "RaiseException", "Continue", "LoadDriver",
        "UnloadDriver", "SetDefaultHardErrorPort", "InitializeRegistry",
        "SuspendThread", "SuspendProcess", "DelayExecution", "SetSystemInformation",
        "SetSystemTime", "SetTimerResolution", "UserCallNoParam", "UserDestroyWindow",
        "GdiFlush", "ResumeThread", "DebugActiveProcess", NULL };
    int i;
    for (i = 0; bad[i]; i++) if (strstr(name, bad[i])) return 1;
    return 0;
}

/*
 * One deterministic step: always draws the same PRNG values (selection index
 * then one GenArg per argument) so a skipped step and an executed step consume
 * the stream identically. Executes the call only when Execute is set.
 */
static void StepCall(HMODULE ntdll, unsigned callno, int Execute)
{
    ULONG_PTR a[20];
    const KF_SYSCALL *sc;
    unsigned idx = g_sel[Rnd(g_selCount ? g_selCount : 1)];
    int i;
    char line[160];
    void *fn;

    sc = &KfSyscalls[idx];
    for (i = 0; i < 20; i++) a[i] = 0;
    for (i = 0; i < sc->Args; i++) a[i] = GenArg();

    if (!Execute || IsDangerous(sc->Name)) return;

    g_exec++;
    if (!g_quiet)
    {
        _snprintf(line, sizeof(line) - 1, "KVFUZZ: #%u %s(%u) a0=%p a1=%p a2=%p\n",
                  callno, sc->Name, sc->Args, (void*)a[0], (void*)a[1], (void*)a[2]);
        line[sizeof(line)-1] = 0;
        OutputDebugStringA(line);
    }

    if (sc->Target == KF_NT)
    {
        fn = (void*)GetProcAddress(ntdll, sc->Name);
        if (fn) CallNt(fn, a, sc->Args);
    }
    else
    {
        CallW32(0x1000u | sc->W32Index, a, sc->Args);
    }
}

static void WriteState(unsigned long long seed, unsigned callno)
{
    FILE *f = fopen("C:\\kvfuzz_state.txt", "w");
    if (f) { fprintf(f, "seed=%llu callno=%u\n", seed, callno); fflush(f); fclose(f); }
}

int main(int argc, char **argv)
{
    unsigned long long seed = 0;
    unsigned maxcalls = 200000, fromcall = 0, i;
    const char *target = "all";

    for (i = 1; (int)i < argc; i++)
    {
        if (!strcmp(argv[i], "--seed") && (int)i+1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--max") && (int)i+1 < argc) maxcalls = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--from") && (int)i+1 < argc) fromcall = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--target") && (int)i+1 < argc) target = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) g_quiet = 1;
    }
    if (seed == 0) seed = ((unsigned long long)GetTickCount() << 16) ^ GetCurrentProcessId();

    g_state = seed;
    InitPool();
    BuildSelection(target);
    {
        char hdr[96];
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        _snprintf(hdr, sizeof(hdr)-1, "KVFUZZ: start seed=%llu max=%u from=%u target=%s sel=%u\n",
                  seed, maxcalls, fromcall, target, g_selCount);
        OutputDebugStringA(hdr);
        WriteState(seed, 0);

        for (i = 0; i < maxcalls; i++)
        {
            /* Skipped steps still draw the PRNG so a range replays identically;
             * only steps at/after --from execute. This is the minimisation hook:
             * the harness bisects --from upward until the crash disappears. */
            StepCall(ntdll, i, i >= fromcall);
            if ((i & 0xFF) == 0) WriteState(seed, i);
            if (g_quiet && (i & 0x1FFF) == 0)
            {
                char prg[64];
                _snprintf(prg, sizeof(prg)-1, "KVFUZZ: at #%u exec=%u\n", i, g_exec);
                OutputDebugStringA(prg);
            }
        }
        {
            char fin[64];
            _snprintf(fin, sizeof(fin)-1, "KVFUZZ: done calls=%u exec=%u\n",
                      maxcalls, g_exec);
            OutputDebugStringA(fin);
        }
    }
    return 0;
}
