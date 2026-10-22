/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for how CopyFile2 tells the source from the file it is to overwrite
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

/*
 * The copy of kernel32 is compiled into this test (copy2.c, with its CopyFile2 called Copy2Embedded), so that its
 * identity code can be called, and so that the query of the device name can be made to fail: a cloned volume has
 * the serial number and the file indexes of the volume it was cloned from, and cannot be had in a test run.
 */

#include "precomp.h"
#include <ndk/obfuncs.h>

static BOOL FailObjectQuery;

static NTSTATUS NTAPI
TestNtQueryObject(
    _In_opt_ HANDLE Handle,
    _In_ OBJECT_INFORMATION_CLASS ObjectInformationClass,
    _Out_opt_ PVOID ObjectInformation,
    _In_ ULONG Length,
    _Out_opt_ PULONG ResultLength);

#define CopyFile2 Copy2Embedded
#define NtQueryObject TestNtQueryObject
#include "../../../../dll/win32/kernel32/client/file/copy2.c"
#undef NtQueryObject
#undef CopyFile2

static NTSTATUS NTAPI
TestNtQueryObject(
    _In_opt_ HANDLE Handle,
    _In_ OBJECT_INFORMATION_CLASS ObjectInformationClass,
    _Out_opt_ PVOID ObjectInformation,
    _In_ ULONG Length,
    _Out_opt_ PULONG ResultLength)
{
    if (FailObjectQuery)
        return STATUS_ACCESS_DENIED;
    return NtQueryObject(Handle, ObjectInformationClass, ObjectInformation, Length, ResultLength);
}

static WCHAR TempDir[MAX_PATH];

static void WriteFileOf(PCWSTR Name, const char *Data)
{
    HANDLE hFile = CreateFileW(Name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD Written;

    ok(hFile != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    WriteFile(hFile, Data, (DWORD)strlen(Data), &Written, NULL);
    CloseHandle(hFile);
}

static BOOL ContentIs(PCWSTR Name, const char *Data)
{
    HANDLE hFile = CreateFileW(Name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    char Buffer[64] = { 0 };
    DWORD Read = 0;
    BOOL Same;

    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    ReadFile(hFile, Buffer, sizeof(Buffer) - 1, &Read, NULL);
    CloseHandle(hFile);
    Same = (Read == strlen(Data) && !memcmp(Buffer, Data, Read));
    return Same;
}

static void InitId(COPY2_ID *Id, DWORD Volume, ULONGLONG Index, PCWSTR Device)
{
    RtlZeroMemory(Id, sizeof(*Id));
    Id->Valid = TRUE;
    Id->Volume = Volume;
    Id->Index = Index;
    if (Device)
    {
        Id->HasDevice = TRUE;
        wcscpy(Id->Device, Device);
    }
}

static void TestComparison(void)
{
    COPY2_ID A, B;

    InitId(&A, 0x1234, 5, L"\\DEVICE\\HARDDISKVOLUME2");
    InitId(&B, 0x1234, 5, L"\\DEVICE\\HARDDISKVOLUME2");
    ok(Copy2SameFile(&A, &B) == Copy2Same, "The same device, volume and index are one file\n");

    /* A cloned volume: the serial number and the index are the same, the device is not */
    InitId(&B, 0x1234, 5, L"\\DEVICE\\HARDDISKVOLUME3");
    ok(Copy2SameFile(&A, &B) == Copy2Different, "Two devices are two files\n");

    /* An identity without a device is never the same as anything */
    InitId(&B, 0x1234, 5, NULL);
    ok(Copy2SameFile(&A, &B) == Copy2Unknown, "No device on one side: %d\n", Copy2SameFile(&A, &B));
    ok(Copy2SameFile(&B, &A) == Copy2Unknown, "No device on one side: %d\n", Copy2SameFile(&B, &A));
    InitId(&A, 0x1234, 5, NULL);
    ok(Copy2SameFile(&A, &B) == Copy2Unknown, "No device on both sides: %d\n", Copy2SameFile(&A, &B));

    InitId(&A, 0x1234, 5, L"\\DEVICE\\HARDDISKVOLUME2");
    InitId(&B, 0x1234, 6, L"\\DEVICE\\HARDDISKVOLUME2");
    ok(Copy2SameFile(&A, &B) == Copy2Different, "Two indexes are two files\n");
    InitId(&B, 0x4321, 5, L"\\DEVICE\\HARDDISKVOLUME2");
    ok(Copy2SameFile(&A, &B) == Copy2Different, "Two volumes are two files\n");
    InitId(&B, 0x1234, 5, L"\\DEVICE\\HARDDISKVOLUME2");
    B.Valid = FALSE;
    ok(Copy2SameFile(&A, &B) == Copy2Unknown, "An identity that could not be read\n");
}

/* A directory tree whose names are longer than the buffers of the identity code */
static WCHAR DeepRoot[MAX_PATH];
static WCHAR DeepDir[2048];
static int DeepLevels;

static BOOL MakeDeepDirectory(void)
{
    WCHAR Level[64];
    int i;

    StringCchPrintfW(DeepRoot, _countof(DeepRoot), L"\\\\?\\%sCopyFile2Identity-%lu", TempDir, GetCurrentProcessId());
    if (!CreateDirectoryW(DeepRoot, NULL))
        return FALSE;
    StringCchCopyW(DeepDir, _countof(DeepDir), DeepRoot);
    for (i = 0; i < 14; i++)
    {
        StringCchPrintfW(Level, _countof(Level), L"\\%0*d", 50, i);
        StringCchCatW(DeepDir, _countof(DeepDir), Level);
        if (!CreateDirectoryW(DeepDir, NULL))
            return FALSE;
        DeepLevels++;
    }
    return TRUE;
}

static void RemoveDeepDirectory(void)
{
    while (DeepLevels > 0)
    {
        RemoveDirectoryW(DeepDir);
        *wcsrchr(DeepDir, L'\\') = UNICODE_NULL;
        DeepLevels--;
    }
    RemoveDirectoryW(DeepRoot);
}

static void TestLongPath(void)
{
    WCHAR Source[2100], Dest[2100], Link[2100];
    HANDLE hFirst, hSecond;
    COPY2_ID Id;
    HRESULT hr;

    if (!MakeDeepDirectory())
    {
        skip("The long directory tree cannot be made: %lu\n", GetLastError());
        RemoveDeepDirectory();
        return;
    }

    StringCchPrintfW(Source, _countof(Source), L"%s\\source.txt", DeepDir);
    StringCchPrintfW(Dest, _countof(Dest), L"%s\\dest.txt", DeepDir);
    StringCchPrintfW(Link, _countof(Link), L"%s\\link.txt", DeepDir);
    WriteFileOf(Source, "the source");
    WriteFileOf(Dest, "keep this");
    ok(wcslen(Source) > COPY2_NAME_CHARS, "The path is only %Iu characters\n", wcslen(Source));

    /* The identity of a file with a long name has its device */
    hFirst = CreateFileW(Source, GENERIC_READ, COPY2_SHARE_ALL, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFirst != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
    if (hFirst != INVALID_HANDLE_VALUE)
    {
        Copy2ReadId(hFirst, &Id);
        ok(Id.Valid && Id.HasDevice, "A long path: Valid %d, HasDevice %d\n", Id.Valid, Id.HasDevice);
        ok(Id.HasDevice && !wcsncmp(Id.Device, L"\\DEVICE\\", 8), "The device is %ls\n", Id.Device);

        hSecond = CreateFileW(Dest, GENERIC_READ, COPY2_SHARE_ALL, NULL, OPEN_EXISTING, 0, NULL);
        ok(hSecond != INVALID_HANDLE_VALUE, "CreateFileW failed with %lu\n", GetLastError());
        if (hSecond != INVALID_HANDLE_VALUE)
        {
            ok(Copy2CompareHandles(hFirst, hSecond) == Copy2Different, "Two long-path files are different\n");
            ok(Copy2CompareHandles(hFirst, hFirst) == Copy2Same, "A long-path file is the same as itself\n");
            FailObjectQuery = TRUE;
            ok(Copy2CompareHandles(hFirst, hSecond) == Copy2Unknown, "No device name: %d\n", Copy2CompareHandles(hFirst, hSecond));
            FailObjectQuery = FALSE;
            CloseHandle(hSecond);
        }
        CloseHandle(hFirst);
    }

    /* A copy between two long-path files works and one onto the source does not touch it */
    hr = Copy2Embedded(Source, Dest, NULL);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    ok(ContentIs(Dest, "the source"), "The copy differs\n");
    hr = Copy2Embedded(Source, Source, NULL);
    ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "hr = %#lx\n", hr);
    ok(ContentIs(Source, "the source"), "The source was changed\n");

    if (CreateHardLinkW(Link, Source, NULL))
    {
        hr = Copy2Embedded(Source, Link, NULL);
        ok(hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), "A hard link: hr = %#lx\n", hr);
        ok(ContentIs(Source, "the source") && ContentIs(Link, "the source"), "The source was changed\n");
        DeleteFileW(Link);
    }

    DeleteFileW(Source);
    DeleteFileW(Dest);
    RemoveDeepDirectory();
}

static void TestFailedQuery(void)
{
    WCHAR Source[MAX_PATH], Dest[MAX_PATH];
    COPYFILE2_EXTENDED_PARAMETERS Params;
    HANDLE hFile;
    COPY2_ID Id;
    HRESULT hr;

    StringCchPrintfW(Source, _countof(Source), L"%sCopyFile2Identity-%lu.src", TempDir, GetCurrentProcessId());
    StringCchPrintfW(Dest, _countof(Dest), L"%sCopyFile2Identity-%lu.dst", TempDir, GetCurrentProcessId());
    DeleteFileW(Dest);
    WriteFileOf(Source, "the source");

    /* The identity of a file whose device name cannot be read is not valid */
    hFile = CreateFileW(Source, GENERIC_READ, COPY2_SHARE_ALL, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        FailObjectQuery = TRUE;
        Copy2ReadId(hFile, &Id);
        FailObjectQuery = FALSE;
        ok(!Id.Valid, "The identity of a file with no device is valid\n");
        Copy2ReadId(hFile, &Id);
        ok(Id.Valid && Id.HasDevice, "The identity is valid again: %d, %d\n", Id.Valid, Id.HasDevice);

        ok(Copy2CompareHandles(INVALID_HANDLE_VALUE, hFile) == Copy2Unknown, "An invalid handle is not known\n");
        CloseHandle(hFile);
    }

    /* A copy over a file that exists cannot tell it from the source, so it fails and leaves the file as it was */
    WriteFileOf(Dest, "keep this");
    ZeroMemory(&Params, sizeof(Params));
    Params.dwSize = sizeof(Params);
    FailObjectQuery = TRUE;
    hr = Copy2Embedded(Source, Dest, &Params);
    FailObjectQuery = FALSE;
    ok(FAILED(hr), "A copy over a file that exists went on without an identity: hr = %#lx\n", hr);
    ok(ContentIs(Dest, "keep this"), "The destination was changed\n");
    ok(ContentIs(Source, "the source"), "The source was changed\n");

    /* the same copy onto the source itself */
    FailObjectQuery = TRUE;
    hr = Copy2Embedded(Source, Source, &Params);
    FailObjectQuery = FALSE;
    ok(FAILED(hr), "A copy onto itself went on without an identity: hr = %#lx\n", hr);
    ok(ContentIs(Source, "the source"), "The source was truncated\n");

    /* a copy to a new file needs no identity to be safe: it either fails and leaves nothing, or it is complete */
    DeleteFileW(Dest);
    FailObjectQuery = TRUE;
    hr = Copy2Embedded(Source, Dest, &Params);
    FailObjectQuery = FALSE;
    if (FAILED(hr))
        ok(GetFileAttributesW(Dest) == INVALID_FILE_ATTRIBUTES, "The failed copy left a file\n");
    else
        ok(ContentIs(Dest, "the source"), "The copy differs\n");
    ok(ContentIs(Source, "the source"), "The source was changed\n");

    /* with the query working the same copy is fine */
    hr = Copy2Embedded(Source, Dest, &Params);
    ok(hr == S_OK, "hr = %#lx\n", hr);
    ok(ContentIs(Dest, "the source"), "The copy differs\n");

    DeleteFileW(Source);
    DeleteFileW(Dest);
}

START_TEST(CopyFile2Identity)
{
    if (!is_reactos())
    {
        skip("The test is about the code of ReactOS\n");
        return;
    }

    GetTempPathW(_countof(TempDir), TempDir);
    TestComparison();
    TestLongPath();
    TestFailedQuery();
}
