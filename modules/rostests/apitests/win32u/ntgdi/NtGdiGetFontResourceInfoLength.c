/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Contract test for the length handling of
 *                  NtGdiGetFontResourceInfoInternalW (empty / oversized file
 *                  name length, *pdwBytes larger than the caller's buffer).
 */

#include "../win32nt.h"

/* Returns a buffer of cb bytes that ends exactly at an inaccessible page */
static PBYTE
AllocBeforeGuard(SIZE_T cb, PVOID *ppBase)
{
    DWORD dwOld;
    BOOL bOk;
    PBYTE pb = VirtualAlloc(NULL, 2 * 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    ok(pb != NULL, "VirtualAlloc failed\n");
    bOk = VirtualProtect(pb + 0x1000, 0x1000, PAGE_NOACCESS, &dwOld);
    ok(bOk, "VirtualProtect failed\n");
    *ppBase = pb;
    return pb + 0x1000 - cb;
}

START_TEST(NtGdiGetFontResourceInfoLength)
{
    static const WCHAR wszName[] = L"\\??\\C:\\nonexistent\\nofont.ttf";
    BOOL bRet;
    DWORD dwBytes;
    PVOID pBase, pBaseName;
    PBYTE pbBuf;
    PWCHAR pwszName;
    WCHAR wszDir[MAX_PATH], wszFile[MAX_PATH];
    WIN32_FIND_DATAW wfd;
    HANDLE hFind;
    UNICODE_STRING NtFileName;
    BOOL bFound = FALSE;

    /* The name is placed right before a guard page so that any read past the
       given length faults */
    pwszName = (PWCHAR)AllocBeforeGuard(sizeof(wszName), &pBaseName);
    memcpy(pwszName, wszName, sizeof(wszName));

    pbBuf = AllocBeforeGuard(sizeof(LOGFONTW), &pBase);

    /* Zero length: the kernel used to compute (length - sizeof(WCHAR)) and
       clear that many bytes of an empty allocation */
    dwBytes = sizeof(LOGFONTW);
    bRet = NtGdiGetFontResourceInfoInternalW(pwszName, 0, 1, sizeof(LOGFONTW),
                                             &dwBytes, pbBuf, 2);
    ok(bRet == FALSE, "cwc == 0 must fail\n");

    /* Absurd lengths are rejected (overflow of cwc * sizeof(WCHAR)) */
    dwBytes = sizeof(LOGFONTW);
    bRet = NtGdiGetFontResourceInfoInternalW(pwszName, 0x80000000, 1, sizeof(LOGFONTW),
                                             &dwBytes, pbBuf, 2);
    ok(bRet == FALSE, "cwc == 0x80000000 must fail\n");
    dwBytes = sizeof(LOGFONTW);
    bRet = NtGdiGetFontResourceInfoInternalW(pwszName, 0xFFFFFFFF, 1, sizeof(LOGFONTW),
                                             &dwBytes, pbBuf, 2);
    ok(bRet == FALSE, "cwc == 0xFFFFFFFF must fail\n");

    /* Name length running into the guard page */
    dwBytes = sizeof(LOGFONTW);
    bRet = NtGdiGetFontResourceInfoInternalW(pwszName, _countof(wszName) + 8, 1,
                                             sizeof(LOGFONTW), &dwBytes, pbBuf, 2);
    ok(bRet == FALSE, "name beyond the mapping must fail\n");

    /* *pdwBytes larger than the caller's buffer (cjIn): must fail or succeed
       cleanly, never touch memory behind pbBuf */
    dwBytes = 0x100000;
    bRet = NtGdiGetFontResourceInfoInternalW(pwszName, _countof(wszName), 1,
                                             sizeof(LOGFONTW), &dwBytes, pbBuf, 2);
    ok(bRet == FALSE, "nonexistent font must fail\n");

    /* Try a real font if there is one */
    GetWindowsDirectoryW(wszDir, _countof(wszDir));
    wcscat(wszDir, L"\\Fonts\\");
    wcscpy(wszFile, wszDir);
    wcscat(wszFile, L"*.ttf");
    hFind = FindFirstFileW(wszFile, &wfd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            wcscpy(wszFile, wszDir);
            wcscat(wszFile, wfd.cFileName);
            if (!RtlDosPathNameToNtPathName_U(wszFile, &NtFileName, NULL, NULL))
                continue;

            dwBytes = sizeof(LOGFONTW);
            memset(pbBuf, 0, sizeof(LOGFONTW));
            bRet = NtGdiGetFontResourceInfoInternalW(NtFileName.Buffer,
                                                     (NtFileName.Length / sizeof(WCHAR)) + 1,
                                                     1, sizeof(LOGFONTW),
                                                     &dwBytes, pbBuf, 2);
            if (bRet)
            {
                /* Same call with a lying *pdwBytes */
                dwBytes = sizeof(LOGFONTW) + 0x1000;
                bRet = NtGdiGetFontResourceInfoInternalW(NtFileName.Buffer,
                                                         (NtFileName.Length / sizeof(WCHAR)) + 1,
                                                         1, sizeof(LOGFONTW),
                                                         &dwBytes, pbBuf, 2);
                ok(bRet, "valid font with larger *pdwBytes failed\n");
                ok(dwBytes == sizeof(LOGFONTW), "dwBytes = %lu\n", dwBytes);
                bFound = TRUE;
            }
            RtlFreeHeap(RtlGetProcessHeap(), 0, NtFileName.Buffer);
        } while (!bFound && FindNextFileW(hFind, &wfd));
        FindClose(hFind);
    }
    if (!bFound)
        skip("no loaded TrueType font found for the success path\n");

    VirtualFree(pBase, 0, MEM_RELEASE);
    VirtualFree(pBaseName, 0, MEM_RELEASE);
}
