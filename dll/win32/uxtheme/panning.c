/*
 * PROJECT:     ReactOS uxtheme
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Boundary feedback for panning gestures
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 */

#include "uxthemep.h"

/*
 * Boundary feedback is a transient visual effect: the window appears to move with an overpan and to
 * spring back when the gesture ends, without its placement changing. The window manager here has no
 * such transform (moving the real window would send ordinary position changes to the application),
 * so these functions fail with ERROR_NOT_SUPPORTED after checking the window. A caller then simply
 * shows no feedback.
 */

static
BOOL
PanningFeedbackNotSupported(
    _In_ HWND hwnd)
{
    if (!IsWindow(hwnd))
    {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
BeginPanningFeedback(
    _In_ HWND hwnd)
{
    return PanningFeedbackNotSupported(hwnd);
}

/*
 * @unimplemented
 */
BOOL
WINAPI
UpdatePanningFeedback(
    _In_ HWND hwnd,
    _In_ LONG lTotalOverpanOffsetX,
    _In_ LONG lTotalOverpanOffsetY,
    _In_ BOOL fInInertia)
{
    return PanningFeedbackNotSupported(hwnd);
}

/*
 * @unimplemented
 */
BOOL
WINAPI
EndPanningFeedback(
    _In_ HWND hwnd,
    _In_ BOOL fAnimateBack)
{
    return PanningFeedbackNotSupported(hwnd);
}
