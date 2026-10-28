
#include <windows.h>

void init_locale( HMODULE module );

// Used by wine/locale.c
char system_dir[MAX_PATH];

static
void
InitSystemDir(void)
{
    GetSystemDirectoryA(system_dir, MAX_PATH);
}

/*
 * Windows has kernelbase.dll in every process. Here the code is in kernelbase_ros.dll, and kernelbase.dll is a module
 * that only forwards to it, which no process loads on its own. Programs expect it to be there: Chrome binds the
 * AppContainer functions with GetModuleHandle(L"kernelbase.dll") and fails a CHECK (browser exit) when it is missing.
 * Until now it was there by accident: kernel32's QueryUnbiasedInterruptTimePrecise forwarded to it, and Chrome's
 * first lookup of that function loaded it. Every process that loads kernelbase_ros (shlwapi imports it) keeps it
 * loaded instead, before any sandbox lockdown.
 */
static
void
PinKernelbase(void)
{
    char Path[MAX_PATH + 16];

    if (GetSystemDirectoryA(Path, MAX_PATH) && lstrlenA(Path) < MAX_PATH)
    {
        lstrcatA(Path, "\\kernelbase.dll");
        LoadLibraryA(Path);
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            InitSystemDir();
            PinKernelbase();
            //init_locale(hinstDLL);
            break;

        case DLL_THREAD_ATTACH:
            break;

        case DLL_THREAD_DETACH:
            break;

        case DLL_PROCESS_DETACH:
            break;
    }

    return TRUE;
}
