/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Extending data sections on a file with NtExtendSection
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"

#define FILE_SIZE 0x1800

static LONGLONG GetSize(HANDLE hFile)
{
    LARGE_INTEGER Size;

    if (!GetFileSizeEx(hFile, &Size))
        return -1;
    return Size.QuadPart;
}

static LONGLONG QuerySize(HANDLE hSection)
{
    SECTION_BASIC_INFORMATION Sbi;
    NTSTATUS Status;

    Status = NtQuerySection(hSection, SectionBasicInformation, &Sbi, sizeof(Sbi), NULL);
    ok(Status == STATUS_SUCCESS, "NtQuerySection: 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
        return -1;
    return Sbi.Size.QuadPart;
}

static HANDLE CreateDataFile(PCWSTR Path, BYTE Fill)
{
    BYTE Data[FILE_SIZE];
    HANDLE hFile;
    DWORD Written;

    hFile = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return NULL;
    memset(Data, Fill, sizeof(Data));
    if (!WriteFile(hFile, Data, sizeof(Data), &Written, NULL) || Written != sizeof(Data))
    {
        CloseHandle(hFile);
        return NULL;
    }
    return hFile;
}

static NTSTATUS Extend(HANDLE hSection, LONGLONG NewSize, PLONGLONG Returned)
{
    LARGE_INTEGER Size;
    NTSTATUS Status;

    Size.QuadPart = NewSize;
    Status = NtExtendSection(hSection, &Size);
    *Returned = Size.QuadPart;
    trace("EXT extend to 0x%I64x: 0x%08lx, returned 0x%I64x\n", NewSize, Status, Size.QuadPart);
    return Status;
}

/* Reads one byte of a view; -1 if the read faults */
static int ReadByte(volatile BYTE *Address)
{
    int Value = -1;

    _SEH2_TRY
    {
        Value = *Address;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Value = -1;
    }
    _SEH2_END;
    return Value;
}

static BOOL WriteByte(volatile BYTE *Address, BYTE Value)
{
    BOOL Ret = TRUE;

    _SEH2_TRY
    {
        *Address = Value;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Ret = FALSE;
    }
    _SEH2_END;
    return Ret;
}

static int ReadFileByte(HANDLE hFile, LONGLONG Offset)
{
    LARGE_INTEGER Position;
    BYTE Value;
    DWORD Read;

    Position.QuadPart = Offset;
    if (!SetFilePointerEx(hFile, Position, NULL, FILE_BEGIN) ||
        !ReadFile(hFile, &Value, 1, &Read, NULL) || Read != 1)
        return -1;
    return Value;
}

/*
 * A writable section on a file extended by a size that is not page-aligned:
 * the section, the file and a view of the whole section all take the new size.
 */
static void Test_ExtendWritable(PCWSTR Path)
{
    HANDLE hFile, hSection = NULL;
    LONGLONG Returned;
    NTSTATUS Status;
    PVOID Base = NULL;
    SIZE_T ViewSize = 0;
    PBYTE View;

    hFile = CreateDataFile(Path, 0x5A);
    if (!hFile)
    {
        skip("Cannot create %ls: %lu\n", Path, GetLastError());
        return;
    }

    Status = NtCreateSection(&hSection, SECTION_ALL_ACCESS, NULL, NULL, PAGE_READWRITE, SEC_COMMIT, hFile);
    ok(Status == STATUS_SUCCESS, "NtCreateSection: 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
    {
        CloseHandle(hFile);
        return;
    }
    ok(QuerySize(hSection) == FILE_SIZE, "Initial size 0x%I64x\n", QuerySize(hSection));

    /* 0x2800 + PAGE_SIZE - 1 has bit 12 set: a wrong rounding mask leaves 0x27FF */
    Status = Extend(hSection, 0x2800, &Returned);
    ok(Status == STATUS_SUCCESS, "Extend to 0x2800: 0x%08lx\n", Status);
    ok(Returned == 0x2800, "Extend to 0x2800 returned 0x%I64x\n", Returned);
    ok(QuerySize(hSection) == 0x2800, "Size after extension 0x%I64x\n", QuerySize(hSection));
    ok(GetSize(hFile) == 0x2800, "File size after extension 0x%I64x\n", GetSize(hFile));

    /* A view of the whole section covers the extended range up to the next page */
    Status = NtMapViewOfSection(hSection, NtCurrentProcess(), &Base, 0, 0, NULL, &ViewSize,
                                ViewShare, 0, PAGE_READWRITE);
    ok(Status == STATUS_SUCCESS, "NtMapViewOfSection: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        View = Base;
        ok(ViewSize == 0x3000, "ViewSize 0x%Ix\n", ViewSize);
        ok(ReadByte(View + FILE_SIZE - 1) == 0x5A, "Last byte of the file: %d\n", ReadByte(View + FILE_SIZE - 1));
        ok(ReadByte(View + FILE_SIZE) == 0, "First extended byte: %d\n", ReadByte(View + FILE_SIZE));
        ok(ReadByte(View + 0x27FF) == 0, "Last extended byte: %d\n", ReadByte(View + 0x27FF));
        trace("EXT byte at 0x2800 (past the section, in its last page): %d\n", ReadByte(View + 0x2800));
        ok(WriteByte(View + 0x27FF, 0xA5), "Write to the last extended byte faulted\n");
        ok(WriteByte(View + 0x2000, 0xA6), "Write to the extended page faulted\n");
        NtUnmapViewOfSection(NtCurrentProcess(), Base);
    }

    /* A smaller size shrinks the section object, not the file */
    Status = Extend(hSection, 0x1000, &Returned);
    ok(Status == STATUS_SUCCESS, "Extend to 0x1000: 0x%08lx\n", Status);
    ok(Returned == 0x1000, "Extend to 0x1000 returned 0x%I64x\n", Returned);
    ok(QuerySize(hSection) == 0x1000, "Size after the smaller extension 0x%I64x\n", QuerySize(hSection));
    ok(GetSize(hFile) == 0x2800, "File size after the smaller extension 0x%I64x\n", GetSize(hFile));
    Base = NULL;
    ViewSize = 0x2000;
    Status = NtMapViewOfSection(hSection, NtCurrentProcess(), &Base, 0, 0, NULL, &ViewSize,
                                ViewShare, 0, PAGE_READONLY);
    ok(Status == STATUS_INVALID_VIEW_SIZE, "View 0x2000 of the shrunk section: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
        NtUnmapViewOfSection(NtCurrentProcess(), Base);
    Status = Extend(hSection, 0, &Returned);
    ok(Status == STATUS_SUCCESS, "Extend to 0: 0x%08lx\n", Status);
    ok(QuerySize(hSection) == 0, "Size after extension to 0: 0x%I64x\n", QuerySize(hSection));
    Base = NULL;
    ViewSize = 0;
    Status = NtMapViewOfSection(hSection, NtCurrentProcess(), &Base, 0, 0, NULL, &ViewSize,
                                ViewShare, 0, PAGE_READONLY);
    ok(Status == STATUS_INVALID_VIEW_SIZE, "View of the size-0 section: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
        NtUnmapViewOfSection(NtCurrentProcess(), Base);
    Status = Extend(hSection, -1, &Returned);
    ok(Status == STATUS_SECTION_TOO_BIG, "Extend to -1: 0x%08lx\n", Status);
    ok(QuerySize(hSection) == 0, "Size after extension to -1: 0x%I64x\n", QuerySize(hSection));
    ok(GetSize(hFile) == 0x2800, "File size after extension to -1: 0x%I64x\n", GetSize(hFile));

    /* Extensions that end one byte past a page boundary, and on one */
    Status = Extend(hSection, 0x3001, &Returned);
    ok(Status == STATUS_SUCCESS, "Extend to 0x3001: 0x%08lx\n", Status);
    ok(QuerySize(hSection) == 0x3001, "Size after extension to 0x3001: 0x%I64x\n", QuerySize(hSection));
    Status = Extend(hSection, 0x5000, &Returned);
    ok(Status == STATUS_SUCCESS, "Extend to 0x5000: 0x%08lx\n", Status);
    ok(QuerySize(hSection) == 0x5000, "Size after extension to 0x5000: 0x%I64x\n", QuerySize(hSection));

    Base = NULL;
    ViewSize = 0;
    Status = NtMapViewOfSection(hSection, NtCurrentProcess(), &Base, 0, 0, NULL, &ViewSize,
                                ViewShare, 0, PAGE_READWRITE);
    ok(Status == STATUS_SUCCESS, "NtMapViewOfSection after 0x5000: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        ok(ViewSize == 0x5000, "ViewSize after 0x5000: 0x%Ix\n", ViewSize);
        ok(ReadByte((PBYTE)Base + 0x27FF) == 0xA5, "Byte at 0x27FF after 0x5000: %d\n", ReadByte((PBYTE)Base + 0x27FF));
        ok(ReadByte((PBYTE)Base + 0x4FFF) == 0, "Byte at 0x4FFF: %d\n", ReadByte((PBYTE)Base + 0x4FFF));
        NtUnmapViewOfSection(NtCurrentProcess(), Base);
    }

    /* What was written through the view in the extended range is in the file */
    NtClose(hSection);
    ok(GetSize(hFile) == 0x5000, "File size after close 0x%I64x\n", GetSize(hFile));
    ok(ReadFileByte(hFile, 0x27FF) == 0xA5, "File byte at 0x27FF: %d\n", ReadFileByte(hFile, 0x27FF));
    ok(ReadFileByte(hFile, 0x2000) == 0xA6, "File byte at 0x2000: %d\n", ReadFileByte(hFile, 0x2000));
    ok(ReadFileByte(hFile, 0x4FFF) == 0, "File byte at 0x4FFF: %d\n", ReadFileByte(hFile, 0x4FFF));
    CloseHandle(hFile);
    DeleteFileW(Path);
}

/* Sections that cannot be extended */
static void Test_ExtendRefused(PCWSTR Path)
{
    HANDLE hFile, hSection;
    LARGE_INTEGER Size;
    LONGLONG Returned;
    NTSTATUS Status;

    hFile = CreateDataFile(Path, 0x5A);
    if (!hFile)
    {
        skip("Cannot create %ls: %lu\n", Path, GetLastError());
        return;
    }

    /* Without SECTION_EXTEND_SIZE */
    hSection = NULL;
    Status = NtCreateSection(&hSection, SECTION_QUERY | SECTION_MAP_READ | SECTION_MAP_WRITE, NULL, NULL,
                             PAGE_READWRITE, SEC_COMMIT, hFile);
    ok(Status == STATUS_SUCCESS, "NtCreateSection: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        Status = Extend(hSection, 0x2800, &Returned);
        ok(Status == STATUS_ACCESS_DENIED, "Extend without access: 0x%08lx\n", Status);
        NtClose(hSection);
    }

    /* A read-only section */
    hSection = NULL;
    Status = NtCreateSection(&hSection, SECTION_ALL_ACCESS, NULL, NULL, PAGE_READONLY, SEC_COMMIT, hFile);
    ok(Status == STATUS_SUCCESS, "NtCreateSection read-only: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        Status = Extend(hSection, 0x2800, &Returned);
        ok(Status == STATUS_SECTION_NOT_EXTENDED, "Extend read-only: 0x%08lx\n", Status);
        ok(QuerySize(hSection) == FILE_SIZE, "Read-only size 0x%I64x\n", QuerySize(hSection));
        ok(GetSize(hFile) == FILE_SIZE, "Read-only file size 0x%I64x\n", GetSize(hFile));
        Status = Extend(hSection, 0x1000, &Returned);
        ok(Status == STATUS_SUCCESS, "Shrink read-only: 0x%08lx\n", Status);
        ok(QuerySize(hSection) == 0x1000, "Read-only size after shrinking 0x%I64x\n", QuerySize(hSection));
        Status = Extend(hSection, FILE_SIZE, &Returned);
        trace("EXT read-only section back to the file size: 0x%08lx, size 0x%I64x\n", Status, QuerySize(hSection));
        NtClose(hSection);
    }

    /* A page-file section */
    hSection = NULL;
    Size.QuadPart = 0x1800;
    Status = NtCreateSection(&hSection, SECTION_ALL_ACCESS, NULL, &Size, PAGE_READWRITE, SEC_COMMIT, NULL);
    ok(Status == STATUS_SUCCESS, "NtCreateSection page file: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        Status = Extend(hSection, 0x2800, &Returned);
        ok(Status == STATUS_SECTION_NOT_EXTENDED, "Extend page file: 0x%08lx\n", Status);
        ok(QuerySize(hSection) == 0x2000, "Page-file size 0x%I64x\n", QuerySize(hSection));
        NtClose(hSection);
    }
    hSection = NULL;
    Status = NtCreateSection(&hSection, SECTION_ALL_ACCESS, NULL, &Size, PAGE_READWRITE, SEC_RESERVE, NULL);
    ok(Status == STATUS_SUCCESS, "NtCreateSection page file reserve: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        Status = Extend(hSection, 0x2800, &Returned);
        ok(Status == STATUS_SECTION_NOT_EXTENDED, "Extend page-file reserve: 0x%08lx\n", Status);
        ok(QuerySize(hSection) == 0x2000, "Page-file reserve size 0x%I64x\n", QuerySize(hSection));
        NtClose(hSection);
    }

    ok(GetSize(hFile) == FILE_SIZE, "File size at the end 0x%I64x\n", GetSize(hFile));
    CloseHandle(hFile);
    DeleteFileW(Path);

    /* An image section */
    {
        WCHAR Self[MAX_PATH];

        if (GetModuleFileNameW(NULL, Self, ARRAYSIZE(Self)))
        {
            hFile = CreateFileW(Self, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, 0, NULL);
            if (hFile != INVALID_HANDLE_VALUE)
            {
                LONGLONG ImageSize;

                hSection = NULL;
                Status = NtCreateSection(&hSection, SECTION_ALL_ACCESS, NULL, NULL, PAGE_READONLY, SEC_IMAGE, hFile);
                ok(Status == STATUS_SUCCESS, "NtCreateSection image: 0x%08lx\n", Status);
                if (NT_SUCCESS(Status))
                {
                    ImageSize = QuerySize(hSection);
                    Status = Extend(hSection, ImageSize + 0x1800, &Returned);
                    ok(Status == STATUS_SECTION_NOT_EXTENDED, "Extend image: 0x%08lx\n", Status);
                    ok(QuerySize(hSection) == ImageSize, "Image size 0x%I64x, was 0x%I64x\n", QuerySize(hSection), ImageSize);
                    NtClose(hSection);
                }
                CloseHandle(hFile);
            }
        }
    }
}

/* A view that starts at the end of a 64 KB section has nothing left to map */
static void Test_ViewAtEnd(PCWSTR Path)
{
    HANDLE hFile, hSection;
    LARGE_INTEGER Offset;
    NTSTATUS Status;
    PVOID Base;
    SIZE_T ViewSize;
    ULONG Protect;

    hFile = CreateDataFile(Path, 0x5A);
    if (!hFile)
    {
        skip("Cannot create %ls: %lu\n", Path, GetLastError());
        return;
    }
    Offset.QuadPart = 0x10000;
    if (!SetFilePointerEx(hFile, Offset, NULL, FILE_BEGIN) || !SetEndOfFile(hFile))
    {
        skip("Cannot set the file size: %lu\n", GetLastError());
        CloseHandle(hFile);
        DeleteFileW(Path);
        return;
    }

    for (Protect = PAGE_READONLY; Protect <= PAGE_READWRITE; Protect <<= 1)
    {
        hSection = NULL;
        Status = NtCreateSection(&hSection, SECTION_ALL_ACCESS, NULL, NULL, Protect, SEC_COMMIT, hFile);
        ok(Status == STATUS_SUCCESS, "NtCreateSection 0x%lx: 0x%08lx\n", Protect, Status);
        if (!NT_SUCCESS(Status))
            continue;
        Base = NULL;
        ViewSize = 0;
        Offset.QuadPart = 0x10000;
        Status = NtMapViewOfSection(hSection, NtCurrentProcess(), &Base, 0, 0, &Offset, &ViewSize,
                                    ViewShare, 0, PAGE_READONLY);
        ok(Status == STATUS_INVALID_VIEW_SIZE, "View at the end, protect 0x%lx: 0x%08lx\n", Protect, Status);
        if (NT_SUCCESS(Status))
            NtUnmapViewOfSection(NtCurrentProcess(), Base);
        NtClose(hSection);
    }

    CloseHandle(hFile);
    DeleteFileW(Path);
}

/* The section's file handle is closed before the section is extended */
static void Test_ExtendClosedFile(PCWSTR Path)
{
    HANDLE hFile, hSection = NULL;
    LONGLONG Returned;
    NTSTATUS Status;
    PVOID Base = NULL;
    SIZE_T ViewSize = 0;
    WCHAR Root[4], FsName[16];
    BOOL IsFat, Todo, Written;
    LONGLONG Size;
    int Byte;

    lstrcpynW(Root, Path, 4);
    IsFat = GetVolumeInformationW(Root, NULL, 0, NULL, NULL, NULL, FsName, ARRAYSIZE(FsName)) &&
            !_wcsnicmp(FsName, L"FAT", 3);
    trace("EXT closed-file case on %ls\n", IsFat ? L"FAT" : L"another file system");

    hFile = CreateDataFile(Path, 0x5A);
    if (!hFile)
    {
        skip("Cannot create %ls: %lu\n", Path, GetLastError());
        return;
    }
    Status = NtCreateSection(&hSection, SECTION_ALL_ACCESS, NULL, NULL, PAGE_READWRITE, SEC_COMMIT, hFile);
    ok(Status == STATUS_SUCCESS, "NtCreateSection: 0x%08lx\n", Status);
    CloseHandle(hFile);
    if (!NT_SUCCESS(Status))
    {
        DeleteFileW(Path);
        return;
    }

    /* ReactOS fastfat refuses a new end of file on a cleaned-up, uncached file object (STATUS_FILE_CLOSED) */
    Todo = is_reactos() && IsFat;
    Status = Extend(hSection, 0x2800, &Returned);
    todo_if(Todo) ok(Status == STATUS_SUCCESS, "Extend after closing the file: 0x%08lx\n", Status);
    Size = QuerySize(hSection);
    todo_if(Todo) ok(Size == 0x2800, "Size after extension 0x%I64x\n", Size);
    Status = NtMapViewOfSection(hSection, NtCurrentProcess(), &Base, 0, 0, NULL, &ViewSize,
                                ViewShare, 0, PAGE_READWRITE);
    ok(Status == STATUS_SUCCESS, "NtMapViewOfSection: 0x%08lx\n", Status);
    if (NT_SUCCESS(Status))
    {
        Written = WriteByte((PBYTE)Base + 0x27FF, 0xA7);
        todo_if(Todo) ok(Written, "Write to the extended range faulted\n");
        NtUnmapViewOfSection(NtCurrentProcess(), Base);
    }

    /* The file grew while the section was still open */
    hFile = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok(hFile != INVALID_HANDLE_VALUE, "Cannot reopen %ls: %lu\n", Path, GetLastError());
    if (hFile != INVALID_HANDLE_VALUE)
    {
        Size = GetSize(hFile);
        Byte = ReadFileByte(hFile, 0x27FF);
        todo_if(Todo) ok(Size == 0x2800, "File size after extension 0x%I64x\n", Size);
        todo_if(Todo) ok(Byte == 0xA7, "File byte at 0x27FF: %d\n", Byte);
        CloseHandle(hFile);
    }
    NtClose(hSection);
    DeleteFileW(Path);
}

/*
 * Two threads extend and shrink the same section at once. The section never
 * ends past the file, and every call succeeds.
 */
typedef struct
{
    HANDLE hSection;
    HANDLE hFile;
    ULONG Seed;
    LONG Failures;
    LONG Beyond;
} EXTEND_THREAD;

static DWORD WINAPI ExtendThread(PVOID Param)
{
    EXTEND_THREAD *Data = Param;
    LONGLONG SectionSize, FileSize;
    LARGE_INTEGER Size;
    NTSTATUS Status;
    ULONG i;

    for (i = 0; i < 1500; i++)
    {
        Data->Seed = Data->Seed * 1103515245 + 12345;
        Size.QuadPart = 0x1000 + ((Data->Seed >> 8) % 0x40000) + i * 0x10;
        Status = NtExtendSection(Data->hSection, &Size);
        if (Status != STATUS_SUCCESS)
        {
            if (InterlockedIncrement(&Data->Failures) <= 3)
                trace("EXT concurrent extend to 0x%I64x: 0x%08lx\n", Size.QuadPart, Status);
        }
        SectionSize = QuerySize(Data->hSection);
        FileSize = GetSize(Data->hFile);
        if (SectionSize > FileSize)
        {
            if (InterlockedIncrement(&Data->Beyond) <= 3)
                trace("EXT section 0x%I64x past the file 0x%I64x\n", SectionSize, FileSize);
        }
    }
    return 0;
}

static void Test_ExtendConcurrent(PCWSTR Path)
{
    EXTEND_THREAD Data[2];
    HANDLE hFile, hSection = NULL, hThreads[2];
    NTSTATUS Status;
    ULONG i;

    hFile = CreateDataFile(Path, 0x5A);
    if (!hFile)
    {
        skip("Cannot create %ls: %lu\n", Path, GetLastError());
        return;
    }
    Status = NtCreateSection(&hSection, SECTION_ALL_ACCESS, NULL, NULL, PAGE_READWRITE, SEC_COMMIT, hFile);
    ok(Status == STATUS_SUCCESS, "NtCreateSection: 0x%08lx\n", Status);
    if (!NT_SUCCESS(Status))
    {
        CloseHandle(hFile);
        DeleteFileW(Path);
        return;
    }

    for (i = 0; i < 2; i++)
    {
        Data[i].hSection = hSection;
        Data[i].hFile = hFile;
        Data[i].Seed = 0x5EED + i * 7919;
        Data[i].Failures = 0;
        Data[i].Beyond = 0;
        hThreads[i] = CreateThread(NULL, 0, ExtendThread, &Data[i], 0, NULL);
        ok(hThreads[i] != NULL, "CreateThread failed: %lu\n", GetLastError());
    }
    for (i = 0; i < 2; i++)
    {
        if (!hThreads[i])
            continue;
        ok(WaitForSingleObject(hThreads[i], 120000) == WAIT_OBJECT_0, "Thread %lu did not finish\n", i);
        CloseHandle(hThreads[i]);
        ok(Data[i].Failures == 0, "Thread %lu: %ld extensions failed\n", i, Data[i].Failures);
        ok(Data[i].Beyond == 0, "Thread %lu: the section ended past the file %ld times\n", i, Data[i].Beyond);
    }
    ok(QuerySize(hSection) <= GetSize(hFile), "Section 0x%I64x past the file 0x%I64x\n",
       QuerySize(hSection), GetSize(hFile));

    NtClose(hSection);
    CloseHandle(hFile);
    DeleteFileW(Path);
}

START_TEST(NtExtendSection)
{
    WCHAR TempDir[MAX_PATH], Path[MAX_PATH];

    if (!GetTempPathW(ARRAYSIZE(TempDir), TempDir) ||
        !GetTempFileNameW(TempDir, L"nxs", 0, Path))
    {
        skip("No temporary file: %lu\n", GetLastError());
        return;
    }

    Test_ExtendWritable(Path);
    Test_ExtendRefused(Path);
    Test_ViewAtEnd(Path);
    Test_ExtendClosedFile(Path);
    Test_ExtendConcurrent(Path);
}
