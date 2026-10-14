/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the working directory of a shortcut started with ShellExecuteEx
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "shelltest.h"
#include <strsafe.h>
#include <shellutils.h>

static
BOOL
MakeLink(
    _In_ PCWSTR LinkPath,
    _In_ PCWSTR OutputPath,
    _In_opt_ PCWSTR WorkingDirectory)
{
    CComPtr<IShellLinkW> ShellLink;
    CComPtr<IPersistFile> PersistFile;
    WCHAR CommandPath[MAX_PATH];
    WCHAR Arguments[2 * MAX_PATH];

    if (!GetEnvironmentVariableW(L"ComSpec", CommandPath, _countof(CommandPath)))
        return FALSE;
    StringCchPrintfW(Arguments, _countof(Arguments), L"/d /c cd > \"%s\"", OutputPath);
    if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARG(IShellLinkW, &ShellLink))))
    {
        return FALSE;
    }
    ShellLink->SetPath(CommandPath);
    ShellLink->SetArguments(Arguments);
    if (WorkingDirectory)
        ShellLink->SetWorkingDirectory(WorkingDirectory);
    if (FAILED(ShellLink->QueryInterface(IID_PPV_ARG(IPersistFile, &PersistFile))))
        return FALSE;
    return SUCCEEDED(PersistFile->Save(LinkPath, TRUE));
}

/* Run the link and return the directory the started process saw as its current directory */
static
BOOL
RunLink(
    _In_ PCWSTR LinkPath,
    _In_ PCWSTR OutputPath,
    _In_ PCWSTR Directory,
    _Out_ PWSTR Result,
    _In_ UINT ResultLength)
{
    SHELLEXECUTEINFOW ExecuteInfo = { sizeof(ExecuteInfo) };
    CHAR Buffer[2 * MAX_PATH] = "";
    UINT Tries;

    Result[0] = UNICODE_NULL;
    ExecuteInfo.fMask = SEE_MASK_FLAG_NO_UI;
    ExecuteInfo.lpFile = LinkPath;
    ExecuteInfo.lpDirectory = Directory;
    ExecuteInfo.nShow = SW_HIDE;
    if (!ShellExecuteExW(&ExecuteInfo))
        return FALSE;

    for (Tries = 0; Tries < 100; Tries++)
    {
        HANDLE FileHandle = CreateFileW(OutputPath, GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        NULL, OPEN_EXISTING, 0, NULL);
        if (FileHandle != INVALID_HANDLE_VALUE)
        {
            DWORD BytesRead = 0;
            BOOL DidRead = ReadFile(FileHandle, Buffer, sizeof(Buffer) - 1, &BytesRead, NULL);

            CloseHandle(FileHandle);
            if (DidRead && BytesRead > 2 && strchr(Buffer, '\n'))
            {
                Buffer[BytesRead] = ANSI_NULL;
                break;
            }
        }
        Sleep(100);
    }
    Buffer[strcspn(Buffer, "\r\n")] = ANSI_NULL;
    return MultiByteToWideChar(CP_ACP, 0, Buffer, -1, Result, ResultLength) > 1;
}

static
BOOL
IsAscii(
    _In_ PCWSTR String)
{
    for (; *String; String++)
    {
        if (*String > 0x7F)
            return FALSE;
    }
    return TRUE;
}

START_TEST(ShellExecuteLinkDir)
{
    WCHAR TempPath[MAX_PATH];
    WCHAR RootDir[MAX_PATH];
    WCHAR WorkDir[MAX_PATH];
    WCHAR InvokerDir[MAX_PATH];
    WCHAR LinkPath[MAX_PATH];
    WCHAR FirstOutput[MAX_PATH];
    WCHAR SecondOutput[MAX_PATH];
    WCHAR Seen[MAX_PATH];
    HRESULT CoResult;
    DWORD TempLength;
    BOOL IsRun;

    TempLength = GetTempPathW(_countof(TempPath), TempPath);
    if (!TempLength || TempLength >= _countof(TempPath))
    {
        skip("Cannot get the temporary path\n");
        return;
    }
    if (FAILED(StringCchPrintfW(RootDir, _countof(RootDir), L"%sSjLinkDir%lu", TempPath,
                                GetCurrentProcessId())) ||
        FAILED(StringCchPrintfW(WorkDir, _countof(WorkDir), L"%s\\work", RootDir)) ||
        FAILED(StringCchPrintfW(InvokerDir, _countof(InvokerDir), L"%s\\invoker", RootDir)) ||
        FAILED(StringCchPrintfW(LinkPath, _countof(LinkPath), L"%s\\test.lnk", RootDir)) ||
        FAILED(StringCchPrintfW(FirstOutput, _countof(FirstOutput), L"%s\\first.txt", RootDir)) ||
        FAILED(StringCchPrintfW(SecondOutput, _countof(SecondOutput), L"%s\\second.txt", RootDir)))
    {
        skip("The temporary path is too long\n");
        return;
    }
    if (!IsAscii(RootDir))
    {
        skip("The output of cmd is read as ANSI text, the path is not ASCII\n");
        return;
    }
    CoResult = CoInitialize(NULL);
    CreateDirectoryW(RootDir, NULL);
    CreateDirectoryW(WorkDir, NULL);
    CreateDirectoryW(InvokerDir, NULL);

    /* A link without a working directory takes the directory ShellExecuteEx was given */
    if (!MakeLink(LinkPath, FirstOutput, NULL))
    {
        skip("Cannot create the link\n");
    }
    else
    {
        IsRun = RunLink(LinkPath, FirstOutput, InvokerDir, Seen, _countof(Seen));
        ok(IsRun, "ShellExecuteEx did not run the link\n");
        if (IsRun)
        {
            ok(!lstrcmpiW(Seen, InvokerDir), "link without directory: expected %ls, got %ls\n",
               InvokerDir, Seen);
        }
    }

    /* A link with a working directory keeps it */
    if (!MakeLink(LinkPath, SecondOutput, WorkDir))
    {
        skip("Cannot create the link\n");
    }
    else
    {
        IsRun = RunLink(LinkPath, SecondOutput, InvokerDir, Seen, _countof(Seen));
        ok(IsRun, "ShellExecuteEx did not run the link\n");
        if (IsRun)
        {
            ok(!lstrcmpiW(Seen, WorkDir), "link with directory: expected %ls, got %ls\n",
               WorkDir, Seen);
        }
    }

    DeleteFileW(FirstOutput);
    DeleteFileW(SecondOutput);
    DeleteFileW(LinkPath);
    RemoveDirectoryW(WorkDir);
    RemoveDirectoryW(InvokerDir);
    RemoveDirectoryW(RootDir);
    if (SUCCEEDED(CoResult))
        CoUninitialize();
}
