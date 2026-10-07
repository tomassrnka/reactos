/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Guest-side file system workload with a verifying model and a manifest
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * ngstress: a C file system workload (the Go workloads need Win7 APIs that some builds lack).
 * Whole-file writes, shrinks, renames, deletes and directories under -root, every read verified
 * against a model (contents are regenerated from (file id, version), nothing is stored), status
 * lines to the kernel debug log (NGS:), and a manifest in the ngstress format (F path size sha256 /
 * D path) for the offline comparison.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXF 256
#define NDIRS 8
typedef struct { int exists, dir; unsigned len, ver, cut; } FENT;
static FENT F[MAXF];
static char Root[MAX_PATH];
static unsigned long long Rng;
static int LongNames;      /* -long 1: names that are not valid 8.3 names (DOS names get generated) */
static int Fails, Ops;
static unsigned char *Buf, *Buf2;

static unsigned rnd(void) { Rng = Rng * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)(Rng >> 33); }

static void dbg(const char *fmt, ...)
{
    char line[512]; va_list ap; int n;
    strcpy(line, "NGS:");
    va_start(ap, fmt); n = _vsnprintf(line + 4, sizeof(line) - 6, fmt, ap); va_end(ap);
    if (n < 0) n = (int)sizeof(line) - 6;
    strcpy(line + 4 + n, "\n");
    OutputDebugStringA(line);
    fputs(line, stdout);
}

/* content of file id at version v: bytes from a generator seeded by (id, v), first cut bytes valid */
static void gen(int id, unsigned ver, unsigned len, unsigned char *out)
{
    unsigned long long s = (unsigned long long)id * 0x9E3779B97F4A7C15ULL ^ ((unsigned long long)ver << 20) ^ 0xA5A5;
    for (unsigned i = 0; i < len; i++) {
        s = s * 6364136223846793005ULL + 1;
        out[i] = (unsigned char)(s >> 56);
    }
}

static const char *fname(int id)
{
    static char b[64];
    if (LongNames) sprintf(b, "Long File Name %03d.Data", id); else sprintf(b, "f%03d.dat", id);
    return b;
}
static void path(int id, char *p) { sprintf(p, "%s\\d%d\\%s", Root, F[id].dir, fname(id)); }

static int writefile(const char *p, const unsigned char *b, unsigned len)
{
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n = 0; BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return -1;
    ok = len ? WriteFile(h, b, len, &n, NULL) : TRUE;
    CloseHandle(h);
    return ok && n == len ? 0 : -1;
}

static int readfile(const char *p, unsigned char *b, unsigned max, unsigned *len)
{
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0; BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return -1;
    ok = ReadFile(h, b, max, &n, NULL);
    CloseHandle(h);
    *len = n;
    return ok ? 0 : -1;
}

static void verify(int id)
{
    char p[MAX_PATH]; unsigned got;
    path(id, p);
    if (readfile(p, Buf2, (1 << 21), &got)) { Fails++; dbg("FAIL read %s err=%lu", p, GetLastError()); return; }
    gen(id, F[id].ver, F[id].len, Buf);
    if (got != F[id].cut || memcmp(Buf, Buf2, got)) { Fails++; dbg("FAIL verify %s len=%u want=%u", p, got, F[id].cut); }
}

/* ---- SHA-256 for the manifest */
typedef struct { unsigned h[8]; unsigned long long len; unsigned char b[64]; unsigned n; } SHA;
static const unsigned K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,
0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,
0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void blk(SHA *s, const unsigned char *p)
{
    unsigned w[64], a, b, c, d, e, f, g, h, t1, t2; int i;
    for (i = 0; i < 16; i++) w[i] = (unsigned)p[4*i]<<24 | (unsigned)p[4*i+1]<<16 | (unsigned)p[4*i+2]<<8 | p[4*i+3];
    for (i = 16; i < 64; i++) w[i] = w[i-16] + (ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3)) + w[i-7] + (ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10));
    a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
    for (i = 0; i < 64; i++) { t1=h+(ROR(e,6)^ROR(e,11)^ROR(e,25))+((e&f)^(~e&g))+K[i]+w[i]; t2=(ROR(a,2)^ROR(a,13)^ROR(a,22))+((a&b)^(a&c)^(b&c)); h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
    s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void sha(const unsigned char *p, unsigned len, char *hex)
{
    static const unsigned iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    SHA s; unsigned i; unsigned char pad[128]; unsigned long long bits = (unsigned long long)len * 8; unsigned rem, plen;
    memcpy(s.h, iv, sizeof(iv));
    for (i = 0; i + 64 <= len; i += 64) blk(&s, p + i);
    rem = len - i; memset(pad, 0, sizeof(pad)); memcpy(pad, p + i, rem); pad[rem] = 0x80;
    plen = rem < 56 ? 64 : 128;
    for (int k = 0; k < 8; k++) pad[plen - 1 - k] = (unsigned char)(bits >> (8 * k));
    blk(&s, pad); if (plen == 128) blk(&s, pad + 64);
    for (i = 0; i < 8; i++) sprintf(hex + 8 * i, "%08x", s.h[i]);
}

int main(int argc, char **argv)
{
    unsigned seed = 1, dur = 120; const char *man = NULL; char p[MAX_PATH], q[MAX_PATH];
    DWORD t0, last;
    strcpy(Root, "C:\\ngstc");
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-root")) strcpy(Root, argv[i + 1]);
        else if (!strcmp(argv[i], "-seed")) seed = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-dur")) dur = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "-manifest")) man = argv[i + 1];
        else if (!strcmp(argv[i], "-long")) LongNames = atoi(argv[i + 1]);
    }
    Rng = seed * 2654435761ULL + 7;
    Buf = malloc(1 << 21); Buf2 = malloc(1 << 21);
    if (!Buf || !Buf2) return 3;
    if (!CreateDirectoryA(Root, NULL)) { dbg("FATAL root %s exists or cannot be created (%lu)", Root, GetLastError()); return 2; }
    for (int d = 0; d < NDIRS; d++) { sprintf(p, "%s\\d%d", Root, d); CreateDirectoryA(p, NULL); }
    dbg("START root=%s seed=%u dur=%us", Root, seed, dur);
    t0 = last = GetTickCount();
    while (GetTickCount() - t0 < dur * 1000) {
        int id = rnd() % MAXF, k = rnd() % 100;
        Ops++;
        if (!F[id].exists || k < 35) {
            unsigned len = rnd() % ((rnd() % 8) ? 65536 : (1 << 21));
            if (!F[id].exists) F[id].dir = rnd() % NDIRS;
            path(id, p);
            gen(id, F[id].ver + 1, len, Buf);
            if (writefile(p, Buf, len)) { Fails++; dbg("FAIL write %s err=%lu", p, GetLastError()); continue; }
            F[id].exists = 1; F[id].ver++; F[id].len = F[id].cut = len;
        } else if (k < 50) {
            unsigned cut = F[id].cut ? rnd() % F[id].cut : 0; HANDLE h;
            path(id, p);
            h = CreateFileA(p, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE || SetFilePointer(h, cut, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER || !SetEndOfFile(h))
                { Fails++; dbg("FAIL shrink %s err=%lu", p, GetLastError()); }
            else F[id].cut = cut;
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        } else if (k < 65) {
            int nd = rnd() % NDIRS;
            path(id, p); sprintf(q, "%s\\d%d\\%s", Root, nd, fname(id));
            if (nd != F[id].dir) {
                if (!MoveFileExA(p, q, 0)) { Fails++; dbg("FAIL rename %s err=%lu", p, GetLastError()); }
                else F[id].dir = nd;
            }
        } else if (k < 75) {
            path(id, p);
            if (!DeleteFileA(p)) { Fails++; dbg("FAIL delete %s err=%lu", p, GetLastError()); }
            else F[id].exists = 0;
        } else {
            verify(id);
        }
        if (GetTickCount() - last > 20000) { last = GetTickCount(); dbg("PROGRESS ops=%d fails=%d t=%lus", Ops, Fails, (GetTickCount() - t0) / 1000); }
    }
    for (int id = 0; id < MAXF; id++) if (F[id].exists) verify(id);
    if (man) {
        FILE *m = fopen(man, "w");
        char hex[65];
        for (int d = 0; d < NDIRS; d++) if (m) fprintf(m, "D d%d\n", d);
        for (int id = 0; id < MAXF && m; id++) if (F[id].exists) {
            gen(id, F[id].ver, F[id].len, Buf); sha(Buf, F[id].cut, hex);
            fprintf(m, "F d%d/%s %u %s\n", F[id].dir, fname(id), F[id].cut, hex);
        }
        if (m) fclose(m);
    }
    dbg("DONE ops=%d fails=%d", Ops, Fails);
    return Fails ? 1 : 0;
}
