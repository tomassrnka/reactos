/*
 * PROJECT:     ReactOS user32 - Windows 8 and later input and window APIs
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Interface types that the public headers of this tree only declare for WINVER >= 0x0602
 */

#ifndef _USER32_VISTA_WIN8USER_H
#define _USER32_VISTA_WIN8USER_H

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>

#define POINTER_DEVICE_PRODUCT_STRING_MAX 520
#define PT_POINTER  1
#define PT_TOUCH    2
#define PT_PEN      3
#define PT_MOUSE    4
#define PT_TOUCHPAD 5

typedef DWORD POINTER_INPUT_TYPE;
typedef UINT32 POINTER_FLAGS;

typedef struct tagWIN8_POINTER_INFO
{
    POINTER_INPUT_TYPE pointerType;
    UINT32 pointerId;
    UINT32 frameId;
    POINTER_FLAGS pointerFlags;
    HANDLE sourceDevice;
    HWND hwndTarget;
    POINT ptPixelLocation;
    POINT ptHimetricLocation;
    POINT ptPixelLocationRaw;
    POINT ptHimetricLocationRaw;
    DWORD dwTime;
    UINT32 historyCount;
    INT32 InputData;
    DWORD dwKeyStates;
    UINT64 PerformanceCount;
    DWORD ButtonChangeType;
} WIN8_POINTER_INFO;

typedef struct tagWIN8_POINTER_TOUCH_INFO
{
    WIN8_POINTER_INFO pointerInfo;
    UINT32 touchFlags;
    UINT32 touchMask;
    RECT rcContact;
    RECT rcContactRaw;
    UINT32 orientation;
    UINT32 pressure;
} WIN8_POINTER_TOUCH_INFO;

typedef struct tagWIN8_POINTER_PEN_INFO
{
    WIN8_POINTER_INFO pointerInfo;
    UINT32 penFlags;
    UINT32 penMask;
    UINT32 pressure;
    UINT32 rotation;
    INT32 tiltX;
    INT32 tiltY;
} WIN8_POINTER_PEN_INFO;

typedef struct tagWIN8_POINTER_DEVICE_INFO
{
    DWORD displayOrientation;
    HANDLE device;
    DWORD pointerDeviceType;
    HMONITOR monitor;
    ULONG startingCursorId;
    USHORT maxActiveContacts;
    WCHAR productString[POINTER_DEVICE_PRODUCT_STRING_MAX];
} WIN8_POINTER_DEVICE_INFO;

typedef struct tagWIN8_POINTER_DEVICE_PROPERTY
{
    INT32 logicalMin;
    INT32 logicalMax;
    INT32 physicalMin;
    INT32 physicalMax;
    UINT32 unit;
    UINT32 unitExponent;
    USHORT usagePageId;
    USHORT usageId;
} WIN8_POINTER_DEVICE_PROPERTY;

typedef struct tagWIN8_POINTER_DEVICE_CURSOR_INFO
{
    UINT32 cursorId;
    DWORD cursor;
} WIN8_POINTER_DEVICE_CURSOR_INFO;

typedef struct tagWIN8_INPUT_TRANSFORM
{
    float m[16];
} WIN8_INPUT_TRANSFORM;

typedef struct tagWIN8_TOUCH_HIT_TESTING_INPUT
{
    UINT32 pointerId;
    POINT point;
    RECT boundingBox;
    RECT nonOccludedBoundingBox;
    UINT32 orientation;
} WIN8_TOUCH_HIT_TESTING_INPUT;

typedef struct tagWIN8_TOUCH_HIT_TESTING_PROXIMITY_EVALUATION
{
    UINT16 score;
    POINT adjustedPoint;
} WIN8_TOUCH_HIT_TESTING_PROXIMITY_EVALUATION;

typedef struct tagWIN8_INPUT_MESSAGE_SOURCE
{
    DWORD deviceType;
    DWORD originId;
} WIN8_INPUT_MESSAGE_SOURCE;

#endif /* _USER32_VISTA_WIN8USER_H */
