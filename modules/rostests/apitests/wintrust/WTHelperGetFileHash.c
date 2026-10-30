/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Tests for the undocumented wintrust export WTHelperGetFileHash
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * PowerShell's getfilesiginforedist.dll imports WTHelperGetFileHash. When the export is missing
 * the loader raises its modal "Entry Point Not Found" hard error and the 32-bit pwsh stops at the
 * first use of the Security module (Get-Acl). The expected values below were probed on Windows 10
 * 22H2 with this very program.
 *
 * A file with an embedded signature is needed for the success path: set WTHELPER_SIGNED_FILE, or
 * the test tries kernel32.dll and explorer.exe of the Windows directory.
 */

#define WIN32_NO_STATUS
#include <apitest.h>
#include <wincrypt.h>
#include <winerror.h>

#ifndef TRUST_E_NOSIGNATURE
#define TRUST_E_NOSIGNATURE _HRESULT_TYPEDEF_(0x800B0100L)
#endif

typedef HRESULT (WINAPI *PFN_WTHelperGetFileHash)(LPCWSTR, DWORD, PVOID, BYTE *, DWORD *, ALG_ID *);

static PFN_WTHelperGetFileHash pWTHelperGetFileHash;

/* Offset of data inside the first section that has raw data, or 0 */
static DWORD FirstSectionDataOffset(const WCHAR *Path)
{
    HANDLE File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    IMAGE_DOS_HEADER Dos;
    IMAGE_NT_HEADERS32 Nt;
    IMAGE_SECTION_HEADER Section;
    DWORD Read, Result = 0;
    WORD i;

    if (File == INVALID_HANDLE_VALUE)
        return 0;
    if (ReadFile(File, &Dos, sizeof(Dos), &Read, NULL) && Read == sizeof(Dos) && Dos.e_magic == IMAGE_DOS_SIGNATURE &&
        SetFilePointer(File, Dos.e_lfanew, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        ReadFile(File, &Nt, sizeof(Nt), &Read, NULL) && Read == sizeof(Nt) && Nt.Signature == IMAGE_NT_SIGNATURE)
    {
        SetFilePointer(File, Dos.e_lfanew + FIELD_OFFSET(IMAGE_NT_HEADERS32, OptionalHeader) +
                       Nt.FileHeader.SizeOfOptionalHeader, NULL, FILE_BEGIN);
        for (i = 0; i < Nt.FileHeader.NumberOfSections; i++)
        {
            if (!ReadFile(File, &Section, sizeof(Section), &Read, NULL) || Read != sizeof(Section))
                break;
            if (Section.SizeOfRawData > 64 && Section.PointerToRawData)
            {
                Result = Section.PointerToRawData + 32;
                break;
            }
        }
    }
    CloseHandle(File);
    return Result;
}

static void TestSignedFile(const WCHAR *Path)
{
    BYTE Hash[128], Hash2[128];
    DWORD Size, Needed;
    ALG_ID Alg;
    HRESULT hr;
    WCHAR Temp[MAX_PATH], TempDir[MAX_PATH];
    DWORD Offset;

    Size = sizeof(Hash);
    Alg = 0xdead;
    memset(Hash, 0, sizeof(Hash));
    hr = pWTHelperGetFileHash(Path, 0, NULL, Hash, &Size, &Alg);
    ok(hr == S_OK, "Signed file: got %#lx\n", hr);
    ok(Size == 16 || Size == 20 || Size == 32 || Size == 48 || Size == 64, "Digest size %lu\n", Size);
    ok(Alg != 0xdead, "No algorithm reported\n");
    if (hr != S_OK)
        return;
    Needed = Size;

    /* Size query without a buffer */
    Size = 0;
    Alg = 0xdead;
    hr = pWTHelperGetFileHash(Path, 0, NULL, NULL, &Size, &Alg);
    ok(hr == S_OK, "NULL hash buffer: got %#lx\n", hr);
    ok(Size == Needed, "NULL hash buffer: size %lu, expected %lu\n", Size, Needed);

    /* Buffer too small: ERROR_MORE_DATA and the needed size */
    Size = 4;
    hr = pWTHelperGetFileHash(Path, 0, NULL, Hash2, &Size, &Alg);
    ok(hr == ERROR_MORE_DATA, "Short buffer: got %#lx\n", hr);
    ok(Size == Needed, "Short buffer: size %lu, expected %lu\n", Size, Needed);

    /* Same digest again */
    Size = sizeof(Hash2);
    hr = pWTHelperGetFileHash(Path, 0, NULL, Hash2, &Size, &Alg);
    ok(hr == S_OK && Size == Needed && !memcmp(Hash, Hash2, Needed), "Second call differs (%#lx)\n", hr);

    /* A modified image no longer matches its signature */
    if (!GetTempPathW(MAX_PATH, TempDir) || !GetTempFileNameW(TempDir, L"wth", 0, Temp))
        return;
    if (CopyFileW(Path, Temp, FALSE))
    {
        Offset = FirstSectionDataOffset(Temp);
        if (Offset)
        {
            HANDLE File = CreateFileW(Temp, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            BYTE Byte;
            DWORD Done;

            SetFilePointer(File, Offset, NULL, FILE_BEGIN);
            ReadFile(File, &Byte, 1, &Done, NULL);
            Byte ^= 0xFF;
            SetFilePointer(File, Offset, NULL, FILE_BEGIN);
            WriteFile(File, &Byte, 1, &Done, NULL);
            CloseHandle(File);

            Size = sizeof(Hash2);
            hr = pWTHelperGetFileHash(Temp, 0, NULL, Hash2, &Size, &Alg);
            ok(hr == TRUST_E_NOSIGNATURE, "Modified image: got %#lx\n", hr);
            ok(Size == 0, "Modified image: size %lu\n", Size);
        }
        else
            skip("No section data to modify\n");
    }
    DeleteFileW(Temp);
}

START_TEST(WTHelperGetFileHash)
{
    static const WCHAR *Candidates[] = { L"kernel32.dll", L"explorer.exe" };
    WCHAR Path[MAX_PATH], Dir[MAX_PATH];
    BYTE Hash[64];
    DWORD Size, Length, i;
    ALG_ID Alg;
    HRESULT hr;
    HMODULE hWintrust = LoadLibraryW(L"wintrust.dll");

    ok(hWintrust != NULL, "wintrust.dll failed to load: %lu\n", GetLastError());
    if (!hWintrust)
        return;

    pWTHelperGetFileHash = (PFN_WTHelperGetFileHash)GetProcAddress(hWintrust, "WTHelperGetFileHash");
    ok(pWTHelperGetFileHash != NULL, "WTHelperGetFileHash is not exported (PowerShell's getfilesiginforedist.dll needs it)\n");
    if (!pWTHelperGetFileHash)
        return;

    /* No file, no name, no signature: all the same error, and the size is zeroed */
    Size = sizeof(Hash);
    hr = pWTHelperGetFileHash(L"C:\\nonexistent\\wthelper.exe", 0, NULL, Hash, &Size, &Alg);
    ok(hr == TRUST_E_NOSIGNATURE, "Missing file: got %#lx\n", hr);
    ok(Size == 0, "Missing file: size %lu\n", Size);

    Size = sizeof(Hash);
    hr = pWTHelperGetFileHash(NULL, 0, NULL, Hash, &Size, &Alg);
    ok(hr == TRUST_E_NOSIGNATURE, "NULL name: got %#lx\n", hr);

    if (GetTempPathW(MAX_PATH, Dir) && GetTempFileNameW(Dir, L"wth", 0, Path))
    {
        HANDLE File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD Written;

        WriteFile(File, "not an image", 12, &Written, NULL);
        CloseHandle(File);
        Size = sizeof(Hash);
        hr = pWTHelperGetFileHash(Path, 0, NULL, Hash, &Size, &Alg);
        ok(hr == TRUST_E_NOSIGNATURE, "Text file: got %#lx\n", hr);
        ok(Size == 0, "Text file: size %lu\n", Size);

        /* This test program itself: built here, never signed */
        if (GetModuleFileNameW(NULL, Path, MAX_PATH))
        {
            Size = sizeof(Hash);
            hr = pWTHelperGetFileHash(Path, 0, NULL, Hash, &Size, &Alg);
            ok(hr == TRUST_E_NOSIGNATURE, "Unsigned image: got %#lx\n", hr);
        }
        GetTempFileNameW(Dir, L"wth", 0, Path);
        DeleteFileW(Path);
    }

    Length = GetEnvironmentVariableW(L"WTHELPER_SIGNED_FILE", Path, MAX_PATH);
    if (Length && Length < MAX_PATH)
    {
        TestSignedFile(Path);
        return;
    }
    if (!GetWindowsDirectoryW(Dir, MAX_PATH))
        return;
    for (i = 0; i < ARRAYSIZE(Candidates); i++)
    {
        WCHAR Full[MAX_PATH];

        lstrcpyW(Full, Dir);
        lstrcatW(Full, i == 0 ? L"\\system32\\" : L"\\");
        lstrcatW(Full, Candidates[i]);
        Size = sizeof(Hash);
        hr = pWTHelperGetFileHash(Full, 0, NULL, Hash, &Size, &Alg);
        if (hr == S_OK)
        {
            TestSignedFile(Full);
            return;
        }
    }
    skip("No file with an embedded signature (set WTHELPER_SIGNED_FILE)\n");
}
