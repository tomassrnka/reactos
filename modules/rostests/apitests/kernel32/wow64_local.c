/* Application-local import fixture. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <windows.h>

DWORD WINAPI Wow64LocalValue(void)
{
    return 0x12345678;
}
