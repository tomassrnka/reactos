/* ReactOS WoW64 kernel-to-user32 callbacks.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64win.h"

static PVOID GuestDispatcher;
typedef struct { STRING32 Library, Init; BOOL Unload, ApiHook; } LIBRARY32;
C_ASSERT(sizeof(LIBRARY32) == 24);
typedef struct { ULONG String, Dc; INT x, y; UINT Flags; RECT Rect; UINT Count; BOOL HasRect; } LPK32;
C_ASSERT(sizeof(LPK32) == 44);
typedef struct
{
    INT HookId, Code;
    ULONG wParam, lParam, Proc;
    INT Mod;
    ULONG offPfn;
    BOOLEAN Ansi;
    LONG Result;
    UINT lParamSize;
    WCHAR ModuleName[512];
} HOOK32;
typedef struct { ULONG lParam, wParam, message, hwnd; } CWP32;
typedef struct { LONG Result; CWP32 Cwp; } CWPR32;
typedef struct { HOOK32 Header; CWP32 Cwp; ULONG Extra[4]; } CWPCALL32;
typedef struct { HOOK32 Header; CWPR32 Cwp; ULONG Extra[4]; } CWPRCALL32;
C_ASSERT(sizeof(HOOK32) == 1064);
C_ASSERT(sizeof(CWPCALL32) == 1096);
C_ASSERT(sizeof(CWPRCALL32) == 1100);
typedef struct
{
    WOW64_CPU_STATE *Previous;
    PVOID Native;
    ULONG NativeLength, GuestLength, Id, ExceptionList;
    ULONG64 Handles[7];
} CALLBACK_FRAME;

static VOID ProbeGuestStack(ULONG Top, ULONG_PTR Bottom)
{
    TEB32 *Teb = WOW64_TEB32(NtCurrentTeb());
    ULONG_PTR Address, Reserve = Teb->DeallocationStack, Base = Teb->NtTib.StackBase;
    SIZE_T Guarantee;
    if (Base <= Reserve || Top > Base || Bottom > Top || Bottom < Reserve)
        RtlRaiseStatus(STATUS_STACK_OVERFLOW);
    Guarantee = ROUND_TO_PAGES(min((SIZE_T)Teb->GuaranteedStackBytes, Base - Reserve));
    if (Bottom - Reserve <= PAGE_SIZE + Guarantee)
        RtlRaiseStatus(STATUS_STACK_OVERFLOW);
    /* Fault each guard page in order, like _chkstk. MiCheckForUserStackOverflow
     * identifies the guest stack by its address even though this code is 64-bit. */
    Address = (Top - 1) & ~(ULONG_PTR)(PAGE_SIZE - 1);
    while (Address > Bottom)
    {
        *(volatile BYTE *)Address = *(volatile BYTE *)Address;
        Address -= PAGE_SIZE;
    }
    *(volatile BYTE *)Bottom = *(volatile BYTE *)Bottom;
}

static LONG PayloadSize(UINT Msg, WPARAM Wparam, LONG Size)
{
    if (Size < 0) return Size;
    switch (Msg)
    {
    case WM_CREATE: case WM_NCCREATE:
        return Size >= sizeof(CREATESTRUCTW) ? Size - sizeof(CREATESTRUCTW) + sizeof(CREATE32) : -2;
    case WM_WINDOWPOSCHANGING: case WM_WINDOWPOSCHANGED:
        return Size == sizeof(WINDOWPOS) ? sizeof(WINDOWPOS32) : -2;
    case WM_NCCALCSIZE:
        if (!Wparam) return Size == sizeof(RECT) ? Size : -2;
        return Size == sizeof(NCCALCSIZE_PARAMS) + sizeof(WINDOWPOS) ? sizeof(NCCALCSIZE32) + sizeof(WINDOWPOS32) : -2;
    case WM_GETMINMAXINFO: case WM_GETTEXT: case WM_SETTEXT: case WM_SETTINGCHANGE:
    case WM_STYLECHANGING: case WM_STYLECHANGED: case WM_SIZING: case WM_MOVING:
    case WM_ERASEBKGND: case WM_PAINT: case WM_NCPAINT:
        return Size;
    case WM_MEASUREITEM:
        return Size == sizeof(MEASUREITEMSTRUCT) ? sizeof(MEASUREITEM32) : -2;
    case WM_DRAWITEM:
        return Size == sizeof(DRAWITEMSTRUCT) ? sizeof(DRAWITEM32) : -2;
    case WM_COPYDATA:
        /* The record is followed by its cbData bytes */
        return Size >= sizeof(COPYDATASTRUCT) ? Size - sizeof(COPYDATASTRUCT) + sizeof(COPYDATA32) : -2;
    default: return -2;
    }
}

static VOID PackPayload(PVOID To, const PVOID From, UINT Msg, WPARAM Wparam, ULONG Size)
{
    switch (Msg)
    {
    case WM_CREATE: case WM_NCCREATE:
    {
        const CREATESTRUCTW *Native = From;
        CREATE32 *Guest = To;
        ULONG Tail = Size - sizeof(*Native);
        /* The strings are offsets into this packed block, not addresses. */
        if ((ULONG_PTR)Native->lpszName < sizeof(*Native) ||
            (ULONG_PTR)Native->lpszName >= Size ||
            (ULONG_PTR)Native->lpszClass < sizeof(*Native) ||
            (ULONG_PTR)Native->lpszClass >= Size)
            RtlRaiseStatus(STATUS_INVALID_PARAMETER);
        CreateNativeTo32(Guest, Native);
        Guest->lpszName -= sizeof(*Native) - sizeof(*Guest);
        Guest->lpszClass -= sizeof(*Native) - sizeof(*Guest);
        RtlCopyMemory(Guest + 1, Native + 1, Tail);
        break;
    }
    case WM_WINDOWPOSCHANGING: case WM_WINDOWPOSCHANGED:
        WindowPosNativeTo32(To, From); break;
    case WM_MEASUREITEM:
    {
        /* win32k's menu code measures owner-drawn items this way; the item
         * data of a guest's menu item is a guest value. */
        const MEASUREITEMSTRUCT *Native = From;
        MEASUREITEM32 *Guest = To;
        Guest->CtlType = Native->CtlType; Guest->CtlID = Native->CtlID; Guest->itemID = Native->itemID;
        Guest->itemWidth = Native->itemWidth; Guest->itemHeight = Native->itemHeight;
        Guest->itemData = (ULONG)Native->itemData;
        break;
    }
    case WM_DRAWITEM:
    {
        /* hwndItem is the HMENU for ODT_MENU */
        const DRAWITEMSTRUCT *Native = From;
        DRAWITEM32 *Guest = To;
        Guest->CtlType = Native->CtlType; Guest->CtlID = Native->CtlID; Guest->itemID = Native->itemID;
        Guest->itemAction = Native->itemAction; Guest->itemState = Native->itemState;
        Guest->hwndItem = HandleToUlong(Native->hwndItem); Guest->hDC = HandleToUlong(Native->hDC);
        Guest->rcItem = Native->rcItem; Guest->itemData = (ULONG)Native->itemData;
        break;
    }
    case WM_COPYDATA:
    {
        const COPYDATASTRUCT *Native = From;
        COPYDATA32 *Guest = To;
        ULONG Tail = Size - sizeof(*Native);
        /* Read-only for the receiver: nothing is copied back. A sender's
         * pointer-sized dwData keeps its low 32 bits. */
        if (Native->cbData != Tail) RtlRaiseStatus(STATUS_INVALID_PARAMETER);
        Guest->dwData = (ULONG)Native->dwData;
        Guest->cbData = Tail;
        Guest->lpData = PtrToUlong(Guest + 1);
        RtlCopyMemory(Guest + 1, Native + 1, Tail);
        break;
    }
    case WM_NCCALCSIZE:
        if (Wparam)
        {
            NCCALCSIZE32 *Guest = To;
            NCCALCSIZE_PARAMS *Native = From;
            RtlCopyMemory(Guest->rgrc, Native->rgrc, sizeof(Guest->rgrc));
            Guest->lppos = PtrToUlong(Guest + 1);
            WindowPosNativeTo32((PVOID)(Guest + 1), (PVOID)(Native + 1));
            break;
        }
        /* fall through */
    default: RtlCopyMemory(To, From, Size); break;
    }
}

static VOID UnpackPayload(PVOID To, const PVOID From, UINT Msg, WPARAM Wparam, ULONG Size)
{
    switch (Msg)
    {
    case WM_CREATE: case WM_NCCREATE:
        /* The guest dispatcher has changed the packed string offsets to
         * addresses, just as native user32 does. Preserve the reply fields. */
        Create32ToNative(To, From);
        break;
    case WM_WINDOWPOSCHANGING:
        WindowPos32ToNative(To, From); break;
    case WM_NCCALCSIZE:
        if (Wparam)
        {
            NCCALCSIZE_PARAMS *Native = To;
            const NCCALCSIZE32 *Guest = From;
            RtlCopyMemory(Native->rgrc, Guest->rgrc, sizeof(Native->rgrc));
            WindowPos32ToNative((PVOID)(Native + 1), (PVOID)(Guest + 1));
        }
        else RtlCopyMemory(To, From, sizeof(RECT));
        break;
    case WM_MEASUREITEM:
    {
        /* The whole record is the reply, as win32k copies it back. A
         * native itemData keeps its high bits unless the guest changed it. */
        MEASUREITEMSTRUCT *Native = To;
        const MEASUREITEM32 *Guest = From;
        Native->CtlType = Guest->CtlType; Native->CtlID = Guest->CtlID; Native->itemID = Guest->itemID;
        Native->itemWidth = Guest->itemWidth;
        Native->itemHeight = Guest->itemHeight;
        if (Guest->itemData != (ULONG)Native->itemData) Native->itemData = Guest->itemData;
        break;
    }
    case WM_GETMINMAXINFO: case WM_GETTEXT: case WM_STYLECHANGING: case WM_SIZING: case WM_MOVING:
        RtlCopyMemory(To, From, Size); break;
    }
}

/* CALLWND hooks receive a CWP structure followed by the message buffer.
 * Unlike WINDOWPROC's CREATE packet, these are ordinary CREATESTRUCT fields,
 * not offsets. Other pointer messages use the same kernel packing rules. */
static LONG HookPayloadSize(const HOOKPROC_CALLBACK_ARGUMENTS *Header)
{
    const CWPSTRUCT *Cwp;
    if (Header->HookId == WH_CALLWNDPROC)
        Cwp = &((const CWP_Struct *)Header)->cwps;
    else
        Cwp = (const CWPSTRUCT *)&((const CWPR_Struct *)Header)->cwprs.lParam;
    if (!Header->lParamSize) return 0;
    if (Cwp->message == WM_CREATE || Cwp->message == WM_NCCREATE)
        return Header->lParamSize == sizeof(CREATESTRUCTW) ? sizeof(CREATE32) : -2;
    return PayloadSize(Cwp->message, Cwp->wParam, Header->lParamSize);
}

static VOID PackHook(HOOK32 *To, const HOOKPROC_CALLBACK_ARGUMENTS *From, LONG Size)
{
    const CWPSTRUCT *Native;
    CWP32 *Guest;
    PVOID Input, Output;
    To->HookId = From->HookId; To->Code = From->Code;
    To->wParam = From->wParam; To->lParam = From->lParam;
    To->Proc = PtrToUlong(From->Proc); To->Mod = From->Mod;
    To->offPfn = From->offPfn; To->Ansi = From->Ansi;
    To->Result = 0; To->lParamSize = Size;
    RtlCopyMemory(To->ModuleName, From->ModuleName, sizeof(To->ModuleName));
    if (From->HookId == WH_GETMESSAGE)
    {
        To->lParam = sizeof(*To);
        MsgNativeTo32((MSG32 *)(To + 1), (const MSG *)(From + 1));
        return;
    }
    if (From->HookId == WH_CALLWNDPROC)
    {
        Native = &((const CWP_Struct *)From)->cwps;
        Input = (PVOID)&((const CWP_Struct *)From)->Extra;
        Guest = &((CWPCALL32 *)To)->Cwp;
        Output = &((CWPCALL32 *)To)->Extra;
    }
    else
    {
        Native = (const CWPSTRUCT *)&((const CWPR_Struct *)From)->cwprs.lParam;
        Input = (PVOID)&((const CWPR_Struct *)From)->Extra;
        Guest = &((CWPRCALL32 *)To)->Cwp.Cwp;
        ((CWPRCALL32 *)To)->Cwp.Result = ((const CWPR_Struct *)From)->cwprs.lResult;
        Output = &((CWPRCALL32 *)To)->Extra;
    }
    Guest->lParam = Native->lParam; Guest->wParam = Native->wParam;
    Guest->message = Native->message; Guest->hwnd = HandleToUlong(Native->hwnd);
    if (Size)
    {
        if (Native->message == WM_CREATE || Native->message == WM_NCCREATE)
        {
            const CREATESTRUCTW *Create = Input;
            /* Original guest strings/pointers are retained by the kernel. */
            if ((ULONG_PTR)Create->lpszName > MAXULONG || (ULONG_PTR)Create->lpszClass > MAXULONG)
                RtlRaiseStatus(STATUS_NOT_SUPPORTED);
            CreateNativeTo32(Output, Input);
        }
        else PackPayload(Output, Input, Native->message, Native->wParam, From->lParamSize);
    }
}

static NTSTATUS Callback(ULONG Id, PVOID Buffer, ULONG Length)
{
    WOW64_CPU_STATE *Previous = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU];
    WOW64_CPU_STATE Cpu;
    CALLBACK_FRAME Frame;
    ULONG GuestLength = Length, *Stack;
    PVOID Guest;
    LONG Payload = -1;
    NTSTATUS Status = STATUS_NOT_SUPPORTED;
    if (!Previous || !GuestDispatcher) return Status;
    Frame.Previous = Previous; Frame.Native = Buffer;
    Frame.NativeLength = Length; Frame.Id = Id;
    Frame.ExceptionList = WOW64_TEB32(NtCurrentTeb())->NtTib.ExceptionList;
    _SEH2_TRY
    {
        switch (Id)
        {
        case USER32_CALLBACK_HOOKPROC:
        {
            HOOKPROC_CALLBACK_ARGUMENTS *Native = Buffer;
            ULONG Base;
            if (Length < sizeof(*Native)) break;
            if (Native->HookId == WH_GETMESSAGE)
            {
                if (Length != sizeof(*Native) + sizeof(MSG) ||
                    (ULONG_PTR)Native->lParam != sizeof(*Native) || Native->lParamSize) break;
                GuestLength = sizeof(HOOK32) + sizeof(MSG32);
                Payload = 0;
                Status = STATUS_SUCCESS;
                break;
            }
            if (Native->HookId == WH_CALLWNDPROC) Base = sizeof(CWP_Struct);
            else if (Native->HookId == WH_CALLWNDPROCRET) Base = sizeof(CWPR_Struct);
            else break;
            if (Length < Base || Native->lParamSize != Length - Base) break;
            Payload = HookPayloadSize(Native);
            if (Payload < 0) break;
            GuestLength = (Native->HookId == WH_CALLWNDPROC ? sizeof(CWPCALL32) : sizeof(CWPRCALL32)) + Payload;
            Status = STATUS_SUCCESS;
            break;
        }
        case USER32_CALLBACK_WINDOWPROC:
        {
            WINDOWPROC_CALLBACK_ARGUMENTS *Native = Buffer;
            if (Length < sizeof(*Native) || Native->lParamBufferSize < -1) break;
            if (Length != sizeof(*Native) + max(Native->lParamBufferSize, 0)) break;
            Payload = PayloadSize(Native->Msg, Native->wParam, Native->lParamBufferSize);
            if (Payload == -2) { DbgPrint("WoW64win: unsupported callback message %x size %d\n",Native->Msg,Native->lParamBufferSize); break; }
            GuestLength = sizeof(WINDOWPROC32) + max(Payload,0); Status = STATUS_SUCCESS; break;
        }
        case USER32_CALLBACK_SENDASYNCPROC:
            if (Length == sizeof(SENDASYNCPROC_CALLBACK_ARGUMENTS)) { GuestLength = 20; Status = STATUS_SUCCESS; } break;
        case USER32_CALLBACK_CLIENTTHREADSTARTUP: case USER32_CALLBACK_LOADSYSMENUTEMPLATE:
        case USER32_CALLBACK_DELIVERUSERAPC:
            if (!Length) Status = STATUS_SUCCESS;
            break;
        case USER32_CALLBACK_LOADDEFAULTCURSORS:
            if (Length == sizeof(BOOL)) Status = STATUS_SUCCESS;
            break;
        case USER32_CALLBACK_GETCHARSETINFO:
            if (Length == sizeof(GET_CHARSET_INFO)) Status = STATUS_SUCCESS;
            break;
        case USER32_CALLBACK_SETOBM:
            if (Length == sizeof(SETOBM_CALLBACK_ARGUMENTS)) Status = STATUS_SUCCESS;
            break;
        case USER32_CALLBACK_CLIENTLOADLIBRARY:
            if (Length >= sizeof(CLIENT_LOAD_LIBRARY_ARGUMENTS))
            { GuestLength = Length-sizeof(CLIENT_LOAD_LIBRARY_ARGUMENTS)+sizeof(LIBRARY32); Status=STATUS_SUCCESS; } break;
        case USER32_CALLBACK_SETWNDICONS:
            if (Length == sizeof(SETWNDICONS_CALLBACK_ARGUMENTS)) { GuestLength = 7*sizeof(ULONG); Status = STATUS_SUCCESS; } break;
        case USER32_CALLBACK_LOADMENU:
            if (Length >= FIELD_OFFSET(LOADMENU_CALLBACK_ARGUMENTS, MenuName))
            { GuestLength = Length - FIELD_OFFSET(LOADMENU_CALLBACK_ARGUMENTS, MenuName) + 8; Status = STATUS_SUCCESS; } break;
        case USER32_CALLBACK_COPYIMAGE:
            if (Length == sizeof(COPYIMAGE_CALLBACK_ARGUMENTS)) { GuestLength = 20; Status = STATUS_SUCCESS; } break;
        case USER32_CALLBACK_LPK:
            if (Length >= sizeof(LPK_CALLBACK_ARGUMENTS))
            {
                LPK_CALLBACK_ARGUMENTS *Native = Buffer;
                ULONG_PTR Offset = (ULONG_PTR)Native->lpString;
                if (Offset >= sizeof(*Native) && Offset <= Length && Native->count <= (Length - Offset) / sizeof(WCHAR))
                { GuestLength = Length - sizeof(*Native) + sizeof(LPK32); Status = STATUS_SUCCESS; }
            }
            break;
        }
        if (NT_SUCCESS(Status))
        {
            /* Guest stack storage survives nested syscalls and callbacks. The
             * native frame stays live until NtCallbackReturn unwinds this call. */
            if (GuestLength > 1024 * 1024 || Previous->Esp < GuestLength + 256)
                RtlRaiseStatus(STATUS_STACK_OVERFLOW);
            Guest = P32((Previous->Esp - GuestLength - 64) & ~15);
            Stack = (ULONG *)Guest - 8;
            ProbeGuestStack(Previous->Esp, (ULONG_PTR)Stack);
            Frame.GuestLength = GuestLength;
            switch (Id)
            {
            case USER32_CALLBACK_HOOKPROC:
                PackHook(Guest, Buffer, Payload);
                break;
            case USER32_CALLBACK_WINDOWPROC:
            {
                WINDOWPROC_CALLBACK_ARGUMENTS *Native = Buffer;
                WINDOWPROC32 *Target = Guest;
                Target->Proc = PtrToUlong(Native->Proc); Target->IsAnsiProc = Native->IsAnsiProc;
                Target->Wnd = HandleToUlong(Native->Wnd); Target->Msg = Native->Msg;
                Target->wParam = Native->wParam; Target->lParam = Native->lParam;
                Target->lParamBufferSize = Payload; Target->Result = 0;
                if (Payload >= 0) PackPayload(Target+1,Native+1,Native->Msg,Native->wParam,Native->lParamBufferSize);
                break;
            }
            case USER32_CALLBACK_SENDASYNCPROC:
            {
                SENDASYNCPROC_CALLBACK_ARGUMENTS *Native = Buffer; ULONG *Target = Guest;
                Target[0]=PtrToUlong(Native->Callback); Target[1]=HandleToUlong(Native->Wnd);
                Target[2]=Native->Msg; Target[3]=Native->Context; Target[4]=Native->Result; break;
            }
            case USER32_CALLBACK_LOADMENU:
            {
                LOADMENU_CALLBACK_ARGUMENTS *Native = Buffer; ULONG *Target = Guest;
                Target[0]=HandleToUlong(Native->hModule); Target[1]=PtrToUlong(Native->InterSource);
                RtlCopyMemory(Target+2,Native->MenuName,GuestLength-8); break;
            }
            case USER32_CALLBACK_COPYIMAGE:
            {
                COPYIMAGE_CALLBACK_ARGUMENTS *Native = Buffer; ULONG *Target = Guest;
                Target[0]=HandleToUlong(Native->hImage); Target[1]=Native->uType;
                Target[2]=Native->cxDesired; Target[3]=Native->cyDesired; Target[4]=Native->fuFlags; break;
            }
            case USER32_CALLBACK_CLIENTLOADLIBRARY:
            {
                CLIENT_LOAD_LIBRARY_ARGUMENTS *Native=Buffer;
                LIBRARY32 *Target=Guest;
                const UNICODE_STRING *Src[2]={&Native->strLibraryName,&Native->strInitFuncName};
                STRING32 *Dst[2]={&Target->Library,&Target->Init};
                ULONG i;
                RtlZeroMemory(Target,sizeof(*Target));
                for (i=0;i<2;++i) if (Src[i]->Buffer)
                {
                    ULONG_PTR Offset=(ULONG_PTR)Src[i]->Buffer;
                    if (Offset < sizeof(*Native) || Offset > Length || Src[i]->MaximumLength > Length-Offset ||
                        Src[i]->Length > Src[i]->MaximumLength) RtlRaiseStatus(STATUS_INVALID_PARAMETER);
                    Dst[i]->Length=Src[i]->Length; Dst[i]->MaximumLength=Src[i]->MaximumLength;
                    Dst[i]->Buffer=Offset-sizeof(*Native)+sizeof(*Target);
                }
                Target->Unload=Native->Unload; Target->ApiHook=Native->ApiHook;
                RtlCopyMemory(Target+1,Native+1,Length-sizeof(*Native)); break;
            }
            case USER32_CALLBACK_SETWNDICONS:
                RtlZeroMemory(Guest,GuestLength); break;
            case USER32_CALLBACK_LPK:
            {
                LPK_CALLBACK_ARGUMENTS *Native = Buffer; LPK32 *Target = Guest;
                Target->String = (ULONG_PTR)Native->lpString - sizeof(*Native) + sizeof(*Target);
                Target->Dc = HandleToUlong(Native->hdc); Target->x = Native->x; Target->y = Native->y;
                Target->Flags = Native->flags; Target->Rect = Native->rect; Target->Count = Native->count; Target->HasRect = Native->bRect;
                RtlCopyMemory(Target + 1, Native + 1, Length - sizeof(*Native));
                break;
            }
            default: RtlCopyMemory(Guest,Buffer,GuestLength); break;
            }
            /* i386 KiUserCallbackDispatcher: return, index, buffer, length,
             * followed by the two-word exception registration. */
            RtlZeroMemory(Stack,8*sizeof(ULONG));
            Stack[1]=Id; Stack[2]=PtrToUlong(Guest); Stack[3]=GuestLength;
            WOW64_TEB32(NtCurrentTeb())->LastErrorValue = NtCurrentTeb()->LastErrorValue;
            Cpu = *Previous; Cpu.CallbackFrame = &Frame;
            Cpu.Esp = PtrToUlong(Stack); Cpu.Eip = PtrToUlong(GuestDispatcher);
            Wow64CpuRun(&Cpu);
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;
    DbgPrint("WoW64win: callback %lu failed %lx\n",Id,Status);
    return NtCallbackReturn(NULL,0,Status);
}

NTSTATUS NTAPI Wow64WinCallbackReturn(PVOID Buffer, ULONG Length, NTSTATUS Status)
{
    WOW64_CPU_STATE *Cpu = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU];
    CALLBACK_FRAME *Frame = Cpu ? Cpu->CallbackFrame : NULL;
    PVOID Output = NULL;
    ULONG OutputLength = 0, i;
    if (!Frame) return STATUS_NO_CALLBACK_ACTIVE;
    if (Status == STATUS_CALLBACK_POP_STACK)
    {
        WOW64_CPU_STATE *Previous = Frame->Previous;
        ULONG64 NativeStack = Previous->NativeStack;
        PVOID ApcFrame = Previous->ApcFrame;
        PVOID CallbackFrame = Previous->CallbackFrame;

        /* POP_STACK resumes this native NtCallbackReturn call after win32k
         * has unwound its callout. The guest must then resume the i386 stub
         * inside KiUserCallbackExceptionHandler, not the interrupted USER
         * syscall. Move that continuation to the surviving simulation frame;
         * the next guest syscall uses its original native stack. Each nested
         * callback is popped separately as i386 RtlUnwind walks the SEH chain.
         * Leave ExceptionList alone: RtlUnwind owns its removal here. */
        *Previous = *Cpu;
        Previous->NativeStack = NativeStack;
        Previous->ApcFrame = ApcFrame;
        Previous->CallbackFrame = CallbackFrame;
        NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU] = Previous;
        NtCurrentTeb()->LastErrorValue = WOW64_TEB32(NtCurrentTeb())->LastErrorValue;
        return NtCallbackReturn(NULL, 0, Status);
    }
    _SEH2_TRY
    {
        if (NT_SUCCESS(Status)) switch (Frame->Id)
        {
        case USER32_CALLBACK_HOOKPROC:
        {
            HOOKPROC_CALLBACK_ARGUMENTS *Native = Frame->Native;
            if (!Buffer || Length != Frame->GuestLength) { Status = STATUS_INFO_LENGTH_MISMATCH; break; }
            Native->Result = ((HOOK32 *)Buffer)->Result;
            if (Native->HookId == WH_GETMESSAGE)
                Msg32ToNative((MSG *)(Native + 1), (const MSG32 *)((HOOK32 *)Buffer + 1));
            /* CALLWND hooks have no message-buffer writeback contract. */
            Output = Native; OutputLength = Frame->NativeLength;
            break;
        }
        case USER32_CALLBACK_WINDOWPROC:
        {
            WINDOWPROC_CALLBACK_ARGUMENTS *Native = Frame->Native;
            WINDOWPROC32 *Guest = Buffer;
            if (!Buffer || Length != Frame->GuestLength) { Status = STATUS_INFO_LENGTH_MISMATCH; break; }
            Native->Result = Guest->Result;
            if (Native->lParamBufferSize >= 0)
                UnpackPayload(Native+1,Guest+1,Native->Msg,Native->wParam,Native->lParamBufferSize);
            Output = Native; OutputLength = Frame->NativeLength; break;
        }
        case USER32_CALLBACK_CLIENTLOADLIBRARY:
            if (!Buffer || Length != sizeof(BOOL)) { Status = STATUS_INFO_LENGTH_MISMATCH; break; }
            Frame->Handles[0] = *(ULONG *)Buffer;
            Output = Frame->Handles; OutputLength = sizeof(HINSTANCE); break;
        case USER32_CALLBACK_LOADMENU: case USER32_CALLBACK_LOADSYSMENUTEMPLATE:
        case USER32_CALLBACK_LOADDEFAULTCURSORS: case USER32_CALLBACK_COPYIMAGE:
            if (!Buffer || Length != sizeof(ULONG)) { Status = STATUS_INFO_LENGTH_MISMATCH; break; }
            /* COPYIMAGE's image type determines whether this is a GDI handle. */
            Frame->Handles[0] = (Frame->Id == USER32_CALLBACK_COPYIMAGE &&
                ((COPYIMAGE_CALLBACK_ARGUMENTS *)Frame->Native)->uType == IMAGE_BITMAP) ?
                *(ULONG *)Buffer : (ULONG64)(LONG64)*(LONG *)Buffer;
            Output=Frame->Handles; OutputLength=sizeof(ULONG64); break;
        case USER32_CALLBACK_SETWNDICONS:
            if (!Buffer || Length != 7*sizeof(ULONG)) { Status = STATUS_INFO_LENGTH_MISMATCH; break; }
            for (i=0;i<7;++i) Frame->Handles[i] = (LONG64)((LONG *)Buffer)[i];
            Output=Frame->Handles; OutputLength=sizeof(Frame->Handles); break;
        case USER32_CALLBACK_GETCHARSETINFO: case USER32_CALLBACK_SETOBM:
            if (!Buffer || Length != Frame->NativeLength) { Status=STATUS_INFO_LENGTH_MISMATCH; break; }
            RtlCopyMemory(Frame->Native,Buffer,Length);
            Output=Frame->Native; OutputLength=Frame->NativeLength; break;
        case USER32_CALLBACK_LPK:
            if (!Buffer || Length != sizeof(BOOL)) { Status = STATUS_INFO_LENGTH_MISMATCH; break; }
            Frame->Handles[0] = *(ULONG *)Buffer;
            Output = Frame->Handles; OutputLength = sizeof(BOOL); break;
        default: break;
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode(); Output=NULL; OutputLength=0;
    }
    _SEH2_END;
    NtCurrentTeb()->LastErrorValue = WOW64_TEB32(NtCurrentTeb())->LastErrorValue;
    Frame->Previous->FsRedirectionDisabled = Cpu->FsRedirectionDisabled;
    /* Callback changes to the floating-point environment survive the USER
     * call. Keep the outer register data, but propagate its control state. */
    *(USHORT *)Frame->Previous->ExtendedRegisters = *(USHORT *)Cpu->ExtendedRegisters;
    *(ULONG *)(Frame->Previous->ExtendedRegisters + 24) = *(ULONG *)(Cpu->ExtendedRegisters + 24);
    NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU] = Frame->Previous;
    WOW64_TEB32(NtCurrentTeb())->NtTib.ExceptionList = Frame->ExceptionList;
    return NtCallbackReturn(Output,OutputLength,Status);
}

ULONG_PTR Wow64WinCallNextHook(ULONG *a)
{
    WOW64_CPU_STATE *Cpu = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPU];
    CALLBACK_FRAME *Frame = Cpu ? Cpu->CallbackFrame : NULL;
    MSG Message;
    LRESULT Result;
    /* The callback, not a caller-supplied selector, determines lParam's ABI. */
    if (!Frame || Frame->Id != USER32_CALLBACK_HOOKPROC ||
        ((HOOKPROC_CALLBACK_ARGUMENTS *)Frame->Native)->HookId != WH_GETMESSAGE)
    {
        RtlSetLastWin32Error(ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    if (!a[2]) return (LONG_PTR)NtUserCallNextHookEx(a[0], a[1], 0, a[3]);
    Msg32ToNative(&Message, P32(a[2]));
    Result = NtUserCallNextHookEx(a[0], a[1], (LPARAM)&Message, a[3]);
    MsgNativeTo32(P32(a[2]), &Message);
    return (LONG_PTR)Result;
}

#define DEFINE_USER32_CALLBACK(id, value, fn) \
    static NTSTATUS NTAPI Win64_##fn(PVOID Buffer, ULONG Length) { return Callback(value,Buffer,Length); }
#include <u32cb.h>
#undef DEFINE_USER32_CALLBACK
static PVOID NativeCallbacks[] = {
#define DEFINE_USER32_CALLBACK(id, value, fn) Win64_##fn,
#include <u32cb.h>
#undef DEFINE_USER32_CALLBACK
};

NTSTATUS NTAPI Wow64WinInitialize(PVOID Dispatcher)
{
    if (!Dispatcher) return STATUS_INVALID_PARAMETER;
    GuestDispatcher = Dispatcher;
    NtCurrentPeb()->KernelCallbackTable = NativeCallbacks;
    return STATUS_SUCCESS;
}
