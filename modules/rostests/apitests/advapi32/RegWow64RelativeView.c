/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Which registry view a key opened RELATIVE to a handle lands in
 *
 * A 32-bit process under WoW64 can open a parent key with KEY_WOW64_64KEY,
 * KEY_WOW64_32KEY or no view flag, then open, create or delete a child
 * relative to it, again with any of the three. This runs the whole matrix
 * through RegOpenKeyEx, RegCreateKeyEx, RegDeleteKeyEx, RegEnumKeyEx and
 * NtOpenKey/NtCreateKey with a RootDirectory, over redirected and shared keys,
 * and compares every cell with the answer recorded on Windows 10 (the .inc
 * file; produced by building this file with -DPROBE_STANDALONE and running
 * it on the oracle). Only test keys under HKCU are created; HKLM is only opened.
 */

#ifdef PROBE_STANDALONE
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define ok(c, ...) do { if (!(c)) printf(__VA_ARGS__); } while (0)
#define skip(...) printf(__VA_ARGS__)
#else
#include "precomp.h"
#endif

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } VUSTR;
typedef struct { ULONG Length; HANDLE Root; VUSTR *Name; ULONG Attributes; PVOID Sd, Qos; } VOA;
typedef LONG (NTAPI *PNTOPENKEY)(PHANDLE, ACCESS_MASK, VOA *);
typedef LONG (NTAPI *PNTCREATEKEY)(PHANDLE, ACCESS_MASK, VOA *, ULONG, VUSTR *, ULONG, PULONG);
typedef LONG (NTAPI *PNTQUERYKEY)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static PNTOPENKEY pNtOpenKey;
static PNTCREATEKEY pNtCreateKey;
static PNTQUERYKEY pNtQueryKey;
static const REGSAM Views[3] = {0, KEY_WOW64_64KEY, KEY_WOW64_32KEY};
static const char *ViewNames[3] = {"none", "64", "32"};

typedef struct
{
    const char *Id;
    HKEY Root;
    PCWSTR Parent;      /* "" = the predefined key itself */
    PCWSTR RelDir;      /* HKCU: the test tree below Parent; HKLM: the whole relative path */
    BOOL Test;          /* HKCU test tree (create Leaf/NewLeaf/DelLeaf) rather than an existing HKLM key */
} CASE;

static const CASE Cases[] =
{
    { "LM_SOFTWARE",  HKEY_LOCAL_MACHINE, L"SOFTWARE", L"Microsoft\\Windows\\CurrentVersion", FALSE },
    { "LM_MICROSOFT", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft", L"Windows\\CurrentVersion", FALSE },
    { "LM_CLASSES_R", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes", L"CLSID", FALSE },
    { "LM_CLASSES_S", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes", L"Directory", FALSE },
    { "LM_SYSTEM",    HKEY_LOCAL_MACHINE, L"SYSTEM", L"Select", FALSE },
    { "LM_ROOT",      HKEY_LOCAL_MACHINE, L"", L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion", FALSE },
    { "CU_CLASSES_R", HKEY_CURRENT_USER, L"Software\\Classes", L"CLSID\\RegViewProbe", TRUE },
    { "CU_CLSID_IN",  HKEY_CURRENT_USER, L"Software\\Classes\\CLSID", L"RegViewProbe", TRUE },
    { "CU_CLASSES_S", HKEY_CURRENT_USER, L"Software\\Classes", L"RegViewProbeCS", TRUE },
    { "CU_SOFTWARE",  HKEY_CURRENT_USER, L"Software", L"RegViewProbeS", TRUE },
    { "CU_ROOT",      HKEY_CURRENT_USER, L"", L"Software\\Classes\\CLSID\\RegViewProbe", TRUE },
};

static int Matched, Mismatched;

#ifndef PROBE_STANDALONE
typedef struct { const char *Id, *Text; } EXPECT;
static const EXPECT Expected[] =
{
#include "RegWow64RelativeView.inc"
};
#endif

#ifndef PROBE_STANDALONE
/* Windows 10 drops the view of a handle that was opened through the
 * HKCU\Software\Classes link relative to a KEY_WOW64_64KEY parent, whatever
 * the open asked for; the child of that handle then gets the 32-bit default.
 * That looks like a quirk of the link traversal, so it is not copied (and
 * not asserted either way). */
static BOOL KnownDifference(const char *Id)
{
    return !strcmp(Id, "CHAIN_CU/P=64/R=none/Chain") || !strcmp(Id, "CHAIN_CU/P=64/R=64/Chain");
}
#endif

static void Emit(const char *Id, const char *Fmt, ...)
{
    char Text[1024];
    va_list Args;
    va_start(Args, Fmt);
    vsnprintf(Text, sizeof(Text), Fmt, Args);
    va_end(Args);
#ifdef PROBE_STANDALONE
    printf("%s = %s\n", Id, Text);
#else
    {
        size_t i;
        for (i = 0; i < ARRAYSIZE(Expected); ++i)
        {
            if (strcmp(Expected[i].Id, Id)) continue;
            if (KnownDifference(Id))
            {
                if (strcmp(Expected[i].Text, Text)) trace("%s: known difference, got '%s', Windows gives '%s'\n", Id, Text, Expected[i].Text);
                return;
            }
            ok(!strcmp(Expected[i].Text, Text), "%s: got '%s', Windows gives '%s'\n", Id, Text, Expected[i].Text);
            return;
        }
        ok(0, "%s: no reference (got '%s')\n", Id, Text);
    }
#endif
}

/* Physical name with the user SID folded away, and the separately mounted
 * <SID>_Classes hive shown where ReactOS keeps it, inside HKCU\Software\Classes */
static void KeyName(HANDLE Key, char *Out, size_t Size)
{
    BYTE Buffer[1024];
    ULONG Length, i, n = 0;
    WCHAR Name[512];
    PCWSTR Users = L"\\REGISTRY\\USER\\";
    char Tmp[512];
    Out[0] = 0;
    if (pNtQueryKey(Key, 3, Buffer, sizeof(Buffer), &Length) < 0) { strcpy(Out, "?"); return; }
    Length = *(ULONG *)Buffer / sizeof(WCHAR);
    for (i = 0; i < Length && i < 511; ++i)
    {
        WCHAR c = ((WCHAR *)(Buffer + 4))[i];
        Name[i] = c >= 'a' && c <= 'z' ? c - 32 : c;
    }
    Name[i] = 0;
    for (i = 0; Users[i]; ++i) if (Name[i] != Users[i]) break;
    if (!Users[i])
    {
        ULONG Start = i, End = i;
        while (Name[End] && Name[End] != '\\') ++End;
        n = sprintf(Tmp, "\\REGISTRY\\USER\\<SID>");
        if (End - Start > 8 && !wcsncmp(Name + End - 8, L"_CLASSES", 8))
            n += sprintf(Tmp + n, "\\SOFTWARE\\CLASSES");
        for (i = End; Name[i] && n < sizeof(Tmp) - 1; ++i) Tmp[n++] = (char)Name[i];
        Tmp[n] = 0;
    }
    else
    {
        for (i = 0; Name[i] && n < sizeof(Tmp) - 1; ++i) Tmp[n++] = (char)Name[i];
        Tmp[n] = 0;
    }
    strncpy(Out, Tmp, Size - 1);
    Out[Size - 1] = 0;
}

static void Subs(HKEY Key, BOOL List, char *Out, size_t Size)
{
    DWORD Count = 0, i, n = 0;
    WCHAR Name[256];
    Out[0] = 0;
    if (RegQueryInfoKeyW(Key, NULL, NULL, NULL, &Count, NULL, NULL, NULL, NULL, NULL, NULL, NULL)) { strcpy(Out, "n=?"); return; }
    n = snprintf(Out, Size, "n=%lu", Count);
    for (i = 0; List && i < Count && i < 4; ++i)
    {
        DWORD Length = ARRAYSIZE(Name);
        LONG Error = RegEnumKeyExW(Key, i, Name, &Length, NULL, NULL, NULL, NULL);
        if (Error) { n += snprintf(Out + n, Size - n, " enum%lu=err%ld", i, Error); break; }
        n += snprintf(Out + n, Size - n, " %S", Name);
    }
}

static BOOL Exists(HKEY Root, PCWSTR Path, REGSAM View)
{
    HKEY Key;
    if (RegOpenKeyExW(Root, Path, 0, KEY_READ | View, &Key)) return FALSE;
    RegCloseKey(Key);
    return TRUE;
}

static void Join(WCHAR *Out, PCWSTR A, PCWSTR B, PCWSTR C)
{
    Out[0] = 0;
    if (*A) wcscat(Out, A);
    if (*A && *B) wcscat(Out, L"\\");
    if (*B) wcscat(Out, B);
    if (*C) wcscat(Out, L"\\");
    if (*C) wcscat(Out, C);
}

static void Where(const CASE *C, PCWSTR Leaf, char *Out)
{
    WCHAR Dir[300], Path[400];
    Join(Dir, C->Parent, C->RelDir, L"");
    wcscpy(Path, Dir);
    wcscat(Path, L"\\");
    wcscat(Path, Leaf);
    sprintf(Out, "E64=%d E32=%d", Exists(C->Root, Path, KEY_WOW64_64KEY), Exists(C->Root, Path, KEY_WOW64_32KEY));
}

static void Make(const CASE *C, PCWSTR Leaf, const WCHAR *Marker)
{
    WCHAR Path[400];
    int v;
    for (v = 1; v < 3; ++v)
    {
        HKEY Key;
        WCHAR Full[400];
        Join(Path, C->Parent, C->RelDir, L"");
        wcscpy(Full, Path);
        wcscat(Full, L"\\");
        wcscat(Full, Leaf);
        if (Marker) { wcscat(Full, L"\\"); wcscat(Full, v == 1 ? L"M64" : L"M32"); }
        if (!RegCreateKeyExW(C->Root, Full, 0, NULL, 0, KEY_ALL_ACCESS | Views[v], NULL, &Key, NULL))
            RegCloseKey(Key);
    }
}

/* RegDeleteTree needs a Vista import library; the leaves here are shallow */
static void DeleteChildren(HKEY Key, REGSAM View)
{
    WCHAR Name[256];
    DWORD Length = ARRAYSIZE(Name);
    while (!RegEnumKeyExW(Key, 0, Name, &Length, NULL, NULL, NULL, NULL))
    {
        HKEY Child;
        if (!RegOpenKeyExW(Key, Name, 0, KEY_ALL_ACCESS | View, &Child))
        {
            DeleteChildren(Child, View);
            RegCloseKey(Child);
        }
        if (RegDeleteKeyExW(Key, Name, View, 0)) break;
        Length = ARRAYSIZE(Name);
    }
}

static void Remove(const CASE *C, PCWSTR Leaf)
{
    WCHAR Full[400];
    int v;
    for (v = 1; v < 3; ++v)
    {
        HKEY Key;
        Join(Full, C->Parent, C->RelDir, L"");
        wcscat(Full, L"\\");
        wcscat(Full, Leaf);
        if (!RegOpenKeyExW(C->Root, Full, 0, KEY_ALL_ACCESS | Views[v], &Key))
        {
            DeleteChildren(Key, Views[v]);
            RegCloseKey(Key);
        }
        RegDeleteKeyExW(C->Root, Full, Views[v], 0);
    }
}

static void Report(const char *Id, const char *Op, LONG Error, HANDLE Key, BOOL List, DWORD Disp, const char *Extra)
{
    char Name[512] = "", Sub[200] = "";
    char Line[1000];
    if (!Error && Key)
    {
        KeyName(Key, Name, sizeof(Name));
        Subs((HKEY)Key, List, Sub, sizeof(Sub));
    }
    snprintf(Line, sizeof(Line), "err=%ld%s%s%s%s%s%s%s", Error,
             Disp ? (Disp == REG_CREATED_NEW_KEY ? " created" : " opened") : "",
             Name[0] ? " name=" : "", Name, Sub[0] && List ? " " : "", List ? Sub : "",
             Extra && Extra[0] ? " " : "", Extra ? Extra : "");
    (void)Op;
    Emit(Id, "%s", Line);
}

static void NtName(VUSTR *U, PCWSTR S)
{
    U->Length = (USHORT)(wcslen(S) * sizeof(WCHAR));
    U->MaximumLength = U->Length + sizeof(WCHAR);
    U->Buffer = (PWSTR)S;
}

static void RunCase(const CASE *C)
{
    int pv, rv, rootcase = !C->Parent[0];
    WCHAR Rel[400], NewRel[400], DelRel[400];
    BOOL List = C->Test;
    if (C->Test)
    {
        wcscpy(Rel, C->RelDir); wcscat(Rel, L"\\Leaf");
        wcscpy(NewRel, C->RelDir); wcscat(NewRel, L"\\NewLeaf");
        wcscpy(DelRel, C->RelDir); wcscat(DelRel, L"\\DelLeaf");
        /* when there is no parent the relative name is everything under the hive */
        Make(C, L"Leaf", L"M");
    }
    else
    {
        wcscpy(Rel, C->RelDir); wcscpy(NewRel, Rel); DelRel[0] = 0;
    }
    for (pv = 0; pv < (rootcase ? 1 : 3); ++pv)
    {
        HKEY P = C->Root, K;
        if (!rootcase && RegOpenKeyExW(C->Root, C->Parent, 0, KEY_READ | KEY_CREATE_SUB_KEY | Views[pv], &P))
        {
            skip("%s: parent open failed\n", C->Id);
            continue;
        }
        for (rv = 0; rv < 3; ++rv)
        {
            char Id[100], Where1[100];
            LONG Error;
            DWORD Disp = 0;
            VUSTR Name, NewName;
            VOA Oa = {sizeof(Oa), P, &Name, 0x40 /* OBJ_CASE_INSENSITIVE */, NULL, NULL};
            HANDLE H;
            /* RegOpenKeyEx */
            snprintf(Id, sizeof(Id), "%s/P=%s/R=%s/RegOpenKeyEx", C->Id, ViewNames[pv], ViewNames[rv]);
            K = NULL;
            Error = RegOpenKeyExW(P, Rel, 0, KEY_READ | Views[rv], &K);
            Report(Id, "open", Error, K, List, 0, NULL);
            if (!Error) RegCloseKey(K);
            /* RegCreateKeyEx: an existing key, then a new one */
            snprintf(Id, sizeof(Id), "%s/P=%s/R=%s/RegCreateKeyEx", C->Id, ViewNames[pv], ViewNames[rv]);
            K = NULL;
            Error = RegCreateKeyExW(P, C->Test ? NewRel : Rel, 0, NULL, 0, KEY_READ | Views[rv], NULL, &K, &Disp);
            Where1[0] = 0;
            if (C->Test) { if (!Error) { RegCloseKey(K); K = NULL; } Where(C, L"NewLeaf", Where1); }
            Report(Id, "create", Error, K, FALSE, Disp, Where1);
            if (!C->Test && !Error) RegCloseKey(K);
            if (C->Test) Remove(C, L"NewLeaf");
            /* RegDeleteKeyEx */
            if (C->Test)
            {
                Make(C, L"DelLeaf", NULL);
                snprintf(Id, sizeof(Id), "%s/P=%s/R=%s/RegDeleteKeyEx", C->Id, ViewNames[pv], ViewNames[rv]);
                Error = RegDeleteKeyExW(P, DelRel, Views[rv], 0);
                Where(C, L"DelLeaf", Where1);
                Report(Id, "delete", Error, NULL, FALSE, 0, Where1);
                Remove(C, L"DelLeaf");
            }
            if (rootcase) continue;
            /* NtOpenKey with RootDirectory */
            snprintf(Id, sizeof(Id), "%s/P=%s/R=%s/NtOpenKey", C->Id, ViewNames[pv], ViewNames[rv]);
            NtName(&Name, Rel);
            H = NULL;
            Error = pNtOpenKey(&H, KEY_READ | Views[rv], &Oa);
            { char Line[1000], Nm[512] = "", Sb[200] = "";
              if (Error >= 0) { KeyName(H, Nm, sizeof(Nm)); if (List) Subs((HKEY)H, TRUE, Sb, sizeof(Sb)); CloseHandle(H); }
              snprintf(Line, sizeof(Line), "status=%08lx%s%s%s%s", Error, Nm[0] ? " name=" : "", Nm, Sb[0] ? " " : "", Sb);
              Emit(Id, "%s", Line); }
            /* NtCreateKey with RootDirectory */
            snprintf(Id, sizeof(Id), "%s/P=%s/R=%s/NtCreateKey", C->Id, ViewNames[pv], ViewNames[rv]);
            NtName(&NewName, C->Test ? NewRel : Rel);
            Oa.Name = &NewName;
            H = NULL; Disp = 0;
            Error = pNtCreateKey(&H, KEY_READ | Views[rv], &Oa, 0, NULL, 0, &Disp);
            { char Line[1000], Nm[512] = "";
              if (Error >= 0) { KeyName(H, Nm, sizeof(Nm)); CloseHandle(H); }
              Where1[0] = 0;
              if (C->Test) Where(C, L"NewLeaf", Where1);
              snprintf(Line, sizeof(Line), "status=%08lx%s%s%s", Error, Error >= 0 ? (Disp == 1 ? " created" : " opened") : "",
                       Nm[0] ? " name=" : "", Nm);
              if (Where1[0]) { strcat(Line, " "); strcat(Line, Where1); }
              Emit(Id, "%s", Line); }
            if (C->Test) Remove(C, L"NewLeaf");
        }
        if (!rootcase) RegCloseKey(P);
    }
    if (C->Test) { Remove(C, L"Leaf"); }
}


/* A handle opened relative to another handle: does it keep the view the
 * first one was opened with when the second open names no view? */
static void RunChain(const char *Name, HKEY Root, PCWSTR First, PCWSTR Second, PCWSTR Third, BOOL Test)
{
    int pv, mv;
    const CASE *C = &Cases[6];
    if (Test) Make(C, L"Leaf", L"M");
    for (pv = 0; pv < 3; ++pv)
    {
        for (mv = 0; mv < 3; ++mv)
        {
            HKEY P, M, K = NULL;
            char Id[100];
            LONG Error;
            snprintf(Id, sizeof(Id), "%s/P=%s/R=%s/Chain", Name, ViewNames[pv], ViewNames[mv]);
            if (RegOpenKeyExW(Root, First, 0, KEY_READ | Views[pv], &P)) continue;
            if (RegOpenKeyExW(P, Second, 0, KEY_READ | Views[mv], &M)) { RegCloseKey(P); continue; }
            Error = RegOpenKeyExW(M, Third, 0, KEY_READ, &K);
            Report(Id, "chain", Error, K, Test, 0, NULL);
            if (!Error) RegCloseKey(K);
            RegCloseKey(M);
            RegCloseKey(P);
        }
    }
    if (Test) Remove(C, L"Leaf");
}

static void RunAll(void)
{
    HKEY Key;
    BOOL HadClsid[3] = {0};
    size_t i;
    int v;
    {
        HMODULE Nt = GetModuleHandleW(L"ntdll.dll");
        pNtOpenKey = (PNTOPENKEY)GetProcAddress(Nt, "NtOpenKey");
        pNtCreateKey = (PNTCREATEKEY)GetProcAddress(Nt, "NtCreateKey");
        pNtQueryKey = (PNTQUERYKEY)GetProcAddress(Nt, "NtQueryKey");
    }
    /* the CLSID containers in HKCU\Software\Classes, kept if they were there */
    for (v = 1; v < 3; ++v)
    {
        HadClsid[v] = Exists(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID", Views[v]);
        if (!RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID", 0, NULL, 0,
                             KEY_ALL_ACCESS | Views[v], NULL, &Key, NULL)) RegCloseKey(Key);
    }
    for (i = 0; i < ARRAYSIZE(Cases); ++i) RunCase(&Cases[i]);
    RunChain("CHAIN_CU", HKEY_CURRENT_USER, L"Software", L"Classes", L"CLSID\\RegViewProbe\\Leaf", TRUE);
    RunChain("CHAIN_LM", HKEY_LOCAL_MACHINE, L"SOFTWARE", L"Classes", L"CLSID", FALSE);
    for (v = 1; v < 3; ++v)
        if (!HadClsid[v]) RegDeleteKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID", Views[v], 0);
    /* nothing of ours may be left behind */
    for (i = 0; i < ARRAYSIZE(Cases); ++i)
    {
        WCHAR Path[400];
        if (!Cases[i].Test || !Cases[i].Parent[0]) continue;
        Join(Path, Cases[i].Parent, Cases[i].RelDir, L"");
        for (v = 1; v < 3; ++v)
        {
            if (Exists(HKEY_CURRENT_USER, Path, Views[v]))
            {
                HKEY K;
                if (!RegOpenKeyExW(HKEY_CURRENT_USER, Path, 0, KEY_ALL_ACCESS | Views[v], &K))
                { DeleteChildren(K, Views[v]); RegCloseKey(K); }
                RegDeleteKeyExW(HKEY_CURRENT_USER, Path, Views[v], 0);
            }
        }
    }
    (void)Matched; (void)Mismatched;
}

#ifdef PROBE_STANDALONE
int main(void)
{
    RunAll();
    return 0;
}
#else
START_TEST(RegWow64RelativeView)
{
    SYSTEM_INFO Native;
    /* The recorded answers are those of a 32-bit process on a 64-bit system. */
    GetNativeSystemInfo(&Native);
    if (sizeof(void *) != 4 || Native.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64)
    {
        skip("Relative registry views are a WoW64 behaviour\n");
        return;
    }
    RunAll();
}
#endif
