/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Guest-side checks of volume lock, dismount and volume-handle writes
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * usage: ngvol X   (X: a mounted data volume with no other open files; one NGV: line per check,
 *                   NGV:DONE pass=N fail=M at the end; INFO lines record behaviour without a verdict)
 * The volume handle writes only bytes it has just read from the same place.
 */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>

static int Pass, Fail;

static void line(const char *kind, const char *name, const char *fmt, ...)
{
    char out[600], detail[400] = "";
    va_list ap;
    if (fmt)
    {
        va_start(ap, fmt);
        _vsnprintf(detail, sizeof(detail) - 1, fmt, ap);
        va_end(ap);
    }
    _snprintf(out, sizeof(out) - 1, "NGV:%s %s %s\n", kind, name, detail);
    out[sizeof(out) - 1] = 0;
    OutputDebugStringA(out);
    fputs(out, stdout);
    fflush(stdout);
}

static void check(const char *name, int ok, DWORD err)
{
    line(ok ? "PASS" : "FAIL", name, "err=%lu", err);
    if (ok) Pass++; else Fail++;
}

/* 0 on success, else the Win32 error. */
static DWORD fsctl(HANDLE h, DWORD code)
{
    DWORD n;
    return DeviceIoControl(h, code, NULL, 0, NULL, 0, &n, NULL) ? 0 : GetLastError();
}

static DWORD rw(HANDLE h, BOOL write, LONGLONG off, void *buf, DWORD len)
{
    LARGE_INTEGER pos;
    DWORD n = 0;
    BOOL ok;
    pos.QuadPart = off;
    if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN))
        return GetLastError();
    ok = write ? WriteFile(h, buf, len, &n, NULL) : ReadFile(h, buf, len, &n, NULL);
    if (!ok)
        return GetLastError();
    return n == len ? 0 : ERROR_HANDLE_EOF;
}

static HANDLE openvol(const char *vol, DWORD access)
{
    return CreateFileA(vol, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static int readkeep(const char *path, char *buf, DWORD size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0;
    if (h == INVALID_HANDLE_VALUE)
        return -(int)GetLastError();
    ReadFile(h, buf, size - 1, &n, NULL);
    buf[n] = 0;
    CloseHandle(h);
    return (int)n;
}

int main(int argc, char **argv)
{
    static const char Keep[] = "ngvol-keep-0123456789";
    char vol[16], dir[32], keep[64], buf[64];
    HANDLE v, f;
    DWORD e, n;
    BYTE *sec, *sec2;
    const LONGLONG Far = 1024 * 1024;   /* inside the file system's area on any test volume */
    int r;

    if (argc < 2 || !argv[1][0])
    {
        printf("usage: ngvol X\n");
        return 2;
    }
    _snprintf(vol, sizeof(vol), "\\\\.\\%c:", argv[1][0]);
    _snprintf(dir, sizeof(dir), "%c:\\ngvol", argv[1][0]);
    _snprintf(keep, sizeof(keep), "%s\\keep.txt", dir);
    sec = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    sec2 = sec + 4096;

    CreateDirectoryA(dir, NULL);
    f = CreateFileA(keep, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE || !WriteFile(f, Keep, sizeof(Keep) - 1, &n, NULL))
    {
        line("FAIL", "setup", "cannot write %s: %lu", keep, GetLastError());
        return 1;
    }
    FlushFileBuffers(f);
    CloseHandle(f);

    /* An open for attributes only goes to the device, not to the file system (informational). */
    v = openvol(vol, FILE_READ_ATTRIBUTES | SYNCHRONIZE);
    if (v == INVALID_HANDLE_VALUE)
    {
        line("INFO", "weak-open", "err=%lu", GetLastError());
    }
    else
    {
        e = fsctl(v, FSCTL_LOCK_VOLUME);
        line("INFO", "attributes-handle-lock", "err=%lu", e);
        if (!e)
            fsctl(v, FSCTL_UNLOCK_VOLUME);
        e = fsctl(v, FSCTL_DISMOUNT_VOLUME);
        line("INFO", "attributes-handle-dismount", "err=%lu", e);
        CloseHandle(v);
    }

    v = openvol(vol, GENERIC_READ | GENERIC_WRITE);
    if (v == INVALID_HANDLE_VALUE)
    {
        line("FAIL", "open-volume", "err=%lu", GetLastError());
        return 1;
    }

    /* Lock: refused while a file is open, granted once it is closed; then other opens fail. */
    f = CreateFileA(keep, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    e = fsctl(v, FSCTL_LOCK_VOLUME);
    check("lock-busy-refused", e == ERROR_ACCESS_DENIED, e);
    if (!e)
        fsctl(v, FSCTL_UNLOCK_VOLUME);
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    e = fsctl(v, FSCTL_LOCK_VOLUME);
    check("lock-idle-granted", e == 0, e);
    f = CreateFileA(keep, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    e = f == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    check("open-while-locked-refused", f == INVALID_HANDLE_VALUE, e);
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);

    /* The lock holder writes the volume anywhere (format does this before or without a dismount). */
    e = rw(v, FALSE, Far, sec, 4096);
    if (!e)
        e = rw(v, TRUE, Far, sec, 4096);
    check("locked-volume-write", e == 0, e);

    /* Dismount: the dismounting handle keeps raw access; a second dismount is harmless. */
    e = fsctl(v, FSCTL_DISMOUNT_VOLUME);
    check("dismount-locked", e == 0, e);
    e = rw(v, FALSE, 0, sec2, 4096);
    check("read-after-dismount", e == 0 && !memcmp(sec2 + 3, "NTFS    ", 8), e);
    e = rw(v, FALSE, Far, sec2, 4096);
    if (!e)
        e = rw(v, TRUE, Far, sec2, 4096);
    check("write-after-dismount", e == 0 && !memcmp(sec, sec2, 4096), e);
    e = fsctl(v, FSCTL_DISMOUNT_VOLUME);
    check("second-dismount-reports-dismounted", e == ERROR_NOT_READY, e);
    e = fsctl(v, FSCTL_UNLOCK_VOLUME);
    check("unlock-after-dismount", e == 0, e);
    CloseHandle(v);

    /* The next open mounts the volume again; the file is intact. */
    r = readkeep(keep, buf, sizeof(buf));
    check("remount-file-intact", r == (int)sizeof(Keep) - 1 && !strcmp(buf, Keep), r < 0 ? (DWORD)-r : 0);

    /* Without a lock the volume handle may not overwrite the file system's sectors. */
    v = openvol(vol, GENERIC_READ | GENERIC_WRITE);
    if (v != INVALID_HANDLE_VALUE)
    {
        e = rw(v, FALSE, Far, sec, 4096);
        if (!e)
            e = rw(v, TRUE, Far, sec, 4096);
        line("INFO", "unlocked-volume-write", "err=%lu", e);
        /* Dismount without a lock while a file is open (Windows forces it). */
        f = CreateFileA(keep, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        e = fsctl(v, FSCTL_DISMOUNT_VOLUME);
        line("INFO", "dismount-unlocked-busy", "err=%lu", e);
        if (f != INVALID_HANDLE_VALUE)
        {
            n = 0;
            e = ReadFile(f, buf, 8, &n, NULL) ? 0 : GetLastError();
            line("INFO", "read-open-file-after-forced-dismount", "err=%lu", e);
            CloseHandle(f);
        }
        CloseHandle(v);
        r = readkeep(keep, buf, sizeof(buf));
        check("file-intact-at-end", r == (int)sizeof(Keep) - 1 && !strcmp(buf, Keep), r < 0 ? (DWORD)-r : 0);
    }

    DeleteFileA(keep);
    RemoveDirectoryA(dir);
    printf("NGV:DONE pass=%d fail=%d\n", Pass, Fail);
    OutputDebugStringA(Fail ? "NGV:DONE FAIL\n" : "NGV:DONE PASS\n");
    return Fail;
}
