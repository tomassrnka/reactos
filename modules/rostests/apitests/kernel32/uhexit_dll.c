/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Helper DLL for UnhandledExceptionExit: leaves a marker file when it gets DLL_PROCESS_DETACH
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include <windows.h>

/* <temp>uhexit.<pid>.txt, without the C runtime */
static VOID WriteMarker(VOID)
{
    WCHAR Path[MAX_PATH + 32], Digits[16];
    DWORD Length, Pid = GetCurrentProcessId(), Written;
    int Count = 0;
    HANDLE hFile;

    Length = GetTempPathW(MAX_PATH, Path);
    if (!Length || Length >= MAX_PATH)
        return;
    lstrcatW(Path, L"uhexit.");
    do
    {
        Digits[Count++] = L'0' + (WCHAR)(Pid % 10);
        Pid /= 10;
    } while (Pid);
    Length = lstrlenW(Path);
    while (Count)
        Path[Length++] = Digits[--Count];
    Path[Length] = UNICODE_NULL;
    lstrcatW(Path, L".txt");

    hFile = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        WriteFile(hFile, "DETACH", 6, &Written, NULL);
        CloseHandle(hFile);
    }
}

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, PVOID Reserved)
{
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Reserved);
    if (Reason == DLL_PROCESS_DETACH)
        WriteMarker();
    return TRUE;
}

VOID WINAPI UhExitDummy(VOID)
{
}
