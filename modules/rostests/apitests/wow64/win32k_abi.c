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
    if (Msg == WM_USER + 3)
    {
        CHECK(Wparam == 0x87654321 && Lparam == (LPARAM)0xf1234567);
        SetLastError(0x4321);
        return -321;
    }
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
    DECLSPEC_ALIGN(8) BYTE Connect[576+16];
    BYTE *UnalignedConnect = Connect + sizeof(ULONG);
    ULONG ConnectSize;
    IsWow64Process(GetCurrentProcess(),&Wow64);
    printf("START win32k ABI wow64=%u\n",Wow64);
    RtlFillMemory(Connect,sizeof(Connect),0xa5);
    ConnectSize=Wow64 ? 576 : sizeof(USERCONNECT);
    RtlZeroMemory(Connect,ConnectSize);
    *(ULONG *)Connect=USER_VERSION;
    CHECK(NtUserProcessConnect(NtCurrentProcess(),(PUSERCONNECT)Connect,ConnectSize)==STATUS_SUCCESS);
    for (i=ConnectSize;i<sizeof(Connect);++i) CHECK(Connect[i]==0xa5);
    CHECK(NtUserProcessConnect(NtCurrentProcess(),(PUSERCONNECT)Connect,ConnectSize-1)==STATUS_UNSUCCESSFUL);
    /* i386 call stacks and buffers need only four-byte alignment. */
    RtlFillMemory(Connect,sizeof(Connect),0xa5);
    RtlZeroMemory(UnalignedConnect,ConnectSize);
    *(ULONG *)UnalignedConnect=USER_VERSION;
    CHECK(NtUserProcessConnect(NtCurrentProcess(),(PUSERCONNECT)UnalignedConnect,ConnectSize)==STATUS_SUCCESS);
    CHECK(*(ULONG *)Connect==0xa5a5a5a5);
    for (i=sizeof(ULONG)+ConnectSize;i<sizeof(Connect);++i) CHECK(Connect[i]==0xa5);
    SetLastError(0x1234);
    CHECK(NtUserProcessConnect(NtCurrentProcess(),(PUSERCONNECT)(Connect+1),ConnectSize)==STATUS_DATATYPE_MISALIGNMENT);
    CHECK(GetLastError()==ERROR_NOACCESS);
    CHECK(NtUserProcessConnect(NtCurrentProcess(),NULL,ConnectSize)==STATUS_UNSUCCESSFUL);
    Class.lpfnWndProc=Proc; Class.hInstance=GetModuleHandleW(NULL); Class.lpszClassName=L"Wow64Abi";
    CHECK(RegisterClassExW(&Class)!=0);
    Window=CreateWindowExW(0,Class.lpszClassName,L"ABI",0,0,0,80,80,NULL,NULL,Class.hInstance,NULL);
    CHECK(Window!=NULL);
    if (!Window) return 1;
    RtlInitUnicodeString(&Name,Class.lpszClassName);
    RtlFillMemory(&ClassInfo,sizeof(ClassInfo),0xa5); ClassInfo.Class.cbSize=sizeof(ClassInfo.Class);
    CHECK(NtUserGetClassInfo(Class.hInstance,&Name,&ClassInfo.Class,NULL,FALSE));
    CHECK(ClassInfo.Class.lpfnWndProc==Proc);
    CHECK(ClassInfo.Class.lpszClassName==(LPCWSTR)0xa5a5a5a5);
    for(i=0;i<4;++i) CHECK(ClassInfo.Guard[i]==0xa5a5a5a5);
    CHECK(PostMessageW(Window,WM_APP+1,0x87654321,(LPARAM)0xf1234567));
    RtlFillMemory(&Message,sizeof(Message),0xa5);
    CHECK(NtUserPeekMessage(&Message.Msg,Window,WM_APP+1,WM_APP+1,PM_REMOVE));
    CHECK(Message.Msg.hwnd==Window && Message.Msg.wParam==0x87654321 && Message.Msg.lParam==(LPARAM)0xf1234567);
    for(i=0;i<4;++i) CHECK(Message.Guard[i]==0xa5a5a5a5);
    /* Native ReactOS currently returns FALSE on its failed output probe.
     * The translator rejects the bad guest output before removing a message. */
    CHECK(NtUserGetMessage(NULL,(HWND)0x12345678,0,0)==(Wow64 ? -1 : FALSE));
    /* Native NtUserRealInternalGetMessage is a stub that returns FALSE and leaves the MSG alone. In
     * GetMessage mode FALSE is a retrieved WM_QUIT only when the service wrote one, so the translator
     * must not turn the stub's answer into a zeroed MSG or probe the buffer. */
    RtlFillMemory(&Message,sizeof(Message),0xa5);
    CHECK(NtUserRealInternalGetMessage(&Message.Msg,NULL,0,0,PM_NOREMOVE,FALSE)==FALSE);
    CHECK(Message.Msg.message==0xa5a5a5a5 && Message.Msg.time==0xa5a5a5a5);
    for(i=0;i<4;++i) CHECK(Message.Guard[i]==0xa5a5a5a5);
    RtlFillMemory(&Message,sizeof(Message),0xa5);
    CHECK(NtUserRealInternalGetMessage(&Message.Msg,NULL,0,0,PM_NOREMOVE,TRUE)==FALSE);
    CHECK(Message.Msg.message==0xa5a5a5a5 && Message.Msg.time==0xa5a5a5a5);
    for(i=0;i<4;++i) CHECK(Message.Guard[i]==0xa5a5a5a5);
    CHECK(NtUserRealInternalGetMessage(NULL,NULL,0,0,PM_NOREMOVE,TRUE)==FALSE);
    CHECK(NtUserRealInternalGetMessage(NULL,NULL,0,0,PM_NOREMOVE,FALSE)==FALSE);
    CHECK(NtUserMessageCall(Window,WM_APP,0,0,(ULONG_PTR)&Result,FNID_SENDMESSAGE,FALSE));
    CHECK(Result==-123);
    CHECK(GetLastError()==0x1234);
    Result=0;
    CHECK(NtUserMessageCall(Window,WM_USER+3,0x87654321,(LPARAM)0xf1234567,
                           (ULONG_PTR)&Result,FNID_SENDMESSAGE,FALSE));
    CHECK(Result==-321);
    CHECK(GetLastError()==0x4321);
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
