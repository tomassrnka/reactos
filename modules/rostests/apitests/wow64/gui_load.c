/* SysWOW64 GUI DLL load smoke test. SPDX-License-Identifier: LGPL-2.1-or-later */
#define WIN32_NO_STATUS
#include <windows.h>
#include <stdio.h>

int main(void)
{
    static const char *Names[] = { "user32.dll", "gdi32.dll", "imm32.dll" };
    HMODULE Modules[3];
    BOOL (WINAPI *SetRectProc)(LPRECT, int, int, int, int);
    DWORD (WINAPI *GdiGetBatchLimitProc)(void);
    BOOL Wow64;
    RECT Rect;
    unsigned int i;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    if (!IsWow64Process(GetCurrentProcess(), &Wow64))
        return 1;
    printf("PE32 GUI DLL load: wow64=%u\n", Wow64);
    for (i = 0; i < ARRAYSIZE(Names); ++i)
    {
        char Path[MAX_PATH];
        Modules[i] = LoadLibraryA(Names[i]);
        if (!Modules[i])
        {
            printf("FAIL: LoadLibrary(%s), error=%lu\n", Names[i], GetLastError());
            return 1;
        }
        if (!GetModuleFileNameA(Modules[i], Path, ARRAYSIZE(Path)))
            return 1;
        printf("Loaded %s\n", Path);
    }

    SetRectProc = (void *)GetProcAddress(Modules[0], "SetRect");
    GdiGetBatchLimitProc = (void *)GetProcAddress(Modules[1], "GdiGetBatchLimit");
    if (!SetRectProc || !GdiGetBatchLimitProc)
        return 1;
    if (!SetRectProc(&Rect, 1, 2, 3, 4) ||
        Rect.left != 1 || Rect.top != 2 || Rect.right != 3 || Rect.bottom != 4)
        return 1;
    printf("SetRect passed; GdiGetBatchLimit=%lu\n", GdiGetBatchLimitProc());
    for (i = ARRAYSIZE(Names); i != 0; --i)
        FreeLibrary(Modules[i - 1]);
    puts("GUI DLL load smoke test passed");
    return 0;
}
