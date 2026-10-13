/*
 * PROJECT:     Disk Management
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shared declarations of the program
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#ifndef _DISKMGMT_H_
#define _DISKMGMT_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <winreg.h>
#include <winioctl.h>
#include <commctrl.h>
#include <prsht.h>
#include <shellapi.h>
#include <strsafe.h>

#include "diskmodel.h"
#include "resource.h"

/* The part of the model the user picked: a disk, or a block of a disk. */
typedef struct _DM_SELECTION
{
    BOOL Valid;
    DM_DEVICE Device;
    ULONG Number;
    BOOL HasRegion;
    ULONGLONG Offset;
} DM_SELECTION, *PDM_SELECTION;

/* messages from the disk-map control to its parent (WM_APP range) */
#define DMM_SELCHANGE   (WM_APP + 1)
#define DMM_ACTIVATE    (WM_APP + 2)
#define DMM_CONTEXT     (WM_APP + 3)    /* lParam: MAKELPARAM(x, y) in screen coordinates */

extern HINSTANCE g_hInst;
extern HFONT g_hFont;
extern HFONT g_hFontBold;

/* graphview.c */
BOOL GraphRegisterClass(HINSTANCE hInstance);
HWND GraphCreate(HWND hParent, UINT Id);
VOID GraphSetModel(HWND hWnd, PDM_MODEL Model);
VOID GraphSetSelection(HWND hWnd, const DM_SELECTION *Selection);
BOOL GraphGetSelection(HWND hWnd, PDM_SELECTION Selection);
VOID GraphGetSelectedPoint(HWND hWnd, LPPOINT Point);

/* props.c */
VOID ShowRegionProperties(HWND hOwner, const DM_DISK *Disk, const DM_REGION *Region);
VOID ShowDiskProperties(HWND hOwner, const DM_DISK *Disk);

/* diskmgmt.c */
BOOL LoadStringBuffer(UINT Id, PWSTR Buffer, int Count);

#endif
