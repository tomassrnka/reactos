/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the cmd.exe mklink command (junction, /d, file symlink, /h),
 *              from the command line and from a batch file
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "precomp.h"
#include <strsafe.h>

#ifndef FSCTL_GET_REPARSE_POINT
#define FSCTL_GET_REPARSE_POINT 0x000900A8
#endif
#define TEST_TAG_MOUNT_POINT 0xA0000003
#define TEST_TAG_SYMLINK     0xA000000C
#ifndef FILE_FLAG_OPEN_REPARSE_POINT
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000
#endif
#ifndef FILE_SUPPORTS_REPARSE_POINTS
#define FILE_SUPPORTS_REPARSE_POINTS 0x00000080
#endif

static WCHAR s_Dir[MAX_PATH];
static WCHAR s_ComSpec[MAX_PATH];

static BOOL WriteText(PCWSTR pszPath, const char *psz)
{
    DWORD cb = (DWORD)strlen(psz), cbWritten;
    HANDLE hFile = CreateFileW(pszPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    WriteFile(hFile, psz, cb, &cbWritten, NULL);
    CloseHandle(hFile);
    return cbWritten == cb;
}

/* Reads a file into a NUL terminated buffer, returns the length or -1 */
static int ReadText(PCWSTR pszPath, char *pBuf, DWORD cbBuf)
{
    DWORD cbRead = 0;
    HANDLE hFile = CreateFileW(pszPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);

    if (hFile == INVALID_HANDLE_VALUE)
        return -1;
    ReadFile(hFile, pBuf, cbBuf - 1, &cbRead, NULL);
    CloseHandle(hFile);
    pBuf[cbRead] = 0;
    return (int)cbRead;
}

static BOOL HasHello(PCWSTR pszPath)
{
    char sz[64];
    int cb = ReadText(pszPath, sz, sizeof(sz));

    return cb >= 5 && !strncmp(sz, "hello", 5);
}

/* Runs "cmd.exe /d /c <args>" hidden in pszDir and returns its exit code */
static DWORD RunCmd(PCWSTR pszArgs, PCWSTR pszDir)
{
    WCHAR szCmd[2048];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD dwExit = (DWORD)-1;

    StringCchPrintfW(szCmd, _countof(szCmd), L"\"%s\" /d /c %s", s_ComSpec, pszArgs);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, szCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, pszDir, &si, &pi))
    {
        ok(FALSE, "CreateProcessW failed: %lu (%S)\n", GetLastError(), szCmd);
        return dwExit;
    }
    if (WaitForSingleObject(pi.hProcess, 30000) != WAIT_OBJECT_0)
    {
        ok(FALSE, "cmd did not finish: %S\n", szCmd);
        TerminateProcess(pi.hProcess, 1);
    }
    GetExitCodeProcess(pi.hProcess, &dwExit);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return dwExit;
}

static DWORD GetReparse(PCWSTR pszPath, BYTE *pBuf, DWORD cbBuf)
{
    DWORD cbGot = 0;
    HANDLE hFile = CreateFileW(pszPath, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);

    if (hFile == INVALID_HANDLE_VALUE)
        return 0;
    if (!DeviceIoControl(hFile, FSCTL_GET_REPARSE_POINT, NULL, 0, pBuf, cbBuf, &cbGot, NULL))
        cbGot = 0;
    CloseHandle(hFile);
    return cbGot;
}

/* Runs `mklink <args>` either directly (cmd /c) or from a batch file; the output of mklink goes
 * to mkl.out. Batch: the script does a cd and calls mklink with %errorlevel% as the exit code */
static DWORD DoMklink(BOOL bBatch, PCWSTR pszArgs)
{
    WCHAR szCmd[2048], szBat[MAX_PATH], szOut[MAX_PATH];
    char sz[1024];
    HANDLE hFile;
    DWORD cbWritten;

    StringCchPrintfW(szOut, _countof(szOut), L"%s\\mkl.out", s_Dir);
    DeleteFileW(szOut);
    if (!bBatch)
    {
        StringCchPrintfW(szCmd, _countof(szCmd), L"\"mklink %s > \"%s\" 2>&1\"", pszArgs, szOut);
        return RunCmd(szCmd, s_Dir);
    }
    StringCchPrintfW(szBat, _countof(szBat), L"%s\\mk.bat", s_Dir);
    StringCbPrintfA(sz, sizeof(sz), "@echo off\r\nsetlocal\r\ncd /d \"%S\"\r\nmklink %S > \"%S\" 2>&1\r\nexit /b %%errorlevel%%\r\n",
                    s_Dir, pszArgs, szOut);
    hFile = CreateFileW(szBat, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        skip("Cannot create %S: %lu\n", szBat, GetLastError());
        return (DWORD)-1;
    }
    WriteFile(hFile, sz, (DWORD)strlen(sz), &cbWritten, NULL);
    CloseHandle(hFile);
    StringCchPrintfW(szCmd, _countof(szCmd), L"\"%s\"", szBat);
    return RunCmd(szCmd, s_Dir);
}

/* The "created for <link> <<===>> <target>" message echoes the arguments back; arguments that were
 * freed before they are printed show up as garbage here (and in the link target) */
static void CheckMessage(PCWSTR pszLabel, PCWSTR pszLink, PCWSTR pszTarget)
{
    WCHAR szOut[MAX_PATH];
    char szMsg[2048], szWant[2 * MAX_PATH + 32];
    int cb;

    StringCchPrintfW(szOut, _countof(szOut), L"%s\\mkl.out", s_Dir);
    cb = ReadText(szOut, szMsg, sizeof(szMsg));
    ok(cb > 0, "%S: mklink printed nothing\n", pszLabel);
    if (cb <= 0)
        return;
    StringCbPrintfA(szWant, sizeof(szWant), "%S <<===>> %S", pszLink, pszTarget);
    ok(strstr(szMsg, szWant) != NULL, "%S: message is not '%s': %s\n", pszLabel, szWant, szMsg);
}

/* pszExpect: absolute target, no \??\ prefix and no trailing backslash */
static void CheckJunctionBuffer(PCWSTR pszLabel, PCWSTR pszLink, PCWSTR pszExpect)
{
    BYTE Buf[2048];
    DWORD cb = GetReparse(pszLink, Buf, sizeof(Buf)), Tag;
    WORD rdl, so, sl, po, pl;
    WCHAR szSub[MAX_PATH] = L"", szPrint[MAX_PATH] = L"", szExpSub[MAX_PATH];
    size_t cchExp = wcslen(pszExpect);

    ok(cb >= 16, "%S: FSCTL_GET_REPARSE_POINT returned %lu bytes (error %lu)\n", pszLabel, cb, GetLastError());
    if (cb < 16)
        return;
    Tag = *(DWORD *)Buf;
    rdl = *(WORD *)(Buf + 4);
    so = *(WORD *)(Buf + 8);
    sl = *(WORD *)(Buf + 10);
    po = *(WORD *)(Buf + 12);
    pl = *(WORD *)(Buf + 14);
    ok(Tag == TEST_TAG_MOUNT_POINT, "%S: tag is 0x%08lx\n", pszLabel, Tag);
    ok(cb == (DWORD)rdl + 8, "%S: ReparseDataLength %u, buffer %lu bytes\n", pszLabel, rdl, cb);
    ok(*(WORD *)(Buf + 6) == 0, "%S: Reserved is %u\n", pszLabel, *(WORD *)(Buf + 6));
    if (sl < sizeof(szSub) - 2 && 16 + so + sl <= cb)
        memcpy(szSub, Buf + 16 + so, sl);
    if (pl < sizeof(szPrint) - 2 && 16 + po + pl <= cb)
        memcpy(szPrint, Buf + 16 + po, pl);
    StringCchPrintfW(szExpSub, _countof(szExpSub), L"\\??\\%s", pszExpect);
    ok(!_wcsicmp(szSub, szExpSub), "%S: SubstituteName is '%S', expected '%S'\n", pszLabel, szSub, szExpSub);
    ok(!_wcsicmp(szPrint, pszExpect), "%S: PrintName is '%S', expected '%S'\n", pszLabel, szPrint, pszExpect);
    ok(so == 0 && sl == (cchExp + 4) * sizeof(WCHAR), "%S: SubstituteName offset %u length %u (bytes, no NUL)\n",
       pszLabel, so, sl);
    ok(po == sl + sizeof(WCHAR) && pl == cchExp * sizeof(WCHAR), "%S: PrintName offset %u length %u\n",
       pszLabel, po, pl);
    ok(rdl == 8 + sl + 2 + pl + 2, "%S: ReparseDataLength %u, expected %u\n", pszLabel, rdl, 8 + sl + 2 + pl + 2);
    ok(16 + sl <= cb - 1 && *(WCHAR *)(Buf + 16 + sl) == 0 && *(WCHAR *)(Buf + 16 + po + pl) == 0,
       "%S: the names are not NUL terminated\n", pszLabel);
}

/* dir / cd / type through a link. fGating is FALSE for symlinks: whether the file system follows
 * them is not the job of cmd, so only report it */
static void CheckThrough(PCWSTR pszLabel, PCWSTR pszLink, BOOL fGating)
{
    WCHAR szPath[MAX_PATH], szOut[MAX_PATH], szCmd[1024];
    WIN32_FIND_DATAW fd;
    HANDLE hFind;
    char szData[2048];
    BOOL bRead, bFound = FALSE, bDir, bCd;
    int cb;

    StringCchPrintfW(szPath, _countof(szPath), L"%s\\hello.txt", pszLink);
    bRead = HasHello(szPath);
    StringCchPrintfW(szPath, _countof(szPath), L"%s\\*", pszLink);
    hFind = FindFirstFileW(szPath, &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!_wcsicmp(fd.cFileName, L"hello.txt"))
                bFound = TRUE;
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    StringCchPrintfW(szOut, _countof(szOut), L"%s\\through.out", s_Dir);
    StringCchPrintfW(szCmd, _countof(szCmd), L"\"dir \"%s\" > \"%s\" 2>&1\"", pszLink, szOut);
    DeleteFileW(szOut);
    RunCmd(szCmd, s_Dir);
    cb = ReadText(szOut, szData, sizeof(szData));
    bDir = cb > 0 && strstr(szData, "hello.txt") != NULL;
    StringCchPrintfW(szCmd, _countof(szCmd), L"\"cd /d \"%s\" && type hello.txt > \"%s\" 2>&1\"", pszLink, szOut);
    DeleteFileW(szOut);
    RunCmd(szCmd, s_Dir);
    bCd = HasHello(szOut);
    DeleteFileW(szOut);

    if (fGating)
    {
        ok(bRead, "%S: cannot read hello.txt through the link\n", pszLabel);
        ok(bFound, "%S: FindFirstFile through the link does not list hello.txt\n", pszLabel);
        ok(bDir, "%S: cmd dir through the link does not list hello.txt\n", pszLabel);
        ok(bCd, "%S: cmd cd /d and type through the link failed\n", pszLabel);
    }
    else
    {
        trace("%S: through the link: read %d, find %d, dir %d, cd+type %d\n", pszLabel, bRead, bFound, bDir, bCd);
    }
}

static void TestJunction(BOOL bBatch)
{
    PCWSTR pszWhere = bBatch ? L"batch" : L"cmdline";
    WCHAR szTarget[MAX_PATH], szLink[MAX_PATH], szArgs[1024], szLabel[64];
    DWORD dwAttr, dwExit;
    UINT k;

    StringCchPrintfW(szTarget, _countof(szTarget), L"%s\\tgt", s_Dir);
    for (k = 0; k < 2; k++) /* absolute, then relative target */
    {
        StringCchPrintfW(szLabel, _countof(szLabel), L"junction %s %s", pszWhere, k ? L"relative" : L"absolute");
        StringCchPrintfW(szLink, _countof(szLink), L"%s\\j_%s_%s", s_Dir, pszWhere, k ? L"rel" : L"abs");
        if (k)
            StringCchPrintfW(szArgs, _countof(szArgs), L"/j \"%s\" tgt", szLink);
        else
            StringCchPrintfW(szArgs, _countof(szArgs), L"/j \"%s\" \"%s\"", szLink, szTarget);
        dwExit = DoMklink(bBatch, szArgs);
        ok(dwExit == 0, "%S: mklink /j exit code %lu\n", szLabel, dwExit);
        if (dwExit)
            continue;
        CheckMessage(szLabel, szLink, k ? L"tgt" : szTarget);
        dwAttr = GetFileAttributesW(szLink);
        ok(dwAttr != INVALID_FILE_ATTRIBUTES && (dwAttr & FILE_ATTRIBUTE_REPARSE_POINT) &&
           (dwAttr & FILE_ATTRIBUTE_DIRECTORY), "%S: attributes 0x%lx\n", szLabel, dwAttr);
        CheckJunctionBuffer(szLabel, szLink, szTarget);
        CheckThrough(szLabel, szLink, TRUE);
    }
}

static void TestSymlinks(BOOL bBatch)
{
    PCWSTR pszWhere = bBatch ? L"batch" : L"cmdline";
    WCHAR szTarget[MAX_PATH], szLink[MAX_PATH], szArgs[1024], szLabel[64], szExpSub[MAX_PATH];
    BYTE Buf[2048];
    DWORD dwExit, cb;
    int bDir;

    for (bDir = 1; bDir >= 0; bDir--)
    {
        StringCchPrintfW(szLabel, _countof(szLabel), L"%s symlink %s", bDir ? L"directory" : L"file", pszWhere);
        StringCchPrintfW(szTarget, _countof(szTarget), L"%s\\%s", s_Dir, bDir ? L"tgt" : L"tgt\\hello.txt");
        StringCchPrintfW(szLink, _countof(szLink), L"%s\\s_%s_%s", s_Dir, pszWhere, bDir ? L"d" : L"f");
        StringCchPrintfW(szArgs, _countof(szArgs), L"%s \"%s\" \"%s\"", bDir ? L"/d" : L"", szLink, szTarget);
        dwExit = DoMklink(bBatch, szArgs);
        ok(dwExit == 0, "%S: mklink exit code %lu\n", szLabel, dwExit);
        if (dwExit)
            continue;
        CheckMessage(szLabel, szLink, szTarget);
        cb = GetReparse(szLink, Buf, sizeof(Buf));
        ok(cb > 20 && *(DWORD *)Buf == TEST_TAG_SYMLINK, "%S: %lu bytes, tag 0x%08lx\n", szLabel, cb,
           cb >= 4 ? *(DWORD *)Buf : 0);
        if (cb > 20 && *(DWORD *)Buf == TEST_TAG_SYMLINK)
        {
            WORD so = *(WORD *)(Buf + 8), sl = *(WORD *)(Buf + 10), po = *(WORD *)(Buf + 12), pl = *(WORD *)(Buf + 14);
            DWORD dwFlags = *(DWORD *)(Buf + 16);
            WCHAR szSub[MAX_PATH] = L"", szPrint[MAX_PATH] = L"";

            if (sl < sizeof(szSub) - 2 && 20 + so + sl <= cb)
                memcpy(szSub, Buf + 20 + so, sl);
            if (pl < sizeof(szPrint) - 2 && 20 + po + pl <= cb)
                memcpy(szPrint, Buf + 20 + po, pl);
            StringCchPrintfW(szExpSub, _countof(szExpSub), L"\\??\\%s", szTarget);
            ok(!_wcsicmp(szSub, szExpSub), "%S: SubstituteName is '%S'\n", szLabel, szSub);
            ok(!_wcsicmp(szPrint, szTarget), "%S: PrintName is '%S'\n", szLabel, szPrint);
            ok(dwFlags == 0, "%S: Flags 0x%lx\n", szLabel, dwFlags);
            ok(cb == (DWORD)*(WORD *)(Buf + 4) + 8, "%S: ReparseDataLength %u, buffer %lu\n", szLabel,
               *(WORD *)(Buf + 4), cb);
        }
        if (bDir)
            CheckThrough(szLabel, szLink, FALSE);
        else
            trace("%S: file read through the symlink: %d\n", szLabel, HasHello(szLink));
    }
}

static void TestHardLink(BOOL bBatch)
{
    PCWSTR pszWhere = bBatch ? L"batch" : L"cmdline";
    WCHAR szTarget[MAX_PATH], szLink[MAX_PATH], szArgs[1024], szLabel[64];
    BY_HANDLE_FILE_INFORMATION bhfi = { 0 };
    HANDLE hFile;
    DWORD dwExit;

    StringCchPrintfW(szLabel, _countof(szLabel), L"hard link %s", pszWhere);
    StringCchPrintfW(szTarget, _countof(szTarget), L"%s\\tgt\\hl_%s.txt", s_Dir, pszWhere);
    StringCchPrintfW(szLink, _countof(szLink), L"%s\\h_%s.txt", s_Dir, pszWhere);
    WriteText(szTarget, "hello world\r\n");
    StringCchPrintfW(szArgs, _countof(szArgs), L"/h \"%s\" \"%s\"", szLink, szTarget);
    dwExit = DoMklink(bBatch, szArgs);
    ok(dwExit == 0, "%S: mklink /h exit code %lu\n", szLabel, dwExit);
    if (dwExit)
        return;
    CheckMessage(szLabel, szLink, szTarget);
    ok(HasHello(szLink), "%S: the link has no content\n", szLabel);
    hFile = CreateFileW(szTarget, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        GetFileInformationByHandle(hFile, &bhfi);
        CloseHandle(hFile);
    }
    ok(bhfi.nNumberOfLinks >= 2, "%S: nNumberOfLinks is %lu\n", szLabel, bhfi.nNumberOfLinks);
}

/* Symbolic links need a privilege and a file system that has them; hard links a file system that has them */
static BOOL CanCreate(BOOL bSymlink)
{
    WCHAR szLink[MAX_PATH], szTgt[MAX_PATH];
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    BOOL bOk = FALSE;

    StringCchPrintfW(szLink, _countof(szLink), L"%s\\probe_%u", s_Dir, bSymlink);
    StringCchPrintfW(szTgt, _countof(szTgt), L"%s\\tgt%s", s_Dir, bSymlink ? L"" : L"\\hello.txt");
    if (bSymlink)
    {
        BOOL (WINAPI *pCreateSymbolicLinkW)(LPCWSTR, LPCWSTR, DWORD) =
            (PVOID)GetProcAddress(hKernel32, "CreateSymbolicLinkW");
        bOk = pCreateSymbolicLinkW && pCreateSymbolicLinkW(szLink, szTgt, 1);
        if (bOk)
            RemoveDirectoryW(szLink);
    }
    else
    {
        BOOL (WINAPI *pCreateHardLinkW)(LPCWSTR, LPCWSTR, LPSECURITY_ATTRIBUTES) =
            (PVOID)GetProcAddress(hKernel32, "CreateHardLinkW");
        bOk = pCreateHardLinkW && pCreateHardLinkW(szLink, szTgt, NULL);
        if (bOk)
            DeleteFileW(szLink);
    }
    return bOk;
}

static void RemoveTree(void)
{
    WCHAR szCmd[MAX_PATH * 2];

    /* rmdir removes a junction or directory symlink itself, not what it points to */
    StringCchPrintfW(szCmd, _countof(szCmd), L"\"rmdir /s /q \"%s\"\"", s_Dir);
    RunCmd(szCmd, NULL);
}

START_TEST(mklink)
{
    WCHAR szTemp[MAX_PATH], szRoot[4], szHello[MAX_PATH], szTgt[MAX_PATH];
    DWORD cch, dwFlags = 0;
    BOOL bBatch, bSymlink, bHardLink;

    cch = GetTempPathW(_countof(szTemp) - 40, szTemp);
    if (!cch || cch >= _countof(szTemp) - 40)
    {
        skip("Cannot get the temporary path\n");
        return;
    }
    StringCchCopyNW(szRoot, _countof(szRoot), szTemp, 3);
    if (!GetVolumeInformationW(szRoot, NULL, 0, NULL, NULL, &dwFlags, NULL, 0) ||
        !(dwFlags & FILE_SUPPORTS_REPARSE_POINTS))
    {
        skip("%S does not support reparse points\n", szRoot);
        return;
    }
    if (FAILED(StringCchPrintfW(s_Dir, _countof(s_Dir), L"%smklink test %lu", szTemp, GetCurrentProcessId())))
    {
        skip("The temporary path is too long\n");
        return;
    }
    if (!CreateDirectoryW(s_Dir, NULL))
    {
        skip("Cannot create %S: %lu\n", s_Dir, GetLastError());
        return;
    }
    /* %TEMP% may be an 8.3 name (ADMINI~1); cmd's cd and GetFullPathName hand back the long one,
     * and the junction targets are compared as strings */
    cch = GetLongPathNameW(s_Dir, szTemp, _countof(szTemp));
    if (cch && cch < _countof(szTemp))
        StringCchCopyW(s_Dir, _countof(s_Dir), szTemp);
    cch = GetEnvironmentVariableW(L"ComSpec", s_ComSpec, _countof(s_ComSpec));
    if (!cch || cch >= _countof(s_ComSpec))
        StringCchCopyW(s_ComSpec, _countof(s_ComSpec), L"cmd.exe");

    StringCchPrintfW(szTgt, _countof(szTgt), L"%s\\tgt", s_Dir);
    StringCchPrintfW(szHello, _countof(szHello), L"%s\\tgt\\hello.txt", s_Dir);
    if (!CreateDirectoryW(szTgt, NULL) || !WriteText(szHello, "hello world\r\n"))
    {
        skip("Cannot create the test files: %lu\n", GetLastError());
        RemoveTree();
        return;
    }

    bSymlink = CanCreate(TRUE);
    bHardLink = CanCreate(FALSE);
    for (bBatch = FALSE; bBatch <= TRUE; bBatch++)
    {
        TestJunction(bBatch);
        if (bSymlink)
            TestSymlinks(bBatch);
        else
            skip("Symbolic links cannot be created here\n");
        if (bHardLink)
            TestHardLink(bBatch);
        else
            skip("Hard links cannot be created here\n");
    }

    RemoveTree();
    ok(GetFileAttributesW(s_Dir) == INVALID_FILE_ATTRIBUTES, "%S was not removed\n", s_Dir);
}
