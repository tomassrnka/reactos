/* WM_COPYDATA between threads, through win32k in both directions: the
 * sender's record is widened for the kernel, the receiver's callback gets
 * the i386 record with its data. 32-bit Total Commander and its 64-bit
 * instances and helpers exchange command lines this way.
 * Same results on native i386.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define REPLY 0x1234c0de
static unsigned Checks, Failures;
static HWND Window;
static HANDLE Ready;
static char Received[64];
static ULONG_PTR ReceivedTag;
static DWORD ReceivedSize = MAXDWORD;
static BOOL ReceivedPointer;
#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; printf("FAIL line %u: %s error=%lu\n", __LINE__, #x, GetLastError()); } } while (0)

static LRESULT CALLBACK WindowProc(HWND Wnd, UINT Message, WPARAM Wparam, LPARAM Lparam)
{
    if (Message == WM_COPYDATA)
    {
        const COPYDATASTRUCT *Data = (const COPYDATASTRUCT *)Lparam;
        ReceivedTag = Data->dwData;
        ReceivedSize = Data->cbData;
        /* user32 points lpData behind the record, even for no data */
        ReceivedPointer = Data->lpData == (PVOID)(Data + 1);
        if (Data->cbData && Data->cbData <= sizeof(Received) && Data->lpData)
            memcpy(Received, Data->lpData, Data->cbData);
        return REPLY;
    }
    return DefWindowProcA(Wnd, Message, Wparam, Lparam);
}

static DWORD WINAPI Receiver(LPVOID Parameter)
{
    MSG Msg;
    (void)Parameter;
    Window = CreateWindowA("Wow64CopyData", "copydata", WS_OVERLAPPED, 0, 0, 50, 50, NULL, NULL, NULL, NULL);
    SetEvent(Ready);
    if (!Window) return 1;
    while (GetMessageA(&Msg, NULL, 0, 0) > 0) DispatchMessageA(&Msg);
    return 0;
}

int main(void)
{
    WNDCLASSA Class = {0};
    COPYDATASTRUCT Data;
    static const char Text[] = "/O /L=C:\\ total commander";
    HANDLE Thread;
    DWORD_PTR Result = 0;
    BOOL Wow64 = FALSE;
    setvbuf(stdout, NULL, _IONBF, 0);
    /* A PE32 fixture: WoW64 on amd64, the native control on i386 */
    CHECK(sizeof(void *) == 4);
    CHECK(IsWow64Process(GetCurrentProcess(), &Wow64));
    printf("START copydata wow64=%u\n", Wow64);
    Class.lpfnWndProc = WindowProc;
    Class.hInstance = GetModuleHandleA(NULL);
    Class.lpszClassName = "Wow64CopyData";
    CHECK(RegisterClassA(&Class) != 0);
    Ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    Thread = CreateThread(NULL, 0, Receiver, NULL, 0, NULL);
    CHECK(Thread != NULL);
    if (!Thread) return 1;
    WaitForSingleObject(Ready, 30000);
    CHECK(Window != NULL);
    if (Window)
    {
        /* Before the fix the send failed with ERROR_CALL_NOT_IMPLEMENTED */
        Data.dwData = 0x7c;
        Data.cbData = sizeof(Text);
        Data.lpData = (PVOID)Text;
        CHECK(SendMessageTimeoutA(Window, WM_COPYDATA, 0, (LPARAM)&Data, SMTO_BLOCK, 10000, &Result) != 0);
        CHECK(Result == REPLY);
        CHECK(ReceivedTag == 0x7c);
        CHECK(ReceivedSize == sizeof(Text));
        CHECK(!memcmp(Received, Text, sizeof(Text)));
        CHECK(ReceivedPointer);
        /* No data at all */
        Data.dwData = 0x7d; Data.cbData = 0; Data.lpData = NULL; Result = 0;
        CHECK(SendMessageTimeoutA(Window, WM_COPYDATA, 0, (LPARAM)&Data, SMTO_BLOCK, 10000, &Result) != 0);
        CHECK(Result == REPLY && ReceivedTag == 0x7d && ReceivedSize == 0 && ReceivedPointer);
        PostMessageA(Window, WM_QUIT, 0, 0);
    }
    CHECK(WaitForSingleObject(Thread, 30000) == WAIT_OBJECT_0);
    printf("COPYDATA: %u checks, %u failures\n", Checks, Failures);
    return Failures ? 1 : 0;
}
