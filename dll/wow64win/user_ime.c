/* ReactOS WoW64 USER IME and input-context translation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "wow64win.h"
#include <reactos/undocuser.h>

typedef struct {
    ULONG hkl; IMEINFO ImeInfo; WCHAR wszUIClass[16];
    ULONG fdwInitConvMode; INT fInitOpen, fLoadFlag; DWORD dwProdVersion, dwImeWinVersion;
    WCHAR wszImeDescription[50], wszImeFile[80]; ULONG Flags;
} IMEINFOEX32;
C_ASSERT(sizeof(IMEINFO) == 28);
C_ASSERT(sizeof(IMEINFOEX32) == 348);
C_ASSERT(sizeof(IMEINFOEX) == 352);
C_ASSERT(FIELD_OFFSET(IMEINFOEX32, ImeInfo) == 4 && FIELD_OFFSET(IMEINFOEX32, wszImeDescription) == 84);
C_ASSERT(FIELD_OFFSET(IMEINFOEX32, wszImeFile) == 184 && FIELD_OFFSET(IMEINFOEX32, Flags) == 344);
C_ASSERT(FIELD_OFFSET(IMEINFOEX, ImeInfo) == 8 && FIELD_OFFSET(IMEINFOEX, wszImeDescription) == 88);
C_ASSERT(FIELD_OFFSET(IMEINFOEX, wszImeFile) == 188 && FIELD_OFFSET(IMEINFOEX, wszImeFile) + sizeof(((IMEINFOEX *)0)->wszImeFile) == 348);

/* The handle table holds fewer than 0x8000 objects, which bounds any HIMC list. */
#define HIMC_LIST_MAX 0x10000

static BOOL GuestRange(ULONG Address, ULONG64 Size)
{
    return Address && (ULONG64)Address + Size <= 0x100000000ULL;
}

/* Mirrors ProbeForWrite: touching each page lets a stack guard page grow the stack instead of failing. */
static BOOL GuestWritable(ULONG Address, ULONG64 Size)
{
    ULONG64 Off;
    BOOL Ok = TRUE;
    if (!Size) return TRUE;
    if (!GuestRange(Address, Size)) return FALSE;
    _SEH2_TRY
    {
        for (Off = 0; Off < Size; Off += 0x1000)
            *(volatile BYTE *)P32(Address + Off) = *(volatile BYTE *)P32(Address + Off);
        *(volatile BYTE *)P32(Address + Size - 1) = *(volatile BYTE *)P32(Address + Size - 1);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { Ok = FALSE; }
    _SEH2_END;
    return Ok;
}

static VOID ImeInfoExIn(IMEINFOEX *To, const IMEINFOEX32 *From)
{
    RtlZeroMemory(To, sizeof(*To));
    To->hkl = H32(From->hkl);
    To->ImeInfo = From->ImeInfo;
    RtlCopyMemory(To->wszUIClass, From->wszUIClass, sizeof(To->wszUIClass));
    To->fdwInitConvMode = From->fdwInitConvMode; To->fInitOpen = From->fInitOpen; To->fLoadFlag = From->fLoadFlag;
    To->dwProdVersion = From->dwProdVersion; To->dwImeWinVersion = From->dwImeWinVersion;
    RtlCopyMemory(To->wszImeDescription, From->wszImeDescription, sizeof(To->wszImeDescription));
    RtlCopyMemory(To->wszImeFile, From->wszImeFile, sizeof(To->wszImeFile));
    /* The bitfield word closes both layouts; copying it whole keeps reserved bits. */
    RtlCopyMemory((PUCHAR)To + sizeof(*To) - sizeof(ULONG), &From->Flags, sizeof(ULONG));
}

static VOID ImeInfoExOut(IMEINFOEX32 *To, const IMEINFOEX *From)
{
    To->hkl = HandleToUlong(From->hkl);
    To->ImeInfo = From->ImeInfo;
    RtlCopyMemory(To->wszUIClass, From->wszUIClass, sizeof(To->wszUIClass));
    To->fdwInitConvMode = From->fdwInitConvMode; To->fInitOpen = From->fInitOpen; To->fLoadFlag = From->fLoadFlag;
    To->dwProdVersion = From->dwProdVersion; To->dwImeWinVersion = From->dwImeWinVersion;
    RtlCopyMemory(To->wszImeDescription, From->wszImeDescription, sizeof(To->wszImeDescription));
    RtlCopyMemory(To->wszImeFile, From->wszImeFile, sizeof(To->wszImeFile));
    RtlCopyMemory(&To->Flags, (const UCHAR *)From + sizeof(*From) - sizeof(ULONG), sizeof(ULONG));
}

static ULONG_PTR BuildHimcList(ULONG *a)
{
    HIMC *List = NULL; ULONG Native = min(a[1], HIMC_LIST_MAX), Real = 0, i; NTSTATUS Status = STATUS_UNSUCCESSFUL;
    /* Native probes the list, then the count, before writing either; failure leaves them untouched. */
    if (!GuestWritable(a[2], (ULONG64)a[1] * sizeof(ULONG)) || !GuestWritable(a[3], sizeof(ULONG)))
        return STATUS_UNSUCCESSFUL;
    if (Native)
    {
        List = RtlAllocateHeap(RtlGetProcessHeap(), 0, (SIZE_T)Native * sizeof(HIMC));
        if (!List) return STATUS_NO_MEMORY;
    }
    _SEH2_TRY
    {
        Status = NtUserBuildHimcList(a[0], Native, List, &Real);
        /* The native service fills the count and list for both of these results. */
        if (Status == STATUS_SUCCESS || Status == STATUS_BUFFER_TOO_SMALL)
        {
            for (i = 0; i < min(Real, Native); ++i) ((ULONG *)P(2))[i] = HandleToUlong(List[i]);
            *(ULONG *)P(3) = Real;
        }
    }
    _SEH2_FINALLY { if (List) RtlFreeHeap(RtlGetProcessHeap(), 0, List); }
    _SEH2_END;
    return Status;
}

ULONG_PTR Wow64WinUserImeService(ULONG Number, ULONG *a)
{
    switch (Number)
    {
    case W64_NtUserBuildHimcList: return BuildHimcList(a);
    case W64_NtUserGetImeHotKey:
    {
        UINT Modifiers = 0, Key = 0; HKL Layout = NULL; BOOL Ret;
        if (!GuestWritable(a[1], sizeof(ULONG)) || !GuestWritable(a[2], sizeof(ULONG)) || (a[3] && !GuestWritable(a[3], sizeof(ULONG))))
            return FALSE;
        Ret = NtUserGetImeHotKey(a[0], &Modifiers, &Key, a[3] ? &Layout : NULL);
        if (Ret)
        {
            *(ULONG *)P(1) = Modifiers; *(ULONG *)P(2) = Key;
            if (a[3]) *(ULONG *)P(3) = HandleToUlong(Layout);
        }
        return Ret;
    }
    case W64_NtUserGetImeInfoEx:
    {
        IMEINFOEX Info; IMEINFOEX32 Guest; BOOL Ret;
        if (!GuestRange(a[0], sizeof(Guest))) return FALSE;
        Guest = *(const IMEINFOEX32 *)P(0);
        ImeInfoExIn(&Info, &Guest);
        Ret = NtUserGetImeInfoEx(&Info, (IMEINFOEXCLASS)a[1]);
        if (!Ret || !GuestWritable(a[0], sizeof(Guest))) return FALSE;
        ImeInfoExOut(&Guest, &Info); *(IMEINFOEX32 *)P(0) = Guest;
        return TRUE;
    }
    case W64_NtUserSetImeInfoEx:
    {
        IMEINFOEX Info; IMEINFOEX32 Guest;
        if (!GuestRange(a[0], sizeof(Guest))) return FALSE;
        Guest = *(const IMEINFOEX32 *)P(0);
        ImeInfoExIn(&Info, &Guest);
        return NtUserSetImeInfoEx(&Info);
    }
    case W64_NtUserLoadKeyboardLayoutEx:
    {
        UNICODE_STRING Id; const STRING32 *Guest = P(4); BOOL Valid = GuestRange(a[4], sizeof(*Guest));
        if (Valid)
        {
            _SEH2_TRY { Id.Length = Guest->Length; Id.MaximumLength = Guest->MaximumLength; Id.Buffer = P32(Guest->Buffer); }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { Valid = FALSE; }
            _SEH2_END;
        }
        /* An unreadable ID goes down as NULL so the kernel validates Flags before it faults on the ID. */
        return HandleToUlong(NtUserLoadKeyboardLayoutEx(H(0), a[1], P(2), H(3), Valid ? &Id : NULL, a[5], a[6]));
    }
    default:
        return Wow64WinUserDisplayService(Number, a);
    }
}
