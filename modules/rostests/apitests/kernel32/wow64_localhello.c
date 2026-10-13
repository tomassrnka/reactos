/* Application-local import fixture. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <windows.h>

__declspec(dllimport) DWORD WINAPI Wow64LocalValue(void);

void __cdecl mainCRTStartup(void)
{
    ExitProcess(Wow64LocalValue() == 0x12345678 ? 0 : 10);
}
