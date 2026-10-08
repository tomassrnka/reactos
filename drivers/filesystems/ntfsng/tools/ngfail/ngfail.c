/*
 * PROJECT:     ReactOS NTFS-NG file system driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Guest-side checks that the features ntfsng does not implement fail with their
 *              documented status, change nothing, and leave the volume usable
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * usage: ngfail DIR [SPARSEFILE COMPRESSEDFILE]
 *   DIR is created.  SPARSEFILE and COMPRESSEDFILE are existing sparse and compressed files (made
 *   by another NTFS implementation) whose first 64 KB hold the pattern (i * 7 + 3) & 0xff.
 *   One NGX: line per check, NGX:DONE pass=N fail=M at the end.
 */
#define WIN32_NO_STATUS
#include <windows.h>
#define NTOS_MODE_USER
#include <ndk/iofuncs.h>
#include <ndk/rtlfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/mmfuncs.h>
#include <ndk/setypes.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>

#ifndef FSCTL_REQUEST_OPLOCK_LEVEL_1
#define FSCTL_REQUEST_OPLOCK_LEVEL_1 CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_REQUEST_OPLOCK_LEVEL_2
#define FSCTL_REQUEST_OPLOCK_LEVEL_2 CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 1, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_OPLOCK_BREAK_ACKNOWLEDGE
#define FSCTL_OPLOCK_BREAK_ACKNOWLEDGE CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_REQUEST_BATCH_OPLOCK
#define FSCTL_REQUEST_BATCH_OPLOCK CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_REQUEST_FILTER_OPLOCK
#define FSCTL_REQUEST_FILTER_OPLOCK CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 23, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef FSCTL_SET_ZERO_DATA
#define FSCTL_SET_ZERO_DATA CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 50, METHOD_BUFFERED, FILE_WRITE_DATA)
#endif

/* FSCTL_SET_ZERO_DATA input (FILE_ZERO_DATA_INFORMATION) */
typedef struct { LARGE_INTEGER FileOffset, BeyondFinalZero; } NG_ZERO_DATA;

static int Pass, Fail;

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
    _snprintf(line, sizeof(line) - 1, "NGX:%s %s %s\n", ok ? "PASS" : "FAIL", name, detail);
    line[sizeof(line) - 1] = 0;
    OutputDebugStringA(line);
    fputs(line, stdout);
    if (ok) Pass++; else Fail++;
}

/* Opens PATH (Win32 form) with NtCreateFile so the exact status is visible. */
static NTSTATUS ntopen(const WCHAR *path, ACCESS_MASK access, ULONG disp, ULONG options, PVOID ea, ULONG ealen,
                       PSECURITY_DESCRIPTOR sd, HANDLE *h)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st;
    *h = NULL;
    if (!RtlDosPathNameToNtPathName_U(path, &name, NULL, NULL))
        return STATUS_OBJECT_PATH_INVALID;
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, sd);
    st = NtCreateFile(h, access | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disp,
                      options | FILE_SYNCHRONOUS_IO_NONALERT, ea, ealen);
    RtlFreeUnicodeString(&name);
    return st;
}

static NTSTATUS fsctl(HANDLE h, ULONG code, PVOID in, ULONG inlen, PVOID out, ULONG outlen)
{
    IO_STATUS_BLOCK iosb;
    return NtFsControlFile(h, NULL, NULL, NULL, &iosb, code, in, inlen, out, outlen);
}

/* Write, read back and compare: the file and volume still work after a refused request. */
static int usable(HANDLE h)
{
    static const char msg[] = "still usable";
    char buf[32] = "";
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    off.QuadPart = 0;
    if (!NT_SUCCESS(NtWriteFile(h, NULL, NULL, NULL, &iosb, (PVOID)msg, sizeof(msg), &off, NULL)))
        return 0;
    if (!NT_SUCCESS(NtReadFile(h, NULL, NULL, NULL, &iosb, buf, sizeof(msg), &off, NULL)))
        return 0;
    return memcmp(buf, msg, sizeof(msg)) == 0;
}

static ULONG fs_attributes(HANDLE h)
{
    UCHAR buf[sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 64];
    IO_STATUS_BLOCK iosb;
    if (!NT_SUCCESS(NtQueryVolumeInformationFile(h, &iosb, buf, sizeof(buf), FileFsAttributeInformation)))
        return 0xffffffff;
    return ((PFILE_FS_ATTRIBUTE_INFORMATION)buf)->FileSystemAttributes;
}

static ULONG file_attributes(HANDLE h)
{
    FILE_BASIC_INFORMATION bi;
    IO_STATUS_BLOCK iosb;
    if (!NT_SUCCESS(NtQueryInformationFile(h, &iosb, &bi, sizeof(bi), FileBasicInformation)))
        return 0xffffffff;
    return bi.FileAttributes;
}

/* An existing sparse or compressed file: readable with the right content, not writable. */
static void existing(const char *what, const char *path, ULONG attr)
{
    WCHAR w[MAX_PATH];
    HANDLE h;
    NTSTATUS st;
    static UCHAR buf[65536];
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    ULONG i, bad = 0;
    char name[64];

    MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH);
    _snprintf(name, sizeof(name), "%s-write-open", what);
    st = ntopen(w, FILE_WRITE_DATA, FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL, &h);
    report(name, st == STATUS_ACCESS_DENIED, "status 0x%08lx, want STATUS_ACCESS_DENIED", st);
    if (NT_SUCCESS(st)) NtClose(h);
    _snprintf(name, sizeof(name), "%s-overwrite", what);
    st = ntopen(w, FILE_READ_DATA, FILE_OVERWRITE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL, &h);
    report(name, st == STATUS_ACCESS_DENIED, "status 0x%08lx, want STATUS_ACCESS_DENIED", st);
    if (NT_SUCCESS(st)) NtClose(h);
    _snprintf(name, sizeof(name), "%s-max-allowed", what);
    st = ntopen(w, MAXIMUM_ALLOWED, FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL, &h);
    if (NT_SUCCESS(st))
    {
        OBJECT_BASIC_INFORMATION obi;
        HANDLE sec = NULL;
        NTSTATUS qs = NtQueryObject(h, ObjectBasicInformation, &obi, sizeof(obi), NULL);
        NTSTATUS ss = NtCreateSection(&sec, SECTION_ALL_ACCESS, NULL, NULL, PAGE_READWRITE, SEC_COMMIT, h);
        if (NT_SUCCESS(ss))
            NtClose(sec);
        report(name, NT_SUCCESS(qs) && !(obi.GrantedAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA)) && !NT_SUCCESS(ss),
               "granted 0x%lx (query 0x%08lx), read-write section 0x%08lx", NT_SUCCESS(qs) ? obi.GrantedAccess : 0, qs, ss);
        NtClose(h);
    }
    else
        report(name, 0, "open failed 0x%08lx", st);
    _snprintf(name, sizeof(name), "%s-max-allowed-write", what);
    st = ntopen(w, MAXIMUM_ALLOWED | FILE_WRITE_DATA, FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL, &h);
    report(name, st == STATUS_ACCESS_DENIED, "status 0x%08lx, want STATUS_ACCESS_DENIED", st);
    if (NT_SUCCESS(st)) NtClose(h);
    {
        /* With backup intent and the restore privilege the I/O manager grants write rights itself. */
        BOOLEAN was;
        if (NT_SUCCESS(RtlAdjustPrivilege(SE_RESTORE_PRIVILEGE, TRUE, FALSE, &was)))
        {
            _snprintf(name, sizeof(name), "%s-backup-max-allowed-write", what);
            st = ntopen(w, MAXIMUM_ALLOWED | FILE_WRITE_DATA, FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT,
                        NULL, 0, NULL, &h);
            report(name, st == STATUS_ACCESS_DENIED, "status 0x%08lx, want STATUS_ACCESS_DENIED", st);
            if (NT_SUCCESS(st)) NtClose(h);
            _snprintf(name, sizeof(name), "%s-backup-max-allowed", what);
            st = ntopen(w, MAXIMUM_ALLOWED, FILE_OPEN, FILE_NON_DIRECTORY_FILE | FILE_OPEN_FOR_BACKUP_INTENT, NULL, 0, NULL, &h);
            if (NT_SUCCESS(st))
            {
                OBJECT_BASIC_INFORMATION obi;
                NTSTATUS qs = NtQueryObject(h, ObjectBasicInformation, &obi, sizeof(obi), NULL);
                report(name, NT_SUCCESS(qs) && !(obi.GrantedAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA)),
                       "granted 0x%lx (query 0x%08lx)", NT_SUCCESS(qs) ? obi.GrantedAccess : 0, qs);
                NtClose(h);
            }
            else
                report(name, 0, "open failed 0x%08lx", st);
            if (!was)
                RtlAdjustPrivilege(SE_RESTORE_PRIVILEGE, FALSE, FALSE, &was);
        }
    }
    _snprintf(name, sizeof(name), "%s-read", what);
    st = ntopen(w, FILE_READ_DATA | FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL, &h);
    if (!NT_SUCCESS(st))
    {
        report(name, 0, "open for read failed 0x%08lx", st);
        return;
    }
    off.QuadPart = 0;
    st = NtReadFile(h, NULL, NULL, NULL, &iosb, buf, sizeof(buf), &off, NULL);
    for (i = 0; NT_SUCCESS(st) && i < iosb.Information; i++)
        if (buf[i] != (UCHAR)((i * 7 + 3) & 0xff))
            bad++;
    report(name, NT_SUCCESS(st) && iosb.Information == sizeof(buf) && !bad, "status 0x%08lx, %Iu bytes, %lu differ",
           st, iosb.Information, bad);
    _snprintf(name, sizeof(name), "%s-attribute", what);
    report(name, (file_attributes(h) & attr) != 0, "attributes 0x%lx", file_attributes(h));
    NtClose(h);
}

int main(int argc, char **argv)
{
    WCHAR dir[MAX_PATH], p[MAX_PATH];
    HANDLE h, v;
    NTSTATUS st;
    USHORT fmt;
    ULONG a;
    ULONG eabuf[16];
    PFILE_FULL_EA_INFORMATION ea = (PFILE_FULL_EA_INFORMATION)eabuf;
    IO_STATUS_BLOCK iosb;
    NG_ZERO_DATA zero;
    SECURITY_DESCRIPTOR sd;
    UCHAR aclbuf[256], sdout[1024];
    PACL acl = (PACL)aclbuf;
    SID_IDENTIFIER_AUTHORITY world = { SECURITY_WORLD_SID_AUTHORITY };
    PSID everyone = NULL;
    ULONG need;

    if (argc < 2)
    {
        printf("usage: ngfail DIR [SPARSEFILE COMPRESSEDFILE]\n");
        return 2;
    }
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, dir, MAX_PATH);
    CreateDirectoryW(dir, NULL);

    /* Oplocks: never granted; acknowledgements are protocol errors; the file keeps working. */
    _snwprintf(p, MAX_PATH, L"%s\\oplock.txt", dir);
    st = ntopen(p, FILE_READ_DATA | FILE_WRITE_DATA, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL, &h);
    if (NT_SUCCESS(st))
    {
        st = fsctl(h, FSCTL_REQUEST_OPLOCK_LEVEL_1, NULL, 0, NULL, 0);
        report("oplock-level1", st == STATUS_OPLOCK_NOT_GRANTED, "status 0x%08lx, want STATUS_OPLOCK_NOT_GRANTED", st);
        st = fsctl(h, FSCTL_REQUEST_OPLOCK_LEVEL_2, NULL, 0, NULL, 0);
        report("oplock-level2", st == STATUS_OPLOCK_NOT_GRANTED, "status 0x%08lx", st);
        st = fsctl(h, FSCTL_REQUEST_BATCH_OPLOCK, NULL, 0, NULL, 0);
        report("oplock-batch", st == STATUS_OPLOCK_NOT_GRANTED, "status 0x%08lx", st);
        st = fsctl(h, FSCTL_REQUEST_FILTER_OPLOCK, NULL, 0, NULL, 0);
        report("oplock-filter", st == STATUS_OPLOCK_NOT_GRANTED, "status 0x%08lx", st);
        st = fsctl(h, FSCTL_OPLOCK_BREAK_ACKNOWLEDGE, NULL, 0, NULL, 0);
        report("oplock-ack", st == STATUS_INVALID_OPLOCK_PROTOCOL, "status 0x%08lx, want STATUS_INVALID_OPLOCK_PROTOCOL", st);
        report("oplock-usable", usable(h), NULL);
        NtClose(h);
    }
    else
        report("oplock-open", 0, "status 0x%08lx", st);

    /* Extended attributes: a create with an EA buffer fails and creates nothing; EA calls fail. */
    memset(eabuf, 0, sizeof(eabuf));
    ea->EaNameLength = 6;
    ea->EaValueLength = 1;
    memcpy(ea->EaName, "NGTEST\0" "1", 8);
    _snwprintf(p, MAX_PATH, L"%s\\ea.txt", dir);
    st = ntopen(p, FILE_WRITE_DATA, FILE_CREATE, FILE_NON_DIRECTORY_FILE, ea, sizeof(eabuf), NULL, &h);
    report("ea-create", st == STATUS_EAS_NOT_SUPPORTED, "status 0x%08lx, want STATUS_EAS_NOT_SUPPORTED", st);
    if (NT_SUCCESS(st)) NtClose(h);
    report("ea-create-nothing-left", GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES, NULL);
    st = ntopen(p, FILE_READ_DATA | FILE_WRITE_DATA | FILE_WRITE_EA, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL, &h);
    if (NT_SUCCESS(st))
    {
        st = NtSetEaFile(h, &iosb, ea, sizeof(eabuf));
        report("ea-set", !NT_SUCCESS(st), "status 0x%08lx (no EA is stored)", st);
        a = fs_attributes(h);
        report("ea-volume-flag", a != 0xffffffff && !(a & FILE_SUPPORTS_EXTENDED_ATTRIBUTES), "attributes 0x%lx", a);
        report("ea-usable", usable(h), NULL);
        NtClose(h);
    }
    else
        report("ea-open", 0, "status 0x%08lx", st);

    /* Sparse and compressed writes: the requests are refused, the file stays plain and usable. */
    _snwprintf(p, MAX_PATH, L"%s\\plain.bin", dir);
    st = ntopen(p, FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_OVERWRITE_IF,
                FILE_NON_DIRECTORY_FILE, NULL, 0, NULL, &h);
    if (NT_SUCCESS(st))
    {
        a = fs_attributes(h);
        report("volume-no-sparse-flag", a != 0xffffffff && !(a & FILE_SUPPORTS_SPARSE_FILES), "attributes 0x%lx", a);
        report("volume-no-compression-flag", a != 0xffffffff && !(a & FILE_FILE_COMPRESSION), "attributes 0x%lx", a);
        st = fsctl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0);
        report("sparse-set", st == STATUS_INVALID_DEVICE_REQUEST, "status 0x%08lx, want STATUS_INVALID_DEVICE_REQUEST", st);
        zero.FileOffset.QuadPart = 0;
        zero.BeyondFinalZero.QuadPart = 4096;
        st = fsctl(h, FSCTL_SET_ZERO_DATA, &zero, sizeof(zero), NULL, 0);
        report("sparse-zero-data", st == STATUS_INVALID_DEVICE_REQUEST, "status 0x%08lx, want STATUS_INVALID_DEVICE_REQUEST", st);
        fmt = COMPRESSION_FORMAT_LZNT1;
        st = fsctl(h, FSCTL_SET_COMPRESSION, &fmt, sizeof(fmt), NULL, 0);
        report("compress-set", st == STATUS_NOT_SUPPORTED, "status 0x%08lx, want STATUS_NOT_SUPPORTED", st);
        fmt = COMPRESSION_FORMAT_NONE;
        st = fsctl(h, FSCTL_SET_COMPRESSION, &fmt, sizeof(fmt), NULL, 0);
        report("compress-none", st == STATUS_SUCCESS, "status 0x%08lx (a no-op on a plain file)", st);
        a = file_attributes(h);
        report("plain-attributes", a != 0xffffffff && !(a & (FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_COMPRESSED)),
               "attributes 0x%lx", a);
        report("plain-usable", usable(h), NULL);
        NtClose(h);
    }
    else
        report("plain-open", 0, "status 0x%08lx", st);
    if (argc >= 4)
    {
        existing("sparse", argv[2], FILE_ATTRIBUTE_SPARSE_FILE);
        existing("compressed", argv[3], FILE_ATTRIBUTE_COMPRESSED);
    }

    /* Dismount: refused, the volume stays mounted and writable. */
    st = ntopen(L"\\\\.\\C:", FILE_READ_DATA | FILE_WRITE_DATA, FILE_OPEN, 0, NULL, 0, NULL, &v);
    if (NT_SUCCESS(st))
    {
        st = fsctl(v, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0);
        report("dismount", st == STATUS_ACCESS_DENIED, "status 0x%08lx, want STATUS_ACCESS_DENIED", st);
        NtClose(v);
        _snwprintf(p, MAX_PATH, L"%s\\after-dismount.txt", dir);
        st = ntopen(p, FILE_READ_DATA | FILE_WRITE_DATA, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, NULL, 0, NULL, &h);
        report("dismount-volume-usable", NT_SUCCESS(st) && usable(h), "status 0x%08lx", st);
        if (NT_SUCCESS(st)) NtClose(h);
    }
    else
        report("dismount-open-volume", 0, "status 0x%08lx", st);

    /* A security descriptor given at create time is stored: the query returns its DACL. */
    RtlAllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone);
    RtlCreateAcl(acl, sizeof(aclbuf), ACL_REVISION);
    RtlAddAccessAllowedAce(acl, ACL_REVISION, FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE, everyone);
    RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    RtlSetDaclSecurityDescriptor(&sd, TRUE, acl, FALSE);
    _snwprintf(p, MAX_PATH, L"%s\\sd-file.txt", dir);
    st = ntopen(p, FILE_READ_DATA | FILE_WRITE_DATA | READ_CONTROL, FILE_CREATE, FILE_NON_DIRECTORY_FILE, NULL, 0, &sd, &h);
    report("sd-create-file", NT_SUCCESS(st), "status 0x%08lx", st);
    if (NT_SUCCESS(st))
    {
        BOOLEAN present = FALSE, defaulted;
        PACL got = NULL;
        PACCESS_ALLOWED_ACE ace = NULL;
        st = NtQuerySecurityObject(h, DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION, sdout, sizeof(sdout), &need);
        if (NT_SUCCESS(st))
            RtlGetDaclSecurityDescriptor(sdout, &present, &got, &defaulted);
        if (got && got->AceCount == 1)
            RtlGetAce(got, 0, (PVOID *)&ace);
        report("sd-create-file-dacl", NT_SUCCESS(st) && present && ace && ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
               ace->Mask == (FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE) && RtlEqualSid(&ace->SidStart, everyone),
               "status 0x%08lx, %u ACEs, mask 0x%lx", st, got ? got->AceCount : 0, ace ? ace->Mask : 0);
        NtClose(h);
    }
    _snwprintf(p, MAX_PATH, L"%s\\sd-dir", dir);
    st = ntopen(p, FILE_LIST_DIRECTORY | READ_CONTROL, FILE_CREATE, FILE_DIRECTORY_FILE, NULL, 0, &sd, &h);
    report("sd-create-dir", NT_SUCCESS(st), "status 0x%08lx", st);
    if (NT_SUCCESS(st))
    {
        BOOLEAN present = FALSE, defaulted;
        PACL got = NULL;
        st = NtQuerySecurityObject(h, DACL_SECURITY_INFORMATION, sdout, sizeof(sdout), &need);
        if (NT_SUCCESS(st))
            RtlGetDaclSecurityDescriptor(sdout, &present, &got, &defaulted);
        report("sd-create-dir-dacl", NT_SUCCESS(st) && present && got && got->AceCount == 1, "status 0x%08lx, %u ACEs",
               st, got ? got->AceCount : 0);
        NtClose(h);
    }
    if (everyone)
        RtlFreeSid(everyone);

    {
        char line[64];
        _snprintf(line, sizeof(line), "NGX:DONE pass=%d fail=%d\n", Pass, Fail);
        OutputDebugStringA(line);
        fputs(line, stdout);
    }
    return Fail ? 1 : 0;
}
