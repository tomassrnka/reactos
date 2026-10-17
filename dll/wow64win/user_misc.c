/* ReactOS WoW64 USER miscellaneous service translation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64win.h"

typedef struct { USHORT Atom, Reserved; ULONG Data; } PROPLIST32;
typedef struct { BOOL fIcon; DWORD xHotspot, yHotspot; ULONG hbmMask, hbmColor; } ICONINFO32;
C_ASSERT(sizeof(PROPLIST32) == 8);
C_ASSERT(sizeof(ICONINFO32) == 20);

static VOID ProbeOutput(ULONG Address, ULONG Size)
{
    volatile BYTE *Bytes = P32(Address);
    ULONG i;
    if (Address > MAXULONG - Size) RtlRaiseStatus(STATUS_ACCESS_VIOLATION);
    for (i = 0; i < Size; ++i) Bytes[i] = Bytes[i];
}

static VOID ProbeInput(ULONG Address, ULONG Size)
{
    volatile const BYTE *Bytes = P32(Address);
    ULONG i;
    if (Address > MAXULONG - Size) RtlRaiseStatus(STATUS_ACCESS_VIOLATION);
    for (i = 0; i < Size; ++i) (VOID)Bytes[i];
}

static VOID StringIn(UNICODE_STRING *Native, ULONG Address)
{
    const STRING32 *Guest;
    ProbeInput(Address, sizeof(*Guest));
    Guest = P32(Address);
    Native->Length = Guest->Length;
    Native->MaximumLength = Guest->MaximumLength;
    Native->Buffer = P32(Guest->Buffer);
}

/* PROPLISTITEM stride is 8 in the guest and 16 natively; Count is items when
 * a buffer is given and bytes (native stride) when it is not. */
static NTSTATUS BuildPropList(ULONG *a)
{
    PROPLISTITEM *List;
    PROPLIST32 *Out = P(1);
    DWORD Needed = 0, Items = 0, Capacity, i;
    NTSTATUS Status;

    if (!a[1])
    {
        Status = NtUserBuildPropList(H(0), NULL, 0, &Needed);
        if (NT_SUCCESS(Status) && a[3])
        {
            ProbeOutput(a[3], sizeof(ULONG));
            *(ULONG *)P(3) = Needed / sizeof(PROPLISTITEM) * sizeof(PROPLIST32);
        }
        return Status;
    }
    /* The native service validates the window before the size. */
    Status = NtUserBuildPropList(H(0), NULL, 0, &Needed);
    if (!NT_SUCCESS(Status)) return Status;
    if (!a[2] || a[2] % sizeof(PROPLIST32)) return STATUS_INVALID_PARAMETER;
    Capacity = min(a[2] / sizeof(PROPLIST32), Needed / sizeof(PROPLISTITEM));
    if (!Capacity) Capacity = 1;
    List = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, Capacity * sizeof(*List));
    if (!List) return STATUS_NO_MEMORY;
    _SEH2_TRY
    {
        Status = NtUserBuildPropList(H(0), List, Capacity * sizeof(*List), &Items);
        if (NT_SUCCESS(Status))
        {
            Items = min(Items, Capacity);
            ProbeOutput(a[1], Items * sizeof(PROPLIST32));
            for (i = 0; i < Items; ++i)
            {
                Out[i].Atom = List[i].Atom;
                Out[i].Reserved = 0;
                Out[i].Data = HandleToUlong(List[i].Data);
            }
            if (a[3]) { ProbeOutput(a[3], sizeof(ULONG)); *(ULONG *)P(3) = Items; }
        }
    }
    _SEH2_FINALLY { RtlFreeHeap(RtlGetProcessHeap(), 0, List); }
    _SEH2_END;
    return Status;
}

/* Only csrss may succeed natively; a length mismatch is forwarded as length 0
 * so the native access check still comes first. */
static NTSTATUS ConsoleControl(ULONG *a)
{
    switch (a[0])
    {
    case ConsoleCtrlDesktopConsoleThread:
    {
        DESKTOP_CONSOLE_THREAD Info = {0};
        ULONG *Guest = P(1);
        NTSTATUS Status;
        BOOLEAN Readable = FALSE;
        if (a[2] != 2 * sizeof(ULONG)) return NtUserConsoleControl(a[0], NULL, 0);
        if (a[1])
        {
            _SEH2_TRY
            {
                ProbeOutput(a[1], a[2]);
                Info.DesktopHandle = H32(Guest[0]);
                Info.ThreadId = Guest[1] == MAXULONG ? (ULONG_PTR)INVALID_HANDLE_VALUE : Guest[1];
                Readable = TRUE;
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { }
            _SEH2_END;
        }
        /* An unreadable payload goes native as NULL so the access check still decides first. */
        if (!Readable) return NtUserConsoleControl(a[0], NULL, sizeof(Info));
        Status = NtUserConsoleControl(a[0], &Info, sizeof(Info));
        if (NT_SUCCESS(Status)) Guest[1] = (ULONG)Info.ThreadId;
        return Status;
    }
    case ConsoleMakePalettePublic:
    {
        HPALETTE Palette = NULL;
        BOOLEAN Readable = FALSE;
        if (a[2] != sizeof(ULONG)) return NtUserConsoleControl(a[0], NULL, 0);
        if (a[1])
        {
            _SEH2_TRY
            {
                ProbeInput(a[1], sizeof(ULONG));
                Palette = P32(*(ULONG *)P(1));
                Readable = TRUE;
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { }
            _SEH2_END;
        }
        if (!Readable) return NtUserConsoleControl(a[0], NULL, sizeof(Palette));
        return NtUserConsoleControl(a[0], &Palette, sizeof(Palette));
    }
    default:
        /* GuiConsoleWndClassAtom carries an ATOM; the rest ignore the buffer. */
        return NtUserConsoleControl(a[0], P(1), a[2]);
    }
}

static NTSTATUS SetInformationThread(ULONG *a)
{
    if (a[1] == UserThreadCsrApiPort)
    {
        HANDLE Port = NULL;
        BOOLEAN Readable = FALSE;
        if (a[3] != sizeof(ULONG)) return NtUserSetInformationThread(H(0), a[1], NULL, 0);
        if (a[2])
        {
            _SEH2_TRY
            {
                ProbeInput(a[2], sizeof(ULONG));
                Port = H32(*(ULONG *)P(2));
                Readable = TRUE;
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { }
            _SEH2_END;
        }
        if (!Readable) return NtUserSetInformationThread(H(0), a[1], NULL, sizeof(Port));
        return NtUserSetInformationThread(H(0), a[1], &Port, sizeof(Port));
    }
    /* Shutdown flags and status are 4-byte values; other classes ignore the buffer. */
    return NtUserSetInformationThread(H(0), a[1], P(2), a[3]);
}

ULONG_PTR Wow64WinUserMiscService(ULONG Number, ULONG *a)
{
    switch (Number)
    {
    case W64_NtUserBuildPropList: return BuildPropList(a);
    case W64_NtUserConsoleControl: return ConsoleControl(a);
    case W64_NtUserDrawCaptionTemp:
    {
        UNICODE_STRING Text;
        if (a[5]) StringIn(&Text, a[5]);
        return NtUserDrawCaptionTemp(H(0), G(1), P(2), G(3), H(4), a[5] ? &Text : NULL, a[6]);
    }
    case W64_NtUserGetWOWClass:
    {
        UNICODE_STRING Name;
        PCLS Class;
        if (a[1]) StringIn(&Name, a[1]);
        Class = NtUserGetWOWClass(P(0), a[1] ? &Name : NULL);
        /* The result is a desktop-heap pointer that a guest cannot address. */
        if ((ULONG_PTR)Class > MAXULONG)
        {
            RtlSetLastWin32Error(ERROR_NOT_SUPPORTED);
            return 0;
        }
        return (ULONG_PTR)Class;
    }
    case W64_NtUserQueryInformationThread:
        /* No class reads or writes the buffer natively. */
        return NtUserQueryInformationThread(H(0), a[1], P(2), a[3]);
    case W64_NtUserRealInternalGetMessage:
    {
        MSG Msg = {0};
        BOOL Ret;
        _SEH2_TRY
        {
            Ret = NtUserRealInternalGetMessage(&Msg, H(1), a[2], a[3], a[4], a[5]);
            if (Ret && Ret != -1) { ProbeOutput(a[0], sizeof(MSG32)); MsgNativeTo32(P(0), &Msg); }
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            NtCurrentTeb()->LastErrorValue = RtlNtStatusToDosError(_SEH2_GetExceptionCode());
            Ret = a[5] ? -1 : 0;
        }
        _SEH2_END;
        return Ret;
    }
    case W64_NtUserSetCursorContents:
    {
        ICONINFO Info = {0};
        BOOLEAN Readable = FALSE;
        if (a[1])
        {
            const ICONINFO32 *Guest = P(1);
            _SEH2_TRY
            {
                ProbeInput(a[1], sizeof(*Guest));
                Info.fIcon = Guest->fIcon; Info.xHotspot = Guest->xHotspot; Info.yHotspot = Guest->yHotspot;
                Info.hbmMask = P32(Guest->hbmMask); Info.hbmColor = P32(Guest->hbmColor);
                Readable = TRUE;
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { }
            _SEH2_END;
        }
        return NtUserSetCursorContents(H(0), Readable ? &Info : NULL);
    }
    case W64_NtUserSetInformationThread: return SetInformationThread(a);
    case W64_NtUserSetWindowWord:
        /* Native SetWindowWord sizes these two indexes as LONG, which win32k rejects on 64-bit. */
        if ((INT)a[1] == GWL_HINSTANCE || (INT)a[1] == GWL_HWNDPARENT)
            return (WORD)NtUserSetWindowLongPtr(H(0), (INT)a[1], (WORD)a[2], TRUE);
        return NtUserSetWindowWord(H(0), (INT)a[1], (WORD)a[2]);
    default:
        DbgPrint("WoW64win: unsupported service %lx\n", Number);
        RtlSetLastWin32Error(ERROR_CALL_NOT_IMPLEMENTED);
        return Wow64WinFailure(Number, STATUS_NOT_IMPLEMENTED);
    }
}
