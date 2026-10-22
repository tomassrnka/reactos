/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Defensive contract tests for NtUserGetIconInfo (T69)
 * PROGRAMMERS:     reactos-nitro security lane
 *
 * Covers two hardening fixes in win32ss/user/ntuser/cursoricon.c:
 *   1) The lpModule UNICODE_STRING *descriptor* must be probed before its
 *      fields are read/written (previously only ->Buffer was probed, which
 *      left a caller-supplied descriptor pointer unvalidated). This test
 *      asserts that a bogus descriptor is rejected cleanly (FALSE, no crash)
 *      and that the valid path still succeeds.
 *   2) The copied hbmMask/hbmColor GDI bitmaps must not leak when the
 *      IconInfo write-back faults. This test repeatedly forces the fault
 *      path and asserts the process GDI object count does not grow.
 */

#include "../win32nt.h"

static HICON
MakeIcon(void)
{
    ICONINFO iinfo;
    HBITMAP mask, color;
    HICON hIcon;

    ZeroMemory(&iinfo, sizeof(iinfo));
    mask = CreateBitmap(16, 16, 1, 1, NULL);
    color = CreateBitmap(16, 16, 1, 16, NULL);
    iinfo.hbmMask = mask;
    iinfo.hbmColor = color;
    iinfo.fIcon = TRUE;
    iinfo.xHotspot = 8;
    iinfo.yHotspot = 8;
    hIcon = CreateIconIndirect(&iinfo);
    if (mask) DeleteObject(mask);
    if (color) DeleteObject(color);
    return hIcon;
}

START_TEST(NtUserGetIconInfoProbe)
{
    HICON hIcon;
    ICONINFO iinfo;
    DWORD bpp = 0;
    PVOID faultPage;
    ULONG before, after, i;
    const ULONG kIter = 64;

    hIcon = MakeIcon();
    TEST(hIcon != NULL);
    if (!hIcon)
        return;

    /* --- Positive: a valid IconInfo out-pointer still works. --- */
    ZeroMemory(&iinfo, sizeof(iinfo));
    TEST(NtUserGetIconInfo(hIcon, &iinfo, NULL, NULL, &bpp, FALSE) == TRUE);
    TEST(iinfo.hbmMask != NULL);
    if (iinfo.hbmMask) DeleteObject(iinfo.hbmMask);
    if (iinfo.hbmColor) DeleteObject(iinfo.hbmColor);

    /*
     * --- T69(1): unprobed/invalid lpModule descriptor rejected cleanly. ---
     * A descriptor pointer that is not safely writable must be rejected via
     * SEH (returns FALSE) and must not bugcheck. We cannot safely hand a
     * *mapped* kernel address from user mode, so this exercises the clean-
     * rejection contract; the kernel-pointer rejection itself is enforced by
     * the added ProbeForWrite(lpModule, sizeof(UNICODE_STRING), 1).
     */
    {
        /* Misaligned + unmapped descriptor pointer. */
        PUNICODE_STRING bad = (PUNICODE_STRING)(ULONG_PTR)0x3;
        BOOL ret = NtUserGetIconInfo(hIcon, NULL, bad, NULL, NULL, FALSE);
        TEST(ret == FALSE);
    }

    /* A no-access page as the descriptor must also be rejected cleanly. */
    faultPage = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    TEST(faultPage != NULL);
    if (faultPage)
    {
        BOOL ret = NtUserGetIconInfo(hIcon, NULL, (PUNICODE_STRING)faultPage, NULL, NULL, FALSE);
        TEST(ret == FALSE);
    }

    /*
     * --- T69(2): no GDI-object leak when IconInfo write-back faults. ---
     * The bitmaps are copied (and owned by this process) before the SEH
     * guard that writes them to the caller's IconInfo. If that write faults,
     * the copies must be released. Force the fault path many times and check
     * the process GDI object count stays bounded.
     */
    if (faultPage)
    {
        before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (i = 0; i < kIter; i++)
        {
            /* faultPage is PAGE_NOACCESS: the IconInfo copy faults after the
             * bitmaps have been duplicated inside the kernel. */
            BOOL ret = NtUserGetIconInfo(hIcon, (PICONINFO)faultPage, NULL, NULL, NULL, FALSE);
            TEST(ret == FALSE);
        }
        after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);

        /* Without the fix, each faulting call leaks up to two owned bitmaps,
         * so the count would climb by ~kIter. Allow generous slack. */
        ok(after <= before + (kIter / 4),
           "GDI object leak on fault path: before=%lu after=%lu (iter=%lu)\n",
           before, after, kIter);

        VirtualFree(faultPage, 0, MEM_RELEASE);
    }

    DestroyIcon(hIcon);
}
