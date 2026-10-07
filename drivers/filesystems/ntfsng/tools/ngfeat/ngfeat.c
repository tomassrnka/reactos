/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Guest-side checks of short (8.3) names and named streams
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * usage: ngfeat DIR   (DIR is created; one NGF: line per check, NGF:DONE pass=N fail=M at the end)
 */
#include <windows.h>
#include <winioctl.h>
#ifndef FSCTL_REQUEST_OPLOCK_LEVEL_1
#define FSCTL_REQUEST_OPLOCK_LEVEL_1 CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_OPLOCK_BREAK_ACKNOWLEDGE CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#include <stdio.h>
#include <string.h>

static int Pass, Fail;
static HANDLE OplockHandle;
static OVERLAPPED OplockOv;

/* Waits for the oplock break, acknowledges it and closes the handle so the waiting open goes on. */
static DWORD WINAPI breaker(LPVOID arg)
{
    DWORD n;
    (void)arg;
    if (WaitForSingleObject(OplockOv.hEvent, 20000) != WAIT_OBJECT_0)
        return 1;
    DeviceIoControl(OplockHandle, FSCTL_OPLOCK_BREAK_ACKNOWLEDGE, NULL, 0, NULL, 0, &n, NULL);
    CloseHandle(OplockHandle);
    return 0;
}

static void report(const char *name, int ok, const char *fmt, ...)
{
    char line[600], detail[400] = "";
    va_list ap;
    if (fmt)
    {
        va_start(ap, fmt);
        _vsnprintf(detail, sizeof(detail) - 1, fmt, ap);
        va_end(ap);
    }
    _snprintf(line, sizeof(line) - 1, "NGF:%s %s %s\n", ok ? "PASS" : "FAIL", name, detail);
    line[sizeof(line) - 1] = 0;
    OutputDebugStringA(line);
    fputs(line, stdout);
    if (ok) Pass++; else Fail++;
}

static int writefile(const char *p, const char *data, DWORD disp)
{
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, disp, 0, NULL);
    DWORD n;
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(h, data, (DWORD)strlen(data), &n, NULL);
    CloseHandle(h);
    return ok && n == strlen(data);
}

static int readfile(const char *p, char *buf, DWORD size)
{
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0;
    if (h == INVALID_HANDLE_VALUE) return -1;
    ReadFile(h, buf, size - 1, &n, NULL);
    buf[n] = 0;
    CloseHandle(h);
    return (int)n;
}

int main(int argc, char **argv)
{
    char dir[MAX_PATH], p[MAX_PATH], q[MAX_PATH], sp[MAX_PATH], buf[256];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    DWORD n;

    if (argc < 2) return 2;
    strcpy(dir, argv[1]);
    CreateDirectoryA(dir, NULL);

    /* short names */
    sprintf(p, "%s\\Long File Name One.Document", dir);
    report("create-long", writefile(p, "one", CREATE_ALWAYS), "%s", p);
    n = GetShortPathNameA(p, sp, sizeof(sp));
    report("shortpath", n && strstr(sp, "~1") != NULL, "%s", n ? sp : "(none)");
    h = FindFirstFileA(p, &fd);
    report("find-alternate", h != INVALID_HANDLE_VALUE && fd.cAlternateFileName[0], "alt=%s", h != INVALID_HANDLE_VALUE ? fd.cAlternateFileName : "?");
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    report("open-by-short", n && readfile(sp, buf, sizeof(buf)) == 3 && !strcmp(buf, "one"), "%s", sp);
    sprintf(q, "%s\\*~1.DOC", dir);
    h = FindFirstFileA(q, &fd);
    report("pattern-short", h != INVALID_HANDLE_VALUE, "%s -> %s", q, h != INVALID_HANDLE_VALUE ? fd.cFileName : "none");
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    sprintf(q, "%s\\Long File Name Two.Document", dir);
    writefile(q, "two", CREATE_ALWAYS);
    n = GetShortPathNameA(q, sp, sizeof(sp));
    report("second-short-unique", n && strstr(sp, "~2") != NULL, "%s", n ? sp : "(none)");
    sprintf(q, "%s\\Renamed Long Name.Text", dir);
    report("rename", MoveFileA(p, q), NULL);
    n = GetShortPathNameA(q, sp, sizeof(sp));
    report("short-after-rename", n && strstr(sp, "RENAME~") != NULL, "%s", n ? sp : "(none)");
    sprintf(p, "%s\\short.txt", dir);
    writefile(p, "s", CREATE_ALWAYS);
    h = FindFirstFileA(p, &fd);
    report("no-alternate-for-83", h != INVALID_HANDLE_VALUE && !fd.cAlternateFileName[0], "alt=%s", h != INVALID_HANDLE_VALUE ? fd.cAlternateFileName : "?");
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    report("delete-by-short", n && DeleteFileA(sp) && GetFileAttributesA(q) == INVALID_FILE_ATTRIBUTES, "%s", sp);
    sprintf(p, "%s\\A Directory With Long Name", dir);
    report("mkdir-long", CreateDirectoryA(p, NULL), NULL);
    n = GetShortPathNameA(p, sp, sizeof(sp));
    report("dir-shortpath", n && strstr(sp, "~1") != NULL, "%s", n ? sp : "(none)");

    /* named streams */
    sprintf(p, "%s\\streams.txt", dir);
    writefile(p, "main", CREATE_ALWAYS);
    sprintf(q, "%s:alpha", p);
    report("stream-create", writefile(q, "alpha-data", CREATE_ALWAYS), "%s", q);
    report("stream-read", readfile(q, buf, sizeof(buf)) == 10 && !strcmp(buf, "alpha-data"), "%s", buf);
    report("main-intact", readfile(p, buf, sizeof(buf)) == 4 && !strcmp(buf, "main"), "%s", buf);
    {
        typedef HANDLE (WINAPI *PFFS)(LPCWSTR, int, LPVOID, DWORD);
        typedef BOOL (WINAPI *PFNS)(HANDLE, LPVOID);
        PFFS ffs = (PFFS)GetProcAddress(GetModuleHandleA("kernel32.dll"), "FindFirstStreamW");
        PFNS fns = (PFNS)GetProcAddress(GetModuleHandleA("kernel32.dll"), "FindNextStreamW");
        if (ffs && fns)
        {
            struct { LARGE_INTEGER Size; WCHAR Name[MAX_PATH + 36]; } sd;
            WCHAR wp[MAX_PATH];
            int found = 0, count = 0;
            MultiByteToWideChar(CP_ACP, 0, p, -1, wp, MAX_PATH);
            h = ffs(wp, 0, &sd, 0);
            if (h != INVALID_HANDLE_VALUE)
            {
                do { count++; if (!wcscmp(sd.Name, L":alpha:$DATA")) found = 1; } while (fns(h, &sd));
                FindClose(h);
            }
            report("stream-list", found && count == 2, "count=%d", count);
        }
        else
            report("stream-list", 1, "(FindFirstStreamW not available: skipped)");
    }
    sprintf(q, "%s\\newfile.txt:beta", dir);
    report("stream-on-new-file", writefile(q, "beta", CREATE_NEW), "%s", q);
    sprintf(p, "%s\\newfile.txt", dir);
    report("new-file-main-empty", readfile(p, buf, sizeof(buf)) == 0, NULL);
    sprintf(q, "%s\\streams.txt:alpha", dir);
    report("stream-delete", DeleteFileA(q) && readfile(q, buf, sizeof(buf)) < 0, NULL);
    sprintf(p, "%s\\streams.txt", dir);
    report("main-after-stream-delete", readfile(p, buf, sizeof(buf)) == 4, NULL);
    sprintf(q, "%s:dirstream", dir);
    report("stream-on-directory", writefile(q, "d", CREATE_ALWAYS), NULL);

    /* oplocks: the driver refuses them; the request must fail cleanly without pending */
    sprintf(p, "%s\\oplock.txt", dir);
    writefile(p, "oplock", CREATE_ALWAYS);
    OplockHandle = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    OplockOv.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    {
        DWORD k = 0, t0, ms;
        BOOL r = DeviceIoControl(OplockHandle, FSCTL_REQUEST_OPLOCK_LEVEL_1, NULL, 0, NULL, 0, &k, &OplockOv);
        DWORD e = GetLastError();
        report("oplock-refused", !r && e == ERROR_OPLOCK_NOT_GRANTED, "r=%d err=%lu", r, e);
        if (!r && e == ERROR_IO_PENDING)
        {
            HANDLE th = CreateThread(NULL, 0, breaker, NULL, 0, NULL), h2;
            t0 = GetTickCount();
            h2 = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            ms = GetTickCount() - t0;
            report("oplock-break-open", h2 != INVALID_HANDLE_VALUE && WaitForSingleObject(OplockOv.hEvent, 0) == WAIT_OBJECT_0,
                   "err=%lu after %lu ms", h2 == INVALID_HANDLE_VALUE ? GetLastError() : 0, ms);
            if (h2 != INVALID_HANDLE_VALUE) CloseHandle(h2);
            if (th) { WaitForSingleObject(th, 20000); CloseHandle(th); }
        }
        else
            CloseHandle(OplockHandle);
    }

    /* open by file ID (NtCreateFile with FILE_OPEN_BY_FILE_ID relative to a directory handle) */
    {
        typedef LONG (WINAPI *PNTCF)(PHANDLE, ACCESS_MASK, PVOID, PVOID, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
        typedef LONG (WINAPI *PNTQIF)(HANDLE, PVOID, PVOID, ULONG, ULONG);
        PNTCF ntcf = (PNTCF)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtCreateFile");
        PNTQIF ntqif = (PNTQIF)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationFile");
        BY_HANDLE_FILE_INFORMATION bi;
        HANDLE f, root, byid = NULL;
        struct { USHORT Length, MaximumLength; PVOID Buffer; } us;
        struct { ULONG Length; HANDLE RootDirectory; PVOID ObjectName; ULONG Attributes; PVOID Sd, Sqos; } oa;
        struct { LONG Status; ULONG_PTR Information; } iosb;
        ULONGLONG id;
        LONG st = -1;
        char nameinfo[600];
        sprintf(p, "%s\\byid-target.txt", dir);
        writefile(p, "byid", CREATE_ALWAYS);
        f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        root = CreateFileA(dir, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (f != INVALID_HANDLE_VALUE && root != INVALID_HANDLE_VALUE && GetFileInformationByHandle(f, &bi) && ntcf)
        {
            id = ((ULONGLONG)bi.nFileIndexHigh << 32) | bi.nFileIndexLow;
            us.Length = us.MaximumLength = sizeof(id); us.Buffer = &id;
            oa.Length = sizeof(oa); oa.RootDirectory = root; oa.ObjectName = &us; oa.Attributes = 0x40; oa.Sd = oa.Sqos = NULL;
            st = ntcf(&byid, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, 0, FILE_SHARE_READ, 1 /* FILE_OPEN */,
                      0x2000 /* FILE_OPEN_BY_FILE_ID */ | 0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */, NULL, 0);
        }
        report("open-by-id", st == 0, "status=0x%08lx", (unsigned long)st);
        if (st == 0)
        {
            DWORD k;
            buf[0] = 0;
            ReadFile(byid, buf, 4, &k, NULL); buf[k < 4 ? k : 4] = 0;
            report("by-id-content", !strcmp(buf, "byid"), "%s", buf);
            if (ntqif && !ntqif(byid, &iosb, nameinfo, sizeof(nameinfo), 9 /* FileNameInformation */))
            {
                char narrow[300];
                ULONG wl = *(ULONG *)nameinfo;
                WideCharToMultiByte(CP_ACP, 0, (WCHAR *)(nameinfo + 4), wl / 2, narrow, sizeof(narrow) - 1, NULL, NULL);
                narrow[wl / 2 < sizeof(narrow) - 1 ? wl / 2 : sizeof(narrow) - 1] = 0;
                report("by-id-name", strstr(narrow, "byid-target.txt") != NULL, "%s", narrow);
            }
            CloseHandle(byid);
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (root != INVALID_HANDLE_VALUE) CloseHandle(root);
    }

    /* volume lock: refused while other files are open (the system volume always has some) */
    {
        char vol[8] = "\\\\.\\C:";
        HANDLE v;
        DWORD k;
        vol[4] = dir[0];
        v = CreateFileA(vol, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (v != INVALID_HANDLE_VALUE)
        {
            BOOL r = DeviceIoControl(v, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &k, NULL);
            report("lock-busy-volume-refused", !r && GetLastError() == ERROR_ACCESS_DENIED, "r=%d err=%lu", r, GetLastError());
            if (r) DeviceIoControl(v, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &k, NULL);
            CloseHandle(v);
        }
        else
            report("lock-busy-volume-refused", 0, "cannot open %s (%lu)", vol, GetLastError());
    }

    _snprintf(buf, sizeof(buf), "NGF:DONE pass=%d fail=%d\n", Pass, Fail);
    OutputDebugStringA(buf);
    fputs(buf, stdout);
    return Fail ? 1 : 0;
}
