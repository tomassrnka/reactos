/*
 * PROJECT:     ReactOS user32 - Windows 8 and later window and input helpers
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Input message source, touch hit testing, pointer injection and input targets,
 *              display auto rotation, suspend and resume notification, coalescable timers,
 *              window band and window feedback settings
 * COPYRIGHT:   Copyright 2026 Tomas Srnka
 *              Error codes, parameter order and value ranges were measured on Windows 10
 *              (build 19045); the stubs of the Wine project for the same names (dlls/user32,
 *              Copyright the Wine project) were a reference for the interfaces.
 *
 * Everything that needs the window manager to know about the state (pointer messages, input
 * sources, window arrangement, injection of touch and pen input) is not available here; those
 * functions validate their parameters like Windows and then report the missing support.
 */

#include "win8user.h"
#include <pseh/pseh2.h>

#define NDEBUG
#include <debug.h>

UINT_PTR NTAPI NtUserSetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, TIMERPROC lpTimerFunc);

#define TIMER_TOLERANCE_NONE      0xFFFFFFFF
#define TIMER_TOLERANCE_MAX       0x7FFFFFF5
#define ORIENTATION_PREFERENCE_MASK 0x0F
#define ZBID_DEFAULT              1
#define FEEDBACK_TYPE_COUNT       13
#define PT_TOUCH_TYPE             2
#define PT_PEN_TYPE               3
#define MAX_TOUCH_CONTACTS        256
#define MAX_PEN_CONTACTS          1
#define GWFS_INCLUDE_ANCESTORS    1
#define SUSPEND_RESUME_TAG        0x53524e57  /* 'WNRS' */

/* Feedback settings: two bits per feedback type in one window property, present and value */
static const WCHAR s_FeedbackPropName[] = L"User32Win8Feedback";
static const WCHAR s_PointerNotifyPropName[] = L"User32Win8PointerNotify";

static volatile LONG s_StateLock;

static volatile LONG s_OrientationPreference = 0;

typedef struct _SUSPEND_RESUME_REGISTRATION
{
    ULONG Tag;
    HANDLE Recipient;
    DWORD Flags;
} SUSPEND_RESUME_REGISTRATION;

static BOOL
Fail(DWORD Error)
{
    SetLastError(Error);
    return FALSE;
}

/*
 * Window properties are kept under atoms that are added once; SetPropW cannot be trusted when the atom
 * table is full, as it then stores the property under atom zero. The process holds one reference of its own
 * on each atom for as long as user32 is loaded (a lookup would leave the atom to whoever else added it, and
 * it could be deleted and reused for something else), and gives it back when user32 is unloaded.
 */
static volatile LONG s_FeedbackAtom;
static volatile LONG s_PointerNotifyAtom;

static BOOL
EnsureAtom(volatile LONG *Atom, LPCWSTR Name)
{
    if (!*Atom)
    {
        DWORD Error = GetLastError();
        ATOM Added = GlobalAddAtomW(Name);

        if (!Added)
            return FALSE;

        /* Two threads can come here at the same time: one reference is kept */
        if (InterlockedCompareExchange(Atom, Added, 0) != 0)
            GlobalDeleteAtom(Added);
        SetLastError(Error);
    }
    return TRUE;
}

/* The references taken above are given back */
VOID
Win8UserProcessDetach(VOID)
{
    LONG Atom;

    Atom = InterlockedExchange(&s_FeedbackAtom, 0);
    if (Atom)
        GlobalDeleteAtom((ATOM)Atom);
    Atom = InterlockedExchange(&s_PointerNotifyAtom, 0);
    if (Atom)
        GlobalDeleteAtom((ATOM)Atom);
}

/* GetPropW leaves ERROR_FILE_NOT_FOUND behind when the property does not exist; Windows leaves the last error alone. */
static HANDLE
GetPropQuiet(HWND hwnd, ATOM Atom)
{
    DWORD Error = GetLastError();
    HANDLE Value = GetPropW(hwnd, ((LPCWSTR)(ULONG_PTR)(WORD)(Atom)));

    SetLastError(Error);
    return Value;
}

/* Keeps the last error when it succeeds; a failure leaves the error of SetPropW. */
static BOOL
SetPropQuiet(HWND hwnd, ATOM Atom, HANDLE Value)
{
    DWORD Error = GetLastError();
    BOOL Ret = SetPropW(hwnd, ((LPCWSTR)(ULONG_PTR)(WORD)(Atom)), Value);

    if (Ret)
        SetLastError(Error);
    return Ret;
}

/* The state kept in properties belongs to the process that owns the window; Windows refuses the others. */
static BOOL
IsWindowOfThisProcess(HWND hwnd)
{
    DWORD ProcessId = 0;

    GetWindowThreadProcessId(hwnd, &ProcessId);
    return ProcessId == GetCurrentProcessId();
}

static VOID
LockState(VOID)
{
    while (InterlockedCompareExchange(&s_StateLock, 1, 0))
        Sleep(0);
}

static VOID
UnlockState(VOID)
{
    InterlockedExchange(&s_StateLock, 0);
}

/*
 * Input message source. ReactOS records no source per input message, so every message reports
 * what Windows reports for a message that is no input message: no device and no origin.
 */
BOOL
WINAPI
GetCurrentInputMessageSource(_Out_ WIN8_INPUT_MESSAGE_SOURCE *inputMessageSource)
{
    if (!inputMessageSource)
        return Fail(ERROR_INVALID_PARAMETER);

    if (IsBadWritePtr(inputMessageSource, sizeof(*inputMessageSource)))
        return Fail(ERROR_NOACCESS);

    inputMessageSource->deviceType = 0;
    inputMessageSource->originId = 0;
    return TRUE;
}

BOOL
WINAPI
GetCIMSSM(_Out_ WIN8_INPUT_MESSAGE_SOURCE *inputMessageSource)
{
    return GetCurrentInputMessageSource(inputMessageSource);
}

/* Touch hit testing */

BOOL
WINAPI
RegisterTouchHitTestingWindow(_In_ HWND hwnd, _In_ ULONG value)
{
    UNREFERENCED_PARAMETER(value);
    if (!IsWindow(hwnd))
        return Fail(ERROR_INVALID_WINDOW_HANDLE);
    if (!IsWindowOfThisProcess(hwnd))
        return Fail(ERROR_ACCESS_DENIED);
    return TRUE;
}

static BOOL
PointInRect(const RECT *Rect, LONG X, LONG Y)
{
    return X >= Rect->left && X < Rect->right && Y >= Rect->top && Y < Rect->bottom;
}

static BOOL
PointOnSegment(const POINT *A, const POINT *B, LONG X, LONG Y)
{
    LONGLONG Cross = (LONGLONG)(B->x - A->x) * (Y - A->y) - (LONGLONG)(X - A->x) * (B->y - A->y);

    return Cross == 0 &&
           X >= min(A->x, B->x) && X <= max(A->x, B->x) &&
           Y >= min(A->y, B->y) && Y <= max(A->y, B->y);
}

/* Even-odd rule; a point on an edge counts as inside. */
static BOOL
PointInPolygon(UINT32 Count, const POINT *Vertices, LONG X, LONG Y)
{
    UINT32 i, j;
    BOOL Inside = FALSE;

    for (i = 0, j = Count - 1; i < Count; j = i++)
    {
        const POINT *Vi = &Vertices[i], *Vj = &Vertices[j];
        LONGLONG Diff;

        if (PointOnSegment(Vi, Vj, X, Y))
            return TRUE;

        if ((Vi->y > Y) == (Vj->y > Y))
            continue;

        Diff = (LONGLONG)(X - Vi->x) * (Vj->y - Vi->y) - (LONGLONG)(Vj->x - Vi->x) * (Y - Vi->y);
        if (Vj->y > Vi->y ? Diff < 0 : Diff > 0)
            Inside = !Inside;
    }
    return Inside;
}

/* Windows scores a clear miss with this value; scores between a hit and a clear miss are not modelled. */
#define PROXIMITY_SCORE_MISS 4094

static VOID
EvaluateProximityResult(
    BOOL Hit,
    const WIN8_TOUCH_HIT_TESTING_INPUT *Input,
    WIN8_TOUCH_HIT_TESTING_PROXIMITY_EVALUATION *Evaluation)
{
    Evaluation->score = Hit ? 0 : PROXIMITY_SCORE_MISS;
    Evaluation->adjustedPoint = Input->point;
}

BOOL
WINAPI
EvaluateProximityToRect(
    _In_ const RECT *boundingBox,
    _In_ const WIN8_TOUCH_HIT_TESTING_INPUT *pHitTestingInput,
    _Out_ WIN8_TOUCH_HIT_TESTING_PROXIMITY_EVALUATION *pProximityEval)
{
    if (!boundingBox || !pHitTestingInput || !pProximityEval ||
        pHitTestingInput->boundingBox.left >= pHitTestingInput->boundingBox.right ||
        pHitTestingInput->boundingBox.top >= pHitTestingInput->boundingBox.bottom)
        return Fail(ERROR_BAD_ARGUMENTS);

    EvaluateProximityResult(PointInRect(boundingBox, pHitTestingInput->point.x, pHitTestingInput->point.y),
                            pHitTestingInput, pProximityEval);
    return TRUE;
}

BOOL
WINAPI
EvaluateProximityToPolygon(
    _In_ UINT32 numVertices,
    _In_ const POINT *controlPolygon,
    _In_ const WIN8_TOUCH_HIT_TESTING_INPUT *pHitTestingInput,
    _Out_ WIN8_TOUCH_HIT_TESTING_PROXIMITY_EVALUATION *pProximityEval)
{
    if (numVertices < 3 || !controlPolygon || !pHitTestingInput || !pProximityEval ||
        pHitTestingInput->boundingBox.left >= pHitTestingInput->boundingBox.right ||
        pHitTestingInput->boundingBox.top >= pHitTestingInput->boundingBox.bottom)
        return Fail(ERROR_BAD_ARGUMENTS);

    EvaluateProximityResult(PointInPolygon(numVertices, controlPolygon, pHitTestingInput->point.x,
                                           pHitTestingInput->point.y),
                            pHitTestingInput, pProximityEval);
    return TRUE;
}

/*
 * The packed value holds the score in bits 31..20 and the offset of the adjusted point from the
 * touch point as two signed 10-bit numbers, x in bits 19..10 and y in bits 9..0. A score above
 * 0xFFF, or an offset beyond 510 in either direction, gives the score 0xFFF without offset; that
 * value is not sign extended on 64-bit Windows, a packed value is.
 */
LRESULT
WINAPI
PackTouchHitTestingProximityEvaluation(
    _In_ const WIN8_TOUCH_HIT_TESTING_INPUT *pHitTestingInput,
    _In_ const WIN8_TOUCH_HIT_TESTING_PROXIMITY_EVALUATION *pProximityEval)
{
    LONG Dx, Dy;
    ULONG Score = pProximityEval->score;

    if (Score > 0xFFF)
        return (LRESULT)(DWORD)0xFFF00000;

    Dx = pProximityEval->adjustedPoint.x - pHitTestingInput->point.x;
    Dy = pProximityEval->adjustedPoint.y - pHitTestingInput->point.y;
    if (Dx < -510 || Dx > 510 || Dy < -510 || Dy > 510)
        return (LRESULT)(DWORD)0xFFF00000;

    return (LRESULT)(LONG)((Score << 20) | (((ULONG)Dx & 0x3FF) << 10) | ((ULONG)Dy & 0x3FF));
}

/* Synthetic pointer devices, touch injection and pointer input targets */

static BOOL
IsInjectionTypeValid(DWORD pointerType, ULONG maxCount)
{
    if (pointerType == PT_TOUCH_TYPE)
        return maxCount >= 1 && maxCount <= MAX_TOUCH_CONTACTS;
    if (pointerType == PT_PEN_TYPE)
        return maxCount >= 1 && maxCount <= MAX_PEN_CONTACTS;
    return FALSE;
}

HANDLE
WINAPI
CreateSyntheticPointerDevice(
    _In_ DWORD pointerType,
    _In_ ULONG maxCount,
    _In_ DWORD mode)
{
    if (!IsInjectionTypeValid(pointerType, maxCount) || mode < 1 || mode > 3)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return NULL;
}

VOID
WINAPI
DestroySyntheticPointerDevice(_In_ HANDLE device)
{
    UNREFERENCED_PARAMETER(device);
}

BOOL
WINAPI
InjectSyntheticPointerInput(_In_ HANDLE device, _In_ const VOID *pointerInfo, _In_ UINT32 count)
{
    UNREFERENCED_PARAMETER(pointerInfo);
    UNREFERENCED_PARAMETER(count);
    return Fail(device ? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER);
}

BOOL
WINAPI
InitializeTouchInjection(_In_ UINT32 maxCount, _In_ DWORD dwMode)
{
    if (maxCount < 1 || maxCount > MAX_TOUCH_CONTACTS || dwMode < 1 || dwMode > 3)
        return Fail(ERROR_INVALID_PARAMETER);

    return Fail(ERROR_CALL_NOT_IMPLEMENTED);
}

BOOL
WINAPI
InjectTouchInput(_In_ UINT32 count, _In_ const VOID *contacts)
{
    if (count == 0)
        return Fail(ERROR_INVALID_PARAMETER);

    if (!contacts)
        return Fail(ERROR_NOACCESS);

    /* No touch injection is ever initialized */
    return Fail(ERROR_ACCESS_DENIED);
}

static BOOL
CheckPointerInputTarget(HWND hwnd, DWORD pointerType)
{
    if (!IsWindow(hwnd))
        return Fail(ERROR_INVALID_WINDOW_HANDLE);

    if (pointerType != PT_TOUCH_TYPE && pointerType != PT_PEN_TYPE)
        return Fail(ERROR_INVALID_PARAMETER);

    if (!IsWindowOfThisProcess(hwnd))
        return Fail(ERROR_ACCESS_DENIED);

    return TRUE;
}

BOOL
WINAPI
RegisterPointerInputTarget(_In_ HWND hwnd, _In_ DWORD pointerType)
{
    return CheckPointerInputTarget(hwnd, pointerType);
}

BOOL
WINAPI
UnregisterPointerInputTarget(_In_ HWND hwnd, _In_ DWORD pointerType)
{
    return CheckPointerInputTarget(hwnd, pointerType);
}

BOOL
WINAPI
RegisterPointerInputTargetEx(_In_ HWND hwnd, _In_ DWORD pointerType, _In_ BOOL fObserve)
{
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(pointerType);
    UNREFERENCED_PARAMETER(fObserve);
    return Fail(ERROR_NOT_SUPPORTED);
}

BOOL
WINAPI
UnregisterPointerInputTargetEx(_In_ HWND hwnd, _In_ DWORD pointerType)
{
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(pointerType);
    return Fail(ERROR_NOT_SUPPORTED);
}

/*
 * A window can ask once for pointer device change messages; a second request fails without an
 * error code. For a window of another process Windows 10 returns the number 5 (access denied) as
 * the result, with no error code set; that is kept.
 */
BOOL
WINAPI
RegisterPointerDeviceNotifications(_In_ HWND hwnd, _In_ BOOL notifyRange)
{
    BOOL Ret = FALSE;

    UNREFERENCED_PARAMETER(notifyRange);

    if (!IsWindow(hwnd))
        return Fail(ERROR_INVALID_WINDOW_HANDLE);

    if (!IsWindowOfThisProcess(hwnd))
        return ERROR_ACCESS_DENIED;

    LockState();
    if (EnsureAtom(&s_PointerNotifyAtom, s_PointerNotifyPropName) &&
        !GetPropQuiet(hwnd, s_PointerNotifyAtom))
    {
        Ret = SetPropW(hwnd, ((LPCWSTR)(ULONG_PTR)(WORD)(s_PointerNotifyAtom)), (HANDLE)1);
    }
    UnlockState();

    return Ret;
}

/* Display auto rotation: ReactOS has no orientation sensor, the preference is kept per process. */

BOOL
WINAPI
GetDisplayAutoRotationPreferences(_Out_ DWORD *orientation)
{
    if (!orientation || IsBadWritePtr(orientation, sizeof(*orientation)))
        return Fail(ERROR_INVALID_PARAMETER);

    *orientation = (DWORD)s_OrientationPreference;
    return TRUE;
}

BOOL
WINAPI
SetDisplayAutoRotationPreferences(_In_ DWORD orientation)
{
    if (orientation & ~ORIENTATION_PREFERENCE_MASK)
        return Fail(ERROR_INVALID_PARAMETER);

    InterlockedExchange(&s_OrientationPreference, (LONG)orientation);
    return TRUE;
}

BOOL
WINAPI
GetAutoRotationState(_Out_ DWORD *state)
{
    if (!state || IsBadWritePtr(state, sizeof(*state)))
        return Fail(ERROR_INVALID_PARAMETER);

    *state = AR_NOSENSOR;
    return TRUE;
}

/* Processes and windows */

/* NtQueryObject: the information classes and the size of the basic information structure. */
#define ObjectBasicInformation 0
#define ObjectTypeInformation  2

typedef struct _OBJECT_BASIC_INFORMATION_W8
{
    ULONG Attributes;
    ACCESS_MASK GrantedAccess;
    ULONG HandleCount;
    ULONG PointerCount;
    ULONG PagedPoolCharge;
    ULONG NonPagedPoolCharge;
    ULONG Reserved[3];
    ULONG NameInfoSize;
    ULONG TypeInfoSize;
    ULONG SecurityDescriptorSize;
    LARGE_INTEGER CreationTime;
} OBJECT_BASIC_INFORMATION_W8;

typedef struct _UNICODE_STRING_W8
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} UNICODE_STRING_W8;

LONG NTAPI NtQueryObject(HANDLE Handle, ULONG InformationClass, PVOID Information, ULONG Length, PULONG ReturnLength);

#define PROCESS_QUERY_LIMITED_INFORMATION_W8 0x1000
#define PROCESS_QUERY_INFORMATION_W8         0x0400

/* A process handle with the right to query it; the current process pseudo handle always qualifies. */
static BOOL
IsProcessHandleQueryable(HANDLE Process)
{
    OBJECT_BASIC_INFORMATION_W8 Basic;
    ULONGLONG TypeBuffer[64];  /* the type name string follows the structure */
    UNICODE_STRING_W8 *TypeName = (UNICODE_STRING_W8 *)TypeBuffer;
    ULONG Returned;

    if (Process == GetCurrentProcess())
        return TRUE;

    if (NtQueryObject(Process, ObjectBasicInformation, &Basic, sizeof(Basic), &Returned) < 0)
        return FALSE;

    if (!(Basic.GrantedAccess & (PROCESS_QUERY_LIMITED_INFORMATION_W8 | PROCESS_QUERY_INFORMATION_W8)))
        return FALSE;

    if (NtQueryObject(Process, ObjectTypeInformation, TypeBuffer, sizeof(TypeBuffer), &Returned) < 0)
        return FALSE;

    return TypeName->Length == 7 * sizeof(WCHAR) && !_wcsnicmp(TypeName->Buffer, L"Process", 7);
}

/* No process of ReactOS is a Windows Store (immersive) process. */
BOOL
WINAPI
IsImmersiveProcess(_In_ HANDLE process)
{
    if (!process || !IsProcessHandleQueryable(process))
        return Fail(ERROR_INVALID_PARAMETER);

    return FALSE;
}

BOOL
WINAPI
GetWindowBand(_In_ HWND hwnd, _Out_ DWORD *band)
{
    if (!IsWindow(hwnd))
        return Fail(ERROR_INVALID_WINDOW_HANDLE);

    if (!band)
        return Fail(ERROR_INVALID_PARAMETER);

    _SEH2_TRY
    {
        *band = ZBID_DEFAULT;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return Fail(ERROR_INVALID_ADDRESS));
    }
    _SEH2_END;

    return TRUE;
}

/*
 * Window feedback settings: feedback type k (1..13) owns bit 2(k-1), set when the window has a
 * setting, and the next bit, the value.
 */

#define FEEDBACK_PRESENT_BIT(k) ((UINT_PTR)1 << (2 * ((k) - 1)))
#define FEEDBACK_VALUE_BIT(k)   ((UINT_PTR)1 << (2 * ((k) - 1) + 1))

/* The value of a feedback setting: the window itself, or with the flag the nearest ancestor that has one. */
static BOOL
FindFeedbackSetting(HWND hwnd, DWORD feedback, BOOL IncludeAncestors, BOOL *Value)
{
    HWND Desktop = GetDesktopWindow();

    for (;;)
    {
        UINT_PTR Settings = (UINT_PTR)GetPropQuiet(hwnd, s_FeedbackAtom);

        if (Settings & FEEDBACK_PRESENT_BIT(feedback))
        {
            *Value = (Settings & FEEDBACK_VALUE_BIT(feedback)) != 0;
            return TRUE;
        }

        if (!IncludeAncestors)
            return FALSE;

        hwnd = GetAncestor(hwnd, GA_PARENT);
        if (!hwnd || hwnd == Desktop)
            return FALSE;
    }
}

BOOL
WINAPI
GetWindowFeedbackSetting(
    _In_ HWND hwnd,
    _In_ DWORD feedback,
    _In_ DWORD dwFlags,
    _Inout_opt_ UINT32 *pSize,
    _Out_opt_ VOID *config)
{
    UINT32 Size;
    BOOL Value = FALSE, Present = FALSE;

    if (!IsWindow(hwnd))
        return Fail(ERROR_INVALID_WINDOW_HANDLE);

    if (feedback < 1 || feedback > FEEDBACK_TYPE_COUNT || !pSize || (dwFlags & ~GWFS_INCLUDE_ANCESTORS))
        return Fail(ERROR_INVALID_PARAMETER);

    Size = *pSize;
    *pSize = sizeof(BOOL);

    if (config && Size < sizeof(BOOL))
        return Fail(ERROR_INSUFFICIENT_BUFFER);

    /* A window of another process has no setting for the caller */
    if (IsWindowOfThisProcess(hwnd))
    {
        LockState();
        if (s_FeedbackAtom)
            Present = FindFeedbackSetting(hwnd, feedback, (dwFlags & GWFS_INCLUDE_ANCESTORS) != 0, &Value);
        UnlockState();
    }

    if (config)
        *(BOOL *)config = Present && Value;
    return Present;
}

BOOL
WINAPI
SetWindowFeedbackSetting(
    _In_ HWND hwnd,
    _In_ DWORD feedback,
    _In_ DWORD dwFlags,
    _In_ UINT32 size,
    _In_opt_ const VOID *configuration)
{
    UINT_PTR Settings;
    BOOL Value = FALSE, Ret = FALSE;

    if (!IsWindow(hwnd))
        return Fail(ERROR_INVALID_WINDOW_HANDLE);

    if (feedback < 1 || feedback > FEEDBACK_TYPE_COUNT || dwFlags != 0)
        return Fail(ERROR_INVALID_PARAMETER);

    if (size != 0 && (size != sizeof(BOOL) || !configuration))
        return Fail(ERROR_INVALID_PARAMETER);

    if (configuration)
    {
        if (IsBadReadPtr(configuration, sizeof(BOOL)))
            return Fail(ERROR_NOACCESS);
        Value = *(const BOOL *)configuration != 0;
    }

    /* A window of another process fails, without an error code */
    if (!IsWindowOfThisProcess(hwnd))
    {
        SetLastError(ERROR_SUCCESS);
        return FALSE;
    }

    LockState();
    if (EnsureAtom(&s_FeedbackAtom, s_FeedbackPropName))
    {
        Settings = (UINT_PTR)GetPropQuiet(hwnd, s_FeedbackAtom);

        if (!configuration)
        {
            /* Without a value the setting returns to its default */
            Settings &= ~(FEEDBACK_PRESENT_BIT(feedback) | FEEDBACK_VALUE_BIT(feedback));
        }
        else
        {
            Settings |= FEEDBACK_PRESENT_BIT(feedback);
            if (Value)
                Settings |= FEEDBACK_VALUE_BIT(feedback);
            else
                Settings &= ~FEEDBACK_VALUE_BIT(feedback);
        }

        Ret = SetPropQuiet(hwnd, s_FeedbackAtom, (HANDLE)Settings);
    }
    UnlockState();

    if (!Ret && GetLastError() == ERROR_SUCCESS)
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return Ret;
}

/* Suspend and resume notifications: registered, but ReactOS never sends the power messages. */

HANDLE
WINAPI
RegisterSuspendResumeNotification(_In_ HANDLE recipient, _In_ DWORD flags)
{
    SUSPEND_RESUME_REGISTRATION *Registration;

    if (flags != DEVICE_NOTIFY_WINDOW_HANDLE && flags != 2)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    Registration = HeapAlloc(GetProcessHeap(), 0, sizeof(*Registration));
    if (!Registration)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    Registration->Tag = SUSPEND_RESUME_TAG;
    Registration->Recipient = recipient;
    Registration->Flags = flags;
    return Registration;
}

BOOL
WINAPI
UnregisterSuspendResumeNotification(_In_ HANDLE handle)
{
    SUSPEND_RESUME_REGISTRATION *Registration = handle;

    if (!Registration)
        return Fail(ERROR_INVALID_PARAMETER);

    _SEH2_TRY
    {
        if (Registration->Tag != SUSPEND_RESUME_TAG)
            _SEH2_YIELD(return Fail(ERROR_INVALID_PARAMETER));
        Registration->Tag = 0;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return Fail(ERROR_INVALID_PARAMETER));
    }
    _SEH2_END;

    HeapFree(GetProcessHeap(), 0, Registration);
    return TRUE;
}

/* The tolerance of a coalescable timer has no effect: timers fire on the interval. */
UINT_PTR
WINAPI
SetCoalescableTimer(
    _In_opt_ HWND hwnd,
    _In_ UINT_PTR nIDEvent,
    _In_ UINT uElapse,
    _In_opt_ TIMERPROC lpTimerFunc,
    _In_ ULONG uToleranceDelay)
{
    if (uToleranceDelay != TIMER_TOLERANCE_NONE && uToleranceDelay > TIMER_TOLERANCE_MAX)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (hwnd && !IsWindow(hwnd))
    {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }

    if (hwnd && !IsWindowOfThisProcess(hwnd))
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }

    return NtUserSetTimer(hwnd, nIDEvent, uElapse, lpTimerFunc);
}
