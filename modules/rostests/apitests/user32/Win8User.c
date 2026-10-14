/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Tests for the Windows 8 pointer, input source, touch hit testing, auto rotation,
 *              suspend and resume notification, coalescable timer, window band and window
 *              feedback functions of user32
 * COPYRIGHT:   Copyright 2026 Tomas Srnka <tomas.srnka@e2b.dev>
 *
 * The expected values were measured on Windows 10 (build 19045, a machine without touch screen
 * or pen). Everything that depends on touch, pen or messages that ReactOS does not send is left
 * out, or is only checked when the machine has no such device.
 */

#include "precomp.h"
#include <versionhelpers.h>

typedef struct
{
    DWORD pointerType;
    UINT32 pointerId;
    UINT32 frameId;
    DWORD pointerFlags;
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
} T_POINTER_INFO;

typedef struct
{
    UINT32 pointerId;
    POINT point;
    RECT boundingBox;
    RECT nonOccludedBoundingBox;
    UINT32 orientation;
} T_HIT_INPUT;

typedef struct
{
    UINT16 score;
    POINT adjustedPoint;
} T_HIT_EVAL;

typedef struct
{
    DWORD deviceType;
    DWORD originId;
} T_MESSAGE_SOURCE;

typedef struct
{
    DWORD cbSize;
    DWORD ExtStatus;
} T_CHANGEFILTER;

#define T_PT_TOUCH 2
#define T_PT_PEN   3
#define T_ERROR_SYSTEM_DEVICE_NOT_FOUND 15299
#define T_AR_NOSENSOR 0x10
#define UNSET_ERROR 0xDEADBEEF

#define FN(name) static FARPROC p##name

FN(EnableMouseInPointer);
FN(IsMouseInPointerEnabled);
FN(GetPointerType);
FN(GetPointerCursorId);
FN(GetPointerInfo);
FN(GetPointerInfoHistory);
FN(GetPointerFrameInfo);
FN(GetPointerFrameInfoHistory);
FN(GetPointerTouchInfo);
FN(GetPointerTouchInfoHistory);
FN(GetPointerFrameTouchInfo);
FN(GetPointerFrameTouchInfoHistory);
FN(GetPointerPenInfo);
FN(GetPointerPenInfoHistory);
FN(GetPointerFramePenInfo);
FN(GetPointerFramePenInfoHistory);
FN(SkipPointerFrameMessages);
FN(GetPointerDevice);
FN(GetPointerDevices);
FN(GetPointerDeviceProperties);
FN(GetPointerDeviceRects);
FN(GetPointerDeviceCursors);
FN(RegisterPointerDeviceNotifications);
FN(GetPointerInputTransform);
FN(GetRawPointerDeviceData);
FN(CreateSyntheticPointerDevice);
FN(DestroySyntheticPointerDevice);
FN(InjectSyntheticPointerInput);
FN(InitializeTouchInjection);
FN(InjectTouchInput);
FN(RegisterPointerInputTarget);
FN(UnregisterPointerInputTarget);
FN(EvaluateProximityToRect);
FN(EvaluateProximityToPolygon);
FN(PackTouchHitTestingProximityEvaluation);
FN(RegisterTouchHitTestingWindow);
FN(GetCurrentInputMessageSource);
FN(GetCIMSSM);
FN(GetDisplayAutoRotationPreferences);
FN(SetDisplayAutoRotationPreferences);
FN(GetAutoRotationState);
FN(IsImmersiveProcess);
FN(IsWindowArranged);
FN(RegisterSuspendResumeNotification);
FN(UnregisterSuspendResumeNotification);
FN(SetCoalescableTimer);
FN(GetWindowBand);
FN(GetWindowFeedbackSetting);
FN(SetWindowFeedbackSetting);
FN(ChangeWindowMessageFilterEx);

static const struct
{
    const char *Name;
    FARPROC *Proc;
} s_Functions[] =
{
#define ENTRY(name) { #name, &p##name }
    ENTRY(EnableMouseInPointer), ENTRY(IsMouseInPointerEnabled), ENTRY(GetPointerType),
    ENTRY(GetPointerCursorId), ENTRY(GetPointerInfo), ENTRY(GetPointerInfoHistory),
    ENTRY(GetPointerFrameInfo), ENTRY(GetPointerFrameInfoHistory), ENTRY(GetPointerTouchInfo),
    ENTRY(GetPointerTouchInfoHistory), ENTRY(GetPointerFrameTouchInfo),
    ENTRY(GetPointerFrameTouchInfoHistory), ENTRY(GetPointerPenInfo), ENTRY(GetPointerPenInfoHistory),
    ENTRY(GetPointerFramePenInfo), ENTRY(GetPointerFramePenInfoHistory), ENTRY(SkipPointerFrameMessages),
    ENTRY(GetPointerDevice), ENTRY(GetPointerDevices), ENTRY(GetPointerDeviceProperties),
    ENTRY(GetPointerDeviceRects), ENTRY(GetPointerDeviceCursors), ENTRY(RegisterPointerDeviceNotifications),
    ENTRY(GetPointerInputTransform), ENTRY(GetRawPointerDeviceData), ENTRY(CreateSyntheticPointerDevice),
    ENTRY(DestroySyntheticPointerDevice), ENTRY(InjectSyntheticPointerInput), ENTRY(InitializeTouchInjection),
    ENTRY(InjectTouchInput), ENTRY(RegisterPointerInputTarget), ENTRY(UnregisterPointerInputTarget),
    ENTRY(EvaluateProximityToRect), ENTRY(EvaluateProximityToPolygon),
    ENTRY(PackTouchHitTestingProximityEvaluation), ENTRY(RegisterTouchHitTestingWindow),
    ENTRY(GetCurrentInputMessageSource), ENTRY(GetCIMSSM), ENTRY(GetDisplayAutoRotationPreferences),
    ENTRY(SetDisplayAutoRotationPreferences), ENTRY(GetAutoRotationState), ENTRY(IsImmersiveProcess),
    ENTRY(IsWindowArranged), ENTRY(RegisterSuspendResumeNotification),
    ENTRY(UnregisterSuspendResumeNotification), ENTRY(SetCoalescableTimer), ENTRY(GetWindowBand),
    ENTRY(GetWindowFeedbackSetting), ENTRY(SetWindowFeedbackSetting), ENTRY(ChangeWindowMessageFilterEx),
#undef ENTRY
};

/* A call that has to fail with a given error; an error code of UNSET_ERROR means the call leaves it untouched. */
#define ok_fail(call, error) \
    do { \
        BOOL _ret; \
        SetLastError(UNSET_ERROR); \
        _ret = (BOOL)(INT_PTR)(call); \
        ok(!_ret, "%s succeeded\n", #call); \
        ok(GetLastError() == (error), "%s: error %lu, expected %lu\n", #call, GetLastError(), (DWORD)(error)); \
    } while (0)

/* A call that returns FALSE and leaves the last error alone */
#define ok_false(call) \
    do { \
        BOOL _ret; \
        SetLastError(UNSET_ERROR); \
        _ret = (BOOL)(INT_PTR)(call); \
        ok(!_ret, "%s returned TRUE\n", #call); \
        ok(GetLastError() == UNSET_ERROR, "%s set the last error to %lu\n", #call, GetLastError()); \
    } while (0)

#define ok_succeed(call) \
    do { \
        BOOL _ret; \
        SetLastError(UNSET_ERROR); \
        _ret = (BOOL)(INT_PTR)(call); \
        ok(_ret, "%s failed, error %lu\n", #call, GetLastError()); \
        ok(GetLastError() == UNSET_ERROR, "%s changed the last error to %lu\n", #call, GetLastError()); \
    } while (0)

static LRESULT CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static int Depth;

    if (msg == WM_USER + 7)
    {
        T_MESSAGE_SOURCE Source = { 0xAA, 0xBB };
        BOOL Ret;

        SetLastError(UNSET_ERROR);
        Ret = (BOOL)(INT_PTR)pGetCurrentInputMessageSource(&Source);
        ok(Ret, "GetCurrentInputMessageSource failed in a user message, error %lu\n", GetLastError());
        ok(Source.deviceType == 0 && Source.originId == 0,
           "source in a user message: device 0x%lx, origin 0x%lx\n", Source.deviceType, Source.originId);

        Source.deviceType = 0xAA;
        Source.originId = 0xBB;
        Ret = (BOOL)(INT_PTR)pGetCIMSSM(&Source);
        ok(Ret, "GetCIMSSM failed in a user message, error %lu\n", GetLastError());
        ok(Source.deviceType == 0 && Source.originId == 0,
           "sent source in a user message: device 0x%lx, origin 0x%lx\n", Source.deviceType, Source.originId);
        Depth++;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND CreateTestWindow(void)
{
    static BOOL Registered;
    WNDCLASSW Class = { 0 };

    if (!Registered)
    {
        Class.lpfnWndProc = TestWndProc;
        Class.hInstance = GetModuleHandleW(NULL);
        Class.lpszClassName = L"Win8UserTestClass";
        RegisterClassW(&Class);
        Registered = TRUE;
    }
    return CreateWindowExW(0, L"Win8UserTestClass", L"test", WS_POPUP, 0, 0, 10, 10,
                           NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static BOOL LoadFunctions(void)
{
    HMODULE User32 = GetModuleHandleW(L"user32.dll");
    BOOL All = TRUE;
    size_t i;

    for (i = 0; i < ARRAYSIZE(s_Functions); i++)
    {
        *s_Functions[i].Proc = GetProcAddress(User32, s_Functions[i].Name);
        if (!*s_Functions[i].Proc)
        {
            /* Older Windows versions do not have these functions; ReactOS has to */
            if (IsReactOS())
                ok(0, "user32!%s is not exported\n", s_Functions[i].Name);
            All = FALSE;
        }
    }
    return All;
}

static void TestMouseInPointer(const char *Mode)
{
    DWORD Initial = (Mode[0] == '1');
    DWORD Type = 0xFF;
    BOOL Ret;

    Ret = (BOOL)(INT_PTR)pIsMouseInPointerEnabled();
    ok(!Ret, "mouse in pointer is on at the start\n");

    ok_fail(pEnableMouseInPointer(2), ERROR_INVALID_PARAMETER);
    ok_fail(pEnableMouseInPointer(-1), ERROR_INVALID_PARAMETER);
    ok_succeed(pEnableMouseInPointer(Initial));
    Ret = (BOOL)(INT_PTR)pIsMouseInPointerEnabled();
    ok(Ret == (BOOL)Initial, "IsMouseInPointerEnabled returned %d\n", Ret);

    /* Once set, the other value is refused; the same value is accepted again */
    ok_fail(pEnableMouseInPointer(!Initial), ERROR_ACCESS_DENIED);
    Ret = (BOOL)(INT_PTR)pIsMouseInPointerEnabled();
    ok(Ret == (BOOL)Initial, "IsMouseInPointerEnabled returned %d after the refused change\n", Ret);
    ok_succeed(pEnableMouseInPointer(Initial));
    ok_fail(pEnableMouseInPointer(0x100), ERROR_INVALID_PARAMETER);

    /* The mouse is pointer 1 whether or not it is switched on; nothing else is a pointer */
    SetLastError(UNSET_ERROR);
    Ret = (BOOL)(INT_PTR)pGetPointerType(1, &Type);
    ok(Ret, "GetPointerType(1) failed, error %lu\n", GetLastError());
    ok(Type == 4, "GetPointerType(1) gave type %lu\n", Type);
}

static void RunMouseChild(const char *Mode)
{
    if (!LoadFunctions())
    {
        skip("The pointer functions are not available\n");
        return;
    }
    TestMouseInPointer(Mode);
    ExitProcess(winetest_get_failures());
}

static void SpawnMouseChild(const char *Mode)
{
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    char Path[MAX_PATH], CommandLine[MAX_PATH + 64];
    DWORD ExitCode = 1;

    GetModuleFileNameA(NULL, Path, sizeof(Path));
    sprintf(CommandLine, "\"%s\" Win8User mouse %s", Path, Mode);
    if (!CreateProcessA(NULL, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        ok(0, "CreateProcess failed, error %lu\n", GetLastError());
        return;
    }
    if (WaitForSingleObject(ProcessInfo.hProcess, 60000) != WAIT_OBJECT_0)
    {
        ok(0, "the mouse in pointer child did not end\n");
        TerminateProcess(ProcessInfo.hProcess, 1);
    }
    GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
    ok(ExitCode == 0, "the mouse in pointer child (%s) reported %lu failures\n", Mode, ExitCode);
    CloseHandle(ProcessInfo.hProcess);
    CloseHandle(ProcessInfo.hThread);
}

static void TestPointerQueries(void)
{
    static const UINT32 Ids[] = { 0, 1, 2, 3, 0xFFFFFFFF };
    BYTE Buffer[2048];
    T_POINTER_INFO Info[4];
    UINT32 Count, Entries, Cursor;
    DWORD Type;
    size_t i;

    for (i = 0; i < ARRAYSIZE(Ids); i++)
    {
        UINT32 Id = Ids[i];

        /* The mouse is pointer 1 whether or not mouse in pointer is on, nothing else is a pointer */
        Type = 0xFF;
        if (Id == 1)
        {
            ok_succeed(pGetPointerType(1, &Type));
            ok(Type == 4, "GetPointerType(1): type %lu\n", Type);
        }
        else
        {
            ok_fail(pGetPointerType(Id, &Type), ERROR_INVALID_PARAMETER);
            ok(Type == 0xFF, "GetPointerType(%u) changed the type\n", Id);
        }

        Cursor = 0xFF;
        ok_fail(pGetPointerCursorId(Id, &Cursor), ERROR_INVALID_PARAMETER);
        ok(Cursor == 0xFF, "GetPointerCursorId(%u) changed the cursor\n", Id);

        /* No pointer message is being processed, so there is no pointer data */
        memset(Info, 0xCD, sizeof(Info));
        ok_fail(pGetPointerInfo(Id, Info), ERROR_INVALID_PARAMETER);
        Entries = 4;
        ok_fail(pGetPointerInfoHistory(Id, &Entries, Info), ERROR_INVALID_PARAMETER);
        ok(Entries == 4, "GetPointerInfoHistory(%u) changed the entry count to %u\n", Id, Entries);
        Count = 4;
        ok_fail(pGetPointerFrameInfo(Id, &Count, Info), ERROR_INVALID_PARAMETER);
        ok(Count == 4, "GetPointerFrameInfo(%u) changed the count to %u\n", Id, Count);
        Entries = Count = 4;
        ok_fail(pGetPointerFrameInfoHistory(Id, &Entries, &Count, Info), ERROR_INVALID_PARAMETER);
        ok(Entries == 4 && Count == 4, "GetPointerFrameInfoHistory(%u) changed the counts\n", Id);
        ok_fail(pGetPointerTouchInfo(Id, Buffer), ERROR_INVALID_PARAMETER);
        ok_fail(pGetPointerPenInfo(Id, Buffer), ERROR_INVALID_PARAMETER);
        ok_fail(pGetPointerInputTransform(Id, 1, Buffer), ERROR_INVALID_PARAMETER);
        {
            LONG Values[4];
            ok_fail(pGetRawPointerDeviceData(Id, 1, 1, Buffer, Values), ERROR_INVALID_PARAMETER);
        }
        ok_fail(pSkipPointerFrameMessages(Id), Id == 1 ? ERROR_ACCESS_DENIED : ERROR_INVALID_PARAMETER);
    }

    ok_fail(pGetPointerType(1, NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pGetPointerType(0, NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pGetPointerCursorId(1, NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pGetPointerInfo(1, NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pGetPointerInfoHistory(1, NULL, NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pGetPointerFrameInfo(1, NULL, NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pGetPointerTouchInfo(1, NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pGetPointerPenInfo(1, NULL), ERROR_INVALID_PARAMETER);

    /* An unusable data buffer is an access error on 64-bit Windows and an id error on 32-bit Windows;
       a count buffer and the transform buffer are access errors on both */
    {
        void *Bad = (void *)(ULONG_PTR)0xDEADBEEF;
#ifdef _WIN64
        DWORD DataError = ERROR_NOACCESS;
#else
        DWORD DataError = ERROR_INVALID_PARAMETER;
#endif
        Entries = 2;
        ok_fail(pGetPointerInfo(1, Bad), DataError);
        ok_fail(pGetPointerInfoHistory(1, &Entries, Bad), DataError);
        ok_fail(pGetPointerInfoHistory(1, Bad, Buffer), ERROR_NOACCESS);
        ok_fail(pGetPointerFrameInfo(1, &Entries, Bad), DataError);
        ok_fail(pGetPointerFrameInfo(1, Bad, Buffer), ERROR_NOACCESS);
        ok_fail(pGetPointerFrameInfoHistory(1, &Entries, &Entries, Bad), DataError);
        ok_fail(pGetPointerTouchInfo(1, Bad), DataError);
        ok_fail(pGetPointerPenInfo(1, Bad), DataError);
        ok_fail(pGetPointerInputTransform(1, 1, Bad), ERROR_NOACCESS);
        ok_fail(pGetPointerInputTransform(2, 1, Bad), ERROR_NOACCESS);
        ok_fail(pGetPointerInfo(2, Bad), DataError);
    }

    /* The type of a bad pointer is reported only when the id is the mouse */
    ok_fail(pGetPointerType(1, (void *)(ULONG_PTR)0xDEADBEEF), ERROR_INVALID_ADDRESS);
    ok_fail(pGetPointerType(2, (void *)(ULONG_PTR)0xDEADBEEF), ERROR_INVALID_PARAMETER);
}

static void TestPointerDevices(void)
{
    UINT32 Count = 0xFFFF;
    BYTE Buffer[8192];
    HANDLE Bogus = (HANDLE)(ULONG_PTR)0x1234;
    UINT32 PropertyCount = 3;
    RECT A, B;

    ok_succeed(pGetPointerDevices(&Count, NULL));
    ok_fail(pGetPointerDevices(NULL, NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pGetPointerDevices((void *)(ULONG_PTR)0xDEADBEEF, NULL), ERROR_NOACCESS);

    if (Count != 0)
    {
        skip("This machine has %u pointer devices\n", Count);
    }
    else
    {
        /* Without devices a buffer cannot be filled */
        Count = 0;
        memset(Buffer, 0, sizeof(Buffer));
        ok_fail(pGetPointerDevices(&Count, Buffer), T_ERROR_SYSTEM_DEVICE_NOT_FOUND);
        ok(Count == 0, "the count changed to %u\n", Count);
        Count = 64;
        ok_fail(pGetPointerDevices(&Count, Buffer), T_ERROR_SYSTEM_DEVICE_NOT_FOUND);
        ok(Count == 64, "the count changed to %u\n", Count);

        ok_fail(pGetPointerDevice(Bogus, Buffer), ERROR_INVALID_HANDLE);
        ok_fail(pGetPointerDevice(NULL, Buffer), ERROR_INVALID_PARAMETER);
        ok_fail(pGetPointerDevice(Bogus, NULL), ERROR_INVALID_PARAMETER);
        ok_fail(pGetPointerDeviceProperties(Bogus, &PropertyCount, NULL), ERROR_INVALID_HANDLE);
        ok_fail(pGetPointerDeviceProperties(NULL, &PropertyCount, NULL), ERROR_INVALID_PARAMETER);
        ok_fail(pGetPointerDeviceProperties(Bogus, NULL, NULL), ERROR_INVALID_PARAMETER);
        ok_fail(pGetPointerDeviceProperties(NULL, NULL, NULL), ERROR_INVALID_PARAMETER);
        ok_fail(pGetPointerDeviceRects(Bogus, &A, &B), ERROR_INVALID_PARAMETER);
        ok_fail(pGetPointerDeviceRects(NULL, &A, &B), ERROR_INVALID_PARAMETER);
        ok_fail(pGetPointerDeviceRects(Bogus, NULL, NULL), ERROR_INVALID_PARAMETER);
        ok_fail(pGetPointerDeviceCursors(Bogus, &PropertyCount, NULL), ERROR_INVALID_HANDLE);
        ok_fail(pGetPointerDeviceCursors(NULL, &PropertyCount, NULL), ERROR_INVALID_PARAMETER);
        ok_fail(pGetPointerDeviceCursors(Bogus, NULL, NULL), ERROR_INVALID_PARAMETER);
        ok_fail(pGetPointerDeviceCursors(NULL, NULL, NULL), ERROR_INVALID_PARAMETER);
    }
}

static void TestDeviceNotifications(HWND Window)
{
    HWND Second = CreateTestWindow();

    /* A window can ask for pointer device changes once; asking again fails without an error code */
    ok_succeed(pRegisterPointerDeviceNotifications(Window, FALSE));
    ok_false(pRegisterPointerDeviceNotifications(Window, TRUE));
    ok_false(pRegisterPointerDeviceNotifications(Window, FALSE));
    ok_succeed(pRegisterPointerDeviceNotifications(Second, TRUE));

    ok_fail(pRegisterPointerDeviceNotifications(NULL, TRUE), ERROR_INVALID_WINDOW_HANDLE);
    ok_fail(pRegisterPointerDeviceNotifications((HWND)(ULONG_PTR)0x1234, TRUE), ERROR_INVALID_WINDOW_HANDLE);

    DestroyWindow(Second);
}

static void TestMessageSource(HWND Window)
{
    T_MESSAGE_SOURCE Source = { 0xAA, 0xBB };

    /* Outside a message nothing is known about a source */
    ok_succeed(pGetCurrentInputMessageSource(&Source));
    ok(Source.deviceType == 0 && Source.originId == 0, "source outside a message: %lu %lu\n",
       Source.deviceType, Source.originId);
    Source.deviceType = 0xAA;
    Source.originId = 0xBB;
    ok_succeed(pGetCIMSSM(&Source));
    ok(Source.deviceType == 0 && Source.originId == 0, "sent source outside a message: %lu %lu\n",
       Source.deviceType, Source.originId);
    ok_fail(pGetCurrentInputMessageSource(NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pGetCIMSSM(NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pGetCurrentInputMessageSource((void *)(ULONG_PTR)0xDEADBEEF), ERROR_NOACCESS);
    ok_fail(pGetCIMSSM((void *)(ULONG_PTR)0xDEADBEEF), ERROR_NOACCESS);

    /* A message that is no input message has no source; the window procedure checks that */
    SendMessageW(Window, WM_USER + 7, 0, 0);
}

static void TestTouchHitTesting(HWND Window)
{
    T_HIT_INPUT Input;
    T_HIT_EVAL Eval;
    RECT Target = { 0, 0, 100, 100 };
    POINT Square[4] = { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 } };
    ULONG Value;
    LRESULT Packed;
    BOOL Ret;

    for (Value = 0; Value < 4; Value++)
        ok_succeed(pRegisterTouchHitTestingWindow(Window, Value));
    ok_succeed(pRegisterTouchHitTestingWindow(Window, 0xFFFFFFFF));
    ok_fail(pRegisterTouchHitTestingWindow(NULL, 1), ERROR_INVALID_WINDOW_HANDLE);
    ok_fail(pRegisterTouchHitTestingWindow((HWND)(ULONG_PTR)0x1234, 1), ERROR_INVALID_WINDOW_HANDLE);

    memset(&Input, 0, sizeof(Input));
    Input.pointerId = 1;
    Input.point.x = 10;
    Input.point.y = 10;
    Input.boundingBox.left = 5;
    Input.boundingBox.top = 5;
    Input.boundingBox.right = 15;
    Input.boundingBox.bottom = 15;
    Input.nonOccludedBoundingBox = Input.boundingBox;

    /* A touch well inside the target scores 0 and keeps its point */
    Eval.score = 0xAAAA;
    Eval.adjustedPoint.x = Eval.adjustedPoint.y = 0xBB;
    ok_succeed(pEvaluateProximityToRect(&Target, &Input, &Eval));
    ok(Eval.score == 0, "rect score %u\n", Eval.score);
    ok(Eval.adjustedPoint.x == 10 && Eval.adjustedPoint.y == 10, "rect point %ld,%ld\n",
       Eval.adjustedPoint.x, Eval.adjustedPoint.y);
    Eval.score = 0xAAAA;
    Eval.adjustedPoint.x = Eval.adjustedPoint.y = 0xBB;
    ok_succeed(pEvaluateProximityToPolygon(4, Square, &Input, &Eval));
    ok(Eval.score == 0, "polygon score %u\n", Eval.score);
    ok(Eval.adjustedPoint.x == 10 && Eval.adjustedPoint.y == 10, "polygon point %ld,%ld\n",
       Eval.adjustedPoint.x, Eval.adjustedPoint.y);

    /* A touch far from the target gets the score of a clear miss */
    Input.point.x = 300;
    Input.point.y = 300;
    Input.boundingBox.left = 295;
    Input.boundingBox.top = 295;
    Input.boundingBox.right = 305;
    Input.boundingBox.bottom = 305;
    Input.nonOccludedBoundingBox = Input.boundingBox;
    ok_succeed(pEvaluateProximityToRect(&Target, &Input, &Eval));
    ok(Eval.score == 4094, "rect miss score %u\n", Eval.score);
    ok_succeed(pEvaluateProximityToPolygon(4, Square, &Input, &Eval));
    ok(Eval.score == 4094, "polygon miss score %u\n", Eval.score);
    ok(Eval.adjustedPoint.x == 300 && Eval.adjustedPoint.y == 300, "polygon miss point %ld,%ld\n",
       Eval.adjustedPoint.x, Eval.adjustedPoint.y);

    /* Arguments that cannot be evaluated */
    Eval.score = 0xAAAA;
    ok_fail(pEvaluateProximityToRect(NULL, &Input, &Eval), ERROR_BAD_ARGUMENTS);
    ok_fail(pEvaluateProximityToRect(&Target, NULL, &Eval), ERROR_BAD_ARGUMENTS);
    ok_fail(pEvaluateProximityToRect(&Target, &Input, NULL), ERROR_BAD_ARGUMENTS);
    ok_fail(pEvaluateProximityToPolygon(0, Square, &Input, &Eval), ERROR_BAD_ARGUMENTS);
    ok_fail(pEvaluateProximityToPolygon(2, Square, &Input, &Eval), ERROR_BAD_ARGUMENTS);
    ok_fail(pEvaluateProximityToPolygon(3, NULL, &Input, &Eval), ERROR_BAD_ARGUMENTS);
    ok(Eval.score == 0xAAAA, "the score changed on failure\n");
    memset(&Input.boundingBox, 0, sizeof(Input.boundingBox));
    ok_fail(pEvaluateProximityToRect(&Target, &Input, &Eval), ERROR_BAD_ARGUMENTS);
    ok_fail(pEvaluateProximityToPolygon(4, Square, &Input, &Eval), ERROR_BAD_ARGUMENTS);
    Input.boundingBox.left = Input.boundingBox.top = 55;
    Input.boundingBox.right = Input.boundingBox.bottom = 45;
    ok_fail(pEvaluateProximityToRect(&Target, &Input, &Eval), ERROR_BAD_ARGUMENTS);
    ok(Eval.score == 0xAAAA, "the score changed on failure\n");

    /* The packed evaluation: score in bits 31..20, then the offset of the adjusted point */
    memset(&Input, 0, sizeof(Input));
    Input.pointerId = 1;
    Input.point.x = 50;
    Input.point.y = 60;
    Eval.score = 0;
    Eval.adjustedPoint.x = Eval.adjustedPoint.y = 0;
    SetLastError(UNSET_ERROR);
    Packed = (LRESULT)pPackTouchHitTestingProximityEvaluation(&Input, &Eval);
    ok((LONG)Packed == 0x000F3BC4, "pack: 0x%lx\n", (LONG)Packed);
    ok(GetLastError() == UNSET_ERROR, "pack changed the last error\n");
    Eval.score = 1;
    Packed = (LRESULT)pPackTouchHitTestingProximityEvaluation(&Input, &Eval);
    ok((LONG)Packed == 0x001F3BC4, "pack with score 1: 0x%lx\n", (LONG)Packed);
    Eval.score = 0x64;
    Packed = (LRESULT)pPackTouchHitTestingProximityEvaluation(&Input, &Eval);
    ok((LONG)Packed == 0x064F3BC4, "pack with score 0x64: 0x%lx\n", (LONG)Packed);
    Eval.score = 5;
    Eval.adjustedPoint.x = 51;
    Eval.adjustedPoint.y = 61;
    Packed = (LRESULT)pPackTouchHitTestingProximityEvaluation(&Input, &Eval);
    ok((LONG)Packed == 0x00500401, "pack with offset 1,1: 0x%lx\n", (LONG)Packed);
    Eval.adjustedPoint.x = 49;
    Eval.adjustedPoint.y = 59;
    Packed = (LRESULT)pPackTouchHitTestingProximityEvaluation(&Input, &Eval);
    ok((LONG)Packed == 0x005FFFFF, "pack with offset -1,-1: 0x%lx\n", (LONG)Packed);
    Eval.score = 0x7FF;
    Eval.adjustedPoint.x = 50 + 255;
    Eval.adjustedPoint.y = 60 - 255;
    Packed = (LRESULT)pPackTouchHitTestingProximityEvaluation(&Input, &Eval);
    ok((LONG)Packed == 0x7FF3FF01, "pack with offset 255,-255: 0x%lx\n", (LONG)Packed);
    Eval.score = 5;
    Eval.adjustedPoint.x = 50 + 510;
    Eval.adjustedPoint.y = 60;
    Packed = (LRESULT)pPackTouchHitTestingProximityEvaluation(&Input, &Eval);
    ok((LONG)Packed == 0x0057F800, "pack with offset 510: 0x%lx\n", (LONG)Packed);
    Eval.adjustedPoint.x = 50 + 511;
    Packed = (LRESULT)pPackTouchHitTestingProximityEvaluation(&Input, &Eval);
    ok((ULONG_PTR)Packed == 0xFFF00000, "pack with offset 511: 0x%Ix\n", (ULONG_PTR)Packed);
    Eval.adjustedPoint.x = 50 - 511;
    Packed = (LRESULT)pPackTouchHitTestingProximityEvaluation(&Input, &Eval);
    ok((ULONG_PTR)Packed == 0xFFF00000, "pack with offset -511: 0x%Ix\n", (ULONG_PTR)Packed);
    Eval.adjustedPoint.x = 50;
    Eval.adjustedPoint.y = 60 + 511;
    Packed = (LRESULT)pPackTouchHitTestingProximityEvaluation(&Input, &Eval);
    ok((ULONG_PTR)Packed == 0xFFF00000, "pack with offset y 511: 0x%Ix\n", (ULONG_PTR)Packed);
    Eval.score = 0xFFF;
    Eval.adjustedPoint = Input.point;
    Packed = (LRESULT)pPackTouchHitTestingProximityEvaluation(&Input, &Eval);
    ok(Packed == (LRESULT)(LONG)0xFFF00000, "pack with score 0xFFF: 0x%Ix\n", (ULONG_PTR)Packed);
    Eval.score = 0x1000;
    Packed = (LRESULT)pPackTouchHitTestingProximityEvaluation(&Input, &Eval);
    ok((ULONG_PTR)Packed == 0xFFF00000, "pack with score 0x1000: 0x%Ix\n", (ULONG_PTR)Packed);
    Eval.score = 0x1234;
    Eval.adjustedPoint.x = 60;
    Eval.adjustedPoint.y = 80;
    Packed = (LRESULT)pPackTouchHitTestingProximityEvaluation(&Input, &Eval);
    ok((ULONG_PTR)Packed == 0xFFF00000, "pack with score 0x1234: 0x%Ix\n", (ULONG_PTR)Packed);
    (void)Ret;
}

static void TestInjectionParameters(HWND Window)
{
    BYTE Buffer[256];
    DWORD Type;
    HANDLE Device;

    /* Pointer types that cannot be injected, or counts and modes out of range */
    for (Type = 0; Type <= 6; Type++)
    {
        if (Type == T_PT_TOUCH || Type == T_PT_PEN)
            continue;
        SetLastError(UNSET_ERROR);
        Device = (HANDLE)pCreateSyntheticPointerDevice(Type, 1, 1);
        ok(Device == NULL, "CreateSyntheticPointerDevice(%lu) gave a device\n", Type);
        ok(GetLastError() == ERROR_INVALID_PARAMETER, "type %lu: error %lu\n", Type, GetLastError());
    }
    SetLastError(UNSET_ERROR);
    ok((HANDLE)pCreateSyntheticPointerDevice(T_PT_TOUCH, 0, 1) == NULL, "zero contacts accepted\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());
    SetLastError(UNSET_ERROR);
    ok((HANDLE)pCreateSyntheticPointerDevice(T_PT_TOUCH, 1, 9) == NULL, "feedback mode 9 accepted\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());
    SetLastError(UNSET_ERROR);
    ok((HANDLE)pCreateSyntheticPointerDevice(T_PT_TOUCH, 1, 0) == NULL, "feedback mode 0 accepted\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());
    SetLastError(UNSET_ERROR);
    ok((HANDLE)pCreateSyntheticPointerDevice(T_PT_TOUCH, 257, 1) == NULL, "257 contacts accepted\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());
    SetLastError(UNSET_ERROR);
    ok((HANDLE)pCreateSyntheticPointerDevice(T_PT_TOUCH, 1000, 1) == NULL, "1000 contacts accepted\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "error %lu\n", GetLastError());

    memset(Buffer, 0, sizeof(Buffer));
    ok_fail(pInjectSyntheticPointerInput(NULL, NULL, 0), ERROR_INVALID_PARAMETER);
    ok_fail(pInjectSyntheticPointerInput((HANDLE)(ULONG_PTR)0x1234, Buffer, 1), ERROR_INVALID_HANDLE);

    ok_fail(pInitializeTouchInjection(0, 1), ERROR_INVALID_PARAMETER);
    ok_fail(pInitializeTouchInjection(1, 0), ERROR_INVALID_PARAMETER);
    ok_fail(pInitializeTouchInjection(1, 4), ERROR_INVALID_PARAMETER);
    ok_fail(pInitializeTouchInjection(257, 1), ERROR_INVALID_PARAMETER);
    ok_fail(pInitializeTouchInjection(1000, 1), ERROR_INVALID_PARAMETER);
    ok_fail(pInjectTouchInput(0, NULL), ERROR_INVALID_PARAMETER);

    /* Pointer input targets: touch and pen, in a window of this process */
    for (Type = 0; Type <= 6; Type++)
    {
        if (Type == T_PT_TOUCH || Type == T_PT_PEN)
        {
            ok_succeed(pRegisterPointerInputTarget(Window, Type));
            ok_succeed(pRegisterPointerInputTarget(Window, Type));
            ok_succeed(pUnregisterPointerInputTarget(Window, Type));
            ok_succeed(pUnregisterPointerInputTarget(Window, Type));
        }
        else
        {
            ok_fail(pRegisterPointerInputTarget(Window, Type), ERROR_INVALID_PARAMETER);
            ok_fail(pUnregisterPointerInputTarget(Window, Type), ERROR_INVALID_PARAMETER);
        }
    }
    ok_fail(pRegisterPointerInputTarget(NULL, T_PT_TOUCH), ERROR_INVALID_WINDOW_HANDLE);
    ok_fail(pRegisterPointerInputTarget((HWND)(ULONG_PTR)0x1234, T_PT_TOUCH), ERROR_INVALID_WINDOW_HANDLE);
    ok_fail(pUnregisterPointerInputTarget((HWND)(ULONG_PTR)0x1234, T_PT_TOUCH), ERROR_INVALID_WINDOW_HANDLE);
    ok_fail(pRegisterPointerInputTarget(GetDesktopWindow(), T_PT_TOUCH), ERROR_ACCESS_DENIED);
    ok_fail(pUnregisterPointerInputTarget(GetDesktopWindow(), T_PT_TOUCH), ERROR_ACCESS_DENIED);
}

static void TestAutoRotation(void)
{
    static const DWORD Valid[] = { 0, 1, 2, 4, 8, 3, 0xF, 0 };
    DWORD Preference, State, Saved;
    size_t i;

    ok_succeed(pGetDisplayAutoRotationPreferences(&Saved));
    ok_fail(pGetDisplayAutoRotationPreferences(NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pGetDisplayAutoRotationPreferences((DWORD *)(ULONG_PTR)0xDEADBEEF), ERROR_INVALID_PARAMETER);

    for (i = 0; i < ARRAYSIZE(Valid); i++)
    {
        ok_succeed(pSetDisplayAutoRotationPreferences(Valid[i]));
        Preference = 0xFFFF;
        ok_succeed(pGetDisplayAutoRotationPreferences(&Preference));
        ok(Preference == Valid[i], "preference 0x%lx, expected 0x%lx\n", Preference, Valid[i]);
    }

    /* A rejected value leaves the preference as it was */
    ok_succeed(pSetDisplayAutoRotationPreferences(3));
    ok_fail(pSetDisplayAutoRotationPreferences(0x10), ERROR_INVALID_PARAMETER);
    ok_fail(pSetDisplayAutoRotationPreferences(0xFFFF), ERROR_INVALID_PARAMETER);
    Preference = 0xFFFF;
    ok_succeed(pGetDisplayAutoRotationPreferences(&Preference));
    ok(Preference == 3, "preference 0x%lx after rejected values\n", Preference);
    ok_succeed(pSetDisplayAutoRotationPreferences(Saved));

    State = 0xFFFF;
    ok_succeed(pGetAutoRotationState(&State));
    ok((State & ~0xFF) == 0, "unknown bits in the state 0x%lx\n", State);
    ok_fail(pGetAutoRotationState(NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pGetAutoRotationState((DWORD *)(ULONG_PTR)0xDEADBEEF), ERROR_INVALID_PARAMETER);
}

static void TestProcessAndWindow(HWND Window)
{
    HANDLE Event, Process;
    DWORD Band;

    /* The current process, as a pseudo handle, a real handle or both, is a normal process */
    ok_false(pIsImmersiveProcess(GetCurrentProcess()));
    ok_fail(pIsImmersiveProcess(NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pIsImmersiveProcess((HANDLE)(ULONG_PTR)0x1234), ERROR_INVALID_PARAMETER);
    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok_fail(pIsImmersiveProcess(Event), ERROR_INVALID_PARAMETER);
    CloseHandle(Event);
    Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
    if (Process)
    {
        ok_false(pIsImmersiveProcess(Process));
        CloseHandle(Process);
    }
    /* The handle needs the right to query the process, limited or full; other rights are of no use */
    Process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    ok(Process != NULL, "OpenProcess(PROCESS_QUERY_INFORMATION) failed, error %lu\n", GetLastError());
    if (Process)
    {
        ok_false(pIsImmersiveProcess(Process));
        CloseHandle(Process);
    }
    Process = OpenProcess(PROCESS_VM_READ, FALSE, GetCurrentProcessId());
    if (Process)
    {
        ok_fail(pIsImmersiveProcess(Process), ERROR_INVALID_PARAMETER);
        CloseHandle(Process);
    }
    Process = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
    if (Process)
    {
        ok_fail(pIsImmersiveProcess(Process), ERROR_INVALID_PARAMETER);
        CloseHandle(Process);
    }

    ok_false(pIsWindowArranged(Window));
    ok_fail(pIsWindowArranged(NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pIsWindowArranged((HWND)(ULONG_PTR)0x1234), ERROR_INVALID_PARAMETER);

    Band = 0xFFFF;
    ok_succeed(pGetWindowBand(Window, &Band));
    ok(Band == 1, "band %lu\n", Band);
    Band = 0xFFFF;
    ok_succeed(pGetWindowBand(GetDesktopWindow(), &Band));
    ok(Band == 1, "desktop band %lu\n", Band);
    ok_fail(pGetWindowBand(NULL, &Band), ERROR_INVALID_WINDOW_HANDLE);
    ok_fail(pGetWindowBand(Window, NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pGetWindowBand(Window, (DWORD *)(ULONG_PTR)0xDEADBEEF), ERROR_INVALID_ADDRESS);
}

static void TestFeedback(HWND Window)
{
    UINT32 Size;
    BOOL Config, Value;
    DWORD Feedback;

    for (Feedback = 1; Feedback <= 13; Feedback++)
    {
        /* No setting yet: the call fails without an error code and returns zero */
        Size = 4;
        Config = 0xCC;
        ok_false(pGetWindowFeedbackSetting(Window, Feedback, 0, &Size, &Config));
        ok(Size == 4 && Config == 0, "feedback %lu: initial size %u value %d\n", Feedback, Size, Config);

        Value = TRUE;
        ok_succeed(pSetWindowFeedbackSetting(Window, Feedback, 0, sizeof(BOOL), &Value));
        Size = 4;
        Config = 0xCC;
        ok_succeed(pGetWindowFeedbackSetting(Window, Feedback, 0, &Size, &Config));
        ok(Size == 4 && Config == 1, "feedback %lu: size %u value %d after TRUE\n", Feedback, Size, Config);

        Value = FALSE;
        ok_succeed(pSetWindowFeedbackSetting(Window, Feedback, 0, sizeof(BOOL), &Value));
        Size = 4;
        Config = 0xCC;
        ok_succeed(pGetWindowFeedbackSetting(Window, Feedback, 0, &Size, &Config));
        ok(Size == 4 && Config == 0, "feedback %lu: size %u value %d after FALSE\n", Feedback, Size, Config);

        /* Any non-zero value reads back as TRUE */
        Value = 7;
        ok_succeed(pSetWindowFeedbackSetting(Window, Feedback, 0, sizeof(BOOL), &Value));
        Size = 4;
        Config = 0xCC;
        ok_succeed(pGetWindowFeedbackSetting(Window, Feedback, 0, &Size, &Config));
        ok(Config == 1, "feedback %lu: value %d after 7\n", Feedback, Config);

        /* Without a value the setting goes back to its default */
        ok_succeed(pSetWindowFeedbackSetting(Window, Feedback, 0, 0, NULL));
        Size = 4;
        Config = 0xCC;
        ok_false(pGetWindowFeedbackSetting(Window, Feedback, 0, &Size, &Config));
        ok(Size == 4 && Config == 0, "feedback %lu: size %u value %d after removal\n", Feedback, Size, Config);
    }

    for (Feedback = 0; Feedback <= 14; Feedback += 14)
    {
        Value = TRUE;
        ok_fail(pSetWindowFeedbackSetting(Window, Feedback, 0, sizeof(BOOL), &Value), ERROR_INVALID_PARAMETER);
        Size = 4;
        ok_fail(pGetWindowFeedbackSetting(Window, Feedback, 0, &Size, &Config), ERROR_INVALID_PARAMETER);
    }

    /* A value pointer without a size still stores the setting */
    pSetWindowFeedbackSetting(Window, 1, 0, 0, NULL);
    Value = TRUE;
    ok_succeed(pSetWindowFeedbackSetting(Window, 1, 0, 0, &Value));
    Size = 4;
    Config = 0xCC;
    ok_succeed(pGetWindowFeedbackSetting(Window, 1, 0, &Size, &Config));
    ok(Size == 4 && Config == 1, "size %u value %d after a store without a size\n", Size, Config);
    pSetWindowFeedbackSetting(Window, 1, 0, 0, NULL);
    ok_fail(pSetWindowFeedbackSetting(Window, 1, 0, 4, NULL), ERROR_INVALID_PARAMETER);
    ok_fail(pSetWindowFeedbackSetting(Window, 1, 0, 8, &Value), ERROR_INVALID_PARAMETER);
    ok_fail(pSetWindowFeedbackSetting(Window, 1, 0, 2, &Value), ERROR_INVALID_PARAMETER);
    ok_fail(pSetWindowFeedbackSetting(Window, 1, 1, sizeof(BOOL), &Value), ERROR_INVALID_PARAMETER);
    ok_fail(pSetWindowFeedbackSetting(NULL, 1, 0, sizeof(BOOL), &Value), ERROR_INVALID_WINDOW_HANDLE);
    ok_fail(pGetWindowFeedbackSetting(NULL, 1, 0, &Size, &Config), ERROR_INVALID_WINDOW_HANDLE);
    ok_fail(pGetWindowFeedbackSetting(Window, 1, 0, NULL, &Config), ERROR_INVALID_PARAMETER);
    ok_fail(pGetWindowFeedbackSetting(Window, 1, 2, &Size, &Config), ERROR_INVALID_PARAMETER);
    ok_fail(pGetWindowFeedbackSetting(Window, 1, 0xFF, &Size, &Config), ERROR_INVALID_PARAMETER);

    /* A buffer that is too small, and a query for the size */
    Size = 2;
    Config = 0xCC;
    ok_fail(pGetWindowFeedbackSetting(Window, 1, 0, &Size, &Config), ERROR_INSUFFICIENT_BUFFER);
    ok(Size == 4 && Config == 0xCC, "size %u value %d after the short buffer\n", Size, Config);
    Size = 0;
    ok_false(pGetWindowFeedbackSetting(Window, 1, 0, &Size, NULL));
    ok(Size == 4, "size %u from the size query\n", Size);
    Value = TRUE;
    pSetWindowFeedbackSetting(Window, 1, 0, sizeof(BOOL), &Value);
    Size = 0;
    ok_succeed(pGetWindowFeedbackSetting(Window, 1, 0, &Size, NULL));
    ok(Size == 4, "size %u from the size query of a stored setting\n", Size);
    /* With the flag for ancestors a window that has no setting still has none */
    pSetWindowFeedbackSetting(Window, 1, 0, 0, NULL);
    Size = 4;
    ok_false(pGetWindowFeedbackSetting(Window, 1, 1, &Size, &Config));
    pSetWindowFeedbackSetting(Window, 1, 0, 0, NULL);
}

static void TestSuspendResume(HWND Window)
{
    HANDLE Handle, Other;
    DWORD Flags;

    SetLastError(UNSET_ERROR);
    Handle = (HANDLE)pRegisterSuspendResumeNotification((HANDLE)Window, 0);
    ok(Handle != NULL, "RegisterSuspendResumeNotification failed, error %lu\n", GetLastError());
    ok(GetLastError() == UNSET_ERROR, "the last error changed to %lu\n", GetLastError());
    Other = (HANDLE)pRegisterSuspendResumeNotification((HANDLE)Window, 0);
    ok(Other != NULL && Other != Handle, "a second registration gave %p after %p\n", Other, Handle);
    ok_succeed(pUnregisterSuspendResumeNotification(Handle));
    ok_succeed(pUnregisterSuspendResumeNotification(Other));

    /* The recipient is not looked at when it is registered */
    Handle = (HANDLE)pRegisterSuspendResumeNotification(NULL, 0);
    ok(Handle != NULL, "registration for no window failed, error %lu\n", GetLastError());
    ok_succeed(pUnregisterSuspendResumeNotification(Handle));
    Handle = (HANDLE)pRegisterSuspendResumeNotification((HANDLE)(ULONG_PTR)0x1234, 0);
    ok(Handle != NULL, "registration for a bad window failed, error %lu\n", GetLastError());
    ok_succeed(pUnregisterSuspendResumeNotification(Handle));

    /* Only a window (0) or a callback (2) can be the recipient */
    for (Flags = 1; Flags <= 9; Flags++)
    {
        if (Flags == 2)
            continue;
        SetLastError(UNSET_ERROR);
        Handle = (HANDLE)pRegisterSuspendResumeNotification((HANDLE)Window, Flags);
        ok(Handle == NULL, "flags %lu gave a handle\n", Flags);
        ok(GetLastError() == ERROR_INVALID_PARAMETER, "flags %lu: error %lu\n", Flags, GetLastError());
    }
    ok_fail(pUnregisterSuspendResumeNotification(NULL), ERROR_INVALID_PARAMETER);
}

static void TestCoalescableTimer(HWND Window)
{
    UINT_PTR Id;
    MSG Message;
    DWORD Start;
    BOOL Fired = FALSE;

    SetLastError(UNSET_ERROR);
    Id = (UINT_PTR)pSetCoalescableTimer(Window, (UINT_PTR)77, 30, NULL, 0);
    ok(Id == 77, "timer id %Iu\n", Id);
    ok(GetLastError() == UNSET_ERROR, "the last error changed to %lu\n", GetLastError());
    Start = GetTickCount();
    while (!Fired && GetTickCount() - Start < 3000)
    {
        while (PeekMessageW(&Message, Window, 0, 0, PM_REMOVE))
        {
            if (Message.message == WM_TIMER && Message.wParam == 77)
                Fired = TRUE;
            DispatchMessageW(&Message);
        }
        Sleep(10);
    }
    ok(Fired, "the timer did not fire\n");
    KillTimer(Window, 77);

    /* Tolerances: none, a value, the largest one; larger values are refused */
    Id = (UINT_PTR)pSetCoalescableTimer(Window, (UINT_PTR)78, 50, NULL, 0xFFFFFFFF);
    ok(Id == 78, "no coalescing: timer id %Iu\n", Id);
    KillTimer(Window, 78);
    Id = (UINT_PTR)pSetCoalescableTimer(Window, (UINT_PTR)79, 50, NULL, 100);
    ok(Id == 79, "tolerance 100: timer id %Iu\n", Id);
    KillTimer(Window, 79);
    Id = (UINT_PTR)pSetCoalescableTimer(Window, (UINT_PTR)80, 50, NULL, 0x7FFFFFF5);
    ok(Id == 80, "largest tolerance: timer id %Iu\n", Id);
    KillTimer(Window, 80);
    ok_fail(pSetCoalescableTimer(Window, (UINT_PTR)81, 50, NULL, 0x7FFFFFF6), ERROR_INVALID_PARAMETER);
    ok_fail(pSetCoalescableTimer(Window, (UINT_PTR)82, 50, NULL, 0x80000000), ERROR_INVALID_PARAMETER);
    ok_fail(pSetCoalescableTimer(Window, (UINT_PTR)83, 50, NULL, 0xFFFFFFFE), ERROR_INVALID_PARAMETER);

    /* An interval of zero or the largest one is accepted; a bad window is not */
    Id = (UINT_PTR)pSetCoalescableTimer(Window, (UINT_PTR)84, 0, NULL, 0);
    ok(Id == 84, "interval 0: timer id %Iu\n", Id);
    KillTimer(Window, 84);
    Id = (UINT_PTR)pSetCoalescableTimer(Window, (UINT_PTR)85, 0xFFFFFFFF, NULL, 0);
    ok(Id == 85, "interval 0xFFFFFFFF: timer id %Iu\n", Id);
    KillTimer(Window, 85);
    ok_fail(pSetCoalescableTimer((HWND)(ULONG_PTR)0x1234, (UINT_PTR)86, 50, NULL, 0), ERROR_INVALID_WINDOW_HANDLE);
}

static void TestMessageFilter(HWND Window)
{
    T_CHANGEFILTER Filter;
    UINT Message = WM_USER + 21;
    DWORD Action;

    for (Action = 0; Action <= 3; Action++)
    {
        BOOL Valid = (Action == 1 || Action == 2);

        Filter.cbSize = sizeof(Filter);
        Filter.ExtStatus = 0xAA;
        SetLastError(UNSET_ERROR);
        if (Valid)
        {
            ok(pChangeWindowMessageFilterEx(Window, Message, Action, &Filter), "action %lu failed, error %lu\n", Action, GetLastError());
            ok(Filter.ExtStatus == 0, "action %lu: status %lu\n", Action, Filter.ExtStatus);
            ok(pChangeWindowMessageFilterEx(Window, Message, Action, NULL), "action %lu without a structure failed\n", Action);
        }
        else
        {
            ok(!pChangeWindowMessageFilterEx(Window, Message, Action, &Filter), "action %lu succeeded\n", Action);
            ok(GetLastError() == ERROR_INVALID_PARAMETER, "action %lu: error %lu\n", Action, GetLastError());
            ok(Filter.ExtStatus == 0xAA, "action %lu changed the status\n", Action);
        }
    }

    Filter.cbSize = 4;
    ok_fail(pChangeWindowMessageFilterEx(Window, Message, 1, &Filter), ERROR_INVALID_PARAMETER);
    Filter.cbSize = 12;
    ok_fail(pChangeWindowMessageFilterEx(Window, Message, 1, &Filter), ERROR_INVALID_PARAMETER);
    Filter.cbSize = 0;
    ok_fail(pChangeWindowMessageFilterEx(Window, Message, 1, &Filter), ERROR_INVALID_PARAMETER);
    Filter.cbSize = sizeof(Filter);
    ok_fail(pChangeWindowMessageFilterEx(Window, 0, 1, &Filter), ERROR_INVALID_PARAMETER);
    ok_fail(pChangeWindowMessageFilterEx(NULL, Message, 1, &Filter), ERROR_INVALID_WINDOW_HANDLE);
    ok_fail(pChangeWindowMessageFilterEx((HWND)(ULONG_PTR)0x1234, Message, 1, &Filter), ERROR_INVALID_WINDOW_HANDLE);
    ok_fail(pChangeWindowMessageFilterEx(Window, Message, 1, (void *)(ULONG_PTR)0xDEADBEEF), UNSET_ERROR);
    ok_succeed(pChangeWindowMessageFilterEx(Window, WM_COPYDATA, 1, &Filter));
    ok_succeed(pChangeWindowMessageFilterEx(Window, WM_COPYDATA, 2, &Filter));
}

static void TestForeignWindow(void)
{
    HWND Tray = FindWindowW(L"Shell_TrayWnd", NULL);
    T_CHANGEFILTER Filter = { sizeof(Filter), 0xAA };
    UINT32 Size = 4;
    BOOL Config = 0xCC, Value = TRUE;
    DWORD Band = 0xFFFF;
    DWORD ProcessId = 0;

    if (Tray)
        GetWindowThreadProcessId(Tray, &ProcessId);
    if (!Tray || ProcessId == GetCurrentProcessId())
    {
        skip("There is no window of another process to test with\n");
        return;
    }

    /* The state of a window belongs to its process; the other processes are refused */
    SetLastError(UNSET_ERROR);
    ok(!(BOOL)(INT_PTR)pSetWindowFeedbackSetting(Tray, 1, 0, sizeof(BOOL), &Value), "feedback of a foreign window was set\n");
    ok(GetLastError() == ERROR_SUCCESS, "the foreign feedback set gave error %lu\n", GetLastError());
    ok_false(pGetWindowFeedbackSetting(Tray, 1, 0, &Size, &Config));
    ok(Size == 4 && Config == 0, "foreign feedback size %u value %d\n", Size, Config);

    /* Windows 10 hands the number of the error back as the result here, without an error code */
    SetLastError(UNSET_ERROR);
    ok((INT_PTR)pRegisterPointerDeviceNotifications(Tray, TRUE) == ERROR_ACCESS_DENIED, "the foreign registration did not give 5\n");
    ok(GetLastError() == UNSET_ERROR, "the foreign registration set error %lu\n", GetLastError());

    ok_fail(pRegisterTouchHitTestingWindow(Tray, 1), ERROR_ACCESS_DENIED);
    ok_fail(pRegisterPointerInputTarget(Tray, T_PT_TOUCH), ERROR_ACCESS_DENIED);
    ok_fail(pUnregisterPointerInputTarget(Tray, T_PT_TOUCH), ERROR_ACCESS_DENIED);
    ok_fail(pChangeWindowMessageFilterEx(Tray, WM_USER + 1, 1, &Filter), ERROR_ACCESS_DENIED);
    ok_fail(pSetCoalescableTimer(Tray, (UINT_PTR)5, 100, NULL, 0), ERROR_ACCESS_DENIED);

    /* Looking at a foreign window is allowed */
    ok_succeed(pGetWindowBand(Tray, &Band));
    ok(Band == 1, "foreign window band %lu\n", Band);
    ok_false(pIsWindowArranged(Tray));
}

static void TestFeedbackAncestors(void)
{
    HWND Parent, Child, Grandchild;
    UINT32 Size;
    BOOL Config, Value;

    Parent = CreateWindowExW(0, L"Win8UserTestClass", L"p", WS_POPUP, 0, 0, 50, 50, NULL, NULL, GetModuleHandleW(NULL), NULL);
    Child = CreateWindowExW(0, L"Win8UserTestClass", L"c", WS_CHILD, 0, 0, 20, 20, Parent, NULL, GetModuleHandleW(NULL), NULL);
    Grandchild = CreateWindowExW(0, L"Win8UserTestClass", L"g", WS_CHILD, 0, 0, 10, 10, Child, NULL, GetModuleHandleW(NULL), NULL);
    ok(Parent && Child && Grandchild, "the windows were not created\n");

#define GET_FEEDBACK(window, flags, expect_ret, expect_value) \
    do { \
        Size = 4; \
        Config = 0xCC; \
        SetLastError(UNSET_ERROR); \
        ok((BOOL)(INT_PTR)pGetWindowFeedbackSetting(window, 1, flags, &Size, &Config) == (expect_ret), \
           "line %d: get on " #window " with flags " #flags " gave the wrong result\n", __LINE__); \
        ok(Config == (expect_value), "line %d: value %d, expected " #expect_value "\n", __LINE__, Config); \
        ok(GetLastError() == UNSET_ERROR, "line %d: error %lu\n", __LINE__, GetLastError()); \
    } while (0)

    Value = TRUE;
    ok_succeed(pSetWindowFeedbackSetting(Parent, 1, 0, sizeof(BOOL), &Value));
    GET_FEEDBACK(Child, 0, FALSE, 0);
    GET_FEEDBACK(Child, 1, TRUE, 1);
    GET_FEEDBACK(Grandchild, 0, FALSE, 0);
    GET_FEEDBACK(Grandchild, 1, TRUE, 1);
    GET_FEEDBACK(Parent, 1, TRUE, 1);

    /* The nearest setting wins */
    Value = FALSE;
    ok_succeed(pSetWindowFeedbackSetting(Child, 1, 0, sizeof(BOOL), &Value));
    GET_FEEDBACK(Child, 0, TRUE, 0);
    GET_FEEDBACK(Child, 1, TRUE, 0);
    GET_FEEDBACK(Grandchild, 1, TRUE, 0);
    GET_FEEDBACK(Parent, 1, TRUE, 1);

    ok_succeed(pSetWindowFeedbackSetting(Child, 1, 0, 0, NULL));
    GET_FEEDBACK(Child, 1, TRUE, 1);
    GET_FEEDBACK(Grandchild, 1, TRUE, 1);
    ok_succeed(pSetWindowFeedbackSetting(Parent, 1, 0, 0, NULL));
    GET_FEEDBACK(Child, 1, FALSE, 0);
    GET_FEEDBACK(Grandchild, 1, FALSE, 0);
#undef GET_FEEDBACK

    /* Pointer device notifications are asked for by each window on its own */
    ok_succeed(pRegisterPointerDeviceNotifications(Parent, TRUE));
    ok_succeed(pRegisterPointerDeviceNotifications(Child, TRUE));

    /* A value that cannot be read */
    ok_fail(pSetWindowFeedbackSetting(Child, 1, 0, sizeof(BOOL), (void *)(ULONG_PTR)0xDEADBEEF), ERROR_NOACCESS);
    Value = TRUE;
    ok_succeed(pSetWindowFeedbackSetting(Child, 1, 0, sizeof(BOOL), &Value));

    DestroyWindow(Parent);
}

/*
 * The atom under which the feedback settings are kept: another user of the same name adds it before the first
 * use in this process and lets go of it afterwards; the atom must not be freed under the process.
 */
static void TestAtomLifetime(HWND Window)
{
    static const WCHAR Name[] = L"User32Win8Feedback";
    ATOM External, Found;
    BOOL Value = TRUE;

    if (!is_reactos())
    {
        skip("The atom is an implementation detail of ReactOS\n");
        return;
    }
    if (GlobalFindAtomW(Name))
    {
        skip("The atom exists already\n");
        return;
    }

    External = GlobalAddAtomW(Name);
    ok(External != 0, "GlobalAddAtom failed, error %lu\n", GetLastError());
    if (!External)
        return;

    ok_succeed(pSetWindowFeedbackSetting(Window, 1, 0, sizeof(BOOL), &Value));
    ok(GlobalDeleteAtom(External) == 0, "GlobalDeleteAtom failed, error %lu\n", GetLastError());

    Found = GlobalFindAtomW(Name);
    ok(Found == External, "The atom is 0x%x after the other user let go of it, expected 0x%x\n", Found, External);

    pSetWindowFeedbackSetting(Window, 1, 0, 0, NULL);
}

START_TEST(Win8User)
{
    HWND Window;
    int argc;
    char **argv;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 4 && !strcmp(argv[2], "mouse"))
    {
        RunMouseChild(argv[3]);
        return;
    }

    if (!LoadFunctions())
    {
        skip("The Windows 8 user32 functions are not available\n");
        return;
    }

    SpawnMouseChild("0");
    SpawnMouseChild("1");

    Window = CreateTestWindow();
    ok(Window != NULL, "CreateWindow failed, error %lu\n", GetLastError());

    TestAtomLifetime(Window);
    TestPointerQueries();
    TestPointerDevices();
    TestDeviceNotifications(Window);
    TestMessageSource(Window);
    TestTouchHitTesting(Window);
    TestInjectionParameters(Window);
    TestAutoRotation();
    TestProcessAndWindow(Window);
    TestFeedback(Window);
    TestFeedbackAncestors();
    TestForeignWindow();
    TestSuspendResume(Window);
    TestCoalescableTimer(Window);
    TestMessageFilter(Window);

    DestroyWindow(Window);
}
