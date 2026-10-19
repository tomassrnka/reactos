/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Tests for the shcore stream, thread and registry helpers
 * COPYRIGHT:   Copyright 2026 Tomas Srnka (tomas.srnka@e2b.dev)
 */

#define COBJMACROS
#include <apitest.h>
#include <objbase.h>
#include <shlwapi.h>

typedef HRESULT (WINAPI *FN_IStream_Copy)(IStream *, IStream *, ULONG);
typedef HRESULT (WINAPI *FN_IStream_ReadStr)(IStream *, PWSTR *);
typedef HRESULT (WINAPI *FN_IStream_WriteStr)(IStream *, PCWSTR);
typedef BOOL (WINAPI *FN_SHCreateThreadWithHandle)(LPTHREAD_START_ROUTINE, void *, DWORD, LPTHREAD_START_ROUTINE,
                                                   HANDLE *);
typedef LSTATUS (WINAPI *FN_SHRegGetValueFromHKCUHKLM)(PCWSTR, PCWSTR, DWORD, DWORD *, void *, DWORD *);

#define POISON ((void *)(ULONG_PTR)1)

static HMODULE LoadShcore(void)
{
    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (!hShcore)
        skip("No shcore.dll\n");
    return hShcore;
}

static void Rewind(IStream *pstm)
{
    LARGE_INTEGER liZero = { { 0 } };
    IStream_Seek(pstm, liZero, STREAM_SEEK_SET, NULL);
}

static ULONG Position(IStream *pstm)
{
    LARGE_INTEGER liZero = { { 0 } };
    ULARGE_INTEGER uliPos;
    IStream_Seek(pstm, liZero, STREAM_SEEK_CUR, &uliPos);
    return (ULONG)uliPos.QuadPart;
}

START_TEST(IStreamFunctions)
{
    static const BYTE s_abExpected[] = { 3, 0, 'h', 0, 'e', 0, 'y', 0 };
    HMODULE hShcore = LoadShcore();
    FN_IStream_Copy pIStream_Copy;
    FN_IStream_ReadStr pIStream_ReadStr;
    FN_IStream_WriteStr pIStream_WriteStr;
    IStream *pSrc, *pDst;
    BYTE ab[16];
    ULONG cbRead;
    PWSTR psz;
    WORD wZero = 0;
    HRESULT hr;

    if (!hShcore)
        return;
    pIStream_Copy = (FN_IStream_Copy)GetProcAddress(hShcore, "IStream_Copy");
    pIStream_ReadStr = (FN_IStream_ReadStr)GetProcAddress(hShcore, "IStream_ReadStr");
    pIStream_WriteStr = (FN_IStream_WriteStr)GetProcAddress(hShcore, "IStream_WriteStr");
    if (!pIStream_Copy || !pIStream_ReadStr || !pIStream_WriteStr)
    {
        skip("No stream functions in shcore\n");
        return;
    }

    ok_hr(CreateStreamOnHGlobal(NULL, TRUE, &pSrc), S_OK);
    ok_hr(CreateStreamOnHGlobal(NULL, TRUE, &pDst), S_OK);

    /* IStream_Copy copies from both current positions */
    IStream_Write(pSrc, "abcdefgh", 8, NULL);
    Rewind(pSrc);
    hr = pIStream_Copy(pSrc, pDst, 5);
    ok_hr(hr, S_OK);
    ok_long(Position(pSrc), 5);
    ok_long(Position(pDst), 5);
    Rewind(pDst);
    memset(ab, 0, sizeof(ab));
    IStream_Read(pDst, ab, sizeof(ab), &cbRead);
    ok(cbRead == 5 && !memcmp(ab, "abcde", 5), "copied %lu bytes '%.5s'\n", cbRead, (char *)ab);

    /* Fewer bytes than asked for */
    hr = pIStream_Copy(pSrc, pDst, 10);
    ok_hr(hr, E_FAIL);

    /* IStream_WriteStr writes the character count and the characters, without the terminator */
    IStream_Release(pDst);
    ok_hr(CreateStreamOnHGlobal(NULL, TRUE, &pDst), S_OK);
    hr = pIStream_WriteStr(pDst, L"hey");
    ok_hr(hr, S_OK);
    ok_long(Position(pDst), sizeof(s_abExpected));
    Rewind(pDst);
    memset(ab, 0, sizeof(ab));
    IStream_Read(pDst, ab, sizeof(ab), &cbRead);
    ok(cbRead == sizeof(s_abExpected) && !memcmp(ab, s_abExpected, sizeof(s_abExpected)), "written %lu bytes\n", cbRead);

    Rewind(pDst);
    psz = POISON;
    hr = pIStream_ReadStr(pDst, &psz);
    ok_hr(hr, S_OK);
    ok(psz && psz != POISON && !lstrcmpW(psz, L"hey"), "read '%S'\n", (psz && psz != POISON) ? psz : L"?");
    if (SUCCEEDED(hr))
        CoTaskMemFree(psz);

    /* At the end of the stream */
    psz = POISON;
    hr = pIStream_ReadStr(pDst, &psz);
    ok_hr(hr, E_FAIL);
    ok(psz == NULL, "at the end: %p\n", psz);

    /* A count of zero is an empty string */
    IStream_Release(pDst);
    ok_hr(CreateStreamOnHGlobal(NULL, TRUE, &pDst), S_OK);
    IStream_Write(pDst, &wZero, sizeof(wZero), NULL);
    Rewind(pDst);
    psz = POISON;
    hr = pIStream_ReadStr(pDst, &psz);
    ok_hr(hr, S_OK);
    ok(psz && psz != POISON && !*psz, "zero count: not an empty string\n");
    if (SUCCEEDED(hr))
        CoTaskMemFree(psz);

    IStream_Release(pSrc);
    IStream_Release(pDst);
}

static DWORD WINAPI ThreadProc(LPVOID pv)
{
    *(LONG *)pv = 1;
    return 0x1234;
}

START_TEST(SHCreateThreadWithHandle)
{
    HMODULE hShcore = LoadShcore();
    FN_SHCreateThreadWithHandle pSHCreateThreadWithHandle;
    HANDLE hThread;
    LONG lRan = 0;
    DWORD dwExit = 0;
    BOOL ret;

    if (!hShcore)
        return;
    pSHCreateThreadWithHandle = (FN_SHCreateThreadWithHandle)GetProcAddress(hShcore, "SHCreateThreadWithHandle");
    if (!pSHCreateThreadWithHandle)
    {
        skip("No SHCreateThreadWithHandle in shcore\n");
        return;
    }

    hThread = POISON;
    ret = pSHCreateThreadWithHandle(ThreadProc, &lRan, 0, NULL, &hThread);
    ok(ret == TRUE, "ret %d\n", ret);
    ok(hThread && hThread != POISON, "no thread handle\n");
    if (ret && hThread && hThread != POISON)
    {
        ok_long(WaitForSingleObject(hThread, 10000), WAIT_OBJECT_0);
        GetExitCodeThread(hThread, &dwExit);
        ok_long(dwExit, 0x1234);
        ok_long(lRan, 1);
        CloseHandle(hThread);
    }

    /* The handle pointer is required */
    ret = pSHCreateThreadWithHandle(ThreadProc, &lRan, 0, NULL, NULL);
    ok(ret == FALSE, "ret %d\n", ret);
}

START_TEST(SHRegGetValueFromHKCUHKLM)
{
    HMODULE hShcore = LoadShcore();
    FN_SHRegGetValueFromHKCUHKLM pSHRegGetValueFromHKCUHKLM;
    WCHAR szValue[MAX_PATH], szExpected[MAX_PATH];
    DWORD dwType, cb, cbExpected;
    LSTATUS err;

    if (!hShcore)
        return;
    pSHRegGetValueFromHKCUHKLM = (FN_SHRegGetValueFromHKCUHKLM)GetProcAddress(hShcore, "SHRegGetValueFromHKCUHKLM");
    if (!pSHRegGetValueFromHKCUHKLM)
    {
        skip("No SHRegGetValueFromHKCUHKLM in shcore\n");
        return;
    }

    /* A value that exists under HKLM only */
    cbExpected = sizeof(szExpected);
    err = SHRegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion", L"ProgramFilesDir",
                         SRRF_RT_ANY, NULL, szExpected, &cbExpected);
    ok_long(err, ERROR_SUCCESS);
    dwType = 0;
    cb = sizeof(szValue);
    err = pSHRegGetValueFromHKCUHKLM(L"Software\\Microsoft\\Windows\\CurrentVersion", L"ProgramFilesDir",
                                     SRRF_RT_ANY, &dwType, szValue, &cb);
    ok_long(err, ERROR_SUCCESS);
    ok_long(dwType, REG_SZ);
    ok_long(cb, cbExpected);
    ok(!lstrcmpW(szValue, szExpected), "'%S', expected '%S'\n", szValue, szExpected);

    /* A value under HKCU */
    cb = sizeof(szValue);
    err = pSHRegGetValueFromHKCUHKLM(L"Control Panel\\Desktop", L"Wallpaper", SRRF_RT_ANY, &dwType, szValue, &cb);
    ok_long(err, ERROR_SUCCESS);

    /* Missing in both */
    cb = sizeof(szValue);
    err = pSHRegGetValueFromHKCUHKLM(L"Software\\ReactOS shcore test missing key", L"x", SRRF_RT_ANY, &dwType,
                                     szValue, &cb);
    ok_long(err, ERROR_FILE_NOT_FOUND);
}
