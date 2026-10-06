/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Guest-side checker: verifies files against ntfs-3g reference manifests,
 *              lists directory trees and named streams through the Win32 API
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * usage: ngchk auto MANIFESTDIR OUTDIR | ngchk wait FILE SECONDS
 *   For every drive D: to Z: whose volume serial has MANIFESTDIR\<serial>.manifest,
 *   writes OUTDIR\<serial>.verify (one line per manifest entry), OUTDIR\<serial>.dirs
 *   (FindFirstFile walk) and OUTDIR\<serial>.streams (FileStreamInformation walk).
 */

#define WIN32_NO_STATUS
#include <windows.h>
#define NTOS_MODE_USER
#include <ndk/iofuncs.h>
#include <ndk/rtlfuncs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned int h[8]; unsigned long long len; unsigned char buf[64]; size_t n; } SHA;
static const unsigned int K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(SHA *s, const unsigned char *p)
{
    unsigned int w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (unsigned int)p[4*i] << 24 | (unsigned int)p[4*i+1] << 16 | (unsigned int)p[4*i+2] << 8 | p[4*i+3];
    for (i = 16; i < 64; i++)
    {
        unsigned int s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        unsigned int s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++)
    {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(SHA *s)
{
    static const unsigned int iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h, iv, sizeof(iv)); s->len = 0; s->n = 0;
}

static void sha_update(SHA *s, const unsigned char *p, size_t l)
{
    s->len += l;
    while (l)
    {
        size_t k = 64 - s->n < l ? 64 - s->n : l;
        memcpy(s->buf + s->n, p, k); s->n += k; p += k; l -= k;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
}

static void sha_final(SHA *s, char out[65])
{
    unsigned long long bits = s->len * 8;
    unsigned char pad = 0x80, z = 0, lb[8];
    int i;
    sha_update(s, &pad, 1);
    while (s->n != 56) sha_update(s, &z, 1);
    for (i = 0; i < 8; i++) lb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha_update(s, lb, 8);
    for (i = 0; i < 8; i++) sprintf(out + 8 * i, "%08x", s->h[i]);
}

static HANDLE OutFile;
static unsigned char *IoBuf;
#define IOBUF (256 * 1024)

static void out_utf8(const char *s)
{
    DWORD w;
    WriteFile(OutFile, s, (DWORD)strlen(s), &w, NULL);
}

static void out_w(const WCHAR *s, int len)
{
    char tmp[2048];
    int n;
    if (len < 0)
        len = (int)wcslen(s);
    n = WideCharToMultiByte(CP_UTF8, 0, s, len, tmp, sizeof(tmp) - 1, NULL, NULL);
    DWORD w;
    if (n > 0)
        WriteFile(OutFile, tmp, n, &w, NULL);
}

static HANDLE open_out(const WCHAR *dir, const WCHAR *name, const WCHAR *ext)
{
    WCHAR path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%s\\%s%s", dir, name, ext);
    return CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
}

/* Hashes one file or stream; returns 0 or the Win32 error. */
static DWORD hash_file(const WCHAR *path, unsigned long long *size, char hex[65])
{
    SHA s;
    DWORD got, err = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();
    sha_init(&s);
    *size = 0;
    for (;;)
    {
        if (!ReadFile(h, IoBuf, IOBUF, &got, NULL))
        {
            err = GetLastError();
            if (err == ERROR_HANDLE_EOF)
                err = 0;
            break;
        }
        if (!got)
            break;
        sha_update(&s, IoBuf, got);
        *size += got;
    }
    CloseHandle(h);
    sha_final(&s, hex);
    return err;
}

static long nverify, nok, nbad, nerr;

static void verify(WCHAR drive, const WCHAR *mdir, const WCHAR *serial, const WCHAR *odir)
{
    WCHAR mpath[MAX_PATH], wpath[4096];
    char *data, *line, *next, outl[1024];
    HANDLE mf;
    DWORD msize, got;

    _snwprintf(mpath, MAX_PATH, L"%s\\%s.manifest", mdir, serial);
    mf = CreateFileW(mpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (mf == INVALID_HANDLE_VALUE)
        return;
    msize = GetFileSize(mf, NULL);
    data = malloc(msize + 1);
    if (!data || !ReadFile(mf, data, msize, &got, NULL))
    {
        CloseHandle(mf);
        return;
    }
    data[got] = 0;
    CloseHandle(mf);
    OutFile = open_out(odir, serial, L".verify");
    nverify = nok = nbad = nerr = 0;
    for (line = data; line && *line; line = next)
    {
        char *type, *rpath, *rsize, *rhash, hex[65];
        unsigned long long size = 0;
        DWORD err;
        int n, i;
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        type = line;
        rpath = strchr(type, '\t'); if (!rpath) continue; *rpath++ = 0;
        rsize = strchr(rpath, '\t'); if (!rsize) continue; *rsize++ = 0;
        rhash = strchr(rsize, '\t'); if (!rhash) continue; *rhash++ = 0;
        n = rhash ? (int)strlen(rhash) : 0;
        if (n && rhash[n - 1] == '\r') rhash[n - 1] = 0;
        wpath[0] = drive; wpath[1] = L':'; wpath[2] = L'\\';
        n = MultiByteToWideChar(CP_UTF8, 0, rpath, -1, wpath + 3, 4096 - 4);
        for (i = 3; i < 3 + n; i++)
            if (wpath[i] == L'/') wpath[i] = L'\\';
        nverify++;
        if (!strcmp(type, "d"))
        {
            DWORD a = GetFileAttributesW(wpath);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
            {
                nok++;
                _snprintf(outl, sizeof(outl), "OK\t%s\t", type);
            }
            else
            {
                nbad++;
                _snprintf(outl, sizeof(outl), "BAD-DIR(%lx,%lu)\t%s\t", a, GetLastError(), type);
            }
            out_utf8(outl); out_utf8(rpath); out_utf8("\n");
            continue;
        }
        err = hash_file(wpath, &size, hex);
        if (err)
        {
            nerr++;
            _snprintf(outl, sizeof(outl), "ERR(%lu)\t%s\t", err, type);
            out_utf8(outl); out_utf8(rpath); out_utf8("\n");
            continue;
        }
        if (!strcmp(hex, rhash) && size == _strtoui64(rsize, NULL, 10))
        {
            nok++;
            _snprintf(outl, sizeof(outl), "OK\t%s\t", type);
        }
        else
        {
            nbad++;
            _snprintf(outl, sizeof(outl), "MISMATCH(%I64u,%s)\t%s\t", size, hex, type);
        }
        out_utf8(outl); out_utf8(rpath); out_utf8("\n");
    }
    _snprintf(outl, sizeof(outl), "SUMMARY\tentries=%ld ok=%ld bad=%ld err=%ld\n", nverify, nok, nbad, nerr);
    out_utf8(outl);
    CloseHandle(OutFile);
    free(data);
    printf("ngchk %c: %s", (char)drive, outl);
}

static HANDLE DirsFile, StreamsFile;

static void list_streams(const WCHAR *full, const WCHAR *rel)
{
    IO_STATUS_BLOCK iosb;
    NTSTATUS st;
    PFILE_STREAM_INFORMATION p;
    char tmp[64];
    HANDLE h = CreateFileW(full, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    st = NtQueryInformationFile(h, &iosb, IoBuf, IOBUF, FileStreamInformation);
    CloseHandle(h);
    if (!NT_SUCCESS(st) || !iosb.Information)
        return;
    OutFile = StreamsFile;
    for (p = (PVOID)IoBuf;; p = (PVOID)((PUCHAR)p + p->NextEntryOffset))
    {
        out_w(rel, -1);
        out_utf8("\t");
        out_w(p->StreamName, p->StreamNameLength / sizeof(WCHAR));
        _snprintf(tmp, sizeof(tmp), "\t%I64u\n", p->StreamSize.QuadPart);
        out_utf8(tmp);
        if (!p->NextEntryOffset)
            break;
    }
}

static void walk(WCHAR *full, size_t flen, WCHAR *rel, size_t rlen)
{
    WIN32_FIND_DATAW fd;
    HANDLE f;
    wcscpy(full + flen, L"\\*");
    f = FindFirstFileW(full, &fd);
    full[flen] = 0;
    if (f == INVALID_HANDLE_VALUE)
        return;
    do
    {
        size_t nl = wcslen(fd.cFileName);
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        OutFile = DirsFile;
        if (rlen) out_w(rel, (int)rlen); else out_utf8(".");
        out_utf8("\t");
        out_w(fd.cFileName, (int)nl);
        out_utf8("\n");
        full[flen] = L'\\';
        wcscpy(full + flen + 1, fd.cFileName);
        if (rlen) { rel[rlen] = L'/'; wcscpy(rel + rlen + 1, fd.cFileName); }
        else wcscpy(rel, fd.cFileName);
        list_streams(full, rel);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            walk(full, flen + 1 + nl, rel, rlen ? rlen + 1 + nl : nl);
        full[flen] = 0;
        rel[rlen] = 0;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

int wmain(int argc, WCHAR **argv)
{
    WCHAR drive, root[4], label[64], fsname[32], serial[16], mpath[MAX_PATH];
    static WCHAR full[8192], rel[8192];
    DWORD ser, maxc, flags;
    HANDLE vol;
    char line[256];

    if (argc == 4 && !wcscmp(argv[1], L"wait"))
    {
        /* ngchk wait FILE SECONDS: poll for FILE (the payload has no other way to join a background job). */
        int t;
        for (t = 0; t < _wtoi(argv[3]) * 2; t++)
        {
            if (GetFileAttributesW(argv[2]) != INVALID_FILE_ATTRIBUTES)
                return 0;
            Sleep(500);
        }
        return 1;
    }
    if (argc != 4 || wcscmp(argv[1], L"auto"))
    {
        printf("usage: ngchk auto MANIFESTDIR OUTDIR | ngchk wait FILE SECONDS\n");
        return 2;
    }
    IoBuf = malloc(IOBUF);
    vol = open_out(argv[3], L"volumes", L".txt");
    for (drive = L'D'; drive <= L'Z'; drive++)
    {
        _snwprintf(root, 4, L"%c:\\", drive);
        if (!GetVolumeInformationW(root, label, 64, &ser, &maxc, &flags, fsname, 32))
            continue;
        _snwprintf(serial, 16, L"%08lx", ser);
        OutFile = vol;
        _snprintf(line, sizeof(line), "%c:\tserial=%08lx\tflags=%08lx\tmaxcomp=%lu\tfs=", (char)drive, ser, flags, maxc);
        out_utf8(line); out_w(fsname, -1); out_utf8("\tlabel="); out_w(label, -1); out_utf8("\n");
        _snwprintf(mpath, MAX_PATH, L"%s\\%s.manifest", argv[2], serial);
        if (GetFileAttributesW(mpath) == INVALID_FILE_ATTRIBUTES)
            continue;
        printf("ngchk %c: serial %08lx, checking\n", (char)drive, ser);
        verify(drive, argv[2], serial, argv[3]);
        DirsFile = open_out(argv[3], serial, L".dirs");
        StreamsFile = open_out(argv[3], serial, L".streams");
        _snwprintf(full, 8192, L"%c:", drive);
        rel[0] = 0;
        walk(full, 2, rel, 0);
        CloseHandle(DirsFile);
        CloseHandle(StreamsFile);
    }
    CloseHandle(vol);
    return 0;
}
