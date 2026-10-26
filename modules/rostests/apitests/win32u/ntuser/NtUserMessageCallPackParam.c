/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     GPL - See COPYING in the top level directory
 * PURPOSE:     Contract test: the same-thread send path packs a WM_CREATE /
 *              WM_NCCREATE CREATESTRUCT (and other MsgMemory messages) out of a
 *              caller-supplied lParam. PackParam() interprets the CREATESTRUCT
 *              lpszName/lpszClass as PLARGE_STRING/PUNICODE_STRING.
 *
 *              Security background: the Length fields were read once to size the
 *              kernel pool block and again to copy, and the buffers were
 *              dereferenced with no SEH, so a bad pointer faulted in the kernel
 *              and a Length that changed between the two reads overflowed the
 *              packed block (Astra static audit #8). The fix captures the
 *              lengths/buffers once under SEH and sizes and copies from the same
 *              captured values.
 *
 *              Contract: sending WM_NCCREATE / WM_CREATE with an invalid
 *              CREATESTRUCT to a same-thread window is handled inside the kernel
 *              (no bugcheck, no pool overflow), while real window creation and a
 *              WM_SETTEXT round-trip (which also pack a user buffer) keep working.
 *
 *              Note: NtUserMessageCall(FNID_SENDMESSAGE) always returns TRUE -
 *              the send result is passed back through ResultInfo, not the BOOL -
 *              so the negative case is verified by survival (the run completes
 *              and the window stays valid), not by the return value.
 */

#include "../win32nt.h"

START_TEST(NtUserMessageCallPackParam)
{
    HINSTANCE hinst = GetModuleHandle(NULL);
    HWND hWnd, hWnd2;
    ULONG_PTR Result;
    BOOL bRet;
    PVOID pNoAccess;
    CREATESTRUCTW cs;
    LARGE_STRING lsName;
    UNICODE_STRING usClass;
    WCHAR textBuf[64];

    /* A page that faults on any access, used to stand in for a hostile
       CREATESTRUCT string buffer. */
    pNoAccess = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    ASSERT(pNoAccess != NULL);

    /* An ordinary same-thread window to receive the crafted send. */
    hWnd = CreateWindowA("BUTTON", "PackParamTest",
                         BS_PUSHBUTTON | WS_OVERLAPPEDWINDOW,
                         0, 0, 60, 40, NULL, NULL, hinst, 0);
    ASSERT(hWnd != NULL);

    /* --- Negative contract: WM_NCCREATE with lpszName/lpszClass pointing at a
       LARGE_STRING/UNICODE_STRING whose buffer is inaccessible. PackParam must
       catch the fault in-kernel and abort the pack rather than bugchecking or
       overflowing the packed pool block. The call is wrapped in SEH: PackParam
       aborts the send without surfacing an exception to user mode, so no fault is
       expected here; the real proof is that the whole run completes (no kernel
       bugcheck) with the window still valid. */
    RtlZeroMemory(&cs, sizeof(cs));
    lsName.Length = 32;                 /* non-zero: forces a buffer copy */
    lsName.MaximumLength = 32;
    lsName.bAnsi = 0;
    lsName.Buffer = pNoAccess;          /* inaccessible -> copy must fault safely in-kernel */
    usClass.Length = 32;
    usClass.MaximumLength = 32;
    usClass.Buffer = (PWSTR)pNoAccess; /* > 0xFFFF, so not treated as an atom */
    cs.lpszName = (LPCWSTR)&lsName;
    cs.lpszClass = (LPCWSTR)&usClass;
    cs.hInstance = hinst;

    Result = 0;
    StartSeh()
        NtUserMessageCall(hWnd, WM_NCCREATE, 0, (LPARAM)&cs,
                          (ULONG_PTR)&Result, FNID_SENDMESSAGE, FALSE);
    EndSeh(STATUS_SUCCESS);
    ok(IsWindow(hWnd) == TRUE, "Target window gone after bad WM_NCCREATE send\n");

    /* WM_CREATE goes through the same packing code. */
    Result = 0;
    StartSeh()
        NtUserMessageCall(hWnd, WM_CREATE, 0, (LPARAM)&cs,
                          (ULONG_PTR)&Result, FNID_SENDMESSAGE, FALSE);
    EndSeh(STATUS_SUCCESS);
    ok(IsWindow(hWnd) == TRUE, "Target window gone after bad WM_CREATE send\n");

    /* --- Positive control 1: real window creation packs a genuine
       (kernel-built) CREATESTRUCT with valid name/class and must still work. */
    hWnd2 = CreateWindowExW(0, L"BUTTON", L"RealWindowTitle",
                            BS_PUSHBUTTON | WS_OVERLAPPEDWINDOW,
                            0, 0, 80, 50, NULL, NULL, hinst, NULL);
    ok(hWnd2 != NULL, "Legit CreateWindowExW failed: %lu\n", GetLastError());

    /* --- Positive control 2: WM_SETTEXT with a valid string round-trips (the
       MsgMemory pack path for a real user buffer). */
    if (hWnd2)
    {
        bRet = SetWindowTextW(hWnd2, L"PackedText");
        ok(bRet == TRUE, "SetWindowTextW failed: %lu\n", GetLastError());
        RtlZeroMemory(textBuf, sizeof(textBuf));
        GetWindowTextW(hWnd2, textBuf, ARRAYSIZE(textBuf));
        ok(wcscmp(textBuf, L"PackedText") == 0,
           "WM_SETTEXT pack path broken after fix\n");
        DestroyWindow(hWnd2);
    }

    DestroyWindow(hWnd);
    VirtualFree(pNoAccess, 0, MEM_RELEASE);
}
