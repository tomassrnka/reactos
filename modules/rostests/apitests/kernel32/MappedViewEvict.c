/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Data written through a mapped view past the valid data length must not be lost
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * Covers two FAT/Mm data-loss paths for bytes an application wrote through a writable mapped view past the
 * file system valid data length (VDL):
 *   1. Out-of-order writeback: a cached write to a HIGHER offset extends the VDL and zeroes the gap below it;
 *      a LOWER page still mapped and modified through a view must keep its data (not be gap-zeroed).
 *   2. Trim/refault: a view page past the VDL that the lazy writer cleaned, then memory pressure trimmed, must
 *      read back through the still-mapped view as the bytes written, not zero.
 * Plus a multi-threaded soak. Measured clean on FAT32 and NTFS of Windows Server 2008 R2 and Windows 10 22H2
 * (x64 and WOW64). Uses the volume of the temporary directory, or MAPPEDVIEWEVICT_DIR.
 */

#include "precomp.h"

#define SOAK_THREADS   4
#define SOAK_SECONDS   30
#define SOAK_MAX_FILE  (1024 * 1024)
#define SQUEEZE_CHUNK  (8 * 1024 * 1024)
#define OOO_FILES      32
#define TRIM_FILES     48
#define PREFIX_LEN     0x1000
#define MAP_SIZE       0x20000
#define LOW_OFF        0x4000      /* a page well past the VDL, written through the view */
#define LOW_LEN        0x1000
#define HIGH_OFF       0x10000     /* a higher offset: a cached write here extends the VDL past LOW_OFF */

static WCHAR TestDir[MAX_PATH];
static volatile LONG g_Checks;     /* total read-backs performed; the test fails if it did nothing */

static BYTE Pattern(DWORD Offset, BYTE Seed)
{
    BYTE Value = (BYTE)(Offset * 7 + Offset / 251 + Seed);
    return Value ? Value : 1;
}

static HANDLE OpenNew(PCWSTR Name)
{
    WCHAR Path[MAX_PATH];
    StringCchPrintfW(Path, _countof(Path), L"%s\\%s", TestDir, Name);
    return CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
}

static BOOL WriteAt(HANDLE File, DWORD Offset, DWORD Length, BYTE Seed)
{
    static BYTE Buf[0x4000];
    LARGE_INTEGER p; DWORD w, i;
    if (Length > sizeof(Buf)) return FALSE;
    for (i = 0; i < Length; i++) Buf[i] = Pattern(Offset + i, Seed);
    p.QuadPart = Offset;
    return SetFilePointerEx(File, p, NULL, FILE_BEGIN) && WriteFile(File, Buf, Length, &w, NULL) && w == Length;
}

static DWORD BadRead(HANDLE File, DWORD Offset, DWORD Length, BYTE Seed)
{
    static BYTE Buf[0x10000];
    LARGE_INTEGER p; DWORD r = 0, i, bad = 0;
    if (Length > sizeof(Buf)) return Length;
    p.QuadPart = Offset;
    if (!SetFilePointerEx(File, p, NULL, FILE_BEGIN) || !ReadFile(File, Buf, Length, &r, NULL)) return Length;
    InterlockedIncrement(&g_Checks);
    for (i = 0; i < r; i++) if (Buf[i] != (Seed ? Pattern(Offset + i, Seed) : 0)) bad++;
    return bad + (Length - r);
}

/* ---- Memory pressure held while reading back ---- */
typedef struct { PBYTE *b; DWORD n; } SQUEEZE;
static void SqueezeHold(SQUEEZE *s)
{
    MEMORYSTATUSEX st; s->b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 4096 * sizeof(PBYTE)); s->n = 0;
    if (!s->b) return;
    while (s->n < 4096)
    {
        PBYTE blk; SIZE_T i;
        st.dwLength = sizeof(st);
        if (GlobalMemoryStatusEx(&st) && st.ullAvailPhys < (ULONGLONG)48 * 1024 * 1024) break;
        blk = VirtualAlloc(NULL, SQUEEZE_CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!blk) break;
        for (i = 0; i < SQUEEZE_CHUNK; i += 0x1000) blk[i] = (BYTE)(i >> 12);
        s->b[s->n++] = blk;
    }
}
static void SqueezeFree(SQUEEZE *s) { DWORD i; for (i = 0; i < s->n; i++) VirtualFree(s->b[i], 0, MEM_RELEASE); if (s->b) HeapFree(GetProcessHeap(), 0, s->b); s->b = NULL; s->n = 0; }

/* 1. Out-of-order writeback: a cached write to a higher offset must not gap-zero a lower modified view page */
static void TestOutOfOrder(void)
{
    DWORD i, badLowView = 0, badLowRead = 0, badHigh = 0, badGap = 0;
    for (i = 0; i < OOO_FILES; i++)
    {
        WCHAR Name[32]; HANDLE File, Map; PBYTE View; DWORD k;
        StringCchPrintfW(Name, _countof(Name), L"ooo%lu.bin", i);
        File = OpenNew(Name);
        if (File == INVALID_HANDLE_VALUE) continue;
        if (!WriteAt(File, 0, PREFIX_LEN, 1)) { CloseHandle(File); continue; }   /* VDL = 0x1000 */
        Map = CreateFileMappingW(File, NULL, PAGE_READWRITE, 0, MAP_SIZE, NULL);
        View = Map ? MapViewOfFile(Map, FILE_MAP_WRITE, 0, 0, 0) : NULL;
        if (!View) { if (Map) CloseHandle(Map); CloseHandle(File); continue; }
        for (k = 0; k < LOW_LEN; k++) View[LOW_OFF + k] = Pattern(LOW_OFF + k, 2);  /* modify LOW page past VDL */
        /* cached write to a HIGHER offset: extends VDL past LOW_OFF and zeroes the gap in between */
        WriteAt(File, HIGH_OFF, 0x200, 3);
        /* the low page must survive, read both through the still-mapped view and through the cache */
        for (k = 0; k < LOW_LEN; k++) if (View[LOW_OFF + k] != Pattern(LOW_OFF + k, 2)) badLowView++;
        InterlockedIncrement(&g_Checks);
        badLowRead += BadRead(File, LOW_OFF, LOW_LEN, 2);
        badHigh    += BadRead(File, HIGH_OFF, 0x200, 3);
        badGap     += BadRead(File, LOW_OFF + LOW_LEN, HIGH_OFF - (LOW_OFF + LOW_LEN), 0); /* gap reads zero */
        UnmapViewOfFile(View); CloseHandle(Map); CloseHandle(File);
    }
    ok(badLowView == 0, "out-of-order: low view page lost %lu bytes\n", badLowView);
    ok(badLowRead == 0, "out-of-order: low page (ReadFile) lost %lu bytes\n", badLowRead);
    ok(badHigh == 0, "out-of-order: high write lost %lu bytes\n", badHigh);
    ok(badGap == 0, "out-of-order: gap not zero %lu bytes\n", badGap);
}

/* 2. Trim/refault: a lazily-cleaned view page past the VDL must survive a trim and read back through the view */
static void TestMappedTrim(void)
{
    HANDLE *F = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, TRIM_FILES * sizeof(HANDLE));
    HANDLE *M = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, TRIM_FILES * sizeof(HANDLE));
    PBYTE  *V = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, TRIM_FILES * sizeof(PBYTE));
    SQUEEZE sq; DWORD i, k, badView = 0, made = 0;
    if (!F || !M || !V) { skip("oom\n"); return; }
    for (i = 0; i < TRIM_FILES; i++)
    {
        WCHAR Name[32]; BYTE first;
        StringCchPrintfW(Name, _countof(Name), L"trim%lu.bin", i);
        F[i] = OpenNew(Name);
        if (F[i] == INVALID_HANDLE_VALUE) continue;
        if (!WriteAt(F[i], 0, PREFIX_LEN, 1)) continue;
        M[i] = CreateFileMappingW(F[i], NULL, PAGE_READWRITE, 0, MAP_SIZE, NULL);
        V[i] = M[i] ? MapViewOfFile(M[i], FILE_MAP_WRITE, 0, 0, 0) : NULL;
        if (!V[i]) continue;
        for (k = 0; k < LOW_LEN; k++) V[i][LOW_OFF + k] = Pattern(LOW_OFF + k, 2);   /* past VDL, kept mapped */
        /* a cached write inside the VDL dirties this file's cache view, so the lazy writer flushes the whole
         * granularity - the view page past the VDL included - as the lazy writer (which the fix makes refuse) */
        { LARGE_INTEGER z; DWORD w; z.QuadPart = 0; first = Pattern(0,1);
          SetFilePointerEx(F[i], z, NULL, FILE_BEGIN); WriteFile(F[i], &first, 1, &w, NULL); }
        made++;
    }
    ok(made == TRIM_FILES, "trim: prepared %lu of %u files\n", made, (ULONG)TRIM_FILES);
    Sleep(8000);                 /* let the lazy writer clean the past-VDL pages */
    SqueezeHold(&sq);            /* trim them under pressure, read back through the still-mapped views */
    for (i = 0; i < TRIM_FILES; i++)
    {
        if (!V[i]) continue;
        for (k = 0; k < LOW_LEN; k++) if (V[i][LOW_OFF + k] != Pattern(LOW_OFF + k, 2)) badView++;
        InterlockedIncrement(&g_Checks);
    }
    SqueezeFree(&sq);
    ok(badView == 0, "trim: view page past VDL lost %lu bytes after a trim\n", badView);
    for (i = 0; i < TRIM_FILES; i++)
    { if (V[i]) UnmapViewOfFile(V[i]); if (M[i]) CloseHandle(M[i]); if (F[i] && F[i] != INVALID_HANDLE_VALUE) CloseHandle(F[i]); }
    HeapFree(GetProcessHeap(),0,F); HeapFree(GetProcessHeap(),0,M); HeapFree(GetProcessHeap(),0,V);
}

/* 3. Multi-threaded soak (stress + bugcheck/deadlock shakeout) */
static volatile LONG SoakStop, SoakBad, SoakOps;
static DWORD Rnd(PDWORD st){ *st = *st*1103515245+12345; return (*st>>8)&0xFFFFFF; }
static DWORD WINAPI Pressure(PVOID u){ PBYTE b[4096]; DWORD n=0; (void)u;
    while(!SoakStop){ MEMORYSTATUSEX s; s.dwLength=sizeof(s);
        if(GlobalMemoryStatusEx(&s)&&s.ullAvailPhys>(ULONGLONG)64*1024*1024&&n<4096){ PBYTE p=VirtualAlloc(NULL,SQUEEZE_CHUNK,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); if(p){SIZE_T i;for(i=0;i<SQUEEZE_CHUNK;i+=0x1000)p[i]=(BYTE)i; b[n++]=p;} else Sleep(10);} 
        else if(n>0){ MEMORYSTATUSEX s2; s2.dwLength=sizeof(s2); GlobalMemoryStatusEx(&s2); if(s2.ullAvailPhys<(ULONGLONG)24*1024*1024) VirtualFree(b[--n],0,MEM_RELEASE); else Sleep(5);} else Sleep(5);} 
    while(n>0)VirtualFree(b[--n],0,MEM_RELEASE); return 0; }
static DWORD WINAPI Worker(PVOID param){ DWORD id=(DWORD)(ULONG_PTR)param;
    PBYTE model=HeapAlloc(GetProcessHeap(),0,SOAK_MAX_FILE+0x10000), buf=HeapAlloc(GetProcessHeap(),0,0x10000);
    DWORD round=0, st=0x1234+id*7919; if(!model||!buf)return 1;
    while(!SoakStop){ WCHAR name[32]; HANDLE f,m; PBYTE v; DWORD size=0x200,seed=(id+1)*16+(round&7),w,i;
        StringCchPrintfW(name,_countof(name),L"soak%lu_%lu.bin",id,round++);
        { WCHAR path[MAX_PATH]; StringCchPrintfW(path,_countof(path),L"%s\\%s",TestDir,name);
          f=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,CREATE_ALWAYS,FILE_FLAG_DELETE_ON_CLOSE,NULL); }
        if(f==INVALID_HANDLE_VALUE)continue;
        for(i=0;i<size;i++)model[i]=Pattern(i,(BYTE)seed);
        WriteFile(f,model,size,&w,NULL);
        while(!SoakStop&&size<SOAK_MAX_FILE){ DWORD grow=0x1000+Rnd(&st)%0x8000,ns=size+grow;
            InterlockedIncrement(&SoakOps);
            m=CreateFileMappingW(f,NULL,PAGE_READWRITE,0,ns,NULL); v=m?MapViewOfFile(m,FILE_MAP_WRITE,0,0,0):NULL;
            if(!v){if(m)CloseHandle(m);break;}
            for(i=0;i<grow;i++){ v[size+i]=Pattern(size+i,(BYTE)seed); model[size+i]=v[size+i]; }
            { LARGE_INTEGER z; z.QuadPart=0; SetFilePointerEx(f,z,NULL,FILE_BEGIN); WriteFile(f,model,0x200,&w,NULL);} 
            UnmapViewOfFile(v); CloseHandle(m); size=ns;
            { DWORD off=size>0x10000?(size-0x10000+Rnd(&st)%0x8000):0, len=min(0x10000,size-off); LARGE_INTEGER p; DWORD rd=0; p.QuadPart=off;
              if(len&&SetFilePointerEx(f,p,NULL,FILE_BEGIN)&&ReadFile(f,buf,len,&rd,NULL)){ for(i=0;i<rd;i++) if(buf[i]!=model[off+i]){InterlockedIncrement(&SoakBad);break;} } }
        }
        CloseHandle(f);
    }
    HeapFree(GetProcessHeap(),0,model); HeapFree(GetProcessHeap(),0,buf); return 0; }
static void TestSoak(void){ HANDLE t[SOAK_THREADS+1]; DWORD i;
    SoakStop=0;SoakBad=0;SoakOps=0;
    t[SOAK_THREADS]=CreateThread(NULL,0,Pressure,NULL,0,NULL);
    for(i=0;i<SOAK_THREADS;i++)t[i]=CreateThread(NULL,0,Worker,(PVOID)(ULONG_PTR)i,0,NULL);
    Sleep(SOAK_SECONDS*1000); InterlockedExchange(&SoakStop,1);
    for(i=0;i<=SOAK_THREADS;i++) if(t[i]){WaitForSingleObject(t[i],60000);CloseHandle(t[i]);}
    InterlockedExchangeAdd(&g_Checks,SoakOps);
    ok(SoakBad==0,"soak: %ld read-backs lost data in %ld ops\n",(LONG)SoakBad,(LONG)SoakOps); }

START_TEST(MappedViewEvict)
{
    WCHAR Temp[MAX_PATH], Volume[MAX_PATH], FsName[32]=L"?"; DWORD Length; char Msg[192];
    Length=GetEnvironmentVariableW(L"MAPPEDVIEWEVICT_DIR",Temp,_countof(Temp));
    if(!Length||Length>=_countof(Temp)) GetTempPathW(_countof(Temp),Temp);
    StringCchPrintfW(TestDir,_countof(TestDir),L"%s\\mve%lu",Temp,GetCurrentProcessId());
    if(!CreateDirectoryW(TestDir,NULL)){ skip("cannot create %ls: %lu\n",TestDir,GetLastError()); return; }
    if(GetVolumePathNameW(TestDir,Volume,_countof(Volume))) GetVolumeInformationW(Volume,NULL,0,NULL,NULL,NULL,FsName,_countof(FsName));
    StringCchPrintfA(Msg,sizeof(Msg),"UF23B:START fs=%ls",FsName); OutputDebugStringA(Msg);

    g_Checks=0;
    TestOutOfOrder();
    TestMappedTrim();
    TestSoak();

    ok(g_Checks>0,"the test performed no read-backs (skipped?) checks=%ld\n",(LONG)g_Checks);
    StringCchPrintfA(Msg,sizeof(Msg),"UF23B RESULT: checks=%ld soakOps=%ld soakBad=%ld",(LONG)g_Checks,(LONG)SoakOps,(LONG)SoakBad);
    OutputDebugStringA(Msg); printf("%s\n",Msg); fflush(stdout);
    OutputDebugStringA("UF23B:END");
    RemoveDirectoryW(TestDir);
}
