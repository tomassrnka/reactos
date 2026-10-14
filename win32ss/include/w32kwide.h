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
 *
 * The guest build also makes the compiler reject the two accesses that would be wrong on the low half:
 *  - the 32-bit member of a KFIELD is const, so a store has to go through KSET, KSETSX, KOR, KCLR, KXOR,
 *    KINC or KPUT and reaches all 64 bits (a pointer or GDI handle is zero-extended, the form win32k issues
 *    and compares; a USER handle, keyboard layout or LRESULT is sign-extended by KSETSX);
 *  - a pointer to a THREADINFO, PROCESSINFO or DESKTOP is declared by KPTRFIELD with only its _K64 member,
 *    because those objects live in pool that spans more than 4 GB and a comparison has to see every bit
 *    (KPTR_EQ, KPTR_NULL).
 */

#ifndef _W32KWIDE_H
#define _W32KWIDE_H

/*
 * A WoW64 thread's 64-bit TEB lies one 64-bit TEB (two pages) below its 32-bit TEB, and win32k keeps the
 * thread's THREADINFO pointer in the 64-bit one. w32klayout.h checks both constants against the amd64 TEB.
 */
#define KTEB64_BIAS 0x2000
#define KTEB64_WIN32THREADINFO 0x78

#if defined(W32K_WIDE) && !defined(_WIN64)

#define W32K_WIDE_ACTIVE 1

#define KFIELD(T, n) union DECLSPEC_ALIGN(8) { T const n; ULONG64 n##_K64; }
#define KPTRFIELD(T, n) ULONG64 DECLSPEC_ALIGN(8) n##_K64
#define KARRAY(T, n, c) union DECLSPEC_ALIGN(8) { T const v; ULONG64 v64; } n[c]
#define KA(x) ((x).v)

/*
 * A pointer to a thread, process or desktop at full width. KPTR(x) names the member that KPTRFIELD declared
 * (KPTR(pWnd->head.pti)); KPTRTYPE(T) is the type of a copy; KPTR_EQ and KPTR_NULL refuse an operand that
 * is not 64 bits wide, so a low half can never be compared by mistake.
 */
#define KPTR(x) (x##_K64)
#define KPTRTYPE(T) ULONG64
#define KPTR_LOW(x) ((PVOID)(ULONG_PTR)(x))
#define K_WIDE64(a) ((void)sizeof(char[sizeof(a) == 8 ? 1 : -1]))
#define KPTR_EQ(a, b) (K_WIDE64(a), K_WIDE64(b), (a) == (b))
#define KPTR_NULL(a) (K_WIDE64(a), (a) == 0)

/* The current thread's THREADINFO pointer, from the 64-bit TEB that win32k fills (the 32-bit TEB field stays empty) */
#define KTEB_PTI() (*(volatile ULONG64 *)((PUCHAR)NtCurrentTeb() - KTEB64_BIAS + KTEB64_WIN32THREADINFO))

/* Full-width stores into a KFIELD: a pointer or handle is zero-extended, a mask is applied to all 64 bits */
#define KSET(p, n, v) ((p)->n##_K64 = (ULONG64)(ULONG_PTR)(v))
#define KOR(p, n, v) ((p)->n##_K64 |= (ULONG64)(ULONG_PTR)(v))
#define KCLR(p, n, v) ((p)->n##_K64 &= ~(ULONG64)(ULONG_PTR)(v))
#define KXOR(p, n, v) ((p)->n##_K64 ^= (ULONG64)(ULONG_PTR)(v))
#define KINC(p, n) ((p)->n##_K64++)
/* A USER or kernel handle, a keyboard layout or an LRESULT is sign-extended */
#define KSETSX(p, n, v) ((p)->n##_K64 = (ULONG64)(LONG64)(LONG_PTR)(v))
/* The whole field, for a value that is read now and written back later (KCOPYOF names the type of the copy) */
#define KGET(p, n) ((p)->n##_K64)
#define KASSIGN(d, s) ((void)sizeof(char[sizeof(d) == sizeof(s) ? 1 : -1]), (void)RtlCopyMemory(&(d), &(s), sizeof(d)))
#define KPUT(p, n, v) ((p)->n##_K64 = (v))
#define KCOPYOF(T) ULONG64

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
#define KPTRFIELD(T, n) T n
#define KARRAY(T, n, c) T n[c]
#define KA(x) (x)

#define KPTR(x) (x)
#define KPTRTYPE(T) T
#define KPTR_LOW(x) (x)
#define KPTR_EQ(a, b) ((a) == (b))
#define KPTR_NULL(a) ((a) == NULL)
#define KTEB_PTI() (NtCurrentTeb()->Win32ThreadInfo)

#define KSET(p, n, v) ((p)->n = (v))
#define KOR(p, n, v) ((p)->n |= (v))
#define KCLR(p, n, v) ((p)->n &= ~(v))
#define KXOR(p, n, v) ((p)->n ^= (v))
#define KINC(p, n) ((p)->n++)
#define KSETSX(p, n, v) ((p)->n = (v))
#define KASSIGN(d, s) ((d) = (s))
#define KGET(p, n) ((p)->n)
#define KPUT(p, n, v) ((p)->n = (v))
#define KCOPYOF(T) T

#define W32K_NATIVE_ASSERT(x) (x)

#define KLIST_ENTRY LIST_ENTRY
#define KUNICODE_STRING UNICODE_STRING
#define KMSG MSG
#define KFLOATOBJ FLOATOBJ
#define KPATRECT PATRECT
#define KULONG_PTR ULONG_PTR

#endif /* W32K_WIDE_ACTIVE */

#endif /* _W32KWIDE_H */
