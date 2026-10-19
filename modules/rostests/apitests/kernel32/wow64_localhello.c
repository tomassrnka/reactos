/* Application-local import fixture. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <windows.h>

__declspec(dllimport) DWORD WINAPI Wow64LocalValue(void);

static void DirectoryName(WCHAR *Path)
{
    WCHAR *End = Path;
    while (*Path)
    {
        if (*Path == L'\\') End = Path;
        ++Path;
    }
    *End = 0;
}

void __cdecl mainCRTStartup(void)
{
    WCHAR Image[MAX_PATH], Library[MAX_PATH];
    if (Wow64LocalValue() != 0x12345678) ExitProcess(10);
    if (!GetModuleFileNameW(NULL, Image, MAX_PATH) ||
        !GetModuleFileNameW(GetModuleHandleW(L"wow64_local.dll"), Library, MAX_PATH))
        ExitProcess(11);
    DirectoryName(Image);
    DirectoryName(Library);
    /* The staged fixture may also exist in a system directory. Merely
     * resolving the import there must not make the adjacent-DLL test pass. */
    ExitProcess(lstrcmpiW(Image, Library) == 0 ? 0 : 12);
}
