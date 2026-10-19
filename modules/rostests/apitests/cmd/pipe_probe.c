/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Child program for the cmd pipe test: saves its process ids, its input and its command line
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windef.h>
#include <winbase.h>

/* The result file is named after the executable, so that copies do not share it.
 * Layout: DWORD parent id, DWORD own id, DWORD cbInput, the input bytes, then the command line (WCHAR, no null) */
int wmain(void)
{
    static const char Output[] = "from-probe\r\n";
    WCHAR szPath[MAX_PATH + 5];
    DWORD cch, Ids[2] = { 0, GetCurrentProcessId() }, cbWritten, cbInput = 0, cbRead;
    BYTE Input[256];
    HANDLE hFile, hIn = GetStdHandle(STD_INPUT_HANDLE);
    PCWSTR pszCmdLine = GetCommandLineW();

    {
        typedef struct
        {
            PVOID Reserved1;
            PVOID PebBaseAddress;
            PVOID Reserved2[2];
            ULONG_PTR UniqueProcessId;
            ULONG_PTR InheritedFromUniqueProcessId;
        } BASIC_INFO;
        typedef LONG (WINAPI *PFN_QUERY)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        PFN_QUERY pfnQuery = (PFN_QUERY)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
        BASIC_INFO Info;

        ZeroMemory(&Info, sizeof(Info));
        if (!pfnQuery || pfnQuery(GetCurrentProcess(), 0 /* ProcessBasicInformation */, &Info, sizeof(Info), NULL) != 0)
            return 1;
        Ids[0] = (DWORD)Info.InheritedFromUniqueProcessId;
    }

    /* Only a pipe is read: the console of a program at the left of a pipeline would block */
    if (GetFileType(hIn) == FILE_TYPE_PIPE)
    {
        while (cbInput < sizeof(Input) &&
               ReadFile(hIn, Input + cbInput, sizeof(Input) - cbInput, &cbRead, NULL) && cbRead)
        {
            cbInput += cbRead;
        }
    }

    /* The next program of the pipeline reads this; nobody may read it at all */
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), Output, sizeof(Output) - 1, &cbWritten, NULL);

    cch = GetModuleFileNameW(NULL, szPath, MAX_PATH);
    if (!cch || cch >= MAX_PATH)
        return 2;
    lstrcpyW(szPath + cch, L".out");

    hFile = CreateFileW(szPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return 3;
    WriteFile(hFile, Ids, sizeof(Ids), &cbWritten, NULL);
    WriteFile(hFile, &cbInput, sizeof(cbInput), &cbWritten, NULL);
    WriteFile(hFile, Input, cbInput, &cbWritten, NULL);
    WriteFile(hFile, pszCmdLine, lstrlenW(pszCmdLine) * sizeof(WCHAR), &cbWritten, NULL);
    CloseHandle(hFile);
    return 42;
}
