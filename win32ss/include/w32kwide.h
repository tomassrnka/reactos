/*
 * PROJECT:     ReactOS Win32 subsystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Wide view of the structures that win32k shares with user mode
 *
 * A 32-bit WoW64 guest build of user32, gdi32 and imm32 defines W32K_WIDE. In that build every pointer-sized
 * field of a shared structure takes its amd64 size and alignment: the guest still reads the field by its
 * old name (the low half) and win32k's native 64-bit layout is read in place. The field's high half is
 * reachable as <name>_K64. In every other build the macros below expand to the original declarations.
 * The layout of every structure is pinned by w32klayout.h.
 */

#ifndef _W32KWIDE_H
#define _W32KWIDE_H

#if defined(W32K_WIDE) && !defined(_WIN64)

#define W32K_WIDE_ACTIVE 1

#define KFIELD(T, n) union DECLSPEC_ALIGN(8) { T n; ULONG64 n##_K64; }
#define KARRAY(T, n, c) union DECLSPEC_ALIGN(8) { T v; ULONG64 v64; } n[c]
#define KA(x) ((x).v)

#define W32K_NATIVE_ASSERT(x) 1

typedef struct _KLIST_ENTRY
{
    KFIELD(struct _LIST_ENTRY *, Flink);
    KFIELD(struct _LIST_ENTRY *, Blink);
} KLIST_ENTRY;

typedef struct _KUNICODE_STRING
{
    USHORT Length;
    USHORT MaximumLength;
    KFIELD(PWSTR, Buffer);
} KUNICODE_STRING;

typedef struct _KMSG
{
    KFIELD(HWND, hwnd);
    UINT message;
    KFIELD(WPARAM, wParam);
    KFIELD(LPARAM, lParam);
    DWORD time;
    POINT pt;
} KMSG;

typedef FLOAT KFLOATOBJ;

typedef struct _KPATRECT
{
    RECT r;
    KFIELD(HBRUSH, hBrush);
} KPATRECT;

typedef ULONG64 KULONG_PTR;

#else /* !W32K_WIDE_ACTIVE */

#define KFIELD(T, n) T n
#define KARRAY(T, n, c) T n[c]
#define KA(x) (x)

#define W32K_NATIVE_ASSERT(x) (x)

#define KLIST_ENTRY LIST_ENTRY
#define KUNICODE_STRING UNICODE_STRING
#define KMSG MSG
#define KFLOATOBJ FLOATOBJ
#define KPATRECT PATRECT
#define KULONG_PTR ULONG_PTR

#endif /* W32K_WIDE_ACTIVE */

#endif /* _W32KWIDE_H */
