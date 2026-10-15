/*
 * PROJECT:         ReactOS API tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Test for SystemParametersInfo function family
 * PROGRAMMERS:     Giannis Adamopoulos
 */

#include "precomp.h"
#include <winreg.h>

static HWND hWnd1, hWnd2;

static BOOL g_bReadyForDisplayChange = FALSE;
static HANDLE g_hSemDisplayChange;

/* FIXME: test for HWND_TOP, etc...*/
static int get_iwnd(HWND hWnd)
{
    if(hWnd == hWnd1) return 1;
    else if(hWnd == hWnd2) return 2;
    else return 0;
}

LRESULT CALLBACK SysParamsTestProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int iwnd = get_iwnd(hWnd);

    if(message > WM_USER || !iwnd || IsDWmMsg(message) || IseKeyMsg(message))
        return DefWindowProc(hWnd, message, wParam, lParam);

    switch(message)
    {
    case WM_GETICON :
    case WM_SETICON:
        break;
    case WM_WINDOWPOSCHANGING:
    case WM_WINDOWPOSCHANGED:
        {
            WINDOWPOS* pwp = (WINDOWPOS*)lParam;
            ok(wParam==0,"expected wParam=0\n");
            RECORD_MESSAGE(iwnd, message, SENT, get_iwnd(pwp->hwndInsertAfter), pwp->flags);
            break;
        }
    case WM_DISPLAYCHANGE:
        if (g_bReadyForDisplayChange)
        {
            g_bReadyForDisplayChange = FALSE;
            ReleaseSemaphore(g_hSemDisplayChange, 1, 0);
        }
        break;
    default:
        RECORD_MESSAGE(iwnd, message, SENT, 0,0);
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

static void FlushMessages()
{
    MSG msg;

    while (PeekMessage( &msg, 0, 0, 0, PM_REMOVE ))
    {
        int iwnd = get_iwnd(msg.hwnd);
        if(!(msg.message > WM_USER || !iwnd || IsDWmMsg(msg.message) || IseKeyMsg(msg.message)))
            RECORD_MESSAGE(iwnd, msg.message, POST,0,0);
        DispatchMessageA( &msg );
    }
}

static MSG_ENTRY NcMetricsChange_chain[]={
    {2,WM_WINDOWPOSCHANGING, SENT, 0, SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOMOVE|SWP_NOZORDER|SWP_NOREDRAW},
    {2,WM_GETMINMAXINFO},
    {2,WM_NCCALCSIZE},
    {2,WM_WINDOWPOSCHANGED, SENT, 0, SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOMOVE|SWP_NOZORDER|SWP_NOREDRAW | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE | SWP_NOSIZE},
    {1,WM_WINDOWPOSCHANGING, SENT, 0, SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOMOVE|SWP_NOZORDER|SWP_NOREDRAW},
    {1,WM_GETMINMAXINFO},
    {1,WM_NCCALCSIZE},
    {1,WM_WINDOWPOSCHANGED, SENT, 0, SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOMOVE|SWP_NOZORDER|SWP_NOREDRAW | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE | SWP_NOSIZE},
    {0,0}};

static MSG_ENTRY NcMetricsChange1_chain[]={
    {2,WM_WINDOWPOSCHANGING, SENT, 0, SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOMOVE|SWP_NOZORDER|SWP_NOREDRAW},
    {2,WM_GETMINMAXINFO},
    {2,WM_NCCALCSIZE},
    {2,WM_WINDOWPOSCHANGED, SENT, 0,  SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOMOVE|SWP_NOZORDER|SWP_NOREDRAW | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE | SWP_NOSIZE},
    {1,WM_WINDOWPOSCHANGING, SENT, 0, SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOMOVE|SWP_NOZORDER|SWP_NOREDRAW},
    {1,WM_GETMINMAXINFO},
    {1,WM_NCCALCSIZE},
    {1,WM_WINDOWPOSCHANGED, SENT, 0,  SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOMOVE|SWP_NOZORDER|SWP_NOREDRAW | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE | SWP_NOSIZE},
    {2,WM_SETTINGCHANGE},
    {1,WM_SETTINGCHANGE},
    {0,0}};

static MSG_ENTRY CaptionHeight_chain[]={
    {2,WM_WINDOWPOSCHANGING, SENT, 0, SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOMOVE|SWP_NOZORDER|SWP_NOREDRAW},
    {2,WM_GETMINMAXINFO},
    {2,WM_NCCALCSIZE},
    {2,WM_WINDOWPOSCHANGED, SENT, 0, SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOMOVE|SWP_NOZORDER|SWP_NOREDRAW | SWP_NOCLIENTSIZE},
    {2,WM_MOVE},
    {1,WM_WINDOWPOSCHANGING, SENT, 0, SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOMOVE|SWP_NOZORDER|SWP_NOREDRAW},
    {1,WM_GETMINMAXINFO},
    {1,WM_NCCALCSIZE},
    {1,WM_WINDOWPOSCHANGED, SENT, 0,  SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOMOVE|SWP_NOZORDER|SWP_NOREDRAW | SWP_NOCLIENTSIZE},
    {1,WM_MOVE},
    {0,0}};

static void Test_NonClientMetrics()
{
    NONCLIENTMETRICS NonClientMetrics;

    /* WARNING: this test requires themes and dwm to be disabled */

    SetCursorPos(0,0);

    /* Retrieve th non client metrics */
    NonClientMetrics.cbSize = sizeof(NONCLIENTMETRICS);
    SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS), &NonClientMetrics, 0);
    FlushMessages();
    COMPARE_CACHE(empty_chain);

    /* Set the non client metric without making any change */
    SystemParametersInfo(SPI_SETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS), &NonClientMetrics, 0);
    FlushMessages();
    COMPARE_CACHE(NcMetricsChange_chain);

    /* Set the same metrics again with the SPIF_SENDCHANGE param */
    SystemParametersInfo(SPI_SETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS), &NonClientMetrics, SPIF_SENDCHANGE|SPIF_UPDATEINIFILE );
    FlushMessages();
    COMPARE_CACHE(NcMetricsChange1_chain);

    /* Slightly change the caption height */
    NonClientMetrics.iCaptionHeight += 1;
    SystemParametersInfo(SPI_SETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS), &NonClientMetrics, 0);
    FlushMessages();
    COMPARE_CACHE(CaptionHeight_chain);

    /* Restore the original caption height */
    NonClientMetrics.iCaptionHeight -= 1;
    SystemParametersInfo(SPI_SETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS), &NonClientMetrics, 0);
    FlushMessages();
    COMPARE_CACHE(CaptionHeight_chain);
}

static void Test_MouseSpeed()
{
    ULONG ulMouseSpeed, temp;
    BOOL ret;

    ret = SystemParametersInfo(SPI_GETMOUSESPEED, 0, &ulMouseSpeed, 0);
    ok(ret, "SystemParametersInfo failed\n");
    ok(ulMouseSpeed >= 1 && ulMouseSpeed <=20, "Wrong mouse speed (%d)\n", (int)ulMouseSpeed);

    temp = 1;
    ret = SystemParametersInfo(SPI_SETMOUSESPEED, 0, UlongToPtr(temp), SPIF_SENDCHANGE | SPIF_UPDATEINIFILE);
    ok(ret, "SystemParametersInfo failed\n");
    ret = SystemParametersInfo(SPI_GETMOUSESPEED, 0, &temp, 0);
    ok(ret, "SystemParametersInfo failed\n");
    ok(temp == 1, "SPI_GETMOUSESPEED did not get value set by SPI_SETMOUSESPEED (%d instead of 1)\n", (int)temp);

    temp = 20;
    ret = SystemParametersInfo(SPI_SETMOUSESPEED, 0, UlongToPtr(temp), SPIF_SENDCHANGE | SPIF_UPDATEINIFILE);
    ok(ret, "SystemParametersInfo failed\n");
    ret = SystemParametersInfo(SPI_GETMOUSESPEED, 0, &temp, 0);
    ok(ret, "SystemParametersInfo failed\n");
    ok(temp == 20, "SPI_GETMOUSESPEED did not get value set by SPI_SETMOUSESPEED (%d instead of 20)\n", (int)temp);

    temp = 21;
    ret = SystemParametersInfo(SPI_SETMOUSESPEED, 0, UlongToPtr(temp), SPIF_SENDCHANGE | SPIF_UPDATEINIFILE);
    ok(!ret, "expected SystemParametersInfo to fail\n");
    ret = SystemParametersInfo(SPI_GETMOUSESPEED, 0, &temp, 0);
    ok(ret, "SystemParametersInfo failed\n");
    ok(temp == 20, "SPI_GETMOUSESPEED got unexpected value (%d instead of 20)\n", (int)temp);

    temp = 0;
    ret = SystemParametersInfo(SPI_SETMOUSESPEED, 0, UlongToPtr(temp), SPIF_SENDCHANGE | SPIF_UPDATEINIFILE);
    ok(!ret, "expected SystemParametersInfo to fail\n");
    ret = SystemParametersInfo(SPI_GETMOUSESPEED, 0, &temp, 0);
    ok(ret, "SystemParametersInfo failed\n");
    ok(temp == 20, "SPI_GETMOUSESPEED got unexpected value (%d instead of 20)\n", (int)temp);

    ret = SystemParametersInfo(SPI_SETMOUSESPEED, 0, UlongToPtr(ulMouseSpeed), SPIF_SENDCHANGE | SPIF_UPDATEINIFILE);
    ok(ret, "SystemParametersInfo failed\n");
}

static void Test_GradientCaptions(void)
{
    BOOL ret;
    LONG lResult;
    DWORD dwResult;
    BOOL bGradientCaptions, bModeFound;
    DEVMODEW OldDevMode, NewDevMode;
    INT iMode;

    ret = SystemParametersInfo(SPI_GETGRADIENTCAPTIONS, 0, &bGradientCaptions, 0);
    ok(ret, "SystemParametersInfo failed\n");
    if (bGradientCaptions == FALSE)
    {
        skip("GRADIENTCAPTIONS value has changed from its original state\n");
        return;
    }

    /* Try to find a graphics mode with 16 or 256 colors */
    iMode = 0;
    bModeFound = FALSE;
    while (EnumDisplaySettingsW(NULL, iMode, &NewDevMode))
    {
        if ((NewDevMode.dmBitsPerPel == 4) ||
            (NewDevMode.dmBitsPerPel == 8))
        {
            bModeFound = TRUE;
            break;
        }

        ++iMode;
    }
    if (!bModeFound)
    {
        skip("4bpp/8bpp graphics mode is not supported\n");
        return;
    }

    /* Save the current graphics mode */
    ret = EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &OldDevMode);
    ok(ret, "EnumDisplaySettingsW failed\n");

    g_hSemDisplayChange = CreateSemaphoreW(NULL, 0, 1, NULL);

    /* Switch to the new graphics mode */
    g_bReadyForDisplayChange = TRUE;
    lResult = ChangeDisplaySettingsExW(NULL, &NewDevMode, NULL, 0, NULL);
    if (lResult == DISP_CHANGE_SUCCESSFUL)
    {
        dwResult = WaitForSingleObject(g_hSemDisplayChange, 10000);
        ok(dwResult == WAIT_OBJECT_0, "Waiting for the WM_DISPLAYCHANGE message timed out\n");
    }
    g_bReadyForDisplayChange = FALSE;
    ok(lResult == DISP_CHANGE_SUCCESSFUL, "ChangeDisplaySettingsExW returned %ld\n", lResult);

    /* SPI_GETGRADIENTCAPTIONS will now always return FALSE */
    ret = SystemParametersInfo(SPI_GETGRADIENTCAPTIONS, 0, &bGradientCaptions, 0);
    ok(ret, "SystemParametersInfo failed\n");
    ok(bGradientCaptions == FALSE, "SPI_GETGRADIENTCAPTIONS got unexpected value (%d instead of 0)\n", bGradientCaptions);

    /* Enable gradient captions */
    bGradientCaptions = TRUE;
    SystemParametersInfo(SPI_SETGRADIENTCAPTIONS, 0, UlongToPtr(bGradientCaptions), 0);

    /* Still FALSE */
    ret = SystemParametersInfo(SPI_GETGRADIENTCAPTIONS, 0, &bGradientCaptions, 0);
    ok(ret, "SystemParametersInfo failed\n");
    ok(bGradientCaptions == FALSE, "SPI_GETGRADIENTCAPTIONS got unexpected value (%d instead of 0)\n", bGradientCaptions);

    /* Restore the previous graphics mode */
    g_bReadyForDisplayChange = TRUE;
    lResult = ChangeDisplaySettingsExW(NULL, &OldDevMode, NULL, 0, NULL);
    if (lResult == DISP_CHANGE_SUCCESSFUL)
    {
        dwResult = WaitForSingleObject(g_hSemDisplayChange, 10000);
        ok(dwResult == WAIT_OBJECT_0, "Waiting for the WM_DISPLAYCHANGE message timed out\n");
    }
    g_bReadyForDisplayChange = FALSE;
    ok(lResult == DISP_CHANGE_SUCCESSFUL, "ChangeDisplaySettingsExW returned %ld\n", lResult);

    /* The original value should be restored */
    ret = SystemParametersInfo(SPI_GETGRADIENTCAPTIONS, 0, &bGradientCaptions, 0);
    ok(ret, "SystemParametersInfo failed\n");
    ok(bGradientCaptions == TRUE, "SPI_GETGRADIENTCAPTIONS got unexpected value (%d instead of 1)\n", bGradientCaptions);

    CloseHandle(g_hSemDisplayChange);
}

static BOOL GetUserPrefMaskBytes(BYTE *pData, DWORD *pcbData)
{
    HKEY hKey;
    LONG lResult;
    DWORD dwType;

    lResult = RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_QUERY_VALUE, &hKey);
    if (lResult != ERROR_SUCCESS)
        return FALSE;
    lResult = RegQueryValueExW(hKey, L"UserPreferencesMask", NULL, &dwType, pData, pcbData);
    RegCloseKey(hKey);
    return (lResult == ERROR_SUCCESS && dwType == REG_BINARY);
}

static void SetUserPrefMaskBytes(const BYTE *pData, DWORD cbData)
{
    HKEY hKey;
    LONG lResult;

    lResult = RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_SET_VALUE, &hKey);
    ok(lResult == ERROR_SUCCESS, "RegOpenKeyExW failed: %ld\n", lResult);
    if (lResult != ERROR_SUCCESS)
        return;
    lResult = RegSetValueExW(hKey, L"UserPreferencesMask", 0, REG_BINARY, pData, cbData);
    ok(lResult == ERROR_SUCCESS, "RegSetValueExW failed: %ld\n", lResult);
    RegCloseKey(hKey);
}

static void Test_UserPrefBool(UINT uiGet, UINT uiSet, const char *pszName, UINT iByte, BYTE bBit)
{
    BOOL ret, bOrig, bValue, bRead;
    BYTE OrigMask[16], Mask[16];
    DWORD cbOrigMask = sizeof(OrigMask), cbMask;

    bOrig = 0xdeadbeef;
    ret = SystemParametersInfoW(uiGet, 0, &bOrig, 0);
    ok(ret, "%s: get failed\n", pszName);
    ok(bOrig == FALSE || bOrig == TRUE, "%s: got %d\n", pszName, bOrig);
    if (!ret || (bOrig != FALSE && bOrig != TRUE))
        return;
    if (!GetUserPrefMaskBytes(OrigMask, &cbOrigMask) || cbOrigMask <= iByte)
    {
        skip("%s: no usable UserPreferencesMask\n", pszName);
        return;
    }

    /* The new value is passed in pvParam */
    ret = SystemParametersInfoW(uiSet, 0, UlongToPtr(TRUE), 0);
    ok(ret, "%s: set TRUE failed\n", pszName);
    bValue = 0xdeadbeef;
    ret = SystemParametersInfoW(uiGet, 0, &bValue, 0);
    ok(ret, "%s: get failed\n", pszName);
    ok(bValue == TRUE, "%s: expected TRUE, got %d\n", pszName, bValue);

    ret = SystemParametersInfoW(uiSet, 0, UlongToPtr(FALSE), 0);
    ok(ret, "%s: set FALSE failed\n", pszName);
    bValue = 0xdeadbeef;
    ret = SystemParametersInfoW(uiGet, 0, &bValue, 0);
    ok(ret, "%s: get failed\n", pszName);
    ok(bValue == FALSE, "%s: expected FALSE, got %d\n", pszName, bValue);

    /* SPIF_UPDATEINIFILE stores the value as a bit of UserPreferencesMask */
    ret = SystemParametersInfoW(uiSet, 0, UlongToPtr(TRUE), SPIF_UPDATEINIFILE);
    ok(ret, "%s: set TRUE (ini) failed\n", pszName);
    ZeroMemory(Mask, sizeof(Mask));
    cbMask = sizeof(Mask);
    bRead = GetUserPrefMaskBytes(Mask, &cbMask);
    ok(bRead, "%s: UserPreferencesMask unreadable\n", pszName);
    if (bRead)
    {
        ok(cbMask > iByte && (Mask[iByte] & bBit) != 0,
           "%s: bit 0x%02x of byte %u not set (byte 0x%02x)\n", pszName, bBit, iByte, cbMask > iByte ? Mask[iByte] : 0);
    }

    ret = SystemParametersInfoW(uiSet, 0, UlongToPtr(FALSE), SPIF_UPDATEINIFILE);
    ok(ret, "%s: set FALSE (ini) failed\n", pszName);
    ZeroMemory(Mask, sizeof(Mask));
    cbMask = sizeof(Mask);
    bRead = GetUserPrefMaskBytes(Mask, &cbMask);
    ok(bRead, "%s: UserPreferencesMask unreadable\n", pszName);
    if (bRead)
    {
        ok(cbMask > iByte && (Mask[iByte] & bBit) == 0,
           "%s: bit 0x%02x of byte %u still set (byte 0x%02x)\n", pszName, bBit, iByte, cbMask > iByte ? Mask[iByte] : 0);
    }

    /* Restore */
    ret = SystemParametersInfoW(uiSet, 0, UlongToPtr(bOrig), 0);
    ok(ret, "%s: restore failed\n", pszName);
    SetUserPrefMaskBytes(OrigMask, cbOrigMask);
}

#ifndef SPI_GETWHEELSCROLLCHARS
#define SPI_GETWHEELSCROLLCHARS 0x006C
#define SPI_SETWHEELSCROLLCHARS 0x006D
#endif

/* Firefox only doubles the wheel speed when both the lines and the characters are the default of 3 */
static void Test_WheelScroll(void)
{
    UINT lines = 0xAAAA, chars = 0xAAAA, saved, expected = 3;
    BOOL ret;
    HKEY hKey;
    WCHAR szValue[16];
    DWORD cbValue = sizeof(szValue), dwType;

    ret = SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    ok(ret, "SPI_GETWHEELSCROLLLINES failed, error %lu\n", GetLastError());
    ok(lines != 0xAAAA, "SPI_GETWHEELSCROLLLINES did not write\n");

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(hKey, L"WheelScrollChars", NULL, &dwType, (LPBYTE)szValue, &cbValue) == ERROR_SUCCESS &&
            dwType == REG_SZ)
            expected = _wtoi(szValue);
        RegCloseKey(hKey);
    }

    SetLastError(0xdeadbeef);
    ret = SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &chars, 0);
    ok(ret, "SPI_GETWHEELSCROLLCHARS failed, error %lu\n", GetLastError());
    ok(chars == expected, "SPI_GETWHEELSCROLLCHARS: got %u, expected %u\n", chars, expected);
    if (!ret)
        return;

    saved = chars;
    ret = SystemParametersInfoW(SPI_SETWHEELSCROLLCHARS, 5, NULL, 0);
    ok(ret, "SPI_SETWHEELSCROLLCHARS failed, error %lu\n", GetLastError());
    chars = 0;
    ret = SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &chars, 0);
    ok(ret && chars == 5, "SPI_GETWHEELSCROLLCHARS after the set: %d, %u\n", ret, chars);
    ret = SystemParametersInfoW(SPI_SETWHEELSCROLLCHARS, saved, NULL, 0);
    ok(ret, "restoring SPI_SETWHEELSCROLLCHARS failed, error %lu\n", GetLastError());
}

static void Test_MouseSonarVanish(void)
{
    Test_UserPrefBool(SPI_GETMOUSESONAR, SPI_SETMOUSESONAR, "MOUSESONAR", 1, 0x40);
    Test_UserPrefBool(SPI_GETMOUSEVANISH, SPI_SETMOUSEVANISH, "MOUSEVANISH", 2, 0x01);
}

START_TEST(SystemParametersInfo)
{
    RegisterSimpleClass(SysParamsTestProc, L"sysparamstest");
    hWnd1 = CreateWindowW(L"sysparamstest", L"sysparamstest", WS_OVERLAPPEDWINDOW,
                        20, 20, 300, 300, NULL, NULL, 0, NULL);

    hWnd2 = CreateWindowW(L"sysparamstest", L"sysparamstest", WS_OVERLAPPEDWINDOW,
                         200, 200, 300, 300, NULL, NULL, 0, NULL);

    Test_NonClientMetrics();
    Test_MouseSpeed();
    Test_GradientCaptions();
    Test_MouseSonarVanish();
    Test_WheelScroll();

    DestroyWindow(hWnd1);
    DestroyWindow(hWnd2);
    UnregisterClassW(L"sysparamstest", 0);
}
