/* Private ABI boundary regression tests, same PE32 binary on i386 and WoW64.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_NO_STATUS
#include <windows.h>
#include <stdio.h>
#define NTOS_MODE_USER
#include <ndk/ntndk.h>
#include <imm.h>
#include <immdev.h>
#include <imm32_undoc.h>
#include <ntusrtyp.h>
#include <ntuser.h>

static unsigned Failures;
#define CHECK(c) do { if (!(c)) { ++Failures; printf("FAIL line %u: %s error=%lu\n",__LINE__,#c,GetLastError()); } } while(0)
static LRESULT CALLBACK Proc(HWND Window,UINT Msg,WPARAM Wparam,LPARAM Lparam)
{
    if (Msg == WM_APP) { SetLastError(0x1234); return -123; }
    return DefWindowProcW(Window,Msg,Wparam,Lparam);
}
int main(void)
{
    struct { MSG Msg; ULONG Guard[4]; } Message;
    struct { PAINTSTRUCT Paint; ULONG Guard[4]; } Paint;
    struct { WNDCLASSEXW Class; ULONG Guard[4]; } ClassInfo;
    WNDCLASSEXW Class={sizeof(Class)};
    UNICODE_STRING Name;
    HWND Window;
    LRESULT Result=0;
    ULONG i;
    BOOL Wow64=FALSE;
    BYTE Connect[576+16];
    ULONG ConnectSize;
    IsWow64Process(GetCurrentProcess(),&Wow64);
    printf("START win32k ABI wow64=%u\n",Wow64);
    RtlFillMemory(Connect,sizeof(Connect),0xa5);
    ConnectSize=Wow64 ? 576 : sizeof(USERCONNECT);
    RtlZeroMemory(Connect,ConnectSize);
    *(ULONG *)Connect=USER_VERSION;
    CHECK(NtUserProcessConnect(NtCurrentProcess(),(PUSERCONNECT)Connect,ConnectSize)==STATUS_SUCCESS);
    for (i=ConnectSize;i<sizeof(Connect);++i) CHECK(Connect[i]==0xa5);
    CHECK(NtUserProcessConnect(NtCurrentProcess(),(PUSERCONNECT)Connect,ConnectSize-1)==STATUS_INFO_LENGTH_MISMATCH);
    Class.lpfnWndProc=Proc; Class.hInstance=GetModuleHandleW(NULL); Class.lpszClassName=L"Wow64Abi";
    CHECK(RegisterClassExW(&Class)!=0);
    Window=CreateWindowExW(0,Class.lpszClassName,L"ABI",0,0,0,80,80,NULL,NULL,Class.hInstance,NULL);
    CHECK(Window!=NULL);
    if (!Window) return 1;
    RtlInitUnicodeString(&Name,Class.lpszClassName);
    RtlFillMemory(&ClassInfo,sizeof(ClassInfo),0xa5); ClassInfo.Class.cbSize=sizeof(ClassInfo.Class);
    CHECK(NtUserGetClassInfo(Class.hInstance,&Name,&ClassInfo.Class,NULL,FALSE));
    CHECK(ClassInfo.Class.lpfnWndProc==Proc);
    for(i=0;i<4;++i) CHECK(ClassInfo.Guard[i]==0xa5a5a5a5);
    CHECK(PostMessageW(Window,WM_APP+1,0x87654321,(LPARAM)0xf1234567));
    RtlFillMemory(&Message,sizeof(Message),0xa5);
    CHECK(NtUserPeekMessage(&Message.Msg,Window,WM_APP+1,WM_APP+1,PM_REMOVE));
    CHECK(Message.Msg.hwnd==Window && Message.Msg.wParam==0x87654321 && Message.Msg.lParam==(LPARAM)0xf1234567);
    for(i=0;i<4;++i) CHECK(Message.Guard[i]==0xa5a5a5a5);
    CHECK(NtUserGetMessage(NULL,NULL,0,0)==-1);
    CHECK(NtUserMessageCall(Window,WM_APP,0,0,(ULONG_PTR)&Result,FNID_SENDMESSAGE,FALSE));
    CHECK(Result==-123);
    CHECK(GetLastError()==0x1234);
    RtlFillMemory(&Paint,sizeof(Paint),0xa5);
    CHECK(NtUserBeginPaint(Window,&Paint.Paint)!=NULL);
    for(i=0;i<4;++i) CHECK(Paint.Guard[i]==0xa5a5a5a5);
    CHECK(NtUserEndPaint(Window,&Paint.Paint));
    CHECK(SetWindowTextW(Window,L"translated title"));
    CHECK(DestroyWindow(Window));
    CHECK(UnregisterClassW(Class.lpszClassName,Class.hInstance));
    printf("RESULT win32k ABI failures=%u\n",Failures);
    return Failures ? 1 : 0;
}
