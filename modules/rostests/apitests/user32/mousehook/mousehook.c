/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WH_MOUSE hook module for the MouseHookTransport test: records the bytes of the
 *              MOUSEHOOKSTRUCTEX it is handed into a shared mapping
 */

#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include "mousehook.h"

LRESULT WINAPI MouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && lParam)
    {
        HANDLE map = OpenFileMappingW(FILE_MAP_WRITE, FALSE, MOUSEHOOK_MAPPING_NAME);

        if (map)
        {
            MOUSEHOOK_SHARED *shared = MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, sizeof(*shared));

            if (shared)
            {
                LONG i = InterlockedIncrement(&shared->count) - 1;

                if (i < MOUSEHOOK_MAX_RECORDS)
                {
                    MOUSEHOOK_RECORD *r = &shared->record[i];

                    r->message = (DWORD)wParam;
                    r->processId = GetCurrentProcessId();
                    r->threadId = GetCurrentThreadId();
                    r->size = sizeof(MOUSEHOOKSTRUCTEX);
                    CopyMemory(r->data, (PVOID)lParam, sizeof(MOUSEHOOKSTRUCTEX));
                }
                UnmapViewOfFile(shared);
            }
            CloseHandle(map);
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}
