/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NTFS: setting and reading the volume label
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define SHARE_RW (FILE_SHARE_READ | FILE_SHARE_WRITE)
#define LABEL_MAX 32

static WCHAR LabelRoot[MAX_PATH], VolumePath[MAX_PATH];

/* The test directory's volume: its root (LabelRoot) and a name that opens the volume itself (VolumePath). */
static BOOLEAN LabelInit(PCSTR TestName)
{
    size_t Len;

    if (!NtInit(TestName))
        return FALSE;
    if (!GetVolumePathNameW(NtTestDir(), LabelRoot, MAX_PATH))
    {
        skip("GetVolumePathNameW failed %lu\n", GetLastError());
        return FALSE;
    }
    if (wcslen(LabelRoot) == 3 && LabelRoot[1] == L':')
    {
        _snwprintf(VolumePath, RTL_NUMBER_OF(VolumePath), L"\\\\.\\%c:", LabelRoot[0]);
    }
    else if (GetVolumeNameForVolumeMountPointW(LabelRoot, VolumePath, MAX_PATH) &&
             (Len = wcslen(VolumePath)) > 1 && VolumePath[Len - 1] == L'\\')
    {
        VolumePath[Len - 1] = UNICODE_NULL;
    }
    else
    {
        skip("No volume name for %ls\n", LabelRoot);
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN GetLabel(PWSTR Out)
{
    Out[0] = UNICODE_NULL;
    return GetVolumeInformationW(LabelRoot, Out, MAX_PATH, NULL, NULL, NULL, NULL, 0);
}

/* The label as FileFsVolumeInformation reports it, raw, with its length in bytes; FALSE if the query failed. */
static BOOLEAN NtQueryLabel(PWSTR Out, ULONG *Bytes)
{
    ULONGLONG Buffer[(sizeof(FILE_FS_VOLUME_INFORMATION) + MAX_PATH * sizeof(WCHAR)) / sizeof(ULONGLONG)];
    PFILE_FS_VOLUME_INFORMATION Info = (PFILE_FS_VOLUME_INFORMATION)Buffer;
    IO_STATUS_BLOCK Iosb;
    HANDLE Volume;
    NTSTATUS Status;

    Out[0] = UNICODE_NULL;
    *Bytes = 0;
    /* A volume handle with attribute access only is a direct device open on Windows: use the root. */
    Status = NtOpen(LabelRoot, FILE_READ_ATTRIBUTES | SYNCHRONIZE, SHARE_RW, FILE_OPEN,
                    FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, &Volume);
    if (!NT_SUCCESS(Status))
    {
        trace("Volume open for the query: 0x%08lx\n", Status);
        return FALSE;
    }
    Status = NtQueryVolumeInformationFile(Volume, &Iosb, Info, sizeof(Buffer), FileFsVolumeInformation);
    NtClose(Volume);
    if (!NT_SUCCESS(Status) || Info->VolumeLabelLength >= MAX_PATH * sizeof(WCHAR))
    {
        trace("FileFsVolumeInformation: 0x%08lx, label length %lu\n", Status, Info->VolumeLabelLength);
        return FALSE;
    }
    RtlCopyMemory(Out, Info->VolumeLabel, Info->VolumeLabelLength);
    Out[Info->VolumeLabelLength / sizeof(WCHAR)] = UNICODE_NULL;
    *Bytes = Info->VolumeLabelLength;
    return TRUE;
}

/* NtSetVolumeInformationFile(FileFsLabelInformation) with a label of LabelBytes bytes in an Info of InfoLength bytes. */
static NTSTATUS NtSetLabel(HANDLE Handle, PCWSTR Label, ULONG LabelBytes, ULONG InfoLength)
{
    ULONGLONG Buffer[(sizeof(FILE_FS_LABEL_INFORMATION) + 128 * sizeof(WCHAR)) / sizeof(ULONGLONG)];
    PFILE_FS_LABEL_INFORMATION Info = (PFILE_FS_LABEL_INFORMATION)Buffer;
    IO_STATUS_BLOCK Iosb;

    RtlZeroMemory(Buffer, sizeof(Buffer));
    Info->VolumeLabelLength = LabelBytes;
    if (Label)
        RtlCopyMemory(Info->VolumeLabel, Label, min(LabelBytes, sizeof(Buffer) - FIELD_OFFSET(FILE_FS_LABEL_INFORMATION, VolumeLabel)));
    return NtSetVolumeInformationFile(Handle, &Iosb, Info, InfoLength, FileFsLabelInformation);
}

static NTSTATUS OpenVolume(ACCESS_MASK Access, PHANDLE Handle)
{
    return NtOpen(VolumePath, Access | SYNCHRONIZE, SHARE_RW, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, Handle);
}

/* Sets the label to Bytes bytes of Label through the native call, exactly as given, and checks it took. */
static BOOLEAN RestoreLabel(PCWSTR Label, ULONG Bytes)
{
    WCHAR Got[MAX_PATH];
    ULONG GotBytes;
    HANDLE Handle;
    NTSTATUS Status;

    Status = OpenVolume(FILE_WRITE_DATA, &Handle);
    if (NT_SUCCESS(Status))
    {
        Status = NtSetLabel(Handle, Label, Bytes, sizeof(FILE_FS_LABEL_INFORMATION) + Bytes);
        NtClose(Handle);
    }
    ok(Status == STATUS_SUCCESS, "Restoring the label: 0x%08lx\n", Status);
    return NT_SUCCESS(Status) && NtQueryLabel(Got, &GotBytes) && GotBytes == Bytes && !memcmp(Got, Label, Bytes);
}

/* SetVolumeLabelW, then the label both query paths report must be Expect. */
static VOID SetAndCheck(int Line, PCWSTR Label, BOOL ExpectOk, DWORD ExpectError, PCWSTR Expect)
{
    WCHAR Got[MAX_PATH], NtGot[MAX_PATH];
    ULONG Bytes;
    BOOL Ret;
    DWORD Error;

    SetLastError(0xdeadbeef);
    Ret = SetVolumeLabelW(LabelRoot, Label);
    Error = GetLastError();
    ok_(__FILE__, Line)(Ret == ExpectOk, "SetVolumeLabelW(\"%ls\") returned %d, error %lu\n",
                        Label ? Label : L"(null)", Ret, Error);
    if (!ExpectOk)
        ok_(__FILE__, Line)(Error == ExpectError, "SetVolumeLabelW(\"%ls\") error %lu, expected %lu\n",
                            Label ? Label : L"(null)", Error, ExpectError);
    ok_(__FILE__, Line)(GetLabel(Got), "GetVolumeInformationW failed %lu\n", GetLastError());
    ok_(__FILE__, Line)(!wcscmp(Got, Expect), "Label is \"%ls\", expected \"%ls\"\n", Got, Expect);
    ok_(__FILE__, Line)(NtQueryLabel(NtGot, &Bytes), "FileFsVolumeInformation failed\n");
    ok_(__FILE__, Line)(Bytes == wcslen(Expect) * sizeof(WCHAR) && !wcscmp(NtGot, Expect),
                        "FileFsVolumeInformation label \"%ls\" (%lu bytes), expected \"%ls\"\n", NtGot, Bytes, Expect);
}
#define SetAndCheck(Label, ExpectOk, ExpectError, Expect) SetAndCheck(__LINE__, Label, ExpectOk, ExpectError, Expect)

/* A native label set must fail with Expect and leave the label at Before. */
static VOID NativeRefused(int Line, HANDLE Handle, PCWSTR Label, ULONG LabelBytes, ULONG InfoLength, NTSTATUS Expect,
                          PCWSTR Before, PCSTR What)
{
    WCHAR Got[MAX_PATH];
    NTSTATUS Status = NtSetLabel(Handle, Label, LabelBytes, InfoLength);
    ok_(__FILE__, Line)(Status == Expect, "%s: 0x%08lx, expected 0x%08lx\n", What, Status, Expect);
    ok_(__FILE__, Line)(GetLabel(Got) && !wcscmp(Got, Before), "%s: label is \"%ls\", expected \"%ls\"\n", What, Got, Before);
}
#define NativeRefused(Handle, Label, LabelBytes, InfoLength, Expect, Before, What) \
    NativeRefused(__LINE__, Handle, Label, LabelBytes, InfoLength, Expect, Before, What)

/*
 * Set, read back, replace with the same label, the 32-character limit, characters, clearing,
 * and the NtSetVolumeInformationFile checks.  The volume's own label is restored at the end.
 */
START_TEST(NtfsVolumeLabel)
{
    static const WCHAR Max[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    static const WCHAR TooLong[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456";
    /* NTFS takes characters that FAT labels and file names refuse, control characters included. */
    static const WCHAR Chars[] = L"*?/\\|<>\":+=,;[].\t\x01\x7f";
    static const WCHAR NonAscii[] = L"P\x0159\x00edli\x0161 \x017elu\x0165ou\x010dk\x00fd k\x016f\x0148 \x00fap\x011bl \x65e5\x672c";
    static const WCHAR Odd[] = L"Odd";
    WCHAR Original[MAX_PATH], Label[MAX_PATH], Got[MAX_PATH], Path[MAX_PATH];
    HANDLE Handle;
    NTSTATUS Status;
    ULONG i, Bytes, OriginalBytes;

    if (!LabelInit("NtfsVolumeLabel"))
        return;
    ok(wcslen(Max) == LABEL_MAX && wcslen(TooLong) == LABEL_MAX + 1, "Test labels have the wrong length\n");
    /* The volume's own label, raw, so that it can be put back exactly; nothing changes without it. */
    if (!NtQueryLabel(Original, &OriginalBytes))
    {
        skip("Cannot read the label of %ls\n", LabelRoot);
        return;
    }
    trace("Volume %ls (%ls), label of %lu bytes\n", LabelRoot, VolumePath, OriginalBytes);

    SetAndCheck(L"NtfsApiLabel", TRUE, 0, L"NtfsApiLabel");
    /* The same label again, as a property sheet closed with OK would set it. */
    SetAndCheck(L"NtfsApiLabel", TRUE, 0, L"NtfsApiLabel");
    /* Case and spaces are kept as given. */
    SetAndCheck(L"MiXeD case Label", TRUE, 0, L"MiXeD case Label");
    SetAndCheck(L" lead and trail ", TRUE, 0, L" lead and trail ");
    SetAndCheck(NonAscii, TRUE, 0, NonAscii);
    SetAndCheck(Max, TRUE, 0, Max);
    SetAndCheck(TooLong, FALSE, ERROR_LABEL_TOO_LONG, Max);
    for (i = 0; Chars[i]; i++)
    {
        _snwprintf(Label, RTL_NUMBER_OF(Label), L"A%cB", Chars[i]);
        SetAndCheck(Label, TRUE, 0, Label);
    }
    SetAndCheck(NULL, TRUE, 0, L"");
    SetAndCheck(L"NtfsApiLabel", TRUE, 0, L"NtfsApiLabel");
    SetAndCheck(L"", TRUE, 0, L"");

    /* The native call needs a volume handle with write access. */
    Status = OpenVolume(FILE_READ_DATA, &Handle);
    ok(Status == STATUS_SUCCESS, "Volume open for read: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        NativeRefused(Handle, L"NoWrite", 14, sizeof(FILE_FS_LABEL_INFORMATION) + 14, STATUS_ACCESS_DENIED, L"",
                      "Volume handle without write access");
        NtClose(Handle);
    }
    Status = OpenVolume(FILE_WRITE_DATA, &Handle);
    ok(Status == STATUS_SUCCESS, "Volume open for write: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        Status = NtSetLabel(Handle, L"Native", 12, FIELD_OFFSET(FILE_FS_LABEL_INFORMATION, VolumeLabel) + 12);
        ok(Status == STATUS_SUCCESS, "Native label: 0x%08lx\n", Status);
        ok(GetLabel(Got) && !wcscmp(Got, L"Native"), "Label is \"%ls\"\n", Got);
        NativeRefused(Handle, L"Short", 10, FIELD_OFFSET(FILE_FS_LABEL_INFORMATION, VolumeLabel) + 4, STATUS_INVALID_PARAMETER,
                      L"Native", "Label longer than the buffer");
        NativeRefused(Handle, L"Tiny", 0, 2, STATUS_INFO_LENGTH_MISMATCH, L"Native", "Buffer smaller than the structure");
        NativeRefused(Handle, TooLong, (LABEL_MAX + 1) * sizeof(WCHAR), sizeof(FILE_FS_LABEL_INFORMATION) + 128,
                      STATUS_INVALID_VOLUME_LABEL, L"Native", "Native label too long");
        /* An odd byte count: Windows 10 refuses it, Server 2008 R2 drops the odd byte. */
        Status = NtSetLabel(Handle, Odd, 5, sizeof(FILE_FS_LABEL_INFORMATION) + 6);
        GetLabel(Got);
        ok((Status == STATUS_INVALID_VOLUME_LABEL && !wcscmp(Got, L"Native")) || (Status == STATUS_SUCCESS && !wcscmp(Got, L"Od")),
           "Odd length: 0x%08lx, label \"%ls\"\n", Status, Got);
        /* The label is stored as given: an embedded NUL and an unpaired surrogate are kept. */
        Status = NtSetLabel(Handle, L"A\0B", 6, sizeof(FILE_FS_LABEL_INFORMATION) + 6);
        ok(Status == STATUS_SUCCESS, "Embedded NUL: 0x%08lx\n", Status);
        ok(NtQueryLabel(Got, &Bytes) && Bytes == 6 && !memcmp(Got, L"A\0B", 6), "Embedded NUL: %lu bytes\n", Bytes);
        Status = NtSetLabel(Handle, L"A\xd800" L"B", 6, sizeof(FILE_FS_LABEL_INFORMATION) + 6);
        ok(Status == STATUS_SUCCESS, "Unpaired surrogate: 0x%08lx\n", Status);
        ok(NtQueryLabel(Got, &Bytes) && Bytes == 6 && !memcmp(Got, L"A\xd800" L"B", 6), "Unpaired surrogate: %lu bytes\n", Bytes);
        Status = NtSetLabel(Handle, NULL, 0, sizeof(FILE_FS_LABEL_INFORMATION));
        ok(Status == STATUS_SUCCESS, "Native empty label: 0x%08lx\n", Status);
        ok(GetLabel(Got) && !wcscmp(Got, L""), "Label is \"%ls\"\n", Got);
        NtClose(Handle);
    }
    /* A directory or file handle of the volume, even with write access, does not set the label. */
    Status = NtOpen(LabelRoot, FILE_WRITE_DATA | SYNCHRONIZE, SHARE_RW, FILE_OPEN, FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                    NULL, &Handle);
    ok(Status == STATUS_SUCCESS, "Root directory open for write: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        NativeRefused(Handle, L"RootDir", 14, sizeof(FILE_FS_LABEL_INFORMATION) + 14, STATUS_ACCESS_DENIED, L"",
                      "Root directory handle");
        NtClose(Handle);
    }
    NtPath(Path, L"file.txt");
    Status = NtOpen(Path, FILE_WRITE_DATA | SYNCHRONIZE, SHARE_RW, FILE_CREATE, FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                    NULL, &Handle);
    ok(Status == STATUS_SUCCESS, "File create: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        NativeRefused(Handle, L"AFile", 10, sizeof(FILE_FS_LABEL_INFORMATION) + 10, STATUS_ACCESS_DENIED, L"", "File handle");
        NtClose(Handle);
    }

    ok(RestoreLabel(Original, OriginalBytes), "The label of %ls was not restored\n", LabelRoot);
    NtCleanup();
}

/* What the first run of NtfsVolumeLabelReboot leaves for the second: both labels, raw. */
typedef struct _LABEL_MARKER
{
    ULONG Magic;
    ULONG OriginalBytes;
    WCHAR Original[LABEL_MAX];
    ULONG ExpectedBytes;
    WCHAR Expected[LABEL_MAX];
} LABEL_MARKER;
#define LABEL_MARKER_MAGIC 0x4c424c4e

/*
 * A label survives a restart.  The first run (only with NTFS_APITEST_LABEL_REBOOT set, so a run of
 * every test leaves the label alone) records the volume's own label and a new one next to the test
 * directory, then sets the new label; the run after a restart checks it and restores the old one.
 * The record goes only once the old label is back.
 */
START_TEST(NtfsVolumeLabelReboot)
{
    WCHAR Marker[MAX_PATH], Got[MAX_PATH], Expected[LABEL_MAX + 1];
    LABEL_MARKER M;
    ULONG Bytes;
    HANDLE File;
    DWORD Done;
    BOOL Ok;

    if (!LabelInit("NtfsVolumeLabelReboot"))
        return;
    if (wcslen(NtTestDir()) + 7 > RTL_NUMBER_OF(Marker))
    {
        skip("The test directory path is too long for the marker\n");
        return;
    }
    _snwprintf(Marker, RTL_NUMBER_OF(Marker), L"%ls.label", NtTestDir());
    File = CreateFileW(Marker, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
    {
        if (!GetEnvironmentVariableW(L"NTFS_APITEST_LABEL_REBOOT", Got, RTL_NUMBER_OF(Got)))
        {
            skip("Set NTFS_APITEST_LABEL_REBOOT to set a label, then restart and run the test again\n");
            return;
        }
        RtlZeroMemory(&M, sizeof(M));
        M.Magic = LABEL_MARKER_MAGIC;
        if (!NtQueryLabel(Got, &Bytes) || Bytes > sizeof(M.Original))
        {
            skip("Cannot read the label of %ls\n", LabelRoot);
            return;
        }
        M.OriginalBytes = Bytes;
        RtlCopyMemory(M.Original, Got, Bytes);
        _snwprintf(M.Expected, LABEL_MAX, L"Reboot\x017e%04lx", GetTickCount() & 0xffff);
        M.ExpectedBytes = (ULONG)wcslen(M.Expected) * sizeof(WCHAR);
        File = CreateFileW(Marker, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_FLAG_WRITE_THROUGH, NULL);
        ok(File != INVALID_HANDLE_VALUE, "Creating %ls failed %lu\n", Marker, GetLastError());
        if (File == INVALID_HANDLE_VALUE)
            return;
        Ok = WriteFile(File, &M, sizeof(M), &Done, NULL) && Done == sizeof(M);
        ok(Ok, "Writing %ls failed %lu (%lu bytes)\n", Marker, GetLastError(), Done);
        CloseHandle(File);
        if (!Ok)
        {
            DeleteFileW(Marker);
            return;
        }
        ok(SetVolumeLabelW(LabelRoot, M.Expected), "SetVolumeLabelW failed %lu\n", GetLastError());
        ok(NtQueryLabel(Got, &Bytes) && Bytes == M.ExpectedBytes && !memcmp(Got, M.Expected, Bytes), "The new label did not take\n");
        trace("NTFSLABEL:SET %ls; restart and run NtfsVolumeLabelReboot again\n", M.Expected + 7);
        return;
    }
    Ok = ReadFile(File, &M, sizeof(M), &Done, NULL) && Done == sizeof(M) && M.Magic == LABEL_MARKER_MAGIC &&
         M.OriginalBytes <= sizeof(M.Original) && M.ExpectedBytes <= sizeof(M.Expected) &&
         !(M.OriginalBytes & 1) && !(M.ExpectedBytes & 1);
    CloseHandle(File);
    ok(Ok, "%ls is malformed (%lu bytes)\n", Marker, Done);
    if (!Ok)
        return;
    ok(NtQueryLabel(Got, &Bytes), "FileFsVolumeInformation failed\n");
    ok(Bytes == M.ExpectedBytes && !memcmp(Got, M.Expected, Bytes), "The label did not survive the restart (%lu bytes)\n", Bytes);
    RtlCopyMemory(Expected, M.Expected, M.ExpectedBytes);
    Expected[M.ExpectedBytes / sizeof(WCHAR)] = UNICODE_NULL;
    trace("NTFSLABEL:CHECKED %ls\n", Bytes == M.ExpectedBytes && !memcmp(Got, M.Expected, Bytes) && Bytes > 7 * sizeof(WCHAR) ?
          Expected + 7 : L"(different)");
    if (RestoreLabel(M.Original, M.OriginalBytes))
        ok(DeleteFileW(Marker), "DeleteFileW(%ls) failed %lu\n", Marker, GetLastError());
    else
        ok(FALSE, "The old label is not back; %ls is kept for another run\n", Marker);
}
