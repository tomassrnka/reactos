/*
 * Provides default drive shell extension
 *
 * Copyright 2005 Johannes Anderwald
 * Copyright 2012 Rafal Harabien
 * Copyright 2020 Katayama Hirofumi MZ
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "precomp.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <devguid.h>

#define NTOS_MODE_USER
#include <ndk/iofuncs.h>
#include <ndk/obfuncs.h>

WINE_DEFAULT_DEBUG_CHANNEL(shell);

typedef enum
{
    HWPD_STANDARDLIST = 0,
    HWPD_LARGELIST,
    HWPD_MAX = HWPD_LARGELIST
} HWPAGE_DISPLAYMODE, *PHWPAGE_DISPLAYMODE;

EXTERN_C HWND WINAPI
DeviceCreateHardwarePageEx(HWND hWndParent,
                           LPGUID lpGuids,
                           UINT uNumberOfGuids,
                           HWPAGE_DISPLAYMODE DisplayMode);
UINT SH_FormatByteSize(LONGLONG cbSize, LPWSTR pwszResult, UINT cchResultMax);

static VOID
GetDriveNameWithLetter(LPWSTR pwszText, UINT cchTextMax, LPCWSTR pwszDrive)
{
    DWORD dwMaxComp, dwFileSys;
    SIZE_T cchText = 0;

    if (GetVolumeInformationW(pwszDrive, pwszText, cchTextMax, NULL, &dwMaxComp, &dwFileSys, NULL, 0))
    {
        cchText = wcslen(pwszText);
        if (cchText == 0)
        {
            /* load default volume label */
            cchText = LoadStringW(shell32_hInstance, IDS_DRIVE_FIXED, pwszText, cchTextMax);
        }
    }

    StringCchPrintfW(pwszText + cchText, cchTextMax - cchText, L" (%c:)", pwszDrive[0]);
}

static VOID
InitializeChkDskDialog(HWND hwndDlg, LPCWSTR pwszDrive)
{
    WCHAR wszText[100];
    UINT Length;
    SetWindowLongPtr(hwndDlg, DWLP_USER, (INT_PTR)pwszDrive);

    Length = GetWindowTextW(hwndDlg, wszText, sizeof(wszText) / sizeof(WCHAR));
    wszText[Length] = L' ';
    GetDriveNameWithLetter(&wszText[Length + 1], (sizeof(wszText) / sizeof(WCHAR)) - Length - 1, pwszDrive);
    SetWindowText(hwndDlg, wszText);
}

/* Chkdsk() has no context parameter: one check at a time per process uses this state */
static CComAutoCriticalSection ChkdskLock;
static HWND hChkdskDrvDialog = NULL;
static BOOLEAN bChkdskCorrectErrors = FALSE;
static BOOLEAN bChkdskSuccess = FALSE;
static BOOLEAN bChkdskNotSupported = FALSE;
static BOOLEAN bChkdskVolumeInUse = FALSE;

static BOOLEAN NTAPI
ChkdskCallback(
    IN CALLBACKCOMMAND Command,
    IN ULONG SubAction,
    IN PVOID ActionInfo)
{
    PDWORD Progress;
    PBOOLEAN pSuccess;
    switch(Command)
    {
        case PROGRESS:
            Progress = (PDWORD)ActionInfo;
            SendDlgItemMessageW(hChkdskDrvDialog, 14002, PBM_SETPOS, (WPARAM)*Progress, 0);
            break;
        case DONE:
            pSuccess = (PBOOLEAN)ActionInfo;
            bChkdskSuccess = (*pSuccess);
            break;

        case FSNOTSUPPORTED:
            bChkdskNotSupported = TRUE;
            break;

        case VOLUMEINUSE:
            /* Without exclusive access only a read-only check may go on */
            bChkdskVolumeInUse = TRUE;
            return !bChkdskCorrectErrors;

        default:
            break;
    }

    return TRUE;
}

static BOOL
ChkDskNow(HWND hwndDlg, LPCWSTR pwszDrive)
{
    //DWORD ClusterSize = 0;
    WCHAR wszFs[30], wszTitle[100];
    ULARGE_INTEGER TotalNumberOfFreeBytes, FreeBytesAvailableUser;
    BOOLEAN bCorrectErrors = FALSE, bScanDrive = FALSE;
    BOOLEAN bSuccess, bNotSupported, bVolumeInUse;
    UINT idMessage;

    if(!GetVolumeInformationW(pwszDrive, NULL, 0, NULL, NULL, NULL, wszFs, _countof(wszFs)) ||
       !GetDiskFreeSpaceExW(pwszDrive, &FreeBytesAvailableUser, &TotalNumberOfFreeBytes, NULL))
    {
        SHELL_ErrorBox(hwndDlg, GetLastError());
        return FALSE;
    }

    /*if (!GetDefaultClusterSize(wszFs, &ClusterSize, &TotalNumberOfFreeBytes))
    {
        FIXME("invalid cluster size\n");
        return;
    }*/

    if (SendDlgItemMessageW(hwndDlg, 14000, BM_GETCHECK, 0, 0) == BST_CHECKED)
        bCorrectErrors = TRUE;

    if (SendDlgItemMessageW(hwndDlg, 14001, BM_GETCHECK, 0, 0) == BST_CHECKED)
        bScanDrive = TRUE;

    SendDlgItemMessageW(hwndDlg, 14002, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

    ChkdskLock.Lock();
    hChkdskDrvDialog = hwndDlg;
    bChkdskCorrectErrors = bCorrectErrors;
    bChkdskSuccess = FALSE;
    bChkdskNotSupported = FALSE;
    bChkdskVolumeInUse = FALSE;
    Chkdsk((LPWSTR)pwszDrive, (LPWSTR)wszFs, bCorrectErrors, TRUE, FALSE, bScanDrive, NULL, NULL, ChkdskCallback); // FIXME: casts
    bSuccess = bChkdskSuccess;
    bNotSupported = bChkdskNotSupported;
    bVolumeInUse = bChkdskVolumeInUse;
    hChkdskDrvDialog = NULL;
    ChkdskLock.Unlock();

    if (bSuccess)
        idMessage = IDS_CHKDSK_COMPLETE;
    else if (bNotSupported)
        idMessage = IDS_CHKDSK_NOT_SUPPORTED;
    else if (bVolumeInUse)
        idMessage = IDS_CHKDSK_VOLUME_IN_USE;
    else
        idMessage = IDS_CHKDSK_FAILED;

    GetWindowTextW(hwndDlg, wszTitle, _countof(wszTitle));
    ShellMessageBoxW(shell32_hInstance, hwndDlg, MAKEINTRESOURCEW(idMessage), wszTitle,
                     MB_OK | (bSuccess ? MB_ICONINFORMATION : MB_ICONERROR), wszFs);

    return bSuccess;
}

static INT_PTR CALLBACK
ChkDskDlg(
    HWND hwndDlg,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch(uMsg)
    {
        case WM_INITDIALOG:
            SetWindowLongPtr(hwndDlg, DWLP_USER, (LONG_PTR)lParam);
            InitializeChkDskDialog(hwndDlg, (LPCWSTR)lParam);
            return TRUE;
        case WM_COMMAND:
            switch(LOWORD(wParam))
            {
                case IDCANCEL:
                    EndDialog(hwndDlg, 0);
                    break;
                case IDOK:
                {
                    LPCWSTR pwszDrive = (LPCWSTR)GetWindowLongPtr(hwndDlg, DWLP_USER);
                    if (ChkDskNow(hwndDlg, pwszDrive))
                        EndDialog(hwndDlg, IDOK);
                    break;
                }
            }
            break;
    }

    return FALSE;
}

VOID
CDrvDefExt::PaintStaticControls(HWND hwndDlg, LPDRAWITEMSTRUCT pDrawItem)
{
    HBRUSH hBrush;

    if (pDrawItem->CtlID == 14013)
    {
        hBrush = CreateSolidBrush(RGB(0, 0, 255));
        if (hBrush)
        {
            FillRect(pDrawItem->hDC, &pDrawItem->rcItem, hBrush);
            DeleteObject((HGDIOBJ)hBrush);
        }
    }
    else if (pDrawItem->CtlID == 14014)
    {
        hBrush = CreateSolidBrush(RGB(255, 0, 255));
        if (hBrush)
        {
            FillRect(pDrawItem->hDC, &pDrawItem->rcItem, hBrush);
            DeleteObject((HGDIOBJ)hBrush);
        }
    }
    else if (pDrawItem->CtlID == 14015)
    {
        HBRUSH hBlueBrush = CreateSolidBrush(RGB(0, 0, 255));
        HBRUSH hMagBrush = CreateSolidBrush(RGB(255, 0, 255));
        HBRUSH hbrOld;
        HPEN hBlackPen = (HPEN)GetStockObject(BLACK_PEN);
        HPEN hDarkBluePen = CreatePen(PS_SOLID, 1, RGB(0, 0, 128));
        HPEN hDarkMagPen = CreatePen(PS_SOLID, 1, RGB(128, 0, 128));
        HPEN hOldPen = (HPEN)SelectObject(pDrawItem->hDC, hDarkMagPen);
        INT xCenter = (pDrawItem->rcItem.left + pDrawItem->rcItem.right) / 2;
        INT yCenter = (pDrawItem->rcItem.top + pDrawItem->rcItem.bottom - 10) / 2;
        INT cx = pDrawItem->rcItem.right - pDrawItem->rcItem.left;
        INT cy = pDrawItem->rcItem.bottom - pDrawItem->rcItem.top - 10;
        INT xRadial = xCenter + (INT)(cos(M_PI + m_FreeSpacePerc / 100.0f * M_PI * 2.0f) * cx / 2);
        INT yRadial = yCenter - (INT)(sin(M_PI + m_FreeSpacePerc / 100.0f * M_PI * 2.0f) * cy / 2);

        TRACE("FreeSpace %u a %f cx %d\n", m_FreeSpacePerc, M_PI+m_FreeSpacePerc / 100.0f * M_PI * 2.0f, cx);

        for (INT x = pDrawItem->rcItem.left; x < pDrawItem->rcItem.right; ++x)
        {
            double cos_val = (x - xCenter) * 2.0f / cx;
            INT y = yCenter + (INT)(sin(acos(cos_val)) * cy / 2) - 1;
            HPEN hCenterPen;

            if (m_FreeSpacePerc < 50 && x == xRadial)
                SelectObject(pDrawItem->hDC, hDarkBluePen);
            
            /* Temporarily change pens to draw edges */
            if (x == pDrawItem->rcItem.left)
                hCenterPen = (HPEN)SelectObject(pDrawItem->hDC, hBlackPen);
            else if (x == pDrawItem->rcItem.right - 1)
                SelectObject(pDrawItem->hDC, hBlackPen);

            MoveToEx(pDrawItem->hDC, x, y, NULL);
            LineTo(pDrawItem->hDC, x, y + 10);
            SetPixel(pDrawItem->hDC, x, y + 10, RGB(0, 0, 0));
            
            /* Restore fill section pens */
            if (x == pDrawItem->rcItem.left)
                SelectObject(pDrawItem->hDC, hCenterPen);
        }

        SelectObject(pDrawItem->hDC, hBlackPen);

        if (m_FreeSpacePerc > 50)
        {
            hbrOld = (HBRUSH)SelectObject(pDrawItem->hDC, hMagBrush);

            Ellipse(pDrawItem->hDC, pDrawItem->rcItem.left, pDrawItem->rcItem.top,
                    pDrawItem->rcItem.right, pDrawItem->rcItem.bottom - 10);

            SelectObject(pDrawItem->hDC, hBlueBrush);

            if (m_FreeSpacePerc < 100)
            {
                Pie(pDrawItem->hDC, pDrawItem->rcItem.left, pDrawItem->rcItem.top, pDrawItem->rcItem.right,
                    pDrawItem->rcItem.bottom - 10, xRadial, yRadial, pDrawItem->rcItem.left, yCenter);
            }
        }
        else
        {
            hbrOld = (HBRUSH)SelectObject(pDrawItem->hDC, hBlueBrush);

            Ellipse(pDrawItem->hDC, pDrawItem->rcItem.left, pDrawItem->rcItem.top,
                    pDrawItem->rcItem.right, pDrawItem->rcItem.bottom - 10);

            SelectObject(pDrawItem->hDC, hMagBrush);

            if (m_FreeSpacePerc > 0)
            {
                Pie(pDrawItem->hDC, pDrawItem->rcItem.left, pDrawItem->rcItem.top, pDrawItem->rcItem.right,
                    pDrawItem->rcItem.bottom - 10, pDrawItem->rcItem.left, yCenter, xRadial, yRadial);
            }
        }

        SelectObject(pDrawItem->hDC, hbrOld);
        SelectObject(pDrawItem->hDC, hOldPen);

        DeleteObject(hBlueBrush);
        DeleteObject(hMagBrush);
        DeleteObject(hDarkBluePen);
        DeleteObject(hDarkMagPen);
    }
}

// https://stackoverflow.com/questions/3098696/get-information-about-disk-drives-result-on-windows7-32-bit-system/3100268#3100268
static BOOL
GetDriveTypeAndCharacteristics(HANDLE hDevice, DEVICE_TYPE *pDeviceType, ULONG *pCharacteristics)
{
    NTSTATUS Status;
    IO_STATUS_BLOCK IoStatusBlock;
    FILE_FS_DEVICE_INFORMATION DeviceInfo;

    Status = NtQueryVolumeInformationFile(hDevice, &IoStatusBlock,
                                          &DeviceInfo, sizeof(DeviceInfo),
                                          FileFsDeviceInformation);
    if (Status == NO_ERROR)
    {
        *pDeviceType = DeviceInfo.DeviceType;
        *pCharacteristics = DeviceInfo.Characteristics;
        return TRUE;
    }

    return FALSE;
}

BOOL IsDriveFloppyW(LPCWSTR pszDriveRoot)
{
    LPCWSTR RootPath = pszDriveRoot;
    WCHAR szRoot[16], szDeviceName[16];
    UINT uType;
    HANDLE hDevice;
    DEVICE_TYPE DeviceType;
    ULONG ulCharacteristics;
    BOOL ret;

    lstrcpynW(szRoot, RootPath, _countof(szRoot));

    if (L'a' <= szRoot[0] && szRoot[0] <= 'z')
    {
        szRoot[0] += ('A' - 'a');
    }

    if ('A' <= szRoot[0] && szRoot[0] <= L'Z' &&
        szRoot[1] == L':' && szRoot[2] == 0)
    {
        // 'C:' --> 'C:\'
        szRoot[2] = L'\\';
        szRoot[3] = 0;
    }

    if (!PathIsRootW(szRoot))
    {
        return FALSE;
    }

    uType = GetDriveTypeW(szRoot);
    if (uType == DRIVE_REMOVABLE)
    {
        if (szRoot[0] == L'A' || szRoot[0] == L'B')
            return TRUE;
    }
    else
    {
        return FALSE;
    }

    lstrcpynW(szDeviceName, L"\\\\.\\", _countof(szDeviceName));
    szDeviceName[4] = szRoot[0];
    szDeviceName[5] = L':';
    szDeviceName[6] = UNICODE_NULL;

    hDevice = CreateFileW(szDeviceName, FILE_READ_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }

    ret = FALSE;
    if (GetDriveTypeAndCharacteristics(hDevice, &DeviceType, &ulCharacteristics))
    {
        if ((ulCharacteristics & FILE_FLOPPY_DISKETTE) == FILE_FLOPPY_DISKETTE)
            ret = TRUE;
    }

    CloseHandle(hDevice);

    return ret;
}

BOOL IsDriveFloppyA(LPCSTR pszDriveRoot)
{
    WCHAR szRoot[8];
    MultiByteToWideChar(CP_ACP, 0, pszDriveRoot, -1, szRoot, _countof(szRoot));
    return IsDriveFloppyW(szRoot);
}

VOID
CDrvDefExt::InitGeneralPage(HWND hwndDlg)
{
    WCHAR wszVolumeName[MAX_PATH+1] = {0};
    WCHAR wszFileSystem[MAX_PATH+1] = {0};
    WCHAR wszBuf[128];
    BOOL bRet, bFloppy = FALSE, bHasFS = FALSE;

    bRet = GetVolumeInformationW(m_wszDrive, wszVolumeName, _countof(wszVolumeName), NULL, NULL, NULL, wszFileSystem, _countof(wszFileSystem));
    if (bRet)
    {
        /* Set volume label and filesystem */
        SetDlgItemTextW(hwndDlg, 14000, wszVolumeName);
        SetDlgItemTextW(hwndDlg, 14002, wszFileSystem);
        bHasFS = *wszFileSystem != UNICODE_NULL;
    }
    else
    {
        LoadStringW(shell32_hInstance, IDS_FS_UNKNOWN, wszFileSystem, _countof(wszFileSystem));
        SetDlgItemTextW(hwndDlg, 14002, wszFileSystem);
    }

    /* Set drive type and icon */
    // TODO: Call SHGetFileInfo to get this info
    UINT DriveType = GetDriveTypeW(m_wszDrive);
    UINT IconId, TypeStrId;
    switch (DriveType)
    {
        case DRIVE_REMOVABLE:
            bFloppy = IsDriveFloppyW(m_wszDrive);
            IconId = bFloppy ? IDI_SHELL_3_14_FLOPPY : IDI_SHELL_REMOVEABLE;
            TypeStrId = bFloppy ? IDS_DRIVE_FLOPPY : IDS_DRIVE_REMOVABLE;
            break;
        case DRIVE_CDROM: IconId = IDI_SHELL_CDROM; TypeStrId = IDS_DRIVE_CDROM; break;
        case DRIVE_REMOTE: IconId = IDI_SHELL_NETDRIVE; TypeStrId = IDS_DRIVE_NETWORK; break;
        case DRIVE_RAMDISK: IconId = IDI_SHELL_RAMDISK; break;
        default: IconId = IDI_SHELL_DRIVE; TypeStrId = IDS_DRIVE_FIXED;
    }

    BOOL bCanSetLabel = bHasFS;
    if (DriveType == DRIVE_CDROM || DriveType == DRIVE_REMOTE || bFloppy)
    {
        bCanSetLabel = bFloppy && bHasFS;

        /* disk compression */
        ShowWindow(GetDlgItem(hwndDlg, 14011), FALSE);

        /* index */
        ShowWindow(GetDlgItem(hwndDlg, 14012), FALSE);
    }
    /* volume label textbox */
    SendMessage(GetDlgItem(hwndDlg, 14000), EM_SETREADONLY, !bCanSetLabel, 0);

    HICON hIcon = (HICON)LoadImage(shell32_hInstance, MAKEINTRESOURCE(IconId), IMAGE_ICON, 32, 32, LR_SHARED);
    if (hIcon)
        SendDlgItemMessageW(hwndDlg, 14016, STM_SETICON, (WPARAM)hIcon, 0);
    if (TypeStrId && LoadStringW(shell32_hInstance, TypeStrId, wszBuf, _countof(wszBuf)))
        SetDlgItemTextW(hwndDlg, 14001, wszBuf);

    ULARGE_INTEGER FreeBytesAvailable, TotalNumberOfBytes;
    if(GetDiskFreeSpaceExW(m_wszDrive, &FreeBytesAvailable, &TotalNumberOfBytes, NULL))
    {
        /* Init free space percentage used for drawing piechart */
        m_FreeSpacePerc = (UINT)(FreeBytesAvailable.QuadPart * 100ull / TotalNumberOfBytes.QuadPart);

        /* Used space */
        if (SH_FormatByteSize(TotalNumberOfBytes.QuadPart - FreeBytesAvailable.QuadPart, wszBuf, _countof(wszBuf)))
            SetDlgItemTextW(hwndDlg, 14003, wszBuf);

        if (StrFormatByteSizeW(TotalNumberOfBytes.QuadPart - FreeBytesAvailable.QuadPart, wszBuf, _countof(wszBuf)))
            SetDlgItemTextW(hwndDlg, 14004, wszBuf);

        /* Free space */
        if (SH_FormatByteSize(FreeBytesAvailable.QuadPart, wszBuf, _countof(wszBuf)))
            SetDlgItemTextW(hwndDlg, 14005, wszBuf);

        if (StrFormatByteSizeW(FreeBytesAvailable.QuadPart, wszBuf, _countof(wszBuf)))
            SetDlgItemTextW(hwndDlg, 14006, wszBuf);

        /* Total space */
        if (SH_FormatByteSize(TotalNumberOfBytes.QuadPart, wszBuf, _countof(wszBuf)))
            SetDlgItemTextW(hwndDlg, 14007, wszBuf);

        if (StrFormatByteSizeW(TotalNumberOfBytes.QuadPart, wszBuf, _countof(wszBuf)))
            SetDlgItemTextW(hwndDlg, 14008, wszBuf);
    }
    else
    {
        m_FreeSpacePerc = 0;

        if (SH_FormatByteSize(0, wszBuf, _countof(wszBuf)))
        {
            SetDlgItemTextW(hwndDlg, 14003, wszBuf);
            SetDlgItemTextW(hwndDlg, 14005, wszBuf);
            SetDlgItemTextW(hwndDlg, 14007, wszBuf);
        }
        if (StrFormatByteSizeW(0, wszBuf, _countof(wszBuf)))
        {
            SetDlgItemTextW(hwndDlg, 14004, wszBuf);
            SetDlgItemTextW(hwndDlg, 14006, wszBuf);
            SetDlgItemTextW(hwndDlg, 14008, wszBuf);
        }
    }

    /* Set drive description */
    WCHAR wszFormat[50];
    GetDlgItemTextW(hwndDlg, 14009, wszFormat, _countof(wszFormat));
    _swprintf(wszBuf, wszFormat, m_wszDrive[0]);
    SetDlgItemTextW(hwndDlg, 14009, wszBuf);

    /* show disk cleanup button only for fixed drives */
    ShowWindow(GetDlgItem(hwndDlg, 14010), DriveType == DRIVE_FIXED);
}

INT_PTR CALLBACK
CDrvDefExt::GeneralPageProc(
    HWND hwndDlg,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch(uMsg)
    {
        case WM_INITDIALOG:
        {
            LPPROPSHEETPAGEW ppsp = (LPPROPSHEETPAGEW)lParam;
            if (ppsp == NULL)
                break;

            CDrvDefExt *pDrvDefExt = reinterpret_cast<CDrvDefExt *>(ppsp->lParam);
            SetWindowLongPtr(hwndDlg, DWLP_USER, (LONG_PTR)pDrvDefExt);
            pDrvDefExt->InitGeneralPage(hwndDlg);
            return TRUE;
        }
        case WM_DRAWITEM:
        {
            LPDRAWITEMSTRUCT pDrawItem = (LPDRAWITEMSTRUCT)lParam;

            if (pDrawItem->CtlID >= 14013 && pDrawItem->CtlID <= 14015)
            {
                CDrvDefExt *pDrvDefExt = reinterpret_cast<CDrvDefExt *>(GetWindowLongPtr(hwndDlg, DWLP_USER));
                pDrvDefExt->PaintStaticControls(hwndDlg, pDrawItem);
                return TRUE;
            }
            break;
        }
        case WM_PAINT:
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == 14010) /* Disk Cleanup */
            {
                CDrvDefExt *pDrvDefExt = reinterpret_cast<CDrvDefExt *>(GetWindowLongPtr(hwndDlg, DWLP_USER));
                WCHAR wszBuf[256];
                DWORD cbBuf = sizeof(wszBuf);

                if (RegGetValueW(HKEY_LOCAL_MACHINE,
                                 L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MyComputer\\CleanupPath",
                                 NULL,
                                 RRF_RT_REG_SZ,
                                 NULL,
                                 (PVOID)wszBuf,
                                 &cbBuf) == ERROR_SUCCESS)
                {
                    WCHAR wszCmd[MAX_PATH];

                    StringCbPrintfW(wszCmd, sizeof(wszCmd), wszBuf, pDrvDefExt->m_wszDrive[0]);
                    WCHAR* wszArgs = PathGetArgsW(wszCmd);
                    if (wszArgs && *wszArgs && wszArgs != wszCmd)
                        wszArgs[-1] = UNICODE_NULL;
                    else
                        wszArgs = NULL;

                    if (ShellExecuteW(hwndDlg, NULL, wszCmd, wszArgs, NULL, SW_SHOW) <= (HINSTANCE)32)
                        ERR("Failed to create cleanup process %ls\n", wszCmd);
                }
            }
            else if (LOWORD(wParam) == 14000) /* Label */
            {
                if (HIWORD(wParam) == EN_CHANGE)
                    PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
            }
            break;
        case WM_NOTIFY:
            if (((LPNMHDR)lParam)->hwndFrom == GetParent(hwndDlg))
            {
                /* Property Sheet */
                LPPSHNOTIFY lppsn = (LPPSHNOTIFY)lParam;

                if (lppsn->hdr.code == PSN_APPLY)
                {
                    CDrvDefExt *pDrvDefExt = reinterpret_cast<CDrvDefExt *>(GetWindowLongPtr(hwndDlg, DWLP_USER));

                    HRESULT hr = E_FAIL;
                    HWND hLabel = GetDlgItem(hwndDlg, 14000);
                    WCHAR wszBuf[256];
                    *wszBuf = UNICODE_NULL;
                    if (GetWindowTextW(hLabel, wszBuf, _countof(wszBuf)) || GetWindowTextLengthW(hLabel) == 0)
                        hr = CDrivesFolder::SetDriveLabel(hwndDlg, pDrvDefExt->m_wszDrive, wszBuf);

                    SetWindowLongPtr(hwndDlg, DWLP_MSGRESULT, FAILED(hr) ? PSNRET_INVALID : PSNRET_NOERROR);
                    return TRUE;
                }
            }
            break;

        default:
            break;
    }

    return FALSE;
}

/* The file that CreateProcess would take for this name, in its search order (".exe" added without an extension) */
static BOOL
FindToolProgram(LPCWSTR pszName, LPWSTR pszFile, DWORD cchFile)
{
    WCHAR szDir[MAX_PATH];
    CStringW strPath;
    DWORD cch, dwAttr;

    if (!*pszName || StrChrW(pszName, L'"'))
        return FALSE;

    GetModuleFileNameW(NULL, szDir, _countof(szDir));
    PathRemoveFileSpecW(szDir);
    strPath = szDir;
    if (NeedCurrentDirectoryForExePathW(pszName))
        strPath += L";.";
    if (GetSystemDirectoryW(szDir, _countof(szDir)))
        strPath += CStringW(L";") + szDir;
    if (GetWindowsDirectoryW(szDir, _countof(szDir)))
        strPath += CStringW(L";") + szDir + L"\\system;" + szDir;
    cch = GetEnvironmentVariableW(L"PATH", NULL, 0);
    if (cch)
    {
        CStringW strEnv;
        if (GetEnvironmentVariableW(L"PATH", strEnv.GetBuffer(cch), cch) < cch)
        {
            strEnv.ReleaseBuffer();
            strPath += L";" + strEnv;
        }
        else
        {
            strEnv.ReleaseBuffer(0);
        }
    }

    cch = SearchPathW(strPath, pszName, L".exe", cchFile, pszFile, NULL);
    if (!cch || cch >= cchFile)
        return FALSE;
    dwAttr = GetFileAttributesW(pszFile);
    return dwAttr != INVALID_FILE_ATTRIBUTES && !(dwAttr & FILE_ATTRIBUTE_DIRECTORY);
}

/* A REG_SZ or REG_EXPAND_SZ value, terminated and expanded here (RegGetValueW does neither safely) */
static BOOL
ReadToolString(HKEY hKey, LPCWSTR pszSubKey, LPCWSTR pszValue, CStringW &str)
{
    CStringW strRaw;
    HKEY hSubKey = hKey;
    DWORD dwType, cb = 0, cch;
    LPWSTR psz;
    BOOL bRead = FALSE;

    if (pszSubKey && RegOpenKeyExW(hKey, pszSubKey, 0, KEY_QUERY_VALUE, &hSubKey) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(hSubKey, pszValue, NULL, &dwType, NULL, &cb) == ERROR_SUCCESS &&
        (dwType == REG_SZ || dwType == REG_EXPAND_SZ) && cb <= 0x10000)
    {
        cch = cb / sizeof(WCHAR);
        psz = strRaw.GetBuffer(cch + 1);
        bRead = (RegQueryValueExW(hSubKey, pszValue, NULL, &dwType, (LPBYTE)psz, &cb) == ERROR_SUCCESS &&
                 (dwType == REG_SZ || dwType == REG_EXPAND_SZ) && cb <= cch * sizeof(WCHAR));
        psz[bRead ? cb / sizeof(WCHAR) : 0] = UNICODE_NULL;
        strRaw.ReleaseBuffer();
    }
    if (pszSubKey)
        RegCloseKey(hSubKey);
    if (!bRead)
        return FALSE;
    if (dwType == REG_SZ)
    {
        str = strRaw;
        return TRUE;
    }
    cch = ExpandEnvironmentStringsW(strRaw, NULL, 0);
    if (!cch)
        return FALSE;
    psz = str.GetBuffer(cch);
    bRead = (ExpandEnvironmentStringsW(strRaw, psz, cch) - 1 < cch);
    str.ReleaseBuffer(bRead ? -1 : 0);
    return bRead;
}

/* The App Paths entry of a bare program name ("name" or "name.exe"): its target and its Path value */
static BOOL
FindToolAppPath(LPCWSTR pszName, LPWSTR pszTarget, DWORD cchTarget, CStringW &strAppPath)
{
    WCHAR szKey[MAX_PATH + 64];
    CStringW strTarget;
    HKEY hKey;
    BOOL bFound;

    if (!*pszName || !PathIsFileSpecW(pszName) || StrChrW(pszName, L'"') ||
        FAILED(StringCchPrintfW(szKey, _countof(szKey),
                                L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%s", pszName)))
    {
        return FALSE;
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, szKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS &&
        (!PathAddExtensionW(szKey, L".exe") ||
         RegOpenKeyExW(HKEY_LOCAL_MACHINE, szKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS))
    {
        return FALSE;
    }
    bFound = ReadToolString(hKey, NULL, NULL, strTarget) &&
             SUCCEEDED(StringCchCopyW(pszTarget, cchTarget, strTarget));
    if (bFound)
    {
        PathUnquoteSpacesW(pszTarget);
        bFound = (*pszTarget != UNICODE_NULL);
    }
    if (bFound && !ReadToolString(hKey, NULL, L"Path", strAppPath))
        strAppPath.Empty();
    RegCloseKey(hKey);
    return bFound;
}

/* This process's environment with pszAppPath put in front of PATH, as ShellExecuteEx does for App Paths */
static LPWSTR
BuildToolEnvironment(LPCWSTR pszAppPath)
{
    LPWCH pEnv = GetEnvironmentStringsW();
    LPCWSTR pszVar;
    SIZE_T cchEnv = 0, cchNew;
    LPWSTR pNew, pszOut;
    BOOL bPath = FALSE;

    if (!pEnv)
        return NULL;
    for (pszVar = pEnv; *pszVar; pszVar += wcslen(pszVar) + 1)
        ;
    cchEnv = pszVar - pEnv;
    cchNew = cchEnv + wcslen(pszAppPath) + 8;
    pNew = (LPWSTR)LocalAlloc(LMEM_FIXED, cchNew * sizeof(WCHAR));
    if (pNew)
    {
        pszOut = pNew;
        for (pszVar = pEnv; *pszVar; pszVar += wcslen(pszVar) + 1)
        {
            if (!bPath && !_wcsnicmp(pszVar, L"PATH=", 5))
            {
                bPath = TRUE;
                StringCchPrintfW(pszOut, cchNew - (pszOut - pNew), L"PATH=%s;%s", pszAppPath, pszVar + 5);
            }
            else
            {
                StringCchCopyW(pszOut, cchNew - (pszOut - pNew), pszVar);
            }
            pszOut += wcslen(pszOut) + 1;
        }
        if (!bPath)
        {
            StringCchPrintfW(pszOut, cchNew - (pszOut - pNew), L"PATH=%s", pszAppPath);
            pszOut += wcslen(pszOut) + 1;
        }
        *pszOut = UNICODE_NULL;
    }
    FreeEnvironmentStringsW(pEnv);
    return pNew;
}

/* A found program starts through CreateProcess, so the shell cannot fall back to another target;
 * anything else (a document, or a name only the shell resolves) goes through ShellExecuteEx */
static DWORD
StartDriveTool(HWND hwndDlg, LPCWSTR pszFile, LPCWSTR pszArgs, BOOL bFound, LPCWSTR pszAppPath)
{
    LPCWSTR pszExt = PathFindExtensionW(pszFile);
    DWORD dwError = ERROR_SUCCESS;

    if (bFound && (!_wcsicmp(pszExt, L".exe") || !_wcsicmp(pszExt, L".com")))
    {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        CStringW strCmd;
        LPWSTR pEnv = (pszAppPath && *pszAppPath) ? BuildToolEnvironment(pszAppPath) : NULL;

        strCmd.Format(L"\"%s\"", pszFile);
        if (pszArgs && *pszArgs)
            strCmd += CStringW(L" ") + pszArgs;
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNORMAL;
        if (CreateProcessW(pszFile, strCmd.GetBuffer(), NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT,
                           pEnv, NULL, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        else
        {
            dwError = GetLastError();
        }
        if (pEnv)
            LocalFree(pEnv);
        return dwError;
    }

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = hwndDlg;
    sei.lpFile = pszFile;
    sei.lpParameters = (pszArgs && *pszArgs) ? pszArgs : NULL;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
        dwError = GetLastError();
    if (sei.hProcess)
        CloseHandle(sei.hProcess);
    return dwError;
}

static VOID
ShowDriveToolError(HWND hwndDlg, UINT idUnavailable, DWORD dwError, LPCWSTR pszProgram)
{
    WCHAR wszText[512], wszReason[256], wszTitle[MAX_PATH];
    DWORD_PTR Args[3] = { (DWORD_PTR)pszProgram, 0, 0 };

    if (!LoadStringW(shell32_hInstance, idUnavailable, wszText, _countof(wszText)))
        return;
    /* A missing program needs no reason; any other launch error is shown as it is */
    if (dwError != ERROR_FILE_NOT_FOUND && dwError != ERROR_PATH_NOT_FOUND &&
        dwError != ERROR_INVALID_NAME && dwError != ERROR_BAD_PATHNAME &&
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ARGUMENT_ARRAY, NULL, dwError, 0,
                       wszReason, _countof(wszReason), (va_list*)Args))
    {
        StringCchCatW(wszText, _countof(wszText), L"\n\n");
        StringCchCatW(wszText, _countof(wszText), wszReason);
    }
    GetWindowTextW(GetParent(hwndDlg), wszTitle, _countof(wszTitle));
    MessageBoxW(hwndDlg, wszText, wszTitle, MB_OK | MB_ICONWARNING);
}

static VOID
RunDriveTool(HWND hwndDlg, LPCWSTR pwszValueName, WCHAR wchDrive, UINT idUnavailable)
{
    WCHAR wszKey[MAX_PATH], wszCmd[MAX_PATH] = L"", wszProgram[MAX_PATH] = L"";
    WCHAR wszName[MAX_PATH], wszFile[MAX_PATH];
    DWORD dwError = ERROR_FILE_NOT_FOUND;
    BOOL bQuoted = FALSE, bFound = FALSE;
    LPWSTR psz, pszArgs;
    CStringW strValue, strAppPath;

    StringCchPrintfW(wszKey, _countof(wszKey),
                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\MyComputer\\%s", pwszValueName);
    if (ReadToolString(HKEY_LOCAL_MACHINE, wszKey, NULL, strValue) &&
        SUCCEEDED(StringCchCopyW(wszCmd, _countof(wszCmd), strValue)))
    {
        /* The command line may name the drive as %c */
        if ((psz = StrStrW(wszCmd, L"%c")) != NULL)
        {
            psz[0] = wchDrive;
            MoveMemory(&psz[1], &psz[2], (wcslen(&psz[2]) + 1) * sizeof(WCHAR));
        }

        /* A tab outside quotes separates like a space */
        for (psz = wszCmd; *psz; ++psz)
        {
            if (*psz == L'"')
                bQuoted = !bQuoted;
            else if (*psz == L'\t' && !bQuoted)
                *psz = L' ';
        }
        StrTrimW(wszCmd, L" ");
        if (!wszCmd[0])
            goto Fail;

        /*
         * Decide what the value names before anything starts, as Windows does: the program is the quoted
         * name or the first word, through its App Paths entry when it is a bare name that has one; else,
         * as in CreateProcess, the quoted name or the shortest part up to a space that names an existing
         * file. When nothing is found, the name (the first word when unquoted) is given to the shell.
         */
        if (wszCmd[0] == L'"')
        {
            pszArgs = PathGetArgsW(wszCmd);
            StringCchCopyW(wszProgram, _countof(wszProgram), wszCmd);
            PathRemoveArgsW(wszProgram);
            PathUnquoteSpacesW(wszProgram);
            bFound = FindToolAppPath(wszProgram, wszFile, _countof(wszFile), strAppPath) ||
                     FindToolProgram(wszProgram, wszFile, _countof(wszFile));
            if (bFound)
                StringCchCopyW(wszProgram, _countof(wszProgram), wszFile);
        }
        else
        {
            if (!(pszArgs = StrChrW(wszCmd, L' ')))
                pszArgs = wszCmd + wcslen(wszCmd);
            StringCchCopyNW(wszProgram, _countof(wszProgram), wszCmd, pszArgs - wszCmd);
            bFound = FindToolAppPath(wszProgram, wszFile, _countof(wszFile), strAppPath);
            for (psz = wszCmd; !bFound; ++psz)
            {
                if (!(psz = StrChrW(psz, L' ')))
                    psz = wszCmd + wcslen(wszCmd);
                StringCchCopyNW(wszName, _countof(wszName), wszCmd, psz - wszCmd);
                bFound = FindToolProgram(wszName, wszFile, _countof(wszFile));
                if (bFound)
                    pszArgs = psz;
                else if (!*psz)
                    break;
            }
            if (bFound)
                StringCchCopyW(wszProgram, _countof(wszProgram), wszFile);
        }
        while (*pszArgs == L' ')
            ++pszArgs;

        /* Started once: a failure of the program it names is reported, never retried as another program */
        dwError = StartDriveTool(hwndDlg, wszProgram, pszArgs, bFound, strAppPath);
        if (dwError == ERROR_SUCCESS)
            return;
        ERR("Failed to start %ls (error %lu)\n", wszProgram, dwError);
    }

Fail:
    ShowDriveToolError(hwndDlg, idUnavailable, dwError, wszProgram);
}

INT_PTR CALLBACK
CDrvDefExt::ExtraPageProc(
    HWND hwndDlg,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            LPPROPSHEETPAGEW ppsp = (LPPROPSHEETPAGEW)lParam;
            SetWindowLongPtr(hwndDlg, DWLP_USER, (LONG_PTR)ppsp->lParam);
            return TRUE;
        }
        case WM_COMMAND:
        {
            CDrvDefExt *pDrvDefExt = reinterpret_cast<CDrvDefExt *>(GetWindowLongPtr(hwndDlg, DWLP_USER));

            switch(LOWORD(wParam))
            {
                case 14000:
                    DialogBoxParamW(shell32_hInstance, MAKEINTRESOURCEW(IDD_CHECK_DISK), hwndDlg, ChkDskDlg, (LPARAM)pDrvDefExt->m_wszDrive);
                    break;
                case 14001:
                    RunDriveTool(hwndDlg, L"DefragPath", pDrvDefExt->m_wszDrive[0], IDS_DEFRAG_UNAVAILABLE);
                    break;
                case 14002:
                    RunDriveTool(hwndDlg, L"BackupPath", pDrvDefExt->m_wszDrive[0], IDS_BACKUP_UNAVAILABLE);
                    break;
            }
            break;
        }
    }
    return FALSE;
}

INT_PTR CALLBACK
CDrvDefExt::HardwarePageProc(
    HWND hwndDlg,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    UNREFERENCED_PARAMETER(wParam);

    switch(uMsg)
    {
        case WM_INITDIALOG:
        {
            GUID Guids[2];
            Guids[0] = GUID_DEVCLASS_DISKDRIVE;
            Guids[1] = GUID_DEVCLASS_CDROM;

            /* create the hardware page */
            DeviceCreateHardwarePageEx(hwndDlg, Guids, _countof(Guids), HWPD_STANDARDLIST);
            break;
        }
    }

    return FALSE;
}

CDrvDefExt::CDrvDefExt()
{
    m_wszDrive[0] = L'\0';
}

CDrvDefExt::~CDrvDefExt()
{

}

struct CDrop
{
    HRESULT hr;
    STGMEDIUM stgm;
    HDROP hDrop;

    explicit CDrop(IDataObject *pDO)
    {
        FORMATETC format = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        hDrop = SUCCEEDED(hr = pDO->GetData(&format, &stgm)) ? (HDROP)stgm.hGlobal : NULL;
    }

    ~CDrop()
    {
        if (hDrop)
            ReleaseStgMedium(&stgm);
    }

    UINT GetCount()
    {
        return DragQueryFileW(hDrop, -1, NULL, 0);
    }
};

static inline bool
IsValidDrivePath(PCWSTR Path)
{
    return GetDriveTypeW(Path) > DRIVE_NO_ROOT_DIR;
}

HRESULT WINAPI
CDrvDefExt::Initialize(PCIDLIST_ABSOLUTE pidlFolder, IDataObject *pDataObj, HKEY hkeyProgID)
{
    HRESULT hr;

    TRACE("%p %p %p %p\n", this, pidlFolder, pDataObj, hkeyProgID);

    if (!pDataObj)
        return E_FAIL;


    CDrop drop(pDataObj);
    if (FAILED_UNEXPECTEDLY(hr = drop.hr))
        return hr;

    if (!DragQueryFileW(drop.hDrop, 0, m_wszDrive, _countof(m_wszDrive)))
    {
        ERR("DragQueryFileW failed\n");
        return E_FAIL;
    }

    if (drop.GetCount() > 1)
        m_Multiple = pDataObj;

    TRACE("Drive properties %ls\n", m_wszDrive);

    return S_OK;
}

HRESULT WINAPI
CDrvDefExt::QueryContextMenu(HMENU hmenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI
CDrvDefExt::InvokeCommand(LPCMINVOKECOMMANDINFO lpici)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI
CDrvDefExt::GetCommandString(UINT_PTR idCmd, UINT uType, UINT *pwReserved, LPSTR pszName, UINT cchMax)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT
CDrvDefExt::AddMainPage(LPFNADDPROPSHEETPAGE pfnAddPage, LPARAM lParam)
{
    WCHAR szTitle[MAX_PATH], *pszTitle = NULL;
    if (m_Multiple)
    {
        CComHeapPtr<ITEMIDLIST_ABSOLUTE> pidl(SHSimpleIDListFromPathW(m_wszDrive));
        if (SUCCEEDED(SHGetNameAndFlagsW(pidl, SHGDN_INFOLDER, szTitle, _countof(szTitle), NULL)))
            pszTitle = szTitle;
    }

    HPROPSHEETPAGE hPage;
    hPage = SH_CreatePropertySheetPageEx(IDD_DRIVE_PROPERTIES, GeneralPageProc, (LPARAM)this,
                                         pszTitle, &PropSheetPageLifetimeCallback<CDrvDefExt>);
    HRESULT hr = AddPropSheetPage(hPage, pfnAddPage, lParam);
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;
    else
        AddRef(); // For PropSheetPageLifetimeCallback
    return hr;
}

HRESULT WINAPI
CDrvDefExt::AddPages(LPFNADDPROPSHEETPAGE pfnAddPage, LPARAM lParam)
{
    HRESULT hr = AddMainPage(pfnAddPage, lParam);
    if (FAILED_UNEXPECTEDLY(hr))
        return hr;

    if (m_Multiple)
    {
        CDrop drop(m_Multiple);
        UINT count = SUCCEEDED(drop.hr) ? drop.GetCount() : 0;
        for (UINT i = 0; ++i < count;) // Skipping the first drive since it already has a page
        {
            CComPtr<CDrvDefExt> SheetExt;
            if (FAILED_UNEXPECTEDLY(hr = ShellObjectCreator(SheetExt)))
                continue;
            if (!DragQueryFileW(drop.hDrop, i, SheetExt->m_wszDrive, _countof(SheetExt->m_wszDrive)))
                continue;
            if (!IsValidDrivePath(SheetExt->m_wszDrive))
                continue;

            SheetExt->m_Multiple = m_Multiple;
            SheetExt->AddMainPage(pfnAddPage, lParam);
        }
    }
    else
    {
        HPROPSHEETPAGE hPage;
        if (GetDriveTypeW(m_wszDrive) == DRIVE_FIXED)
        {
            hPage = SH_CreatePropertySheetPage(IDD_DRIVE_TOOLS,
                                               ExtraPageProc,
                                               (LPARAM)this,
                                               NULL);
            if (hPage)
                pfnAddPage(hPage, lParam);
        }

        if (GetDriveTypeW(m_wszDrive) != DRIVE_REMOTE)
        {
            hPage = SH_CreatePropertySheetPage(IDD_DRIVE_HARDWARE,
                                               HardwarePageProc,
                                               (LPARAM)this,
                                               NULL);
            if (hPage)
                pfnAddPage(hPage, lParam);
        }
    }

    return S_OK;
}

HRESULT WINAPI
CDrvDefExt::ReplacePage(UINT uPageID, LPFNADDPROPSHEETPAGE pfnReplacePage, LPARAM lParam)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI
CDrvDefExt::SetSite(IUnknown *punk)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

HRESULT WINAPI
CDrvDefExt::GetSite(REFIID iid, void **ppvSite)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}
